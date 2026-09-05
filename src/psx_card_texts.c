/* psx_card_texts.c -- see psx_card_texts.h.
 *
 * THE FILE
 *     ; Yu-Gi-Oh! Forbidden Memories -- card descriptions ...  (comment lines)
 *     [1] Blue-eyes White Dragon
 *     This legendary dragon
 *     is a powerful engine
 *     of destruction.
 *
 *     [2] Mystical Elf
 *     ...
 * One block per card: "[id] Name" then the description with one file line
 * per line on the card (the card shows 6 lines of 20 characters). A block
 * ends at the next "[id]" line; blank lines before it are dropped. Lines
 * starting with ";" are comments.
 *
 * WHERE THE TEXT GOES
 * The card's description is string 0xD100+id of the game's text bank and
 * its name string 0x8000+id of the global table; psx_card_packs relocates an
 * edited card's strings into free RAM and repoints the u16 tables per frame
 * (see the DESC_* / NAMES_* notes there). This file only produces card.ini
 * entries -- `description = line|line|...` and `name = ...` -- in the
 * active set and lets psx_card_packs apply them, so the manager, the share
 * file and the hot reload all see the same edit. */

#include "psx_textfile.h"
#include "psx_card_texts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_dialogue.h"

#define CARDS 722

/* The name / description the game shows for a card right now. */
static void effective(int id, const PsxCardStock *st, char *name, unsigned ncap, char *desc, unsigned dcap)
{
    PsxCardPack pk;
    snprintf(name, ncap, "%s", st->name);
    snprintf(desc, dcap, "%s", st->description);
    if (psx_card_packs_get(id, &pk)) {
        if (pk.name[0]) snprintf(name, ncap, "%s", pk.name);
        if (pk.description[0]) snprintf(desc, dcap, "%s", pk.description);
    }
}

int psx_card_texts_export(const char *path, char *err, unsigned errcap)
{
    if (!psx_card_db_ready()) { snprintf(err, errcap, "The game's card table is not loaded yet"); return 0; }
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) { snprintf(err, errcap, "Cannot write %s", path); return 0; }
    char stamp[64];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    int edited = 0;
    for (int id = 1; id <= CARDS; id++) edited += psx_card_packs_get(id, NULL) != 0;
    fprintf(f,
        "; Yu-Gi-Oh! Forbidden Memories -- card names and descriptions\n"
        "; exported %s from %s (%d edited card%s in the set)\n"
        ";\n"
        "; One block per card: \"[id] Name\", then the description with one line per line on the card.\n"
        "; A card shows up to %d lines of %d characters; a name up to %d characters. Lines starting\n"
        "; with \";\" are comments. Characters the font has: A-Z a-z 0-9 space . , ! ? ' \" - & / # $ %% * + : ( ) < >\n"
        "; (no accents). Import applies every card whose name or description differs from what the\n"
        "; game shows; the change is kept in the same edited-card set as the Card Manager's edits.\n"
        "; A card put back to its original text drops the edit.\n"
        ";\n",
        stamp, psx_card_packs_is_dev() ? "the Card Effects set (mods/card_effects/cards)" : "your edited cards (cards)",
        edited, edited == 1 ? "" : "s",
        PSX_CARD_PACK_DESC_LINES, PSX_CARD_PACK_DESC_COLS, PSX_CARD_PACK_NAME_MAX);
    int n = 0;
    for (int id = 1; id <= CARDS; id++) {
        PsxCardStock st;
        if (!psx_card_packs_stock(id, &st)) continue;
        char name[PSX_CARD_PACK_NAME_MAX + 1], desc[PSX_CARD_PACK_DESC_MAX + 1];
        effective(id, &st, name, sizeof name, desc, sizeof desc);
        fprintf(f, "[%d] %s\n", id, name);
        for (const char *p = desc; ; ) {
            const char *e = strchr(p, '|');
            const int len = e ? (int)(e - p) : (int)strlen(p);
            fprintf(f, "%.*s\n", len, p);
            if (!e) break;
            p = e + 1;
        }
        fputc('\n', f);
        n++;
    }
    fclose(f);
    snprintf(err, errcap, "Exported %d cards to %s", n, path);
    return 1;
}

/* Text -> the encodable check; the first character the font lacks, or 0. */
static int bad_char(const char *s, char *out, unsigned cap)
{
    for (const char *p = s; *p;) {
        if (*p == '|') { p++; continue; }
        int l = 0;
        if (psx_dialogue_encode_char(p, &l) < 0 || l < 1) {
            int k = 0; out[k++] = p[0];
            while (k < 4 && (p[k] & 0xC0) == 0x80) { out[k] = p[k]; k++; }
            if ((unsigned)k < cap) out[k] = 0;
            return 1;
        }
        p += l;
    }
    return 0;
}

static void trim_trailing(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

/* After a save, a card.ini with no "key = value" left is no edit at all:
 * drop the folder so the card reads as stock everywhere (psx_card_packs
 * treats an existing folder as an edited card). */
static void drop_if_empty(int id)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%d/card.ini", psx_card_packs_dir(), id);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512]; int keys = 0;
    while (fgets(line, sizeof line, f)) if (line[0] != ';' && strchr(line, '=')) keys++;
    fclose(f);
    if (keys) return;
    PsxCardPack pk;
    if (psx_card_packs_get(id, &pk) && (pk.has_art || pk.has_thumb || pk.has_title)) return;
    psx_card_packs_remove(id);
}

/* Import bookkeeping and its one-line summary. */
typedef struct { int applied, unchanged, skipped, blocks; char warn[1024]; unsigned wn; } Tally;

