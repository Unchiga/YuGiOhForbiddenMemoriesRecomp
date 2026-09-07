/* psx_mod_package.c -- see psx_mod_package.h.
 *
 * Nothing here knows a manager's file format. Export asks each manager to
 * write its own share file to a scratch name, copies the entries (or the
 * file) into the package, and deletes the scratch; Import cuts the package
 * back into those share files and hands each to its manager's import. The
 * managers' formats therefore stay the single source of truth, and a
 * package made today still opens when a manager's format grows, as long as
 * the manager still reads its old files.
 *
 * The one thing this file owns is mod_settings.ini: the value of every
 * MODS and CHEATS row that persists, read through the menu's row walk and
 * put back through psx_video_menu_set_row, which fires each row's callback
 * the way a click does. */

#include "psx_mod_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define rmdir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "psx_sdl.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_packs.h"
#include "psx_card_share.h"
#include "psx_card_shop.h"
#include "psx_cpu_data.h"
#include "psx_dialogue.h"
#include "psx_drop_edits.h"
#include "psx_drop_db.h"
#include "psx_drop_missing.h"
#include "psx_fusion_table.h"
#include "psx_game_hooks.h"
#include "psx_textfile.h"
#include "psx_video_menu.h"

#define CARD_COUNT 722
#define ENTRY_MAX  1024

static char s_msg[400];
const char *psx_mod_package_last_message(void) { return s_msg; }

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    fprintf(stderr, "mod package: %s\n", m);
    host_osd_push(m, 6000);
}

/* ---- paths ------------------------------------------------------------- */
static void share_dir(char *out, unsigned cap)
{
    const char *dir = psx_mod_player_data_dir();
    snprintf(out, cap, "%s/mod_packages", dir && dir[0] ? dir : ".");
    (void)MKDIR(out);
}

void psx_mod_package_default_path(char *out, unsigned cap)
{
    char dir[1024]; share_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/mod-package.%s", dir, PSX_MOD_PACKAGE_EXT);
}

static void scratch_path(const char *name, char *out, unsigned cap)
{
    char dir[1024]; share_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/.package-%s", dir, name);
}

static void player_file(const char *name, char *out, unsigned cap)
{
    const char *dir = psx_mod_player_data_dir();
    snprintf(out, cap, "%s/%s", dir && dir[0] ? dir : ".", name);
}

