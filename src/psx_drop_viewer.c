/* psx_drop_viewer.c — see psx_drop_viewer.h.
 *
 * WHY A REAL WINDOW
 * -----------------
 * Everything else this title adds is drawn into the game's own picture, because
 * everything else belongs there. A drop table does not: it is a reference you
 * keep open beside the game while you grind, on a second monitor if you have
 * one. An overlay cannot leave the game's screen, so this is an actual OS
 * window with its own renderer.
 *
 * It is created on open and destroyed on close, so a closed viewer costs one
 * predictable branch per frame and nothing else.
 *
 * WHERE THE NUMBERS COME FROM
 * ---------------------------
 * Two different places, for two different reasons.
 *
 *   Cards (name, ATK, DEF) — read live out of guest RAM by psx_card_db.c. The
 *   game EXE carries them and they are resident from the moment it starts, so
 *   there is nothing to bake and no copy to go stale.
 *
 *   Drop tables — baked from the player's disc at build time (psx_drop_db.h).
 *   They cannot be read live: the game keeps exactly ONE duelist's table in
 *   RAM, the current opponent's, and this lists all thirty-nine.
 *
 * MODS > DROP MISSING CARDS
 * -------------------------
 * With that row on, the table a player rolls against is not the stock one. So
 * the viewer applies the mod's own transform — psx_drop_missing_transform(),
 * the same function the mod calls on guest memory — rather than describing the
 * change in its own words. The two cannot disagree, and the header says which
 * table is on screen.
 */

#include "psx_drop_viewer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_drop_db.h"
#include "psx_drop_edits.h"
#include "psx_drop_missing.h"
#include "psx_duelist_icon_cache.h"
#include "psx_duelist_icons.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#include "psx_ui_font8.inc"

/* --- look ---------------------------------------------------------------- */

#define WIN_W  1480
#define WIN_H   820
#define GLYPH   8

/* Text scale. Derived from the drawable each frame (see tick), because the
 * window ranges from a laptop corner to a maximized 4K display and a fixed
 * multiplier is unreadably small at one end or comically large at the other.
 * 740x410 is the layout's natural size at scale 1, so the scale is simply
 * "how many of those fit". Everything below is in TS units for that reason:
 * one scale value moves the whole layout together. */
static int s_ts = 2;
#define TS      s_ts
#define CW      (GLYPH * TS)       /* character cell width               */
#define ROW_H   (11 * TS)          /* table row pitch                    */
#define PAD     (5 * TS)
#define BTN_H   (13 * TS)          /* buttons and header strips          */
#define TXT_DY  ((5 * TS) / 2)     /* centers a glyph in a BTN_H strip   */

#define C_BG      0xFF12141Cu
#define C_PANEL   0xFF1A1D27u
#define C_HEADER  0xFF232735u
#define C_LINE    0xFF2E3344u
#define C_TEXT    0xFFDCE2F0u
#define C_DIM     0xFF8890A6u
#define C_ACCENT  0xFFE8B44Bu
#define C_HOT     0xFF39405Au
#define C_HOVER   0xFF262B3Bu      /* under the pointer; dimmer than C_HOT */

/* --- canvas -------------------------------------------------------------- */

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint32_t     *s_px;
static int           s_w, s_h;
static int           s_dirty = 1;

static void px_fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s_px + (size_t)(y + j) * s_w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

/* Returns the pen x after the string, so a caller can chain without
 * recomputing widths. Clips to `right` so a long card name stops at the
 * column edge instead of running into the next one. */
static int px_text(int x, int y, int right, uint32_t c, const char *s)
{
    for (; *s; s++) {
        const unsigned ch = (unsigned char)*s;
        if (x + CW > right) break;
        if (ch >= 32u && ch < 127u) {
            const unsigned char *g = FONT8[ch - 32u];
            for (int gy = 0; gy < GLYPH; gy++) {
                const unsigned bits = g[gy];
                if (!bits) continue;
                for (int gx = 0; gx < GLYPH; gx++) {
                    if (!(bits & (1u << gx))) continue;
                    px_fill(x + gx * TS, y + gy * TS, TS, TS, c);
                }
            }
        }
        x += CW;
    }
    return x;
}

static int text_w(const char *s) { return (int)strlen(s) * CW; }

/* The duelist's FREE DUEL portrait, scaled to the row. Nearest-neighbour pick,
 * not an average: these are pixel icons and averaging turns their outlines to
 * mud. Falls back to a plain plate, so a duelist whose icon was never captured
 * costs one row's decoration and nothing else. */
#define ICON_PX (10 * TS)          /* fits a ROW_H row with TS to spare */

static void draw_icon(int x, int y, int duelist)
{
    const int dst_px = ICON_PX;
    const uint32_t *src = (duelist >= 0 && duelist < PSX_DUELIST_ICON_N)
                              ? PSX_DUELIST_ICONS[duelist] : 0;
    /* No compile-time portrait (player builds ship none): the runtime cache,
     * captured from this player's own FREE DUEL screen. Same 38x38 layout. */
    if (!src) src = psx_duelist_icon_cache_get(duelist);
    if (!src) { px_fill(x, y, dst_px, dst_px, C_HEADER); return; }
    for (int j = 0; j < dst_px; j++) {
        if (y + j < 0 || y + j >= s_h) continue;
        uint32_t *dst = s_px + (size_t)(y + j) * s_w + x;
        const uint32_t *row =
            src + (size_t)(j * PSX_DUELIST_ICON_H / dst_px) * PSX_DUELIST_ICON_W;
        for (int i = 0; i < dst_px; i++) {
            if (x + i < 0 || x + i >= s_w) continue;
            const uint32_t p = row[i * PSX_DUELIST_ICON_W / dst_px];
            if (p >> 24) dst[i] = 0xFF000000u | (p & 0x00FFFFFFu);
        }
    }
}

static void px_text_right(int right, int y, uint32_t c, const char *s)
{
    px_text(right - text_w(s), y, right, c, s);
}

/* --- the data ------------------------------------------------------------ */

#define NCARDS PSX_DROP_DB_CARDS
#define NDUEL  PSX_DROP_DB_DUELISTS
#define NTIER  PSX_DROP_DB_TIERS

/* The table as the player will actually roll it: stock, then the DROP MISSING
 * CARDS placements when that row is on, then the player's own edits — the
 * same order psx_drop_missing.c's tick writes into guest RAM. Rebuilt only
 * when a layer changes, because 39x3x722 is not per-frame work. */
static uint16_t s_eff[NDUEL][NTIER][NCARDS];
static int      s_eff_valid;
static int      s_eff_modded;
static unsigned s_eff_gen;          /* edit-layer generation it was built at */

/* One tier of that layering, into a caller's array — the piece commit_edit
 * trial-runs before accepting an edit. Returns the edit layer's result code. */
static int eff_tier(int d, int t, uint16_t *w)
{
    memset(w, 0, NCARDS * sizeof(*w));
    const PsxDropDbDuelist *db = &PSX_DROP_DB[d];
    for (int i = 0; i < db->count[t]; i++) {
        const PsxDropWeight *e = &db->tier[t][i];
        if (e->card >= 1 && e->card <= NCARDS)
            w[e->card - 1] = e->weight;
    }
    /* A negative return means the mod would not have touched this band
     * either, so the stock weights standing is the right answer rather than
     * a failure to report. */
    if (psx_drop_missing_enabled())
        (void)psx_drop_missing_transform(d, t, w);
    return psx_drop_edits_apply(d, t, w);
}

static void build_effective(void)
{
    const int on = psx_drop_missing_enabled();
    const unsigned gen = psx_drop_edits_generation();
    if (s_eff_valid && s_eff_modded == on && s_eff_gen == gen) return;
    if (on) psx_drop_missing_ensure_loaded();
    for (int d = 0; d < NDUEL; d++)
        for (int t = 0; t < NTIER; t++)
            (void)eff_tier(d, t, s_eff[d][t]);
    s_eff_modded = on;
    s_eff_gen = gen;
    s_eff_valid = 1;
}

/* --- view state ---------------------------------------------------------- */

enum { VIEW_CARDS = 0, VIEW_DUELISTS = 1 };
enum { SORT_ID = 0, SORT_NAME, SORT_ATK, SORT_DEF, SORT_DROPS, SORT_TYPE };
/* Right pane. WEIGHT and CHANCE are the same number scaled, so they are the
 * same sort key on purpose. */
enum { RSORT_RANK = 0, RSORT_WEIGHT };
/* Left list, BY DUELIST view. Roster order is the game's own and stays the
 * default; the header clicks cycle through the others and back (see
 * set_dsort). */
enum { DSORT_ROSTER = 0, DSORT_NAME, DSORT_DROPS };

static int  s_view = VIEW_CARDS;
static int  s_sort = SORT_ID;
static int  s_desc;
static int  s_rsort = RSORT_RANK;
static int  s_rdesc;
static int  s_dsort = DSORT_ROSTER;
static int  s_ddesc;
/* BY CARD only: pad the droppers list out to the WHOLE roster, non-droppers
 * greyed at the bottom (tier -1 rows), so any duelist is a drag-and-drop
 * target without switching views. */
static int  s_all_cpu;
static char s_search[32];
static int  s_sel_card = 1;
static int  s_sel_duelist;
static int  s_scroll;            /* first visible row of the left list */
static int  s_scroll_right;      /* first visible row of the right pane */

/* Which row the pointer is over, so the window answers hover the way any
 * desktop list does. Pane 0 is the left list, 1 the right; -1 is neither. */
static int  s_hover_pane = -1;
static int  s_hover_row  = -1;

/* The search caret blinks; this is its current phase, flipped from tick. */
static int  s_caret_on = 1;

/* Weight-cell editing: which right-pane row (absolute index into s_rows) has
 * the number box open, and what has been typed into it. -1 = not editing. */
static int  s_edit_row = -1;
static char s_edit_buf[6];
static int  s_edit_len;

/* One-line status ("SAVED", "EDIT REFUSED: ..."), drawn in the title strip
 * until its deadline passes. */
static char     s_msg[64];
static uint32_t s_msg_until;

/* Right-click context menu (also the LOAD button's file list): a handful of
 * actions on whatever was under the pointer. One level, no submenus — band
 * choices are spelled out as items. */
