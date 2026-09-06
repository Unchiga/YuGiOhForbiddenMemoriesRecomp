/* psx_drop_edits.c — see psx_drop_edits.h.
 *
 * THE INI
 * -------
 * drop_table_edits.ini, beside drop_missing_cards.ini in the player-data
 * folder, in the same shape the player already knows from that file:
 *
 *     [Duelist Name]
 *     <card id> = <POW>, <BCD>, <TEC>
 *
 * with one deliberate difference: here a line is the card's COMPLETE weight
 * vector for that duelist — a 0 really means "not in this band", not "leave
 * that band alone". That is what makes band moves and removals expressible,
 * and it is why the viewer records the untouched bands' current values into
 * the entry when the player edits one band.
 *
 * Duelist names come from PSX_DROP_DB (the baked drop database), which is
 * also where the viewer gets them, so section lookup is self-consistent.
 */

#include "psx_drop_edits.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "mod_plugins.h"
#include "psx_drop_db.h"
#include "psx_drop_missing.h"

#define INI_NAME  "drop_table_edits.ini"
#define NDUEL     PSX_DROP_DB_DUELISTS
#define NCARDS    PSX_DROP_DB_CARDS
#define MAX_EDITS 128               /* per duelist; the UI edits one row at a time */

typedef struct { uint16_t card; uint16_t w[3]; } Edit;
static Edit     g_edit[NDUEL][MAX_EDITS];
static int      g_n[NDUEL];
static int      g_loaded;
static int      g_dirty;
static unsigned g_gen = 1;
static char     g_ini_path[1024] = "";
static char     g_status[96] = "not loaded";

static void ini_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", dir, INI_NAME);
    else               snprintf(out, cap, "%s", INI_NAME);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '
                     || e[-1] == '\t')) *--e = 0;
    return s;
}

static int read_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    for (int d = 0; d < NDUEL; d++) g_n[d] = 0;
    char line[256];
    int cur = -1, entries = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            cur = -1;
            for (int d = 0; d < NDUEL; d++)
                if (!strcmp(PSX_DROP_DB[d].name, s + 1)) { cur = d; break; }
            continue;
        }
        if (cur < 0) continue;
        int card = 0, w0 = 0, w1 = 0, w2 = 0;
        /* Fewer than four numbers is a malformed line, not a partial edit —
         * an entry is always the full vector (see the header comment). */
        if (sscanf(s, "%d = %d , %d , %d", &card, &w0, &w1, &w2) != 4) continue;
        if (card < 1 || card > NCARDS || g_n[cur] >= MAX_EDITS) continue;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        Edit *e = &g_edit[cur][g_n[cur]++];
        e->card = (uint16_t)card;
        e->w[0] = (uint16_t)w0; e->w[1] = (uint16_t)w1; e->w[2] = (uint16_t)w2;
        entries++;
    }
    fclose(f);
    return entries;
}

void psx_drop_edits_ensure_loaded(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    ini_path(g_ini_path, sizeof(g_ini_path));
    const int n = read_ini(g_ini_path);
    if (n < 0)      snprintf(g_status, sizeof(g_status), "no ini (no edits)");
    else            snprintf(g_status, sizeof(g_status), "%d entries from ini", n);
    g_dirty = 0;
    g_gen++;
}

int psx_drop_edits_any(void)
{
    psx_drop_edits_ensure_loaded();
    for (int d = 0; d < NDUEL; d++)
        if (g_n[d]) return 1;
    return 0;
}

int psx_drop_edits_count(int duelist)
{
    psx_drop_edits_ensure_loaded();
    return (duelist >= 0 && duelist < NDUEL) ? g_n[duelist] : 0;
}

int      psx_drop_edits_dirty(void)      { return g_dirty; }
unsigned psx_drop_edits_generation(void) { return g_gen; }

static Edit *find(int duelist, int card)
{
    for (int i = 0; i < g_n[duelist]; i++)
        if (g_edit[duelist][i].card == card) return &g_edit[duelist][i];
    return 0;
}

int psx_drop_edits_get(int duelist, int card, uint16_t w[3])
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    const Edit *e = find(duelist, card);
    if (!e) return 0;
    if (w) { w[0] = e->w[0]; w[1] = e->w[1]; w[2] = e->w[2]; }
    return 1;
}

int psx_drop_edits_set(int duelist, int card, const uint16_t w[3])
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || card < 1 || card > NCARDS || !w)
        return 0;
    Edit *e = find(duelist, card);
    if (!e) {
        if (g_n[duelist] >= MAX_EDITS) return 0;
        e = &g_edit[duelist][g_n[duelist]++];
        e->card = (uint16_t)card;
    }
    e->w[0] = w[0]; e->w[1] = w[1]; e->w[2] = w[2];
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_drop_edits_unset(int duelist, int card)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    for (int i = 0; i < g_n[duelist]; i++) {
        if (g_edit[duelist][i].card != card) continue;
        g_edit[duelist][i] = g_edit[duelist][--g_n[duelist]];
        g_dirty = 1;
        g_gen++;
        return 1;
    }
    return 0;
}

int psx_drop_edits_clear(int duelist)
{
    psx_drop_edits_ensure_loaded();
    int removed = 0;
    if (duelist < 0) {
        for (int d = 0; d < NDUEL; d++) { removed += g_n[d]; g_n[d] = 0; }
    } else if (duelist < NDUEL) {
        removed = g_n[duelist];
        g_n[duelist] = 0;
    }
    if (removed) { g_dirty = 1; g_gen++; }
    return removed;
}

