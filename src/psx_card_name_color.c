/* psx_card_text_color.c — tint a card's on-screen text by how rare it is.
 *
 * The stock game draws every card's text in the same colour. This colours it
 * by rarity, where "rarity" is drop scarcity — specifically, the single BEST
 * odds any one duelist ever gives the card (see the rarity model section
 * below), not just how many duelists carry it. Nothing on the disc is
 * touched — the mod only overrides the glyph colour byte the text engine is
 * about to use.
 *
 * SCOPE: THE NAME, AND ONLY THE NAME — CONFIRMED BY CONTENT, NOT BY CALL SITE
 * ------------------------------------------------------------------------
 * Two things had to be found before this worked, in this order:
 *
 * 1. WHICH CARD. func_80037DA4 is the text engine, called from many places.
 *    A first version gated on $ra == 0x80038B98 ("the return site inside the
 *    card-name dispatcher"), assuming that address was specific to drawing a
 *    NAME. It is not: a debug trace on a real CARD VIEW screen (2026-08)
 *    showed that exact $ra firing for the type line, Guardian Stars and
 *    description too — thousands of consecutive entries, no gaps — because
 *    it is the return site for ANY text a card-info widget draws, not the
 *    name specifically. It only looked name-specific on the CHEST/deck-list
 *    screens because nothing else there is drawn as real text (ATK/DEF/id
 *    are sprite digits). So $ra alone answers "is this widget drawing SOME
 *    piece of a card's info", never "is this the name".
 *
 *    What $ra == 0x80038B98 DOES reliably give is WHICH card: at that entry,
 *    a0 is the widget and 0x8009B338 already holds the id of the card whose
 *    info is about to be drawn:
 *
 *        0x8009B338  u16  "Selected card ID"
 *
 *    documented by the community RAM map for this exact game (Data Crystal,
 *    Yu-Gi-Oh! Forbidden Memories:RAM map) and the SAME address this repo
 *    already names SHOP_SIG_A in psx_card_shop.c (a fixed byte there, on
 *    the shopkeeper's menu — one scratch cell meaning different things on
 *    different screens, normal for this game). Confirmed live via the
 *    framework's debug-server RAM poll (scripts/wtrace_id.py peek, now
 *    removed): polling it while scrolling the CHEST screen produced exactly
 *    the on-screen card ids in order (240, 241, 276, 283, 326, 335, 336,
 *    387, 402, 421, 475, 524, 547, 609, 635, ...).
 *
 * 2. WHICH PIECE OF TEXT. Knowing the card, not which of its text elements
 *    is drawing, needs the glyphs themselves — but ONLY as a check against
 *    the one already-known target string (the card's decoded name), never
 *    to search for it among all 722 cards (that earlier, much less certain
 *    approach was tried and abandoned for the id lookup above; reusing the
 *    same glyph-decoding here is a different, narrower job with a single
 *    known answer to confirm, not a search). func_80036C14 delivers one
 *    DECODED character at a time as a1; matching it position-by-position
 *    against the card's name is safe because a type line, Guardian Star
 *    label or description can never coincidentally equal a card's full name
 *    character for character. See the glyph-match section below.
 *
 * THE COLOUR IS STICKY, SO EVERY RUN STARTS FROM WHITE
 * ------------------------------------------------------
 * func_80036C14's a0 is the glyph record; byte +84 is its colour index, and
 * writing it does not just paint one glyph — later glyphs on the SAME widget
 * keep using whatever was last written there, until something changes it
 * again. An early version of this mod (colour set once at $ra ==
 * 0x80038B98, one glyph write, never reset) relied on that entry meaning
 * "the name" and ended up colouring the type line, Guardian Stars and
 * description too, everywhere the entry fired. The fix now is symmetric at
 * the level that is actually reliable: every CARD_NAME_CALLER entry first
 * reverts the widget to white and arms a fresh, unconfirmed match against
 * the known card name; the glyph hook then re-applies the rarity colour
 * ONLY glyph by glyph while that match keeps holding, and stops the moment
 * it does not. A type line or description therefore never gets touched at
 * all — the revert already ran before its first glyph, and the match never
 * starts succeeding against a name it is not.
 *
 * The palette. psx_card_drops.c's "f8 0a nn" escape comment names 00/01/02/
 * 03/05 as white/gold/blue/green/"red-salmon"; measured colour on screen and
 * this project's own naming (see the COL_* constants below) differs slightly
 * — 01 reads as yellow and 05 as orange, with 06 a separate, distinct red:
 *     00 white   01 yellow   02 blue   03 green   04 grey   05 orange   06 red
 *
 * card_name_color.ini
 * --------------------
 * Same shape as card_shop.c's card_shop.ini: a plain, hand-editable file in
 * the player-data directory, written with the built-in values the first
 * time colours are built, so the file documents its own defaults.
 *
 *   [tiers]   both the max-weight cut-off AND the colour for each of the
 *             six rarity tiers (default/uncommon/rare/super_rare/ultra_rare/
 *             legendary) are tunable here — a card takes the first tier,
 *             rarest first, whose cut-off it does not exceed, and is
 *             painted that tier's chosen colour
 *   [cards]   NAME = colour overrides, matched against the game's own
 *             decoded names (see psx_card_db.c) so no id table can rot
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_video_menu.h"

/* psx_drop_db.h lives in the GAME's src/, which is on psx-runtime's include
 * path; PSX_DROP_DB is a generated const array linked into this executable. */