enum { CM_NONE = 0, CM_ADD, CM_EDIT_WEIGHT, CM_MOVE_BAND, CM_REMOVE,
       CM_LOAD_FILE, CM_EXPORT };
#define CMENU_MAX 12
static struct {
    char label[44];
    int  action;
    int  a, b, c;                  /* action args: duelist/card/band or row */
    char s[64];                    /* CM_LOAD_FILE: the file's bare name    */
} s_cmenu[CMENU_MAX];
static int s_cmenu_n;              /* 0 = closed */
static int s_cmenu_x, s_cmenu_y;
static int s_cmenu_hover = -1;

/* Drag and drop. A press on a draggable row ARMS a drag; crossing the
 * threshold makes it live; release either drops or, if it never went live,
 * performs the row's ordinary click action. Right-pane row actions moved to
 * release for exactly this reason. */
enum { DRAG_NONE = 0, DRAG_CARD, DRAG_ROW };
static int s_down;                 /* left button currently held */
static int s_down_x, s_down_y;
static int s_drag_kind;
static int s_drag_card;            /* DRAG_CARD: the card id */
static int s_drag_row;             /* DRAG_ROW: absolute right-pane row */
static int s_drag_live;
static int s_mouse_x, s_mouse_y;

/* The debug server may ask for the window from another thread, and window
 * creation belongs to the main thread; tick consumes this there. */
static volatile int s_open_req;

static int s_order[NCARDS];      /* card ids, filtered and sorted */
static int s_order_n;
static int s_drop_count[NCARDS + 1];

static int s_duel_order[NDUEL];  /* duelist indices in display order */
static int s_duel_total[NDUEL];  /* distinct drop entries, all tiers  */

/* one row of the right-hand pane, either view */
typedef struct { int duelist; int card; int tier; int weight; } DropRow;
static DropRow s_rows[NCARDS * 2];
static int     s_rows_n;

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static int cmp_key(int id, int *atk, int *def)
{
    *atk = *def = -1;
    int a = 0, d = 0, ty = 0;
    if (psx_card_db_stats(id, &a, &d, &ty)) { *atk = a; *def = d; }
    return 0;
}

static const char *type_of(int id)
{
    int a = 0, d = 0, ty = -1;
    if (!psx_card_db_stats(id, &a, &d, &ty)) return "";
    return psx_card_db_type_name(ty);
}

static int order_cmp(const void *pa, const void *pb)
{
    const int ia = *(const int *)pa, ib = *(const int *)pb;
    int r = 0;
    if (s_sort == SORT_ID) {
        r = ia - ib;
    } else if (s_sort == SORT_NAME) {
        r = strcmp(psx_card_db_name(ia), psx_card_db_name(ib));
    } else if (s_sort == SORT_TYPE) {
        r = strcmp(type_of(ia), type_of(ib));
    } else if (s_sort == SORT_DROPS) {
        r = s_drop_count[ia] - s_drop_count[ib];
    } else {
        int aa, ad, ba, bd;
        cmp_key(ia, &aa, &ad);
        cmp_key(ib, &ba, &bd);
        r = (s_sort == SORT_ATK) ? aa - ba : ad - bd;
    }
    if (r == 0) r = ia - ib;         /* id is the tie-break, so order is stable */
    return s_desc ? -r : r;
}

static int duel_cmp(const void *pa, const void *pb)
{
    const int ia = *(const int *)pa, ib = *(const int *)pb;
    int r = 0;
    if (s_dsort == DSORT_NAME)
        r = strcmp(PSX_DROP_DB[ia].name, PSX_DROP_DB[ib].name);
    else if (s_dsort == DSORT_DROPS)
        r = s_duel_total[ia] - s_duel_total[ib];
    if (r == 0) r = ia - ib;         /* roster index is the stable tie-break */
    return s_ddesc ? -r : r;
}

static void rebuild_duel_order(void)
{
    build_effective();
    for (int d = 0; d < NDUEL; d++) {
        int n = 0;
        for (int t = 0; t < NTIER; t++)
            for (int c = 0; c < NCARDS; c++)
                if (s_eff[d][t][c]) n++;
        s_duel_total[d] = n;
        s_duel_order[d] = d;
    }
    if (s_dsort != DSORT_ROSTER)
        qsort(s_duel_order, NDUEL, sizeof(int), duel_cmp);
}

/* Where a duelist sits in the current display order, for centering on it. */
static int duel_pos(int d)
{
    for (int i = 0; i < NDUEL; i++)
        if (s_duel_order[i] == d) return i;
    return 0;
}

static void rebuild_order(void)
{
    build_effective();
    for (int id = 1; id <= NCARDS; id++) {
        int n = 0;
        for (int d = 0; d < NDUEL; d++)
            for (int t = 0; t < NTIER; t++)
                if (s_eff[d][t][id - 1]) n++;
        s_drop_count[id] = n;
    }
    s_order_n = 0;
    for (int id = 1; id <= NCARDS; id++) {
        char idbuf[8];
        snprintf(idbuf, sizeof idbuf, "%d", id);
        if (s_search[0] && !ci_contains(psx_card_db_name(id), s_search)
            && !ci_contains(idbuf, s_search))
            continue;
        s_order[s_order_n++] = id;
    }
    qsort(s_order, (size_t)s_order_n, sizeof(int), order_cmp);
}

static int rows_cmp(const void *pa, const void *pb)
{
    const DropRow *a = (const DropRow *)pa, *b = (const DropRow *)pb;
    /* Grey non-dropper rows (tier -1, ALL CPU toggle) sink to the bottom in
     * roster order, whichever way the real rows are sorted. */
    const int ga = a->tier < 0, gb = b->tier < 0;
    if (ga != gb) return ga - gb;
    if (ga) return a->duelist - b->duelist;
    int r;
    if (s_rsort == RSORT_WEIGHT) {
        r = a->weight - b->weight;
    } else {
        r = a->tier - b->tier;
        /* Within a band the heaviest drop comes first whichever way the band
         * column runs — that is what "rank-then-weight" means. */
        if (r == 0) {
            if (a->weight != b->weight) return b->weight - a->weight;
            return (a->duelist != b->duelist) ? a->duelist - b->duelist
                                              : a->card - b->card;
        }
    }
    if (r == 0) r = (a->duelist != b->duelist) ? a->duelist - b->duelist
                                               : a->card - b->card;
    return s_rdesc ? -r : r;
}

static void rebuild_rows(void)
{
    build_effective();
    s_rows_n = 0;
    if (s_view == VIEW_CARDS) {
        const int id = s_sel_card;
        int drops[NDUEL] = { 0 };
        for (int d = 0; d < NDUEL; d++)
            for (int t = 0; t < NTIER; t++) {
                const int w = s_eff[d][t][id - 1];
                if (!w) continue;
                s_rows[s_rows_n].duelist = d;
                s_rows[s_rows_n].card = id;
                s_rows[s_rows_n].tier = t;
                s_rows[s_rows_n].weight = w;
                s_rows_n++;
                drops[d] = 1;
            }
        if (s_all_cpu)
            for (int d = 0; d < NDUEL; d++) {
                if (drops[d]) continue;
                s_rows[s_rows_n].duelist = d;
                s_rows[s_rows_n].card = id;
                s_rows[s_rows_n].tier = -1;   /* grey: does not drop it */
                s_rows[s_rows_n].weight = 0;
                s_rows_n++;
            }
    } else {
        const int d = s_sel_duelist;
        for (int t = 0; t < NTIER; t++)
            for (int c = 0; c < NCARDS; c++) {
                const int w = s_eff[d][t][c];
                if (!w) continue;
                s_rows[s_rows_n].duelist = d;
                s_rows[s_rows_n].card = c + 1;
                s_rows[s_rows_n].tier = t;
                s_rows[s_rows_n].weight = w;
                s_rows_n++;
            }
    }
    qsort(s_rows, (size_t)s_rows_n, sizeof(DropRow), rows_cmp);
    if (s_scroll_right >= s_rows_n) s_scroll_right = 0;
}

static void invalidate(void)
{
    s_eff_valid = 0;
    rebuild_order();
    rebuild_duel_order();
    rebuild_rows();
    s_dirty = 1;
}

/* --- editing -------------------------------------------------------------- */

static int list_rows(void);        /* defined with the layout, used below */

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 3000u;
    s_dirty = 1;
}

static void edit_begin(int row)
{
    s_edit_row = row;
    s_edit_len = 0;
    s_edit_buf[0] = 0;
    s_dirty = 1;
}

static void edit_end(void)
{
    if (s_edit_row >= 0) { s_edit_row = -1; s_dirty = 1; }
}

/* Record (duelist, card) -> vector, then PROVE all three bands still
 * renormalize by trial-building them; back the entry out and say why when one
 * does not. An edit the viewer accepts is therefore an edit the game can
 * roll. */
static int commit_vector(int d, int card, const uint16_t v[3])
{
    uint16_t old[3];
    const int had = psx_drop_edits_get(d, card, old);
    if (!psx_drop_edits_set(d, card, v)) {
        say("EDIT REFUSED: EDIT TABLE FULL");
        return 0;
    }
    static uint16_t tmp[NCARDS];
    for (int t = 0; t < NTIER; t++) {
        const int rc = eff_tier(d, t, tmp);
        if (rc == 1 || rc == -1) continue;   /* transformed, or nothing to do */
        if (had) (void)psx_drop_edits_set(d, card, old);
        else     (void)psx_drop_edits_unset(d, card);
        say(rc == -4 ? "EDIT REFUSED: BAND WOULD EXCEED 1984"
                     : "EDIT REFUSED: BAND CANNOT RENORMALIZE");
        return 0;
    }
    invalidate();
    return 1;
}

static void commit_edit_weight(int row, int weight)
{
    if (row < 0 || row >= s_rows_n) return;
    const DropRow *r = &s_rows[row];
    if (weight < 0) weight = 0;
    if (weight > (int)PSX_DROP_DB_TOTAL - 64) {
        say("EDIT REFUSED: MAX WEIGHT IS 1984");
        return;
    }
    uint16_t v[NTIER];
    for (int t = 0; t < NTIER; t++) v[t] = s_eff[r->duelist][t][r->card - 1];
    v[r->tier] = (uint16_t)weight;
    if (commit_vector(r->duelist, r->card, v))
        say(weight ? "EDITED - SAVE TO KEEP" : "REMOVED FROM BAND - SAVE TO KEEP");
}