static void warn(Tally *t, const char *fmt, ...)
{
    va_list ap;
    if (t->wn >= sizeof t->warn - 1) return;
    va_start(ap, fmt);
    t->wn += (unsigned)vsnprintf(t->warn + t->wn, sizeof t->warn - t->wn, fmt, ap);
    va_end(ap);
    if (t->wn > sizeof t->warn - 1) t->wn = sizeof t->warn - 1;
}

/* One "[id] Name" block: apply it when the text differs from what the game
 * shows, validate it the way the Card Manager does, and count the outcome. */
static void apply_block(Tally *t, int id, const char *name, char *desc, int block_line)
{
    t->blocks++;
    trim_trailing(desc);
    while (desc[0] && desc[strlen(desc) - 1] == '|') desc[strlen(desc) - 1] = 0;
    if (id < 1 || id > CARDS) { t->skipped++; warn(t, "line %d: card %d does not exist; ", block_line, id); return; }
    PsxCardStock st;
    if (!psx_card_packs_stock(id, &st)) { t->skipped++; return; }
    char cur_name[PSX_CARD_PACK_NAME_MAX + 1], cur_desc[PSX_CARD_PACK_DESC_MAX + 1];
    effective(id, &st, cur_name, sizeof cur_name, cur_desc, sizeof cur_desc);
    if (!strcmp(name, cur_name) && !strcmp(desc, cur_desc)) { t->unchanged++; return; }
    char bad[8]; int lines = 0, longest = 0, wide = 0;
    if (strlen(name) > PSX_CARD_PACK_NAME_MAX) { t->skipped++; warn(t, "[%d] the name is longer than %d; ", id, PSX_CARD_PACK_NAME_MAX); return; }
    if (strlen(desc) > PSX_CARD_PACK_DESC_MAX) { t->skipped++; warn(t, "[%d] the description is longer than %d characters; ", id, PSX_CARD_PACK_DESC_MAX); return; }
    if (bad_char(name, bad, sizeof bad) || bad_char(desc, bad, sizeof bad)) { t->skipped++; warn(t, "[%d] the font has no '%s'; ", id, bad); return; }
    if (!psx_card_packs_desc_layout(desc, &lines, &longest, &wide)) {
        t->skipped++;
        if (wide) warn(t, "[%d] line %d is %d characters (max %d); ", id, wide, longest, PSX_CARD_PACK_DESC_COLS);
        else warn(t, "[%d] %d lines (max %d); ", id, lines, PSX_CARD_PACK_DESC_LINES);
        return;
    }
    PsxCardPack pk;
    if (!psx_card_packs_get(id, &pk)) {
        memset(&pk, 0, sizeof pk); pk.id = id;
        pk.attack = pk.defense = pk.star1 = pk.star2 = pk.type = pk.level = pk.attribute = pk.price = -1;
        psx_card_packs_effects_reset(&pk);
    }
    /* a text equal to stock is no edit: the key is dropped */
    snprintf(pk.name, sizeof pk.name, "%s", strcmp(name, st.name) ? name : "");
    snprintf(pk.description, sizeof pk.description, "%s", strcmp(desc, st.description) ? desc : "");
    const int ok = psx_card_packs_save(&pk);
    if (ok) { t->applied++; if (!pk.name[0] && !pk.description[0]) drop_if_empty(id); }
    else { t->skipped++; warn(t, "[%d] could not write card.ini; ", id); }
}

int psx_card_texts_import(const char *path, char *err, unsigned errcap)
{
    if (!psx_card_db_ready()) { snprintf(err, errcap, "The game's card table is not loaded yet"); return 0; }
    size_t got = 0;
    char *data = psx_read_text_utf8(path, &got, 4 * 1024 * 1024);
    if (!data) { snprintf(err, errcap, "Cannot read %s (missing, or over 4 MB)", path); return 0; }
    if (!got) { free(data); snprintf(err, errcap, "%s is empty", path); return 0; }
    char *p = data;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    Tally t; memset(&t, 0, sizeof t);
    int id = 0; char name[256]; char desc[1024]; int have = 0; int line = 0, block_line = 0;
    while (*p) {
        char *e = strchr(p, '\n');
        const size_t ll = e ? (size_t)(e - p) : strlen(p);
        char *ln = p; p += ll + (e ? 1 : 0);
        line++;
        size_t l = ll;
        if (l && ln[l - 1] == '\r') l--;
        ln[l] = 0;
        if (ln[0] == '[' && ln[1] >= '0' && ln[1] <= '9') {
            if (have) apply_block(&t, id, name, desc, block_line);
            have = 0;
            char *q = ln + 1; int v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            if (*q != ']') { warn(&t, "line %d: bad card header; ", line); continue; }
            q++; while (*q == ' ' || *q == '\t') q++;
            id = v; block_line = line;
            snprintf(name, sizeof name, "%s", q); trim_trailing(name);
            desc[0] = 0; have = 1;
            continue;
        }
        if (!have || ln[0] == ';') continue;
        /* a description line; "|" inside a line is a break too */
        const size_t cur = strlen(desc);
        if (cur + l + 2 < sizeof desc) { if (cur) desc[cur] = '|'; memcpy(desc + cur + (cur ? 1 : 0), ln, l); desc[cur + (cur ? 1 : 0) + l] = 0; }
    }
    if (have) apply_block(&t, id, name, desc, block_line);
    free(data);
    if (!t.blocks) { snprintf(err, errcap, "%s holds no \"[id] Name\" blocks; is it a description export?", path); return 0; }
    snprintf(err, errcap, "%d cards read: %d changed, %d already as the file says%s%s%s", t.applied + t.unchanged + t.skipped, t.applied, t.unchanged,
             t.skipped ? ", some skipped" : "", t.wn ? ": " : "", t.warn);
    return t.applied > 0 || (!t.skipped && t.unchanged > 0);
}