#include "psx_drop_db.h"

#define PSX_TEXT_FN        0x80037DA4u
#define PSX_GLYPH_FN        0x80036C14u  /* per-decoded-character callback */
#define CARD_NAME_CALLER   0x80038B98u   /* $ra when func_80037DA4 draws a
                                          * card-info widget's text (name,
                                          * type, Guardian Stars, or the
                                          * description — see the glyph-match
                                          * section below for why) */
#define PSX_SELECTED_CARD  0x8009B338u   /* u16, "Selected card ID" (Data Crystal) */

#define CARD_COUNT PSX_DROP_DB_CARDS     /* 722 */

#define GLYPH_COLOR_OFF  84u
#define COL_WHITE   0u
#define COL_YELLOW  1u   /* psx_card_drops.c's "f8 0a nn" comment calls this
                          * "gold"; measured live, it renders as yellow. */
#define COL_BLUE    2u
#define COL_GREEN   3u
/* 4 is undocumented by psx_card_drops.c's "f8 0a nn" comment (it lists
 * 00/01/02/03/05) but confirmed live 2026-08: it renders as a distinct dim
 * grey, not garbage. Not used by the default ladder below; still available
 * for [cards] overrides. */
#define COL_GREY    4u
#define COL_ORANGE  5u   /* psx_card_drops.c calls this "red-salmon"; it
                          * actually renders as orange/amber. */
#define COL_RED     6u   /* confirmed live 2026-08: a second, distinct,
                          * brighter red past COL_ORANGE. */
/* THE PALETTE STOPS HERE. Confirmed live 2026-08 by tinting real cards on
 * the CHEST screen across the ENTIRE 0-32 range: 7-10 and 32 make the NAME
 * TEXT INVISIBLE (everything else on the row — id, ATK/DEF, icons — still
 * draws fine, so this is not a crash, the colour byte just has no usable
 * glyph past this point); 16 renders as a near-black maroon; 20 and 21
 * render as visible but corrupted (colour bleed / a solid garbage block) —
 * none of it is a real additional colour, all of it reads as the renderer
 * being fed an index with no defined palette entry. Nothing past 6 is
 * worth exposing as a name here — do not keep re-probing this. */

/* ---- name decoding, so [cards] overrides can be matched by NAME ----------
 * Same RAM map and frequency-code table as psx_card_db.c. */