/* Move a row to another band; the weight travels with it (merging into
 * anything already in the destination). The rank cell cycles through the
 * bands with this; the context menu jumps straight to one. */
static void move_row_band(int row, int to)
{
    if (row < 0 || row >= s_rows_n || to < 0 || to >= NTIER) return;
    const DropRow *r = &s_rows[row];
    if (r->tier < 0 || to == r->tier) return;
    uint16_t v[NTIER];
    for (int t = 0; t < NTIER; t++) v[t] = s_eff[r->duelist][t][r->card - 1];
    unsigned merged = (unsigned)v[to] + (unsigned)v[r->tier];
    if (merged > PSX_DROP_DB_TOTAL - 64u) merged = PSX_DROP_DB_TOTAL - 64u;
    v[r->tier] = 0;
    v[to] = (uint16_t)merged;
    if (commit_vector(r->duelist, r->card, v))
        say("BAND MOVED - SAVE TO KEEP");
}

static void cycle_rank(int row)
{
    if (row < 0 || row >= s_rows_n) return;
    move_row_band(row, (s_rows[row].tier + 1) % NTIER);
}

static void remove_row_band(int row)
{
    if (row < 0 || row >= s_rows_n || s_rows[row].tier < 0) return;
    const DropRow *r = &s_rows[row];
    uint16_t v[NTIER];
    for (int t = 0; t < NTIER; t++) v[t] = s_eff[r->duelist][t][r->card - 1];
    v[r->tier] = 0;
    if (commit_vector(r->duelist, r->card, v))
        say("REMOVED FROM BAND - SAVE TO KEEP");
}

/* Add a card to a duelist's band at a starter weight of 20 (~1%), ready to
 * be tuned in the number box. Refused when the card is already there — that
 * is an edit, not an add, and silently changing it would surprise. */
#define ADD_WEIGHT 20

static int add_card(int d, int card, int band)
{
    if (d < 0 || d >= NDUEL || card < 1 || card > NCARDS) return 0;
    build_effective();
    uint16_t v[NTIER];
    for (int t = 0; t < NTIER; t++) v[t] = s_eff[d][t][card - 1];
    if (v[band]) {
        say("ALREADY IN THAT BAND - EDIT ITS WEIGHT");
        return 0;
    }
    v[band] = ADD_WEIGHT;
    if (!commit_vector(d, card, v)) return 0;
    say("ADDED AT WEIGHT 20 - SAVE TO KEEP");
    /* If the new row is on screen (BY DUELIST, that duelist), open its
     * number box so the starter weight can be typed over immediately. */
    if (s_view == VIEW_DUELISTS && d == s_sel_duelist)
        for (int i = 0; i < s_rows_n; i++)
            if (s_rows[i].card == card && s_rows[i].tier == band) {
                if (i < s_scroll_right
                    || i >= s_scroll_right + list_rows()) {
                    s_scroll_right = i - list_rows() / 2;
                    if (s_scroll_right < 0) s_scroll_right = 0;
                }
                edit_begin(i);
                break;
            }
    return 1;
}

/* --- context menu --------------------------------------------------------- */

static void cmenu_close(void)
{
    if (s_cmenu_n) { s_cmenu_n = 0; s_cmenu_hover = -1; s_dirty = 1; }
}

static void cmenu_add(const char *label, int action, int a, int b, int c)
{
    if (s_cmenu_n >= CMENU_MAX) return;
    snprintf(s_cmenu[s_cmenu_n].label, sizeof(s_cmenu[0].label), "%s", label);
    s_cmenu[s_cmenu_n].action = action;
    s_cmenu[s_cmenu_n].a = a;
    s_cmenu[s_cmenu_n].b = b;
    s_cmenu[s_cmenu_n].c = c;
    s_cmenu[s_cmenu_n].s[0] = '\0';
    s_cmenu_n++;
}

static void cmenu_add_s(const char *label, int action, const char *s)
{
    if (s_cmenu_n >= CMENU_MAX) return;
    cmenu_add(label, action, 0, 0, 0);
    snprintf(s_cmenu[s_cmenu_n - 1].s, sizeof(s_cmenu[0].s), "%s", s);
}