static int write_to(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f,
"; Yu-Gi-Oh! Forbidden Memories - Recompiled : drop table edits\n"
";\n"
"; Written by the Drop Table Manager (VIEW > DROP TABLE MANAGER); hand-editing\n"
"; works too. One section per duelist, one line per edited card:\n"
";\n"
";     <card id> = <POW>, <BCD>, <TEC>\n"
";\n"
"; The line is the card's COMPLETE weight vector for that duelist - a 0 means\n"
"; \"not in this band\", so all three numbers matter. Weights are out of 2048;\n"
"; whatever they claim is taken from the duelist's other drops in proportion,\n"
"; and every band still totals 2048 exactly.\n"
";\n"
"; These edits apply on top of MODS > DROP MISSING CARDS when that row is on.\n"
"; Delete a line (or the file) to fall back to the table underneath.\n"
"\n");
    for (int d = 0; d < NDUEL; d++) {
        if (!g_n[d]) continue;
        fprintf(f, "[%s]\n", PSX_DROP_DB[d].name);
        for (int i = 0; i < g_n[d]; i++) {
            const Edit *e = &g_edit[d][i];
            fprintf(f, "%-3d = %4d, %4d, %4d\n",
                    e->card, e->w[0], e->w[1], e->w[2]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    return 1;
}

int psx_drop_edits_save(void)
{
    psx_drop_edits_ensure_loaded();
    if (!write_to(g_ini_path)) {
        snprintf(g_status, sizeof(g_status), "save FAILED");
        return 0;
    }
    g_dirty = 0;
    snprintf(g_status, sizeof(g_status), "saved");
    return 1;
}

/* --- sharing -------------------------------------------------------------- */

static void share_dir(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/drop_tables", dir);
    else               snprintf(out, cap, "drop_tables");
#ifdef _WIN32
    (void)_mkdir(out);
#else
    (void)mkdir(out, 0755);
#endif
}

void psx_drop_edits_share_dir(char *out, unsigned cap)
{
    if (out && cap) share_dir(out, cap);
}

/* The last path component, whichever slash the platform wrote. */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static int entry_total(void)
{
    int n = 0;
    for (int d = 0; d < NDUEL; d++) n += g_n[d];
    return n;
}

int psx_drop_edits_export_file(const char *path, char *msg, unsigned cap)
{
    psx_drop_edits_ensure_loaded();
    if (!path || !path[0]) {
        if (msg && cap) snprintf(msg, cap, "No file to export to");
        return 0;
    }
    /* A dialog that came back without an extension still means an ini. */
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    if (!strchr(base_name(p), '.')) {
        const size_t n = strlen(p);
        snprintf(p + n, sizeof p - n, ".ini");
    }
    if (!write_to(p)) {
        snprintf(g_status, sizeof(g_status), "export FAILED");
        if (msg && cap) snprintf(msg, cap, "Could not write that file");
        return 0;
    }
    const char *base = base_name(p);
    const int n = entry_total();
    /* Exporting is a copy for someone else; it neither saves the live ini
     * nor clears the unsaved-changes marker. */
    snprintf(g_status, sizeof(g_status), "exported %.60s", base);
    if (msg && cap)
        snprintf(msg, cap, "Exported %d entr%s as %.48s", n, n == 1 ? "y" : "ies", base);
    return 1;
}

int psx_drop_edits_import_file(const char *path, char *msg, unsigned cap)
{
    const int n = psx_drop_edits_load_file(path);
    if (n < 0) {
        if (msg && cap) snprintf(msg, cap, "Could not read that file");
        return 0;
    }
    if (msg && cap)
        snprintf(msg, cap, "Imported %d entr%s. Save to keep them.", n, n == 1 ? "y" : "ies");
    return 1;
}

int psx_drop_edits_load_file(const char *name_or_path)
{
    psx_drop_edits_ensure_loaded();
    if (!name_or_path || !name_or_path[0]) return -1;
    char path[1200];
    if (strchr(name_or_path, '/') || strchr(name_or_path, '\\')) {
        snprintf(path, sizeof path, "%s", name_or_path);
    } else {
        char dir[1024];
        share_dir(dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s", dir, name_or_path);
    }
    const int n = read_ini(path);
    if (n < 0) {
        snprintf(g_status, sizeof(g_status), "load FAILED");
        return -1;
    }
    g_dirty = 1;
    g_gen++;
    snprintf(g_status, sizeof(g_status), "loaded %d entries", n);
    return n;
}

int psx_drop_edits_apply(int duelist, int tier, uint16_t *w)
{
    psx_drop_edits_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || tier < 0 || tier >= 3 || !w)
        return -1;
    if (!g_n[duelist]) return -1;
    uint16_t cards[MAX_EDITS], weights[MAX_EDITS];
    int n = 0;
    for (int i = 0; i < g_n[duelist]; i++) {
        cards[n]   = g_edit[duelist][i].card;
        weights[n] = g_edit[duelist][i].w[tier];
        n++;
    }
    return psx_drop_pins_rescale(w, cards, weights, n);
}

int psx_drop_edits_state_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    psx_drop_edits_ensure_loaded();
    int total = 0, duelists = 0;
    for (int d = 0; d < NDUEL; d++) {
        total += g_n[d];
        if (g_n[d]) duelists++;
    }
    return snprintf(out, cap,
        "\"entries\":%d,\"duelists\":%d,\"dirty\":%d,\"gen\":%u,"
        "\"status\":\"%s\"",
        total, duelists, g_dirty, g_gen, g_status);
}