static int file_exists(const char *p)
{
    FILE *f = psx_fopen_utf8(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int write_bytes(const char *path, const void *d, size_t n)
{
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(d, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int is_zip(const unsigned char *b, long n)
{
    return n >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4;
}

static const char *base_name(const char *p)
{
    const char *base = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    return base;
}

/* ---- the settings part ------------------------------------------------- */
/* Every row a mod registered with a settings key, whatever menu it sits in
 * (the fusion hint, the rank meter and the widescreen switch live under
 * VIEW), except the tool windows' renderer choice: that is about this
 * machine's graphics driver, not about the game. */
static int settings_row_wanted(int h, const char **key)
{
    int kind = -1;
    if (!psx_video_menu_row_info(h, NULL, &kind, key, NULL)) return 0;
    if (!*key || !(*key)[0]) return 0;
    if (kind == PSX_VM_ROW_ACTION) return 0;
    if (!strcmp(*key, "tool_renderer")) return 0;
    return 1;
}

static int settings_text(char *out, unsigned cap)
{
    unsigned n = (unsigned)snprintf(out, cap,
        "; Yu-Gi-Oh! Forbidden Memories - Recompiled : MODS and CHEATS rows\n"
        "; key = value, as menu_settings.ini spells them. Import sets each row.\n");
    int rows = 0;
    const int count = psx_video_menu_row_count();
    for (int h = 0; h < count && n + 80u < cap; h++) {
        const char *key = NULL;
        if (!settings_row_wanted(h, &key)) continue;
        n += (unsigned)snprintf(out + n, cap - n, "%s = %d\n", key, psx_video_menu_get_row(h));
        rows++;
    }
    return rows;
}

static int settings_apply(const char *text)
{
    int applied = 0;
    const int count = psx_video_menu_row_count();
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[200]; snprintf(line, sizeof line, "%.*s", (int)(len < 199 ? len : 199), p);
        p = e ? e + 1 : p + len;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ';' || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        const int v = atoi(eq + 1);
        for (int h = 0; h < count; h++) {
            const char *key = NULL;
            if (!settings_row_wanted(h, &key) || strcmp(key, k)) continue;
            if (psx_video_menu_get_row(h) != v) psx_video_menu_set_row(h, v);
            applied++;
            break;
        }
    }
    if (applied) psx_video_menu_note_change();
    return applied;
}

/* ---- export ------------------------------------------------------------ */
typedef struct { int cards, drops, cpu, portraits, fusion, dialogue, missing, shop, settings; } Parts;

/* Copy every entry of a zip file into the package, renaming through
 * `rename` (NULL keeps names). Returns entries copied, or -1. */
static int add_zip_entries(PsxZipWriter *z, const char *zip_path,
                           const char *(*rename)(const char *), Parts *parts)
{
    long n = 0;
    unsigned char *b = psx_zip_read_file(zip_path, &n);
    if (!b) return -1;
    static PsxZipEntry ents[ENTRY_MAX];
    char err[160];
    const int k = psx_zip_list(b, n, ents, ENTRY_MAX, err, sizeof err);
    if (k < 0) { free(b); return -1; }
    int copied = 0;
    for (int i = 0; i < k; i++) {
        const char *name = rename ? rename(ents[i].name) : ents[i].name;
        if (!name) continue;
        long sz = 0; unsigned char *d = psx_zip_extract(b, n, &ents[i], &sz);
        if (!d) continue;
        const int ok = psx_zip_writer_add(z, name, d, (size_t)sz);
        free(d);
        if (!ok) { free(b); return -1; }
        copied++;
        if (parts) {
            if (!strncmp(name, "cards/", 6)) parts->cards++;
            else if (!strcmp(name, "drop_table_edits.ini")) parts->drops = 1;
            else if (!strncmp(name, "duelists/", 9)) parts->portraits++;
            else if (!strcmp(name, "cpu-duelists.ini")) parts->cpu = 1;
        }
    }
    free(b);
    return copied;
}

static const char *rename_cards(const char *name)
{
    static char buf[64];
    if (!strcmp(name, "manifest.ini")) return "cards-manifest.ini";
    snprintf(buf, sizeof buf, "%s", name);
    return buf;
}

static int add_file(PsxZipWriter *z, const char *path, const char *name)
{
    long n = 0;
    unsigned char *b = psx_zip_read_file(path, &n);
    if (!b) return 0;
    const int ok = psx_zip_writer_add(z, name, b, (size_t)n);
    free(b);
    return ok;
}

int psx_mod_package_export(const char *path, char *msg, unsigned cap)
{
    if (!path || !path[0]) { if (msg && cap) snprintf(msg, cap, "No file to export to"); return 0; }
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    if (!strchr(base_name(p), '.')) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".%s", PSX_MOD_PACKAGE_EXT); }

    Parts parts; memset(&parts, 0, sizeof parts);
    PsxZipWriter *z = psx_zip_writer_open(p);
    if (!z) { if (msg && cap) snprintf(msg, cap, "Could not create %.60s", base_name(p)); return 0; }
    char tmp[1200], why[200] = "";
    int ok = 1;

    /* cards and drop tables: the Card Manager's own file, flattened in.
     * ALWAYS the player's own cards/ set: with Dev Card Effects on the live
     * set is the shipped effects mod, which is not theirs to ship. */
    {
        int edited = 0;
        const char *own = psx_card_packs_own_dir();
        for (int id = 1; id <= CARD_COUNT && !edited; id++) {
            char pth[1300]; snprintf(pth, sizeof pth, "%s/%d/card.ini", own, id);
            edited = file_exists(pth);
        }
        if (edited || psx_drop_edits_any()) {
            scratch_path("cards.ygocards", tmp, sizeof tmp);
            psx_card_share_own_set(1);
            const int wrote = psx_card_share_export(tmp, why, sizeof why);
            psx_card_share_own_set(0);
            if (wrote) ok = add_zip_entries(z, tmp, rename_cards, &parts) >= 0;
            (void)psx_remove_utf8(tmp);
        }
    }
    /* CPU duelists: an ini, or the zip with portraits, either way flattened */
    if (ok) {
        scratch_path("cpu.ygoduelists", tmp, sizeof tmp);
        if (psx_cpu_export_file(tmp, why, sizeof why)) {
            long n = 0; unsigned char *b = psx_zip_read_file(tmp, &n);
            if (b) {
                if (is_zip(b, n)) ok = add_zip_entries(z, tmp, NULL, &parts) >= 0;
                else if (strstr((const char *)b, "\n[")) { ok = psx_zip_writer_add(z, "cpu-duelists.ini", b, (size_t)n); parts.cpu = 1; }
                free(b);
            }
        }
        (void)psx_remove_utf8(tmp);
        /* an ini with no section is a stock table: leave it out */
        if (!strchr(why, '[') && !parts.cpu && !parts.portraits) { /* nothing */ }
    }
    /* fusion edits, only the player's */
    if (ok && (psx_fusion_table_edit_count() > 0 || psx_fusion_table_cleared())) {
        scratch_path("fusion-edits.txt", tmp, sizeof tmp);
        if (psx_fusion_table_export(tmp, 1, why, sizeof why)) { ok = add_file(z, tmp, "fusion-edits.txt"); parts.fusion = ok; }
        (void)psx_remove_utf8(tmp);
    }
    /* the translation: the file the Dialogue Manager keeps, which exists
     * exactly when one was imported, and is readable before the game's text
     * bank is (the bank is not resident at the title) */
    if (ok) {
        const char *kept = psx_dialogue_file();
        if (kept && kept[0] && file_exists(kept)) { ok = add_file(z, kept, "dialogue.txt"); parts.dialogue = ok; }
    }
    /* two inis the mods keep beside the saves */
    if (ok) {
        player_file("drop_missing_cards.ini", tmp, sizeof tmp);
        if (file_exists(tmp)) { ok = add_file(z, tmp, "drop_missing_cards.ini"); parts.missing = ok; }
    }
    if (ok) {
        player_file("card_shop.ini", tmp, sizeof tmp);
        if (file_exists(tmp)) { ok = add_file(z, tmp, "card_shop.ini"); parts.shop = ok; }
    }
    /* the rows */
    if (ok) {
        static char st[8192];
        parts.settings = settings_text(st, sizeof st);
        ok = psx_zip_writer_add(z, "mod_settings.ini", st, strlen(st));
    }
    /* the manifest last: it says what made it in */
    if (ok) {
        char m[1024], stamp[64];
        const time_t t = time(NULL);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
        const int n = snprintf(m, sizeof m,
            "; Yu-Gi-Oh! Forbidden Memories Recompiled -- MOD package\n"
            "format = %s\nversion = %d\ngame = SLUS-01411\ncreated = %s\n"
            "cards = %d\ndrop_tables = %d\ncpu = %d\nportraits = %d\nfusion = %d\n"
            "dialogue = %d\ndrop_missing_cards = %d\ncard_shop = %d\nsettings = %d\n",
            PSX_MOD_PACKAGE_FORMAT, PSX_MOD_PACKAGE_VERSION, stamp,
            parts.cards, parts.drops, parts.cpu, parts.portraits, parts.fusion,
            parts.dialogue, parts.missing, parts.shop, parts.settings);
        ok = psx_zip_writer_add(z, "manifest.ini", m, (size_t)n);
    }
    if (!ok) { psx_zip_writer_abandon(z); if (msg && cap) snprintf(msg, cap, "Writing %.60s failed", base_name(p)); return 0; }
    if (!psx_zip_writer_close(z)) { if (msg && cap) snprintf(msg, cap, "Writing %.60s failed", base_name(p)); return 0; }
    if (msg && cap)
        snprintf(msg, cap, "Exported %.60s: %d card file%s%s%s%s%s%s%s%s, %d setting%s",
                 base_name(p), parts.cards, parts.cards == 1 ? "" : "s",
                 parts.drops ? ", drop tables" : "", parts.cpu ? ", CPU duelists" : "",
                 parts.portraits ? ", portraits" : "", parts.fusion ? ", fusion edits" : "",
                 parts.dialogue ? ", the translation" : "", parts.missing ? ", drop missing cards" : "",
                 parts.shop ? ", card shop" : "", parts.settings, parts.settings == 1 ? "" : "s");
    return 1;
}

/* ---- import ------------------------------------------------------------ */
static int name_is_cards(const char *n) { return !strncmp(n, "cards/", 6) || !strcmp(n, "drop_table_edits.ini") || !strcmp(n, "cards-manifest.ini"); }
static int name_is_cpu(const char *n)   { return !strncmp(n, "duelists/", 9) || !strcmp(n, "cpu-duelists.ini"); }

/* Re-pack a subset of the package into a manager's own share file. */
static int repack(const unsigned char *b, long n, const PsxZipEntry *ents, int k,
                  int (*want)(const char *), const char *out_path, int *count)
{
    PsxZipWriter *z = psx_zip_writer_open(out_path);
    if (!z) return 0;
    int c = 0;
    for (int i = 0; i < k; i++) {
        if (!want(ents[i].name)) continue;
        long sz = 0; unsigned char *d = psx_zip_extract(b, n, &ents[i], &sz);
        if (!d) continue;
        const char *name = !strcmp(ents[i].name, "cards-manifest.ini") ? "manifest.ini" : ents[i].name;
        const int ok = psx_zip_writer_add(z, name, d, (size_t)sz);
        free(d);
        if (!ok) { psx_zip_writer_abandon(z); return 0; }
        c++;
    }
    if (count) *count = c;
    return psx_zip_writer_close(z);
}

static const PsxZipEntry *find_entry(const PsxZipEntry *ents, int k, const char *name)
{
    for (int i = 0; i < k; i++) if (!strcmp(ents[i].name, name)) return &ents[i];
    return NULL;
}

/* One flat entry to a scratch file; NULL when the package lacks it. */
static int extract_to(const unsigned char *b, long n, const PsxZipEntry *e, const char *path)
{
    long sz = 0; unsigned char *d = psx_zip_extract(b, n, e, &sz);
    if (!d) return 0;
    const int ok = write_bytes(path, d, (size_t)sz);
    free(d);
    return ok;
}

static int open_package(const char *path, unsigned char **b, long *n, PsxZipEntry *ents, int *k, char *msg, unsigned cap)
{
    *b = psx_zip_read_file(path, n);
    if (!*b) { if (msg && cap) snprintf(msg, cap, "Could not read that file"); return 0; }
    if (!is_zip(*b, *n)) { free(*b); if (msg && cap) snprintf(msg, cap, "That is not a .%s file", PSX_MOD_PACKAGE_EXT); return 0; }
    char err[160];
    *k = psx_zip_list(*b, *n, ents, ENTRY_MAX, err, sizeof err);
    if (*k < 0) { free(*b); if (msg && cap) snprintf(msg, cap, "%s", err); return 0; }
    const PsxZipEntry *m = find_entry(ents, *k, "manifest.ini");
    long sz = 0; unsigned char *mt = m ? psx_zip_extract(*b, *n, m, &sz) : NULL;
    int good = mt && strstr((const char *)mt, PSX_MOD_PACKAGE_FORMAT) != NULL;
    int version = 0;
    if (mt) { const char *v = strstr((const char *)mt, "version"); if (v && (v = strchr(v, '='))) version = atoi(v + 1); }
    free(mt);
    if (!good) { free(*b); if (msg && cap) snprintf(msg, cap, "That is not a MOD package (no manifest)"); return 0; }
    if (version > PSX_MOD_PACKAGE_VERSION) { free(*b); if (msg && cap) snprintf(msg, cap, "Made by a newer version (%d); update the game", version); return 0; }
    return 1;
}

int psx_mod_package_inspect(const char *path, char *msg, unsigned cap)
{
    unsigned char *b; long n; static PsxZipEntry ents[ENTRY_MAX]; int k;
    if (!open_package(path, &b, &n, ents, &k, msg, cap)) return 0;
    int cards = 0, drops = 0, cpu = 0, portraits = 0, fusion = 0, dialogue = 0, missing = 0, shop = 0, settings = 0;
    for (int i = 0; i < k; i++) {
        const char *nm = ents[i].name;
        if (!strncmp(nm, "cards/", 6)) cards++;
        else if (!strcmp(nm, "drop_table_edits.ini")) drops = 1;
        else if (!strcmp(nm, "cpu-duelists.ini")) cpu = 1;
        else if (!strncmp(nm, "duelists/", 9)) portraits++;
        else if (!strcmp(nm, "fusion-edits.txt")) fusion = 1;
        else if (!strcmp(nm, "dialogue.txt")) dialogue = 1;
        else if (!strcmp(nm, "drop_missing_cards.ini")) missing = 1;
        else if (!strcmp(nm, "card_shop.ini")) shop = 1;
        else if (!strcmp(nm, "mod_settings.ini")) settings = 1;
    }
    free(b);
    if (msg && cap)
        snprintf(msg, cap, "%d card file%s%s%s%s%s%s%s%s%s", cards, cards == 1 ? "" : "s",
                 drops ? ", drop tables" : "", cpu ? ", CPU duelists" : "", portraits ? ", portraits" : "",
                 fusion ? ", fusion edits" : "", dialogue ? ", a translation" : "",
                 missing ? ", drop missing cards" : "", shop ? ", card shop" : "", settings ? ", settings" : "");
    return 1;
}

int psx_mod_package_import(const char *path, char *msg, unsigned cap)
{
    unsigned char *b; long n; static PsxZipEntry ents[ENTRY_MAX]; int k;
    if (!open_package(path, &b, &n, ents, &k, msg, cap)) return 0;
    char tmp[1200], why[300], report[400];
    unsigned rn = 0;
    int parts = 0, failed = 0;
#define NOTE(fmt, ...) do { if (rn < sizeof report) rn += (unsigned)snprintf(report + rn, sizeof report - rn, fmt, __VA_ARGS__); } while (0)
    report[0] = 0;

    /* cards + drop tables through the Card Manager's importer */
    {
        int c = 0;
        int any = 0;
        for (int i = 0; i < k && !any; i++) any = name_is_cards(ents[i].name);
        if (any) {
            scratch_path("cards.ygocards", tmp, sizeof tmp);
            psx_card_share_own_set(1);          /* into cards/, never the Dev set */
            const int good = repack(b, n, ents, k, name_is_cards, tmp, &c) && psx_card_share_import(tmp, why, sizeof why);
            psx_card_share_own_set(0);
            if (good) { parts++; NOTE("%scards ok%s", parts > 1 ? "; " : "", psx_card_packs_is_dev() ? " (into your own set: Dev Card Effects is on, switch it off to see them)" : ""); }
            else { failed++; NOTE("%scards: %.80s", parts + failed > 1 ? "; " : "", why); }
            (void)psx_remove_utf8(tmp);
        }
    }
    /* CPU duelists through theirs */
    {
        int any = 0;
        for (int i = 0; i < k && !any; i++) any = name_is_cpu(ents[i].name);
        if (any) {
            scratch_path("cpu.ygoduelists", tmp, sizeof tmp);
            if (repack(b, n, ents, k, name_is_cpu, tmp, NULL) && psx_cpu_import_file(tmp, why, sizeof why)) { parts++; NOTE("%sCPU ok", parts + failed > 1 ? "; " : ""); }
            else { failed++; NOTE("%sCPU: %.80s", parts + failed > 1 ? "; " : "", why); }
            (void)psx_remove_utf8(tmp);
        }
    }
    const PsxZipEntry *e;
    if ((e = find_entry(ents, k, "fusion-edits.txt")) != NULL) {
        scratch_path("fusion-edits.txt", tmp, sizeof tmp);
        if (extract_to(b, n, e, tmp) && psx_fusion_table_import(tmp, why, sizeof why)) { parts++; NOTE("%sfusion ok", parts + failed > 1 ? "; " : ""); }
        else { failed++; NOTE("%sfusion: %.80s", parts + failed > 1 ? "; " : "", why); }
        (void)psx_remove_utf8(tmp);
    }
    if ((e = find_entry(ents, k, "dialogue.txt")) != NULL) {
        /* through the importer when the text bank is up (it rewrites the bank
         * and keeps the file); at the title the bank is not resident, so the
         * file is put where the module reads it at every launch instead */
        const char *kept = psx_dialogue_file();
        if (psx_dialogue_ready()) {
            scratch_path("dialogue.txt", tmp, sizeof tmp);
            if (extract_to(b, n, e, tmp) && psx_dialogue_import(tmp, why, sizeof why)) { parts++; NOTE("%sdialogue ok", parts + failed > 1 ? "; " : ""); }
            else { failed++; NOTE("%sdialogue: %.80s", parts + failed > 1 ? "; " : "", why); }
            (void)psx_remove_utf8(tmp);
        } else if (kept && kept[0]) {
            char dir[1200]; snprintf(dir, sizeof dir, "%s", kept);
            char *slash = strrchr(dir, '/');
            if (slash) { *slash = 0; (void)MKDIR(dir); }
            (void)psx_dialogue_backup_kept();
            if (extract_to(b, n, e, kept)) { parts++; NOTE("%sdialogue kept for the next launch", parts + failed > 1 ? "; " : ""); }
            else { failed++; NOTE("%sdialogue: could not write", parts + failed > 1 ? "; " : ""); }
        } else { failed++; NOTE("%sdialogue: no player folder", parts + failed > 1 ? "; " : ""); }
    }
    if ((e = find_entry(ents, k, "drop_missing_cards.ini")) != NULL) {
        player_file("drop_missing_cards.ini", tmp, sizeof tmp);
        if (extract_to(b, n, e, tmp)) { psx_drop_missing_reload(); parts++; NOTE("%sdrop missing cards ok", parts + failed > 1 ? "; " : ""); }
        else { failed++; NOTE("%sdrop missing cards: could not write", parts + failed > 1 ? "; " : ""); }
    }
    if ((e = find_entry(ents, k, "card_shop.ini")) != NULL) {
        player_file("card_shop.ini", tmp, sizeof tmp);
        if (extract_to(b, n, e, tmp)) { psx_card_shop_reload_config(); parts++; NOTE("%scard shop ok", parts + failed > 1 ? "; " : ""); }
        else { failed++; NOTE("%scard shop: could not write", parts + failed > 1 ? "; " : ""); }
    }
    if ((e = find_entry(ents, k, "mod_settings.ini")) != NULL) {
        long sz = 0; unsigned char *d = psx_zip_extract(b, n, e, &sz);
        if (d) { const int a = settings_apply((const char *)d); free(d); parts++; NOTE("%s%d setting%s", parts + failed > 1 ? "; " : "", a, a == 1 ? "" : "s"); }
        else { failed++; NOTE("%ssettings: damaged", parts + failed > 1 ? "; " : ""); }
    }
#undef NOTE
    free(b);
    if (msg && cap) {
        if (!parts && !failed) snprintf(msg, cap, "The package is empty");
        else snprintf(msg, cap, "Imported %.60s: %s", base_name(path), report);
    }
    return failed == 0 && parts > 0;
}

/* ---- everything back to stock ------------------------------------------ */
int psx_mod_package_reset_all(char *msg, unsigned cap)
{
    char why[300];
    int cards = 0, drops = 0, cpu = 0, fusion = 0, dialogue = 0, files = 0, rows = 0;
    {   /* the player's own cards/ folders, by hand: psx_card_packs_remove
         * works on the LIVE set, and with Dev Card Effects on that is the
         * shipped effects mod, which stays as it is */
        static const char *const files4[4] = { "card.ini", "art.png", "thumb.png", "title.png" };
        const char *own = psx_card_packs_own_dir();
        for (int id = 1; id <= CARD_COUNT; id++) {
            char d[1300]; snprintf(d, sizeof d, "%s/%d", own, id);
            int any = 0;
            for (int j = 0; j < 4; j++) {
                char pth[1400]; snprintf(pth, sizeof pth, "%s/%s", d, files4[j]);
                if (file_exists(pth)) { any = 1; (void)psx_remove_utf8(pth); }
            }
            if (any) { (void)rmdir(d); cards++; }
        }
    }
    psx_card_packs_reload(0);
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++) {
        if (psx_drop_edits_count(d)) { psx_drop_edits_clear(d); drops++; }
        if (psx_drop_edits_reward(d, NULL)) { psx_drop_edits_reward_set(d, 0, 0); drops++; }
    }
    (void)psx_drop_edits_save();
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++) {
        const int a = psx_cpu_deck_clear(d), b = psx_cpu_ai_clear(d);
        const int c = psx_cpu_name_clear(d), e = psx_cpu_portrait_clear(d);
        cpu += (a || b || c || e);
    }
    (void)psx_cpu_save();
    if (psx_fusion_table_edit_count() > 0 || psx_fusion_table_cleared()) {
        if (psx_fusion_table_restore_stock(why, sizeof why)) fusion = 1;
    }
    if (psx_dialogue_ready() && psx_dialogue_translated_count() > 0) { psx_dialogue_clear(); dialogue = 1; }
    else if (psx_dialogue_backup_kept()) dialogue = 1;      /* not resident yet: the file steps aside */
    {
        char p[1200];
        player_file("drop_missing_cards.ini", p, sizeof p);
        if (file_exists(p) && psx_remove_utf8(p) == 0) { psx_drop_missing_reload(); files++; }
        player_file("card_shop.ini", p, sizeof p);
        if (file_exists(p) && psx_remove_utf8(p) == 0) { psx_card_shop_reload_config(); files++; }
    }
    {
        const int count = psx_video_menu_row_count();
        for (int h = 0; h < count; h++) {
            const char *key = NULL;
            if (settings_row_wanted(h, &key)) rows += psx_video_menu_reset_row(h);
        }
        if (rows) psx_video_menu_note_change();
    }
    if (msg && cap)
        snprintf(msg, cap, "Back to stock: %d card%s, %d drop table%s, %d CPU duelist%s%s%s, %d file%s, %d setting%s",
                 cards, cards == 1 ? "" : "s", drops, drops == 1 ? "" : "s", cpu, cpu == 1 ? "" : "s",
                 fusion ? ", the fusions" : "", dialogue ? ", the translation" : "",
                 files, files == 1 ? "" : "s", rows, rows == 1 ? "" : "s");
    return 1;
}