static void cmenu_geom(int *x, int *y, int *w, int *h)
{
    int wide = 0;
    for (int i = 0; i < s_cmenu_n; i++) {
        const int l = text_w(s_cmenu[i].label);
        if (l > wide) wide = l;
    }
    *w = wide + 2 * PAD;
    *h = s_cmenu_n * BTN_H + 2 * TS;
    *x = s_cmenu_x;
    *y = s_cmenu_y;
    if (*x + *w > s_w) *x = s_w - *w;
    if (*y + *h > s_h) *y = s_h - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static int cmenu_item_at(int x, int y)
{
    int mx, my, mw, mh;
    if (!s_cmenu_n) return -1;
    cmenu_geom(&mx, &my, &mw, &mh);
    if (x < mx || x >= mx + mw || y < my + TS || y >= my + mh - TS) return -1;
    const int i = (y - my - TS) / BTN_H;
    return (i >= 0 && i < s_cmenu_n) ? i : -1;
}

static void cmenu_run(int i)
{
    if (i < 0 || i >= s_cmenu_n) return;
    const int action = s_cmenu[i].action;
    const int a = s_cmenu[i].a, b = s_cmenu[i].b, c = s_cmenu[i].c;
    char sarg[64];
    snprintf(sarg, sizeof sarg, "%s", s_cmenu[i].s);
    cmenu_close();
    switch (action) {
    case CM_ADD:         (void)add_card(a, b, c); break;
    case CM_EDIT_WEIGHT: edit_begin(a); break;
    case CM_MOVE_BAND:   move_row_band(a, b); break;
    case CM_REMOVE:      remove_row_band(a); break;
    case CM_LOAD_FILE: {
        const int n = psx_drop_edits_load_file(sarg);
        if (n >= 0) {
            invalidate();
            char m[64];
            snprintf(m, sizeof m, "LOADED %d ENTRIES - SAVE TO KEEP", n);
            say(m);
        } else {
            say("LOAD FAILED");
        }
        break;
    }
    case CM_EXPORT: {
        char name[64];
        if (psx_drop_edits_export(name, sizeof name)) {
            char m[64];
            snprintf(m, sizeof m, "EXPORTED %.40s", name);
            say(m);
        } else {
            say("EXPORT FAILED");
        }
        break;
    }
    default: break;
    }
}

/* The LOAD button's menu: export the current table for sharing, or load a
 * shared one from <player-data>/drop_tables. */
static void open_load_menu(int x, int y)
{
    char names[CMENU_MAX][64];
    const int n = psx_drop_edits_list_shared(names, CMENU_MAX - 1);
    edit_end();
    s_cmenu_x = x;
    s_cmenu_y = y;
    s_cmenu_n = 0;
    s_cmenu_hover = -1;
    cmenu_add("EXPORT CURRENT TO drop_tables", CM_EXPORT, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        char label[44];
        snprintf(label, sizeof label, "LOAD %.36s", names[i]);
        cmenu_add_s(label, CM_LOAD_FILE, names[i]);
    }
    if (!n)
        cmenu_add("(NO .INI FILES IN drop_tables YET)", CM_NONE, 0, 0, 0);
    s_dirty = 1;
}

/* --- layout ---------------------------------------------------------------
 *
 * Everything is derived from the drawable size, because the window is the
 * player's to resize and drag between displays: a 1180-wide constant becomes
 * a clipped right-hand column the moment they make it narrower. Rows are the
 * only fixed quantity, since a row has to match the font.
 */

#define TOPBAR_H  (BTN_H + 2 * PAD)     /* tabs + search                     */
#define TITLE_H   BTN_H                 /* what is selected, per pane        */
#define HDR_H     BTN_H                 /* column names                      */
#define HDR_Y     (TOPBAR_H + TITLE_H)
#define LIST_Y    (HDR_Y + HDR_H)

/* The top bar's controls, one place, so drawing and clicking cannot drift
 * apart. Widths come from the labels so they survive rescaling. */
static void topbar_cols(int *b1x, int *b1w, int *b2x, int *b2w,
                        int *box_x, int *box_w,
                        int *sv_x, int *sv_w, int *ld_x, int *ld_w,
                        int *df_x, int *df_w)
{
    *b1x = PAD;              *b1w = 9 * CW;   /* "BY CARD" + a cell each side */
    *b2x = *b1x + *b1w + PAD; *b2w = 12 * CW; /* "BY DUELIST"                 */
    const int lx = *b2x + *b2w + 2 * PAD;     /* "Search" label               */
    *box_x = lx + 7 * CW;
    /* From the right: the mod indicator keeps its line (24 cells covers
     * "DROP MISSING CARDS: OFF"), then the view-dependent slot (DEFAULTS or
     * ALL CPU), LOAD and SAVE, then the search box takes what is left. */
    const int right = s_w - 25 * CW - PAD;
    *df_w = 10 * CW;
    *df_x = right - *df_w - PAD;
    *ld_w = 6 * CW;
    *ld_x = *df_x - *ld_w - PAD;
    *sv_w = 7 * CW;
    *sv_x = *ld_x - *sv_w - PAD;
    *box_w = *sv_x - PAD - *box_x;
    if (*box_w < 12 * CW) *box_w = 12 * CW;
}

/* The two views want the space split differently: a card list needs room for
 * long names AND two numeric columns, while a duelist list is short names and
 * one count. Splitting the same way for both starved whichever pane was not
 * being looked at. */
static int split_x(void)
{
    int x = s_w * (s_view == VIEW_CARDS ? 60 : 38) / 100;
    const int min_right = 38 * CW;      /* name + rank + weight + chance */
    if (x < 30 * CW) x = 30 * CW;
    if (x > s_w - min_right) x = s_w - min_right;
    if (x < 0) x = 0;
    return x;
}

static int list_rows(void) { return (s_h - LIST_Y - PAD) / ROW_H; }

/* --- scrollbars -----------------------------------------------------------
 *
 * 722 cards is a lot of wheel. Each pane gets a real scrollbar in its right
 * gutter: proportional thumb, draggable, and the track pages on click. Drawn
 * only when the content overflows, same rule as the N-M OF K indicator.
 */
static int s_sb_drag;              /* 0 none, 1 left pane, 2 right pane */
static int s_sb_grab;              /* pointer offset inside the thumb */

static int pane_total(int pane)
{
    if (pane == 0) return (s_view == VIEW_CARDS) ? s_order_n : NDUEL;
    return s_rows_n;
}

static int *pane_scroll(int pane)
{
    return pane == 0 ? &s_scroll : &s_scroll_right;
}

/* Geometry of one pane's scrollbar; 0 when everything fits. */
static int sb_geom(int pane, int *x0, int *w, int *ty, int *th,
                   int *thumb_y, int *thumb_h)
{
    const int total = pane_total(pane), page = list_rows();
    if (total <= page || page <= 0) return 0;
    *w  = 3 * TS;
    *x0 = (pane == 0 ? split_x() : s_w) - *w - TS;
    *ty = LIST_Y;
    *th = s_h - LIST_Y - PAD;
    int h = *th * page / total;
    if (h < 6 * TS) h = 6 * TS;
    if (h > *th) h = *th;
    *thumb_h = h;
    const int range = total - page;
    const int sc = *pane_scroll(pane);
    *thumb_y = *ty + (range > 0
                          ? (int)((long long)(*th - h) * sc / range) : 0);
    return 1;
}

static void set_scroll(int pane, int value)
{
    const int total = pane_total(pane), page = list_rows();
    if (value > total - page) value = total - page;
    if (value < 0) value = 0;
    if (*pane_scroll(pane) != value) {
        *pane_scroll(pane) = value;
        s_dirty = 1;
    }
}

/* A press on a scrollbar: thumb starts a drag, track pages. Returns 1 when
 * the press was consumed. */
static int sb_press(int x, int y)
{
    for (int pane = 0; pane < 2; pane++) {
        int x0, w, ty, th, tyv, hh;
        if (!sb_geom(pane, &x0, &w, &ty, &th, &tyv, &hh)) continue;
        if (x < x0 - TS || x >= x0 + w + TS || y < ty || y >= ty + th)
            continue;                      /* a whisker of slack either side */
        if (y >= tyv && y < tyv + hh) {
            s_sb_drag = pane + 1;
            s_sb_grab = y - tyv;
        } else {
            const int page = list_rows();
            set_scroll(pane, *pane_scroll(pane)
                                 + (y < tyv ? -page : page));
        }
        s_dirty = 1;
        return 1;
    }
    return 0;
}

static void sb_drag_to(int y)
{
    const int pane = s_sb_drag - 1;
    int x0, w, ty, th, tyv, hh;
    if (pane < 0 || !sb_geom(pane, &x0, &w, &ty, &th, &tyv, &hh)) return;
    const int total = pane_total(pane), page = list_rows();
    const int span = th - hh;
    if (span <= 0) return;
    int top = y - s_sb_grab - ty;
    if (top < 0) top = 0;
    if (top > span) top = span;
    set_scroll(pane, (int)(((long long)top * (total - page) + span / 2)
                           / span));
}

static void pct(char *out, unsigned cap, int weight)
{
    /* Weights are out of 2048 by construction, checked at bake time, so this
     * is the real chance and not an approximation of one. */
    const int hundredths = (weight * 10000 + PSX_DROP_DB_TOTAL / 2)
                           / PSX_DROP_DB_TOTAL;
    snprintf(out, cap, "%d.%02d%%", hundredths / 100, hundredths % 100);
}

static void draw_button(int x, int y, int w, const char *label, int on)
{
    px_fill(x, y, w, BTN_H, on ? C_ACCENT : C_HEADER);
    px_text(x + (w - text_w(label)) / 2, y + TXT_DY, x + w,
            on ? C_BG : C_TEXT, label);
}

static void draw_topbar(void)
{
    int b1x, b1w, b2x, b2w, bx, bw, svx, svw, ldx, ldw, dfx, dfw;
    topbar_cols(&b1x, &b1w, &b2x, &b2w, &bx, &bw, &svx, &svw,
                &ldx, &ldw, &dfx, &dfw);

    px_fill(0, 0, s_w, TOPBAR_H, C_PANEL);
    px_fill(0, TOPBAR_H - 1, s_w, 1, C_LINE);
    draw_button(b1x, PAD, b1w, "BY CARD", s_view == VIEW_CARDS);
    draw_button(b2x, PAD, b2w, "BY DUELIST", s_view == VIEW_DUELISTS);

    /* SAVE carries the unsaved-changes star; LOAD opens the drop_tables
     * share menu. The last slot is view-dependent: DEFAULTS scopes to the
     * BY DUELIST selection, ALL CPU pads the BY CARD droppers list out to
     * the whole roster for drag-and-drop. */
    const int dirty = psx_drop_edits_dirty();
    draw_button(svx, PAD, svw, dirty ? "SAVE*" : "SAVE", dirty);
    draw_button(ldx, PAD, ldw, "LOAD", 0);
    if (s_view == VIEW_DUELISTS)
        draw_button(dfx, PAD, dfw, "DEFAULTS", 0);
    else
        draw_button(dfx, PAD, dfw, "ALL CPU", s_all_cpu);

    px_text(b2x + b2w + 2 * PAD, PAD + TXT_DY, s_w, C_DIM, "Search");
    px_fill(bx, PAD, bw, BTN_H, C_BG);
    px_fill(bx, PAD + BTN_H - 1, bw, 1, C_LINE);
    px_text(bx + 3 * TS, PAD + TXT_DY, bx + bw - 3 * TS,
            s_search[0] ? C_TEXT : C_DIM,
            s_search[0] ? s_search : "type to filter");
    /* The box always has key focus by design, so the caret is always there —
     * blinking, because a solid bar reads as a glyph. With nothing typed it
     * sits at the left edge, in front of the placeholder, which is exactly
     * where a focused empty field puts it. */
    if (s_caret_on)
        px_fill(bx + 3 * TS + text_w(s_search), PAD + 2 * TS, TS, 9 * TS,
                C_ACCENT);

    char buf[80];
    const int on = psx_drop_missing_enabled();
    snprintf(buf, sizeof buf, "DROP MISSING CARDS: %s", on ? "ON" : "OFF");
    px_text_right(s_w - PAD, PAD + TXT_DY, on ? C_ACCENT : C_DIM, buf);
}

/* Both panes carry a title strip naming what is selected, so the right pane's
 * heading never has to share a line with its column names. */
static void draw_titles(const char *left, const char *right)
{
    const int sx = split_x();
    const int page = list_rows();
    char rng[32];
    px_fill(0, TOPBAR_H, s_w, TITLE_H, C_PANEL);
    px_fill(0, HDR_Y, s_w, HDR_H, C_HEADER);
    px_fill(sx, TOPBAR_H, 1, s_h - TOPBAR_H, C_LINE);

    /* Each pane's right edge carries at most ONE piece of side text — the
     * status line ("SAVED", "EDIT REFUSED: ...") while it lives, else the
     * N-M OF K scroll position — and the pane title is CLIPPED short of it,
     * because a long card name drawn under the side text is soup. */
    const char *lside = NULL, *rside = NULL;
    uint32_t rside_color = C_DIM;
    const int ltot = (s_view == VIEW_CARDS) ? s_order_n : NDUEL;
    if (ltot > page) {
        const int hi = (s_scroll + page < ltot) ? s_scroll + page : ltot;
        snprintf(rng, sizeof rng, "%d-%d OF %d", s_scroll + 1, hi, ltot);
        lside = rng;
    }
    char rrng[32];
    if (s_msg[0] && SDL_GetTicks() < s_msg_until) {
        rside = s_msg;
        rside_color = C_ACCENT;
    } else if (s_rows_n > page) {
        const int hi = (s_scroll_right + page < s_rows_n)
                           ? s_scroll_right + page : s_rows_n;
        snprintf(rrng, sizeof rrng, "%d-%d OF %d",
                 s_scroll_right + 1, hi, s_rows_n);
        rside = rrng;
    }
    const int lclip = lside ? sx - 2 * PAD - text_w(lside) : sx - PAD;
    const int rclip = rside ? s_w - 2 * PAD - text_w(rside) : s_w - PAD;
    px_text(PAD, TOPBAR_H + TXT_DY, lclip, C_DIM, left);
    px_text(sx + PAD, TOPBAR_H + TXT_DY, rclip, C_ACCENT, right);
    if (lside) px_text_right(sx - PAD, TOPBAR_H + TXT_DY, C_DIM, lside);
    if (rside) px_text_right(s_w - PAD, TOPBAR_H + TXT_DY, rside_color, rside);
}

/* A sortable column header, with the direction marker when it is the active
 * key. `hot`/`desc` are passed in rather than read from s_sort, because three
 * different sorts now own headers (card list, duelist list, right pane). */
static void draw_col_header(int x, int y, int right, const char *label,
                            int hot, int desc)
{
    const int w = px_text(x, y, right, hot ? C_ACCENT : C_DIM, label);
    if (hot) px_text(w + 2 * TS, y, right, C_ACCENT, desc ? "v" : "^");
}

static void draw_col_header_right(int right, int y, const char *label,
                                  int hot, int desc)
{
    char buf[16];
    if (hot) snprintf(buf, sizeof buf, "%s%s", label, desc ? "v" : "^");
    else     snprintf(buf, sizeof buf, "%s", label);
    px_text_right(right, y, hot ? C_ACCENT : C_DIM, buf);
}

/* Column edges of the card list, from the pane width. NAME takes whatever is
 * left after the fixed-width columns. TYPE gets 13 cells — "Beast-Warrior",
 * the longest name in the game's own table. */
static void card_cols(int sx, int *id_r, int *name_r, int *type_r, int *atk_r,
                      int *def_r, int *drop_r)
{
    *drop_r = sx - PAD;
    *def_r  = *drop_r - 6 * CW - PAD;
    *atk_r  = *def_r - 5 * CW - PAD;
    *type_r = *atk_r - 5 * CW - PAD;
    *name_r = *type_r - 13 * CW - PAD;
    *id_r   = PAD + 3 * CW;
    if (*name_r < *id_r + 6 * CW) *name_r = *id_r + 6 * CW;
}

/* Column edges of the drop list. */
/* Laid out from the right, because the three right-hand columns have known
 * widths ("S/A POW", four digits, "100.00%") and the name should get whatever
 * is left rather than a guess. */
static void drop_cols(int sx, int *rank_x, int *weight_r, int *chance_r)
{
    *chance_r = s_w - PAD;
    *weight_r = *chance_r - 8 * CW;          /* 8 cells hold "100.00%"      */
    /* weight_r is the weight's RIGHT edge, so the rank has to clear the five
     * cells the number occupies as well as its own eight. Computing rank_x
     * from weight_r alone put "S/A POW" underneath the digits. */
    *rank_x   = *weight_r - 5 * CW - PAD - 8 * CW;
    if (*rank_x < sx + PAD + 8 * CW) *rank_x = sx + PAD + 8 * CW;
}

static void draw_drop_rows(int sx, int name_of_card)
{
    int rank_x, weight_r, chance_r;
    drop_cols(sx, &rank_x, &weight_r, &chance_r);
    px_text(sx + PAD + (name_of_card ? 0 : ICON_PX + 4 * TS), HDR_Y + TXT_DY,
            rank_x, C_DIM, name_of_card ? "CARD" : "DUELIST");
    draw_col_header(rank_x, HDR_Y + TXT_DY, weight_r, "RANK",
                    s_rsort == RSORT_RANK, s_rdesc);
    draw_col_header_right(weight_r, HDR_Y + TXT_DY, "WEIGHT",
                          s_rsort == RSORT_WEIGHT, s_rdesc);
    draw_col_header_right(chance_r, HDR_Y + TXT_DY, "CHANCE",
                          s_rsort == RSORT_WEIGHT, s_rdesc);

    const int rows = list_rows();
    for (int r = 0; r < rows; r++) {
        const int i = s_scroll_right + r;
        if (i >= s_rows_n) break;
        const DropRow *d = &s_rows[i];
        const int y = LIST_Y + r * ROW_H;
        if (s_hover_pane == 1 && r == s_hover_row)
            px_fill(sx + 1, y - TS, s_w - sx - 1, ROW_H, C_HOVER);
        /* A grey row is the ALL CPU padding: this duelist does not drop the
         * card. It exists to be a drop target (and a quick-add on click), so
         * it draws dimmed with no numbers. */
        const int grey = d->tier < 0;
        if (name_of_card) {
            char idb[8];
            snprintf(idb, sizeof idb, "%d", d->card);
            px_text_right(sx + PAD + 4 * CW, y, C_DIM, idb);
            px_text(sx + PAD + 5 * CW, y, rank_x - PAD, C_TEXT,
                    psx_card_db_name(d->card));
        } else {
            draw_icon(sx + PAD, y - TS, d->duelist);
            px_text(sx + PAD + ICON_PX + 4 * TS, y, rank_x - PAD,
                    grey ? C_DIM : C_TEXT, PSX_DROP_DB[d->duelist].name);
        }
        if (grey) {
            px_text(rank_x, y, weight_r, C_HOT, "-");
            continue;
        }
        /* An edited (duelist, card) carries an accent tick at the pane edge,
         * so the player can see which rows are theirs. */
        if (psx_drop_edits_get(d->duelist, d->card, 0))
            px_fill(sx + 1, y - TS, TS, ROW_H, C_ACCENT);
        px_text(rank_x, y, weight_r, C_DIM, PSX_DROP_TIER_NAMES[d->tier]);
        char buf[16];
        if (i == s_edit_row) {
            /* The weight cell as a number box: typed digits and a caret. */
            const int ebx = weight_r - 5 * CW;
            px_fill(ebx - TS, y - TS, 5 * CW + 3 * TS, ROW_H, C_BG);
            px_fill(ebx - TS, y - TS + ROW_H - 1, 5 * CW + 3 * TS, 1, C_ACCENT);
            px_text(ebx, y, weight_r + 2 * TS, C_TEXT, s_edit_buf);
            if (s_caret_on)
                px_fill(ebx + text_w(s_edit_buf), y, TS, 8 * TS, C_ACCENT);
        } else {
            snprintf(buf, sizeof buf, "%d", d->weight);
            px_text_right(weight_r, y, C_DIM, buf);
        }
        pct(buf, sizeof buf, d->weight);
        px_text_right(chance_r, y, C_TEXT, buf);
    }
    if (!s_rows_n)
        px_text(sx + PAD, LIST_Y + 2 * TS, s_w - PAD, C_DIM,
                psx_drop_missing_enabled()
                    ? "Nothing here."
                    : "No duelist drops this. Try DROP MISSING CARDS.");
}

static void draw_cards_view(void)
{
    const int sx = split_x();
    char title[80];
    snprintf(title, sizeof title, "%d of %d cards", s_order_n, NCARDS);
    char sel[80];
    snprintf(sel, sizeof sel, "#%d  %s", s_sel_card,
             psx_card_db_name(s_sel_card));
    draw_titles(title, sel);

    int id_r, name_r, type_r, atk_r, def_r, drop_r;
    card_cols(sx, &id_r, &name_r, &type_r, &atk_r, &def_r, &drop_r);
    draw_col_header(PAD, HDR_Y + TXT_DY, id_r, "ID",
                    s_sort == SORT_ID, s_desc);
    draw_col_header(id_r + PAD, HDR_Y + TXT_DY, name_r, "NAME",
                    s_sort == SORT_NAME, s_desc);
    draw_col_header(name_r + PAD, HDR_Y + TXT_DY, type_r, "TYPE",
                    s_sort == SORT_TYPE, s_desc);
    draw_col_header_right(atk_r, HDR_Y + TXT_DY, "ATK",
                          s_sort == SORT_ATK, s_desc);
    draw_col_header_right(def_r, HDR_Y + TXT_DY, "DEF",
                          s_sort == SORT_DEF, s_desc);
    draw_col_header_right(drop_r, HDR_Y + TXT_DY, "DROPS",
                          s_sort == SORT_DROPS, s_desc);

    const int rows = list_rows();
    for (int r = 0; r < rows; r++) {
        const int i = s_scroll + r;
        if (i >= s_order_n) break;
        const int id = s_order[i];
        const int y = LIST_Y + r * ROW_H;
        if (id == s_sel_card) px_fill(0, y - TS, sx, ROW_H, C_HOT);
        else if (s_hover_pane == 0 && r == s_hover_row)
            px_fill(0, y - TS, sx, ROW_H, C_HOVER);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", id);
        px_text_right(id_r, y, C_DIM, buf);
        px_text(id_r + PAD, y, name_r, C_TEXT, psx_card_db_name(id));
        int atk = 0, def = 0, ty = 0;
        if (psx_card_db_stats(id, &atk, &def, &ty)) {
            px_text(name_r + PAD, y, type_r, C_DIM,
                    psx_card_db_type_name(ty));
            snprintf(buf, sizeof buf, "%d", atk);
            px_text_right(atk_r, y, C_TEXT, buf);
            snprintf(buf, sizeof buf, "%d", def);
            px_text_right(def_r, y, C_TEXT, buf);
        }
        snprintf(buf, sizeof buf, "%d", s_drop_count[id]);
        px_text_right(drop_r, y, s_drop_count[id] ? C_TEXT : C_DIM, buf);
    }
    draw_drop_rows(sx, 0);
}

static void draw_duelists_view(void)
{
    const int sx = split_x();
    char sel[80];
    const int ec = psx_drop_edits_count(s_sel_duelist);
    if (ec) snprintf(sel, sizeof sel, "%s  [%d EDIT%s]",
                     PSX_DROP_DB[s_sel_duelist].name, ec, ec == 1 ? "" : "S");
    else    snprintf(sel, sizeof sel, "%s", PSX_DROP_DB[s_sel_duelist].name);
    draw_titles("39 duelists", sel);

    draw_col_header(PAD + ICON_PX + 4 * TS, HDR_Y + TXT_DY, sx, "DUELIST",
                    s_dsort == DSORT_NAME, s_ddesc);
    draw_col_header_right(sx - PAD, HDR_Y + TXT_DY, "DROPS",
                          s_dsort == DSORT_DROPS, s_ddesc);

    const int rows = list_rows();
    for (int r = 0; r < rows; r++) {
        const int i = s_scroll + r;
        if (i >= NDUEL) break;
        const int d = s_duel_order[i];
        const int y = LIST_Y + r * ROW_H;
        if (d == s_sel_duelist) px_fill(0, y - TS, sx, ROW_H, C_HOT);
        else if (s_hover_pane == 0 && r == s_hover_row)
            px_fill(0, y - TS, sx, ROW_H, C_HOVER);
        draw_icon(PAD, y - TS, d);
        px_text(PAD + ICON_PX + 4 * TS, y, sx - 7 * CW, C_TEXT,
                PSX_DROP_DB[d].name);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", s_duel_total[d]);
        px_text_right(sx - PAD, y, C_DIM, buf);
    }
    draw_drop_rows(sx, 1);
}

static void draw_scrollbar(int pane)
{
    int x0, w, ty, th, tyv, hh;
    if (!sb_geom(pane, &x0, &w, &ty, &th, &tyv, &hh)) return;
    px_fill(x0, ty, w, th, C_HEADER);
    px_fill(x0, tyv, w, hh, s_sb_drag == pane + 1 ? C_ACCENT : C_DIM);
}

static void draw_cmenu(void)
{
    if (!s_cmenu_n) return;
    int mx, my, mw, mh;
    cmenu_geom(&mx, &my, &mw, &mh);
    px_fill(mx - 1, my - 1, mw + 2, mh + 2, C_LINE);
    px_fill(mx, my, mw, mh, C_PANEL);
    for (int i = 0; i < s_cmenu_n; i++) {
        const int y = my + TS + i * BTN_H;
        if (i == s_cmenu_hover) px_fill(mx, y, mw, BTN_H, C_HOT);
        px_text(mx + PAD, y + TXT_DY, mx + mw - PAD,
                i == s_cmenu_hover ? C_TEXT : C_DIM, s_cmenu[i].label);
    }
}

static void draw_ghost(void)
{
    char buf[44];
    if (!s_drag_live) return;
    if (s_drag_kind == DRAG_CARD) {
        snprintf(buf, sizeof buf, "%d %.24s", s_drag_card,
                 psx_card_db_name(s_drag_card));
    } else if (s_drag_row >= 0 && s_drag_row < s_rows_n) {
        snprintf(buf, sizeof buf, "%.20s - DROP OUTSIDE TO REMOVE",
                 psx_card_db_name(s_rows[s_drag_row].card));
    } else {
        return;
    }
    const int w = text_w(buf) + 2 * PAD;
    int x = s_mouse_x + 2 * TS, y = s_mouse_y + 2 * TS;
    if (x + w > s_w) x = s_w - w;
    if (y + BTN_H > s_h) y = s_h - BTN_H;
    px_fill(x, y, w, BTN_H, C_ACCENT);
    px_text(x + PAD, y + TXT_DY, x + w - PAD, C_BG, buf);
}

static void draw(void)
{
    px_fill(0, 0, s_w, s_h, C_BG);
    draw_topbar();
    if (!psx_card_db_ready()) {
        px_text(PAD, TOPBAR_H + PAD, s_w, C_DIM,
                "Waiting for the game to load its card table...");
        return;
    }
    if (s_view == VIEW_CARDS) draw_cards_view();
    else                      draw_duelists_view();
    draw_scrollbar(0);
    draw_scrollbar(1);
    draw_cmenu();
    draw_ghost();
}

/* --- input --------------------------------------------------------------- */

static void set_sort(int col)
{
    if (s_sort == col) s_desc = !s_desc;
    else { s_sort = col; s_desc = (col == SORT_DROPS); }
    rebuild_order();
    s_dirty = 1;
}

static void set_rsort(int col)
{
    if (s_rsort == col) s_rdesc = !s_rdesc;
    else { s_rsort = col; s_rdesc = (col == RSORT_WEIGHT); }
    rebuild_rows();
    s_dirty = 1;
}

/* Roster order has no header of its own, so each header's cycle returns to
 * it: default direction, flipped, roster again. */
static void set_dsort(int col)
{
    const int def = (col == DSORT_DROPS);   /* biggest pile first */
    if (s_dsort != col)          { s_dsort = col; s_ddesc = def; }
    else if (s_ddesc == def)     s_ddesc = !def;
    else                         { s_dsort = DSORT_ROSTER; s_ddesc = 0; }
    rebuild_duel_order();
    s_dirty = 1;
}

static void click(int x, int y)
{
    const int sx = split_x();
    /* A click lands somewhere else: whatever number box was open is done.
     * (Clicking a weight cell reopens one right after.) */
    edit_end();
    if (y < TOPBAR_H) {
        int b1x, b1w, b2x, b2w, bx, bw, svx, svw, ldx, ldw, dfx, dfw;
        topbar_cols(&b1x, &b1w, &b2x, &b2w, &bx, &bw, &svx, &svw,
                    &ldx, &ldw, &dfx, &dfw);
        if (y >= PAD && y < PAD + BTN_H) {
            if (x >= b1x && x < b1x + b1w && s_view != VIEW_CARDS) {
                s_view = VIEW_CARDS; s_scroll = 0; s_scroll_right = 0;
                rebuild_rows(); s_dirty = 1;
            } else if (x >= b2x && x < b2x + b2w
                       && s_view != VIEW_DUELISTS) {
                s_view = VIEW_DUELISTS; s_scroll = 0; s_scroll_right = 0;
                rebuild_rows(); s_dirty = 1;
            } else if (x >= svx && x < svx + svw) {
                say(psx_drop_edits_save() ? "SAVED" : "SAVE FAILED");
            } else if (x >= ldx && x < ldx + ldw) {
                open_load_menu(ldx, TOPBAR_H);
            } else if (s_view == VIEW_DUELISTS && x >= dfx && x < dfx + dfw) {
                /* RETURN TO DEFAULT, scoped to the duelist on screen. The
                 * default is whatever the layers underneath produce: stock,
                 * plus the mod when its row is on. */
                if (psx_drop_edits_clear(s_sel_duelist)) {
                    invalidate();
                    say("EDITS CLEARED - SAVE TO KEEP");
                } else {
                    say("NO EDITS FOR THIS DUELIST");
                }
            } else if (s_view == VIEW_CARDS && x >= dfx && x < dfx + dfw) {
                s_all_cpu = !s_all_cpu;
                s_scroll_right = 0;
                rebuild_rows();
                s_dirty = 1;
            }
        }
        return;
    }
    if (y >= HDR_Y && y < LIST_Y) {
        if (x >= sx) {
            /* RANK is left-aligned at rank_x, seven cells wide; everything
             * right of it is the weight in one scaling or another. */
            int rank_x, weight_r, chance_r;
            drop_cols(sx, &rank_x, &weight_r, &chance_r);
            if (x >= rank_x)
                set_rsort(x <= rank_x + 7 * CW ? RSORT_RANK : RSORT_WEIGHT);
        } else if (s_view == VIEW_CARDS) {
            int id_r, name_r, type_r, atk_r, def_r, drop_r;
            card_cols(sx, &id_r, &name_r, &type_r, &atk_r, &def_r, &drop_r);
            if (x <= id_r)        set_sort(SORT_ID);
            else if (x <= name_r) set_sort(SORT_NAME);
            else if (x <= type_r) set_sort(SORT_TYPE);
            else if (x <= atk_r)  set_sort(SORT_ATK);
            else if (x <= def_r)  set_sort(SORT_DEF);
            else                  set_sort(SORT_DROPS);
        } else {
            set_dsort(x <= sx - PAD - 6 * CW ? DSORT_NAME : DSORT_DROPS);
        }
        return;
    }
    if (y < LIST_Y) return;
    const int r = (y - LIST_Y) / ROW_H;
    if (r >= list_rows()) return;
    if (x < sx) {
        if (s_view == VIEW_CARDS) {
            const int i = s_scroll + r;
            if (i >= 0 && i < s_order_n) {
                s_sel_card = s_order[i];
                s_scroll_right = 0;
                rebuild_rows(); s_dirty = 1;
                /* the press may become a drag: card -> a duelist adds it */
                s_drag_kind = DRAG_CARD;
                s_drag_card = s_sel_card;
            }
        } else {
            const int i = s_scroll + r;
            if (i >= 0 && i < NDUEL) {
                s_sel_duelist = s_duel_order[i];
                s_scroll_right = 0;
                rebuild_rows(); s_dirty = 1;
            }
        }
        return;
    }
    /* Right-pane rows act on RELEASE (right_row_click), so a press can grow
     * into a drag instead; here it only arms one. */
    const int i = s_scroll_right + r;
    if (i >= 0 && i < s_rows_n) {
        s_drag_kind = DRAG_ROW;
        s_drag_row = i;
    }
}

/* The deferred right-pane row action, for a press that never became a drag:
 * rank cycles the band, weight (or its chance twin) opens the number box,
 * the name crosses over to the other view — how you follow "who drops this"
 * into "what else do they drop" without hunting for the name. */
static void right_row_click(int x, int y)
{
    const int sx = split_x();
    if (y < LIST_Y || x < sx) return;
    const int r = (y - LIST_Y) / ROW_H;
    const int i = s_scroll_right + r;
    if (r >= list_rows() || i < 0 || i >= s_rows_n) return;
    /* A grey ALL CPU row has no cells to act on — adds happen by DRAG or
     * right-click there, and a plain click falls through to the cross-jump
     * like the name of any other row. */
    if (s_rows[i].tier >= 0) {
        int rank_x, weight_r, chance_r;
        drop_cols(sx, &rank_x, &weight_r, &chance_r);
        if (x >= rank_x && x <= rank_x + 7 * CW) { cycle_rank(i); return; }
        if (x > weight_r - 5 * CW) { edit_begin(i); return; }
    }
    if (s_view == VIEW_CARDS) {
        s_view = VIEW_DUELISTS;
        s_sel_duelist = s_rows[i].duelist;
        s_scroll = duel_pos(s_sel_duelist) - list_rows() / 2;
        if (s_scroll < 0) s_scroll = 0;
    } else {
        s_view = VIEW_CARDS;
        s_sel_card = s_rows[i].card;
        s_search[0] = '\0';
        rebuild_order();
        for (int k = 0; k < s_order_n; k++)
            if (s_order[k] == s_sel_card) {
                s_scroll = k - list_rows() / 2;
                if (s_scroll < 0) s_scroll = 0;
                break;
            }
    }
    s_scroll_right = 0;
    rebuild_rows();
    s_dirty = 1;
}

/* Right-click: the context menu for whatever is under the pointer. */
static void rclick(int x, int y)
{
    edit_end();
    cmenu_close();
    const int sx = split_x();
    if (y < LIST_Y) return;
    const int r = (y - LIST_Y) / ROW_H;
    if (r < 0 || r >= list_rows()) return;
    char buf[44];
    s_cmenu_x = x; s_cmenu_y = y; s_cmenu_n = 0; s_cmenu_hover = -1;
    if (x >= sx) {
        const int i = s_scroll_right + r;
        if (i >= 0 && i < s_rows_n && s_rows[i].tier < 0) {
            /* grey ALL CPU row: the duelist does not drop the card yet */
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "ADD TO %.18s - %s",
                         PSX_DROP_DB[s_rows[i].duelist].name,
                         PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, s_rows[i].duelist, s_rows[i].card, t);
            }
        } else if (i >= 0 && i < s_rows_n) {
            cmenu_add("EDIT WEIGHT", CM_EDIT_WEIGHT, i, 0, 0);
            for (int t = 0; t < NTIER; t++) {
                if (t == s_rows[i].tier) continue;
                snprintf(buf, sizeof buf, "MOVE TO %s",
                         PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_MOVE_BAND, i, t, 0);
            }
            cmenu_add("REMOVE FROM BAND", CM_REMOVE, i, 0, 0);
        } else if (s_view == VIEW_DUELISTS) {
            /* Empty space in a duelist's table: offer to add the card that
             * is selected in the BY CARD view. */
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "ADD %d %.16s - %s", s_sel_card,
                         psx_card_db_name(s_sel_card), PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, s_sel_duelist, s_sel_card, t);
            }
        }
    } else if (s_view == VIEW_CARDS) {
        const int i = s_scroll + r;
        if (i >= 0 && i < s_order_n) {
            /* Right-clicking a card selects it, same as a click would. */
            s_sel_card = s_order[i];
            s_scroll_right = 0;
            rebuild_rows();
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "ADD TO %.18s - %s",
                         PSX_DROP_DB[s_sel_duelist].name,
                         PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, s_sel_duelist, s_sel_card, t);
            }
        }
    } else {
        const int i = s_scroll + r;
        if (i >= 0 && i < NDUEL) {
            const int d = s_duel_order[i];
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "ADD %d %.16s - %s", s_sel_card,
                         psx_card_db_name(s_sel_card), PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, d, s_sel_card, t);
            }
        }
    }
    if (s_cmenu_n) s_dirty = 1;
}