#define NAME_OFFSETS  0x801D5800u
#define NAME_SEGMENT  0x801D0000u
#define NAME_MAX      40u

static const char RAW_ASCII[128] = {
    /* 00 */ ' ',   'e',   't',   'a',   'o',   'i',   'n',   's',
    /* 08 */ 'r',   'h',   'l',   '.',   'd',   'u',   'm',   'c',
    /* 10 */ 'g',   'y',   'w',   'f',   'p',   'b',   'k',   0,
    /* 18 */ 'A',   'v',   'I',   '\'',  'T',   'S',   'M',   ',',
    /* 20 */ 'D',   'O',   'W',   'H',   'Y',   'E',   'R',   0,
    /* 28 */ 0,     'G',   'L',   'C',   'N',   'B',   0,     'P',
    /* 30 */ '-',   'F',   'z',   'K',   'j',   'U',   'x',   'q',
    /* 38 */ '0',   'V',   '2',   'J',   '#',   '1',   'Q',   'Z',
    /* 40 */ 0,     '3',   '5',   '&',   0,     '7',   'X',   0,
    /* 48 */ 0,     0,     '4',   0,     0,     0,     '6',   0,
    /* 50 */ 0,     0,     0,     0,     0,     'a',   0,     '8',
    /* 58 */ 0,     '9',   0,     0,     0,     0,     0,     0,
};
static char s_name[CARD_COUNT + 1][NAME_MAX];
static int  s_names_ready;

static int name_table_resident(void)
{
    return psx_mod_read_half(NAME_OFFSETS + 2u) != 0u;
}

static void decode_names(void)
{
    for (int id = 1; id <= CARD_COUNT; id++) {
        const uint32_t off = psx_mod_read_half(NAME_OFFSETS + (uint32_t)id * 2u);
        const uint32_t a = NAME_SEGMENT + off;
        unsigned n = 0;
        for (unsigned i = 0; i < NAME_MAX * 2u && n + 1u < NAME_MAX; i++) {
            const unsigned raw = psx_mod_read_byte(a + i);
            if (raw == 0xFFu || raw >= 128u)
                break;
            const char c = RAW_ASCII[raw];
            if (!c)
                break;
            s_name[id][n++] = c;
        }
        s_name[id][n] = '\0';
    }
    s_names_ready = 1;
}

static int name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

/* ---- rarity model ---------------------------------------------------------
 * Six tiers, RAREST first: legendary, ultra_rare, super_rare, rare,
 * uncommon, default. A card takes the FIRST tier, rarest first, whose
 * threshold it does not exceed. The value tested against that threshold is
 * the card's MAX WEIGHT: the single BEST odds any one duelist ever gives
 * it — the highest raw weight, out of PSX_DROP_DB_TOTAL (2048; the same
 * denominator the drop table viewer's percentages use), this card reaches
 * in any (duelist, band) entry. Not "how many duelists drop it" (the CARD
 * SHOP's measure, and this mod's first cut) — that treats a card 20
 * duelists each drop at a 1-in-2048 sliver the same as one they all drop
 * generously; max weight tells them apart. legendary (weight 0) still means
 * "nobody drops it at all".
 *
 * The engine's usable palette is only 0-6 (see COL_RED's comment above), so
 * six tiers get six distinct colours, one each. The thresholds below are
 * this project's own estimate, spaced across the 0-2048 weight range —
 * they, and all six colours, are configurable via card_name_color.ini
 * [tiers].
 *
 * THE THRESHOLD VALUES DECIDE THE RARITY ORDER, NOT THE TIER NAMES.
 * "legendary" is not hard-coded as rarest; it just ships with the smallest
 * threshold. Every session, the five non-default tiers are re-sorted by
 * their (possibly ini-edited) threshold, ascending, and checked in THAT
 * order — so setting, say, uncommon_threshold below legendary_threshold
 * genuinely makes uncommon the new rarest tier, no relabelling needed.
 * default has no threshold of its own and is always the fallback,
 * regardless of what the other five say. */