/* ---- the rows ---------------------------------------------------------- */
static char s_pick_path[1200];
static int  s_pick_kind;           /* 1 export, 2 import */
static char s_pick_err[200];
static int  s_pick_err_kind;

#if defined(PSX_SDL3)
static void SDLCALL pick_cb(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    const int kind = (int)(intptr_t)userdata;
    if (!filelist) {
        const char *e = SDL_GetError();
        s_pick_err_kind = kind;
        snprintf(s_pick_err, sizeof s_pick_err, "%s", e && e[0] ? e : "the file dialog could not open");
        return;
    }
    if (!filelist[0]) return;                       /* cancelled */
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
    s_pick_kind = kind;
}
#endif

static void row_export(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "MOD packages", PSX_MOD_PACKAGE_EXT } };
    static char def[1200];
    psx_mod_package_default_path(def, sizeof def);
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)1, NULL, filters, 1, def);
#else
    char def[1200], m[400];
    psx_mod_package_default_path(def, sizeof def);
    psx_mod_package_export(def, m, sizeof m);
    say(m);
#endif
}

static void row_import(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "MOD packages", PSX_MOD_PACKAGE_EXT } };
    static char dir[1024];
    share_dir(dir, sizeof dir);
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)2, NULL, filters, 1, dir, false);
#else
    say("No file dialog in this build: use the debug command mod_package with import:<path>");