/* Where a live drag ends. A card dropped on a duelist (or one of their drop
 * rows, which also names the band) is an add; a table row dropped anywhere
 * outside the right pane is a removal; anything else cancels. */
static void drop_at(int x, int y)
{
    const int sx = split_x();
    if (s_drag_kind == DRAG_CARD) {
        if (y >= LIST_Y) {
            const int r = (y - LIST_Y) / ROW_H;
            if (r >= 0 && r < list_rows()) {
                if (x >= sx) {
                    const int i = s_scroll_right + r;
                    if (s_view == VIEW_CARDS && i >= 0 && i < s_rows_n)
                        (void)add_card(s_rows[i].duelist, s_drag_card,
                                       s_rows[i].tier < 0
                                           ? 0 : s_rows[i].tier);
                    else if (s_view == VIEW_DUELISTS)
                        (void)add_card(s_sel_duelist, s_drag_card,
                                       (i >= 0 && i < s_rows_n)
                                           ? s_rows[i].tier : 0);
                } else if (s_view == VIEW_DUELISTS) {
                    const int i = s_scroll + r;
                    if (i >= 0 && i < NDUEL)
                        (void)add_card(s_duel_order[i], s_drag_card, 0);
                }
            }
        }
    } else if (s_drag_kind == DRAG_ROW) {
        if (x < sx || y < LIST_Y) remove_row_band(s_drag_row);
    }
    s_dirty = 1;
}