enum { TIER_LEGENDARY, TIER_ULTRA_RARE, TIER_SUPER_RARE, TIER_RARE,
       TIER_UNCOMMON, TIER_DEFAULT, RARITY_TIERS };
static uint8_t s_tier_color[RARITY_TIERS] = {
    COL_BLUE, COL_RED, COL_ORANGE, COL_YELLOW, COL_GREEN, COL_WHITE,
};
/* Weight is out of 2048 (~20 is ~1%, per the CARD DROPS docs). This
 * project's own estimate — tune freely. */
static int s_tier_threshold[RARITY_TIERS] = { 0, 20, 60, 150, 400, 2048 };

/* TIER_DEFAULT excluded — it is always the fallback, never sorted in.
 * Recomputed once per build_colors() from whatever s_tier_threshold ends up
 * holding (defaults or ini), so an edited ordering takes effect the same
 * way an edited number does. */
static int s_tier_order[TIER_DEFAULT];

static void sort_tiers_by_threshold(void)
{
    for (int i = 0; i < TIER_DEFAULT; i++)
        s_tier_order[i] = i;
    /* TIER_DEFAULT elements: a plain insertion sort is simplest and fast
     * enough at this size. Stable, so tied thresholds keep their original
     * legendary/ultra_rare/super_rare/rare/uncommon order rather than
     * shuffling. */
    for (int i = 1; i < TIER_DEFAULT; i++) {
        const int key = s_tier_order[i];
        const int key_threshold = s_tier_threshold[key];
        int j = i - 1;
        while (j >= 0 && s_tier_threshold[s_tier_order[j]] > key_threshold) {
            s_tier_order[j + 1] = s_tier_order[j];
            j--;
        }
        s_tier_order[j + 1] = key;
    }
}

/* Per-card rarity colour, indexed by id. Built once from the baked drop DB,
 * the ini's thresholds and the ini's per-card overrides. */
static uint8_t s_card_color[CARD_COUNT + 1];
static int     s_color_built;

/* ---- card_name_color.ini --------------------------------------------------- */
#define INI_NAME "card_name_color.ini"
#define OVERRIDE_MAX 128
typedef struct { char name[NAME_MAX]; uint8_t color; } ColorOverride;
static ColorOverride s_override[OVERRIDE_MAX];
static int           s_override_n;

static int ini_path(char *out, unsigned cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (!dir || !dir[0]) return 0;
    const int n = snprintf(out, cap, "%s/%s", dir, INI_NAME);
    return n > 0 && (unsigned)n < cap;
}

static void ini_trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' '  || e[-1] == '\t')) *--e = 0;
}

/* Named colours are the ones this project has actually seen rendered; the
 * text engine's colour byte may support more (its consumer is a per-glyph
 * record field this codebase has not traced past — offset 22 there is
 * reused by too many unrelated structures to follow statically). A plain
 * number (e.g. "color = 4") is passed straight through un-named, so a value
 * can be tried live before anyone gives it a name here. */
static int color_name_to_val(const char *v, uint8_t *out)
{
    if (name_ieq(v, "white"))  { *out = COL_WHITE;  return 1; }
    if (name_ieq(v, "yellow") || name_ieq(v, "gold")) { *out = COL_YELLOW; return 1; }
    if (name_ieq(v, "blue"))   { *out = COL_BLUE;   return 1; }
    if (name_ieq(v, "green"))  { *out = COL_GREEN;  return 1; }
    if (name_ieq(v, "grey") || name_ieq(v, "gray")) { *out = COL_GREY; return 1; }
    if (name_ieq(v, "orange")) { *out = COL_ORANGE; return 1; }
    if (name_ieq(v, "red"))    { *out = COL_RED;    return 1; }
    if (v[0] >= '0' && v[0] <= '9') {
        const int n = atoi(v);
        if (n >= 0 && n <= 255) { *out = (uint8_t)n; return 1; }
    }
    return 0;
}