#endif
}

/* The menu has no dialog, so the row is armed by its first choice and does
 * the work on a second within ten seconds; the first choice is the warning.
 * No package is written for the player: that is what Export is for, and a
 * revert that always wrote one filled mod_packages with files nobody asked
 * for. */
#define REVERT_ARM_MS 10000u
static unsigned s_revert_armed_ms;
static void row_reset(void)
{
    const unsigned now = SDL_GetTicks();
    if (!s_revert_armed_ms || now - s_revert_armed_ms > REVERT_ARM_MS) {
        s_revert_armed_ms = now;
        say("Revert to Stock loses every edit, saved or not: cards, drop tables, CPU duelists, fusions, the translation and the mod settings. Export MOD package first to keep them. Choose Revert to Stock again within 10 seconds to do it.");
        return;
    }
    s_revert_armed_ms = 0;
    char m[400];
    psx_mod_package_reset_all(m, sizeof m);
    say(m);
}
int psx_mod_package_revert_row_armed(void) { return s_revert_armed_ms != 0 && SDL_GetTicks() - s_revert_armed_ms <= REVERT_ARM_MS; }
void psx_mod_package_revert_row(void) { row_reset(); }

static void tick(void)
{
    if (s_pick_err[0]) {
        char why[200]; snprintf(why, sizeof why, "%s", s_pick_err);
        const int kind = s_pick_err_kind;
        s_pick_err[0] = 0;
        if (kind == 1) {
            char def[1200], m[400];
            psx_mod_package_default_path(def, sizeof def);
            psx_mod_package_export(def, m, sizeof m);
            say(m);
        } else {
            char m[300]; snprintf(m, sizeof m, "The file dialog could not open: %.200s", why);
            say(m);
        }
    }
    if (s_pick_kind) {
        const int kind = s_pick_kind;
        char path[1200]; snprintf(path, sizeof path, "%s", s_pick_path);
        s_pick_kind = 0; s_pick_path[0] = 0;
        char m[400];
        if (kind == 1) psx_mod_package_export(path, m, sizeof m);
        else           psx_mod_package_import(path, m, sizeof m);
        say(m);
    }
}

void psx_mod_package_register_menu(void)
{
    const int hi = psx_video_menu_add_action(PSX_VM_MENU_MODS, "Import MOD package" "\xE2\x80\xA6",
        "Load a .ygomods file: every manager's edits and every MODS and CHEATS setting in it, each replacing yours", row_import);
    const int he = psx_video_menu_add_action(PSX_VM_MENU_MODS, "Export MOD package" "\xE2\x80\xA6",
        "Write one .ygomods file with every manager's edits (cards, drops, CPU duelists, portraits, fusions, translation) and these settings", row_export);
    const int hr = psx_video_menu_add_action(PSX_VM_MENU_MODS, "Revert to Stock",
        "Every manager's edits and every mod setting back to the disc's own, saved or not. Asks twice. Export MOD package first to keep what you have", row_reset);
    /* the bottom of MODS, whatever registers after this: Import, Export, then Revert last */
    psx_video_menu_set_row_order(hi, 1000);
    psx_video_menu_set_row_order(he, 1001);
    psx_video_menu_set_row_order(hr, 1002);
    (void)psx_game_add_frame_hook(tick);
}

PSX_MOD_CONSTRUCTOR(psx_mod_package_install)
{
    psx_mod_package_register_menu();
}