/* While dragging a card, hovering a view tab switches views — the way to
 * carry a card from the BY CARD list onto any duelist in the other view. */
static void spring_tabs(int x, int y)
{
    if (s_drag_kind != DRAG_CARD || y >= TOPBAR_H) return;
    int b1x, b1w, b2x, b2w, bx, bw, svx, svw, ldx, ldw, dfx, dfw;
    topbar_cols(&b1x, &b1w, &b2x, &b2w, &bx, &bw, &svx, &svw,
                &ldx, &ldw, &dfx, &dfw);
    int want = -1;
    if (x >= b1x && x < b1x + b1w) want = VIEW_CARDS;
    if (x >= b2x && x < b2x + b2w) want = VIEW_DUELISTS;
    if (want >= 0 && want != s_view) {
        s_view = want;
        s_scroll = 0;
        s_scroll_right = 0;
        rebuild_rows();
        s_dirty = 1;
    }
}

static void scroll_by(int x, int amount)
{
    const int sx = split_x();
    int *s = (x < sx) ? &s_scroll : &s_scroll_right;
    int n = (x < sx)
                ? (s_view == VIEW_CARDS ? s_order_n : NDUEL)
                : s_rows_n;
    const int page = list_rows();
    *s += amount;
    if (*s > n - page) *s = n - page;
    if (*s < 0) *s = 0;
    s_dirty = 1;
}