/* For writing readable defaults; ini_load() never needs this direction. */
static const char *color_val_to_name(uint8_t c)
{
    switch (c) {
    case COL_WHITE:  return "white";
    case COL_YELLOW: return "yellow";
    case COL_BLUE:   return "blue";
    case COL_GREEN:  return "green";
    case COL_GREY:   return "grey";
    case COL_ORANGE: return "orange";
    case COL_RED:    return "red";
    default:         return "white";
    }
}

static void ini_write_default(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# CARD NAME COLOR - tints each card's name by drop rarity.\n"
        "# Edit and restart to apply. Delete this file to reset.\n"
        "\n[tiers]\n"
        "# Six tiers, rarest to most common: legendary, ultra_rare,\n"
        "# super_rare, rare, uncommon, default. A card gets the rarest\n"
        "# tier whose threshold it does not exceed; default is the\n"
        "# catch-all with no threshold. Thresholds decide the order, not\n"
        "# the tier names - the lowest threshold is always rarest.\n"
        "#\n"
        "# threshold = best drop odds any duelist gives the card, as a\n"
        "# weight out of 2048 (lower = rarer). Estimates - tune freely.\n"
        "#\n"
        "# color = white|yellow|orange|red|blue|green|grey, or a number\n"
        "# 0-6 (higher numbers render broken, not a new colour).\n"
        "legendary_threshold   = %d\n"
        "legendary_color       = %s\n"
        "ultra_rare_threshold  = %d\n"
        "ultra_rare_color      = %s\n"
        "super_rare_threshold  = %d\n"
        "super_rare_color      = %s\n"
        "rare_threshold        = %d\n"
        "rare_color            = %s\n"
        "uncommon_threshold    = %d\n"
        "uncommon_color        = %s\n"
        "default_color         = %s\n"
        "\n[cards]\n"
        "# NAME = color, one per line, overrides the tier for that card.\n"
        "# Blank or invalid values are ignored (falls back to [tiers]).\n"
        "#\n"
        "# Every card is listed below, commented out, at white. Uncomment\n"
        "# a line and change its color to pin that card.\n",
        s_tier_threshold[TIER_LEGENDARY],  color_val_to_name(s_tier_color[TIER_LEGENDARY]),
        s_tier_threshold[TIER_ULTRA_RARE], color_val_to_name(s_tier_color[TIER_ULTRA_RARE]),
        s_tier_threshold[TIER_SUPER_RARE], color_val_to_name(s_tier_color[TIER_SUPER_RARE]),
        s_tier_threshold[TIER_RARE],       color_val_to_name(s_tier_color[TIER_RARE]),
        s_tier_threshold[TIER_UNCOMMON],   color_val_to_name(s_tier_color[TIER_UNCOMMON]),
        color_val_to_name(s_tier_color[TIER_DEFAULT]));
    for (int id = 1; id <= CARD_COUNT; id++)
        if (s_name[id][0])
            fprintf(f, "# %s = white\n", s_name[id]);
    fclose(f);
}