static void type(char ch)
{
    const size_t n = strlen(s_search);
    if (n + 1 >= sizeof s_search) return;
    s_search[n] = ch;
    s_search[n + 1] = '\0';
    s_scroll = 0;
    rebuild_order();
    s_dirty = 1;
}

/* Which pane and visible row the pointer is over. Only a change redraws, so
 * mouse motion over the same row costs two compares. */
static void hover_move(int x, int y)
{
    s_mouse_x = x;
    s_mouse_y = y;
    if (s_cmenu_n) {
        const int it = cmenu_item_at(x, y);
        if (it != s_cmenu_hover) { s_cmenu_hover = it; s_dirty = 1; }
        return;                     /* the menu owns hover while it is open */
    }
    int pane = -1, row = -1;
    if (y >= LIST_Y) {
        const int r = (y - LIST_Y) / ROW_H;
        if (r >= 0 && r < list_rows()) {
            pane = (x < split_x()) ? 0 : 1;
            row = r;
        }
    }
    if (pane != s_hover_pane || row != s_hover_row) {
        s_hover_pane = pane;
        s_hover_row = row;
        s_dirty = 1;
    }
}

static void hover_clear(void)
{
    if (s_hover_pane != -1 || s_hover_row != -1) {
        s_hover_pane = s_hover_row = -1;
        s_dirty = 1;
    }
}

/* EVERY event aimed at this window is consumed, whether or not the viewer has
 * a use for it. Anything that leaks falls through to the runtime, whose mouse
 * and keyboard paths feed the F10 menu without checking which window the
 * event came from — hovering the viewer over the game window used to hover
 * both at once. */
static int on_event(const void *evp)
{
    const SDL_Event *ev = (const SDL_Event *)evp;
    if (!s_win) return 0;
    const Uint32 id = SDL_GetWindowID(s_win);

    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN: {
        if (ev->button.windowID != id) return 0;
        const int x = (int)ev->button.x, y = (int)ev->button.y;
        if (ev->button.button == SDL_BUTTON_RIGHT) {
            rclick(x, y);
            return 1;
        }
        if (ev->button.button != SDL_BUTTON_LEFT) return 1;
        if (s_cmenu_n) {
            /* An open menu owns the press: run the item or dismiss. */
            cmenu_run(cmenu_item_at(x, y));
            cmenu_close();
            return 1;
        }
        s_down = 1;
        s_down_x = x;
        s_down_y = y;
        s_drag_kind = DRAG_NONE;
        s_drag_live = 0;
        if (sb_press(x, y)) return 1;
        click(x, y);
        return 1;
    }
    case SDL_MOUSEBUTTONUP: {
        if (ev->button.windowID != id) return 0;
        if (ev->button.button != SDL_BUTTON_LEFT) return 1;
        const int x = (int)ev->button.x, y = (int)ev->button.y;
        if (s_sb_drag) {
            s_sb_drag = 0;
            s_dirty = 1;
        } else if (s_drag_live) {
            drop_at(x, y);
        } else if (s_drag_kind == DRAG_ROW) {
            /* A press on a right-pane row that never grew into a drag is
             * that row's ordinary click. */
            right_row_click(s_down_x, s_down_y);
        }
        s_down = 0;
        s_drag_kind = DRAG_NONE;
        if (s_drag_live) { s_drag_live = 0; s_dirty = 1; }
        return 1;
    }
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        const int x = (int)ev->motion.x, y = (int)ev->motion.y;
        hover_move(x, y);
        if (s_sb_drag) {
            sb_drag_to(y);
        } else if (s_down && s_drag_kind != DRAG_NONE && !s_drag_live) {
            const int dx = x - s_down_x, dy = y - s_down_y;
            if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > ROW_H / 2) {
                s_drag_live = 1;
                s_dirty = 1;
            }
        }
        if (s_drag_live) {
            spring_tabs(x, y);
            s_dirty = 1;            /* the ghost follows the pointer */
        }
        return 1;
    }
    case SDL_WINDOWEVENT_ENTER:
        return ev->window.windowID == id;
    case SDL_WINDOWEVENT_LEAVE:
        if (ev->window.windowID != id) return 0;
        hover_clear();
        return 1;
    case SDL_KEYUP:
        return ev->key.windowID == id;
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
        /* Which PANE the wheel is over is all this needs. SDL3's wheel event
         * carries the pointer itself; the global mouse state is relative to
         * the FOCUS window, which during a hover-scroll can still be the game
         * window — that picked the wrong pane. */
#if defined(PSX_SDL3)
        const int mx = (int)ev->wheel.mouse_x;
#else
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
#endif
        scroll_by(mx, ev->wheel.y > 0 ? -3 : 3);
        return 1;
    }
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (key == SDLK_ESCAPE && (s_cmenu_n || s_drag_live)) {
            /* Escape backs out of the menu or cancels a drag first. */
            cmenu_close();
            s_drag_kind = DRAG_NONE;
            s_drag_live = 0;
            s_dirty = 1;
            return 1;
        }
        if (s_edit_row >= 0) {
            /* The number box swallows the keyboard while it is open. */
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                if (s_edit_len) commit_edit_weight(s_edit_row, atoi(s_edit_buf));
                edit_end();
            } else if (key == SDLK_ESCAPE) {
                edit_end();
            } else if (key == SDLK_BACKSPACE) {
                if (s_edit_len) s_edit_buf[--s_edit_len] = 0;
            }
            s_dirty = 1;
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            if (s_search[0]) { s_search[0] = '\0'; rebuild_order(); }
            else psx_drop_viewer_close();
        } else if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = '\0'; s_scroll = 0; rebuild_order(); }
        } else if (key == SDLK_TAB) {
            s_view = (s_view == VIEW_CARDS) ? VIEW_DUELISTS : VIEW_CARDS;
            s_scroll = s_scroll_right = 0;
            rebuild_rows();
        } else if (key == SDLK_PAGEUP) {
            scroll_by(0, -list_rows());
        } else if (key == SDLK_PAGEDOWN) {
            scroll_by(0, list_rows());
        }
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        for (const char *p = ev->text.text; *p; p++) {
            if (s_edit_row >= 0) {
                if (*p >= '0' && *p <= '9'
                    && s_edit_len + 1 < (int)sizeof(s_edit_buf)) {
                    s_edit_buf[s_edit_len++] = *p;
                    s_edit_buf[s_edit_len] = 0;
                    s_dirty = 1;
                }
            } else if ((unsigned char)*p >= 32u && (unsigned char)*p < 127u) {
                type(*p);
            }
        }
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_drop_viewer_close();
        return 1;
    case SDL_WINDOWEVENT_RESIZED:
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_EXPOSED:      /* compositor dropped our buffer */
    case SDL_WINDOWEVENT_SHOWN:
    case SDL_WINDOWEVENT_RESTORED:
        if (ev->window.windowID != id) return 0;
        s_dirty = 1;
        return 1;
    default:
        break;
    }
    return 0;
}

/* --- window lifecycle ---------------------------------------------------- */

static int ensure_canvas(int w, int h)
{
    if (w == s_w && h == s_h && s_px && s_tex) return 1;
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    free(s_px);
    s_px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!s_px) { s_w = s_h = 0; return 0; }
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_tex) { free(s_px); s_px = NULL; s_w = s_h = 0; return 0; }
    s_w = w; s_h = h;
    s_dirty = 1;
    return 1;
}

void psx_drop_viewer_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("Drop Table Manager",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("Drop viewer: no window", 2000); return; }
    /* The software renderer, on purpose. This window is a CPU canvas blitted
     * through one texture, so it gains nothing from the GPU -- and SDL's
     * GL-backed renderer, the default on Linux, makes its own context current
     * on every call and never gives the game's GL context back. With that the
     * manager strobed with game frames and the game window froze. Software
     * has no context to steal. */
    s_ren = psx_sdl_create_renderer_named(s_win, "software");
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    if (!s_ren) {
        SDL_DestroyWindow(s_win); s_win = NULL;
        host_osd_push("Drop viewer: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_drop_viewer_close(); return; }
    invalidate();
    SDL_StartTextInput(s_win);
}

void psx_drop_viewer_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_pane = s_hover_row = -1;
    s_cmenu_n = 0;
    s_cmenu_hover = -1;
    s_down = 0;
    s_drag_kind = DRAG_NONE;
    s_drag_live = 0;
    s_sb_drag = 0;
    s_edit_row = -1;
}

void psx_drop_viewer_toggle(void)
{
    if (s_win) psx_drop_viewer_close();
    else       psx_drop_viewer_open();
}

int psx_drop_viewer_is_open(void) { return s_win != NULL; }

/* Per frame. Redraws only when something changed, so an open viewer sitting
 * on a second monitor costs a comparison and a texture blit. */
static void tick(void)
{
    /* Requests from the debug server land here, because the window has to be
     * made and destroyed on this thread. */
    const int req = s_open_req;
    if (req) {
        s_open_req = 0;
        if (req > 0) psx_drop_viewer_open();
        else         psx_drop_viewer_close();
    }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) {
        if (!ensure_canvas(w, h)) { psx_drop_viewer_close(); return; }
    }
    /* Text scale follows the drawable: how many copies of the layout's
     * natural 740x410 fit. A drag onto the 4K display rescales the next
     * frame, the same path as any resize. */
    {
        int ts = s_w / 740 < s_h / 410 ? s_w / 740 : s_h / 410;
        if (ts < 1) ts = 1;
        if (ts > 5) ts = 5;
        if (ts != s_ts) { s_ts = ts; s_dirty = 1; }
    }
    /* The search caret blinks at the usual 2 Hz-ish; only the phase flip
     * redraws. */
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; s_dirty = 1; }
    }
    /* The card table appears part way through boot, the mod row can move at
     * any time, and the edit layer bumps its generation on every change (a
     * hand-edited ini reload included); each changes what is on screen. */
    static int last_ready = -1, last_mod = -1;
    static unsigned last_gen = 0, last_icon_gen = 0;
    const int ready = psx_card_db_ready();
    const int mod = psx_drop_missing_enabled();
    const unsigned gen = psx_drop_edits_generation();
    if (ready != last_ready || mod != last_mod || gen != last_gen) {
        last_ready = ready; last_mod = mod; last_gen = gen;
        if (ready) invalidate();
        s_dirty = 1;
    }
    /* Portraits can arrive WHILE the window is open — the icon cache fills
     * as the player browses FREE DUEL — and a redraw is all that needs. */
    const unsigned icon_gen = psx_duelist_icon_cache_generation();
    if (icon_gen != last_icon_gen) {
        last_icon_gen = icon_gen;
        s_dirty = 1;
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) {
        s_msg[0] = 0;
        s_dirty = 1;
    }
    /* Present only what changed. Every frame was fine on Windows, where a
     * present is a cheap flip, but on Wayland a present may wait on the
     * compositor's frame callback, which an occluded or minimised window does
     * not get -- and this runs on the game's thread. The canvas is complete
     * whenever it is drawn, and an expose asks for it again (below). */
    if (s_dirty) {
        draw();
        SDL_UpdateTexture(s_tex, NULL, s_px, s_w * 4);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
        s_dirty = 0;
    }
}

/* --- the row ------------------------------------------------------------- */

/* An ACTION, not a toggle. A toggle implies a setting that persists and can
 * be read back, and this is neither — the window is closed from its own title
 * bar or with Escape, which a menu row would then be out of step with. Opening
 * one that is already open raises it instead of making a second. */
static void row_activate(void)
{
    psx_drop_viewer_open();
}

void psx_drop_viewer_register_menu(void)
{
    (void)psx_video_menu_add_action(
        PSX_VM_MENU_VIEW, "Drop table manager",
        "View and manage drop tables",
        row_activate);
}

/* Set what the window is showing. Any field may be left out; -1 and NULL mean
 * "leave alone". Returns 0 only when the viewer is closed, because a caller
 * that wanted to look at something has learned something either way. */
int psx_drop_viewer_set(int view, int sort, int desc, int card, int duelist,
                        const char *search)
{
    if (!s_win) return 0;
    if (view >= 0) s_view = view ? VIEW_DUELISTS : VIEW_CARDS;
    if (sort >= 0) { s_sort = sort; }
    if (desc >= 0) s_desc = desc ? 1 : 0;
    if (search) {
        snprintf(s_search, sizeof s_search, "%s", search);
        s_scroll = 0;
    }
    if (card >= 1 && card <= NCARDS) s_sel_card = card;
    if (duelist >= 0 && duelist < NDUEL) s_sel_duelist = duelist;
    rebuild_order();
    rebuild_duel_order();
    rebuild_rows();
    /* Put the selection on screen, so a caller that set one can see it. */
    if (s_view == VIEW_CARDS) {
        for (int k = 0; k < s_order_n; k++)
            if (s_order[k] == s_sel_card) {
                s_scroll = k - list_rows() / 2;
                if (s_scroll < 0) s_scroll = 0;
                break;
            }
    } else {
        s_scroll = duel_pos(s_sel_duelist) - list_rows() / 2;
        if (s_scroll < 0) s_scroll = 0;
    }
    s_scroll_right = 0;
    s_dirty = 1;
    return 1;
}

void psx_drop_viewer_request_open(int open)
{
    s_open_req = open ? 1 : -1;
}

/* The injected events go through SDL's queue with this window's id, so they
 * run the exact path a physical mouse does — on_event's consumption included,
 * which is the part a direct call to click() would skip. SDL_PushEvent is
 * thread-safe, which also keeps the debug thread out of the view state. */
int psx_drop_viewer_inject_motion(int x, int y)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(s_win);
    ev.motion.x = x;
    ev.motion.y = y;
    return SDL_PushEvent(&ev) == 1;
}

static int inject_button(int x, int y, int button, int down)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = SDL_GetWindowID(s_win);
    ev.button.button = (Uint8)button;
#if defined(PSX_SDL3)
    ev.button.down = down ? true : false;
#else
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    return SDL_PushEvent(&ev) == 1;
}

/* A click is press AND release now that some actions run on release; a drag
 * is composed from press / moves / release explicitly. */
int psx_drop_viewer_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_drop_viewer_inject_motion(x, y);
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_drop_viewer_press(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_drop_viewer_inject_motion(x, y);
    return inject_button(x, y, button, 1);
}

int psx_drop_viewer_release(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_drop_viewer_inject_motion(x, y);
    return inject_button(x, y, button, 0);
}

int psx_drop_viewer_inject_key(int keycode)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.windowID = SDL_GetWindowID(s_win);
    ev.key.repeat = 0;
#if defined(PSX_SDL3)
    ev.key.down = true;
    ev.key.key = (SDL_Keycode)keycode;
    ev.key.scancode = SDL_GetScancodeFromKey((SDL_Keycode)keycode, NULL);
#else
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = (SDL_Keycode)keycode;
    ev.key.keysym.scancode = SDL_GetScancodeFromKey((SDL_Keycode)keycode);
#endif
    return SDL_PushEvent(&ev) == 1;
}

int psx_drop_viewer_inject_text(const char *text)
{
    /* SDL3's text event carries a pointer, so the payload has to outlive the
     * queue; a small ring covers a burst of injected commands. */
    static char ring[8][32];
    static unsigned ri;
    SDL_Event ev;
    if (!s_win || !text) return 0;
    char *b = ring[ri++ & 7u];
    snprintf(b, sizeof(ring[0]), "%s", text);
    SDL_zero(ev);
    ev.type = SDL_TEXTINPUT;
    ev.text.windowID = SDL_GetWindowID(s_win);
#if defined(PSX_SDL3)
    ev.text.text = b;
#else
    snprintf(ev.text.text, sizeof(ev.text.text), "%s", b);
#endif
    return SDL_PushEvent(&ev) == 1;
}

int psx_drop_viewer_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    return snprintf(out, cap,
        "\"open\":%d,\"view\":\"%s\",\"sort\":%d,\"desc\":%d,"
        "\"rsort\":%d,\"rdesc\":%d,\"dsort\":%d,\"ddesc\":%d,"
        "\"search\":\"%s\",\"cards_listed\":%d,\"rows\":%d,"
        "\"sel_card\":%d,\"sel_duelist\":%d,\"modded\":%d,\"ready\":%d,"
        "\"canvas\":[%d,%d],\"split_x\":%d,\"list_rows\":%d,\"ts\":%d,"
        "\"hover\":[%d,%d],\"edit_row\":%d,\"edit_buf\":\"%s\","
        "\"edits_dirty\":%d,\"msg\":\"%s\",\"menu\":%d,\"menu_hover\":%d,"
        "\"drag\":%d,\"drag_live\":%d,\"scroll\":[%d,%d],\"all_cpu\":%d",
        s_win != NULL, s_view == VIEW_CARDS ? "cards" : "duelists",
        s_sort, s_desc, s_rsort, s_rdesc, s_dsort, s_ddesc,
        s_search, s_order_n, s_rows_n,
        s_sel_card, s_sel_duelist, s_eff_modded, psx_card_db_ready(),
        s_w, s_h, s_win ? split_x() : 0, s_win ? list_rows() : 0, s_ts,
        s_hover_pane, s_hover_row, s_edit_row, s_edit_buf,
        psx_drop_edits_dirty(), s_msg, s_cmenu_n, s_cmenu_hover,
        s_drag_kind, s_drag_live, s_scroll, s_scroll_right, s_all_cpu);
}

PSX_MOD_CONSTRUCTOR(psx_drop_viewer_install)
{
    psx_drop_viewer_register_menu();
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