static void ini_load(void)
{
    char path[512];
    if (!ini_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) { ini_write_default(path); return; }
    s_override_n = 0;
    char line[160], sect[24] = "";
    while (fgets(line, sizeof line, f)) {
        /* '#' and ';' both introduce a comment; ';' also ends an inline one
         * (e.g. "8   ; rare") without needing it at line start. */
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        ini_trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (!e) continue;
            *e = 0;
            snprintf(sect, sizeof sect, "%s", p + 1);
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        ini_trim(p);
        ini_trim(v);
        if (!strcmp(sect, "tiers")) {
            int tier = -1;
            if      (!strncmp(p, "legendary_", 10))  tier = TIER_LEGENDARY;
            else if (!strncmp(p, "ultra_rare_", 11))  tier = TIER_ULTRA_RARE;
            else if (!strncmp(p, "super_rare_", 11))  tier = TIER_SUPER_RARE;
            else if (!strncmp(p, "rare_", 5))         tier = TIER_RARE;
            else if (!strncmp(p, "uncommon_", 9))     tier = TIER_UNCOMMON;
            else if (!strncmp(p, "default_", 8))      tier = TIER_DEFAULT;
            if (tier < 0) continue;
            if (strstr(p, "threshold")) {
                const int n = atoi(v);
                if (n >= 0) s_tier_threshold[tier] = n;
            } else if (strstr(p, "color") || strstr(p, "colour")) {
                uint8_t col;
                if (color_name_to_val(v, &col)) s_tier_color[tier] = col;
            }
        } else if (!strcmp(sect, "cards")) {
            uint8_t col;
            if (!p[0] || !color_name_to_val(v, &col)) continue;
            if (s_override_n >= OVERRIDE_MAX) continue;
            snprintf(s_override[s_override_n].name,
                     sizeof s_override[0].name, "%s", p);
            s_override[s_override_n].color = col;
            s_override_n++;
        }
    }
    fclose(f);
}

/* An override for card id, or -1 if none. Needs s_names_ready. */
static int override_for(int id)
{
    for (int i = 0; i < s_override_n; i++)
        if (name_ieq(s_override[i].name, s_name[id]))
            return (int)s_override[i].color;
    return -1;
}

static void build_colors(void)
{
    if (s_color_built)
        return;
    if (!name_table_resident())
        return;               /* try again next call, once the EXE is up */
    decode_names();
    ini_load();
    sort_tiers_by_threshold();

    /* max_weight[card] = the single best (highest) raw weight, out of
     * PSX_DROP_DB_TOTAL (2048), any one duelist ever gives this card. */
    static uint16_t max_weight[CARD_COUNT + 1];
    memset(max_weight, 0, sizeof max_weight);
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++)
        for (int t = 0; t < PSX_DROP_DB_TIERS; t++)
            for (int i = 0; i < PSX_DROP_DB[d].count[t]; i++) {
                const int c = PSX_DROP_DB[d].tier[t][i].card;
                const uint16_t w = PSX_DROP_DB[d].tier[t][i].weight;
                if (c >= 1 && c <= CARD_COUNT && w > max_weight[c])
                    max_weight[c] = w;
            }
    for (int id = 1; id <= CARD_COUNT; id++) {
        const int ov = override_for(id);
        if (ov >= 0) {
            s_card_color[id] = (uint8_t)ov;
            continue;
        }
        const int n = (int)max_weight[id];
        uint8_t col = s_tier_color[TIER_DEFAULT];
        for (int i = 0; i < TIER_DEFAULT; i++) {
            const int t = s_tier_order[i];
            if (n <= s_tier_threshold[t]) { col = s_tier_color[t]; break; }
        }
        s_card_color[id] = col;
    }
    s_color_built = 1;
}

/* MODS > CARD NAME COLOR. On by default — this is purely cosmetic (never
 * touches a save), so there is nothing to opt into the way DROP MISSING
 * CARDS' save-shaped change has. */
static int s_enabled = 1;

/* ---- confirming a run of glyphs actually IS the card's name ---------------
 *
 * $ra == CARD_NAME_CALLER fires for EVERY piece of text a card-info widget
 * draws — name, type, Guardian Stars, description — not just the name
 * (confirmed live 2026-08: a debug log showed thousands of consecutive
 * "is_name" entries with no gaps while browsing a CARD VIEW screen, where
 * the type line and description are real text, unlike the CHEST/deck-list
 * screens where nothing else is drawn as text and this filter happens to
 * work by coincidence). So the id from 0x8009B338 says WHICH card; telling
 * the name apart from the card's other text needs the glyphs themselves.
 *
 * Unlike the earlier, abandoned attempt to identify a card FROM its glyphs
 * (narrowing over all 722 names, unreliable — see the file's SCOPE note
 * history), this only ever checks ONE already-known target: does the text
 * about to stream match s_name[id] exactly? A type line or description
 * cannot coincidentally equal a card's full name, so matching is safe with
 * no ambiguity to narrow.
 *
 * func_80036C14 delivers one DECODED character at a time as a1 — a simple
 * cipher (space=0x00, 'A'-'Z'=0x60-0x79, apostrophe=0x66, 'a'-'z'=0x81-0x9A)
 * that is NOT the frequency code s_name[] was decoded from (psx_card_db.c's
 * table), but resolves to the same plain ASCII either way, so comparing
 * decoded characters against s_name[id] one at a time works. */
static char glyph_ascii(unsigned code)
{
    if (code == 0x00u) return ' ';
    if (code == 0x66u) return '\'';
    if (code >= 0x60u && code <= 0x79u) return (char)('A' + (int)(code - 0x60u));
    if (code >= 0x81u && code <= 0x9Au) return (char)('a' + (int)(code - 0x81u));
    return 0;   /* not a name-stream code: an escape, a digit run, etc. */
}

/* The widget currently expecting name-glyphs, the position matched so far
 * against s_match_target, and whether the match is still alive. Reset at
 * every CARD_NAME_CALLER entry (a new run is starting); consumed glyph by
 * glyph in the glyph hook. */
static uint32_t    s_match_obj;
static int         s_match_active;
static int         s_match_pos;
static const char *s_match_target;   /* == s_name[id]; owned by s_name[] */
static uint8_t      s_match_color;

static void card_name_text(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_TEXT_FN)
        return;

    const uint32_t obj = cpu->gpr[4];

    if (s_enabled && cpu->gpr[31] == CARD_NAME_CALLER) {
        const int id = (int)psx_mod_read_half(PSX_SELECTED_CARD);
        build_colors();
        if (s_color_built && id >= 1 && id <= CARD_COUNT && s_name[id][0]) {
            /* Revert first: this run has not been confirmed as the name
             * yet, so start from the stock colour and let the glyph hook
             * re-apply the rarity colour only while the match holds. */
            psx_mod_write_byte(obj + GLYPH_COLOR_OFF, COL_WHITE);
            s_match_obj    = obj;
            s_match_active = 1;
            s_match_pos    = 0;
            s_match_target = s_name[id];
            s_match_color  = s_card_color[id];
            return;
        }
    }

    /* Any other widget entry: nothing here is a candidate name run. */
    if (s_match_obj == obj)
        s_match_active = 0;
}

static void card_name_glyph(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_GLYPH_FN)
        return;
    if (!s_match_active || cpu->gpr[4] != s_match_obj)
        return;

    const char c = glyph_ascii((unsigned)(cpu->gpr[5] & 0xFFu));
    if (c && c == s_match_target[s_match_pos]) {
        psx_mod_write_byte(cpu->gpr[4] + GLYPH_COLOR_OFF, s_match_color);
        s_match_pos++;
        if (!s_match_target[s_match_pos])
            s_match_active = 0;   /* full name matched; nothing more to do */
    } else {
        s_match_active = 0;      /* mismatch, or ran past the name's length:
                                   * this run is not the name after all */
    }
}

static const char *const ONOFF[] = { "OFF", "ON" };

static void card_name_color_enabled_changed(int value)
{
    s_enabled = value ? 1 : 0;
}

static void card_name_color_register_menu(void)
{
    (void)psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "CARD NAME COLOR",
        "TINTS CARD NAMES BY DROP RARITY",
        ONOFF, 2, "card_name_color", 1, card_name_color_enabled_changed);
}

PSX_MOD_CONSTRUCTOR(psx_card_name_color_install)
{
    card_name_color_register_menu();
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.text",
        PSX_TEXT_FN,
        card_name_text);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.glyph",
        PSX_GLYPH_FN,
        card_name_glyph);
}
