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
 *
 * THE LOOK
 * --------
 * Drawn with the F10 menu's toolkit (psx_ui_font's Inter faces, psx_ui_draw's
 * antialiased rounded rects) in the menu's palette, the same way the Card
 * Manager is, so the three read as one program. Every size below is a DESIGN
 * UNIT — one 480th of the window height — so a maximized 4K window gets more
 * detail rather than bigger blocks, and one number moves the whole layout.
 * The 8x8 bitmap sheet this used to draw with is gone.
 */

#include "psx_drop_viewer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_tool_window.h"
#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_drop_db.h"
#include "psx_drop_edits.h"
#include "psx_drop_missing.h"
#include "psx_duelist_icon_cache.h"
#include "psx_duelist_icons.h"
#include "psx_duelist_portraits.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

/* --- look ---------------------------------------------------------------- */

#define WIN_W  1480
#define WIN_H   820

/* The F10 menu's palette, opaque where this window is opaque; the Card
 * Manager carries the same table. */
#define COL_BG        0xFF0F1219u
#define COL_BAR       0xFF141826u
#define COL_PANEL     0xFF1E2233u
#define COL_TEXT      0xFFC9CFDDu
#define COL_DIM       0xFF7C8598u
#define COL_ACCENT    0xFF7FA6FFu
#define COL_SEL_BG    0xFF2C3B60u
#define COL_HOVER     0x14FFFFFFu
#define COL_EDIT_BG   0xFF0E1119u
#define COL_EDITED    0xFF8BD48Bu   /* a value the player changed */
#define COL_BTN       0xFF2A3147u
#define COL_BTN_ON    0xFF3D5A9Cu
#define COL_TRACK     0x40FFFFFFu
#define COL_THUMB     0xFF7C8598u
#define COL_WARN      0xFFE8C36Au

/* Design units: window height / 480. */
#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_ROW_H     14.0f
#define U_TITLE_H   18.0f
#define U_HDR_H     16.0f
#define U_BTN_H     17.0f
#define U_FOOT_H    14.0f
#define U_SB_W       4.0f
#define U_ICON      12.0f          /* duelist portrait, square, inside a row */
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f

/* UTF-8 spellings of the glyphs the embedded Inter subset carries. */
#define S_UP     "\xE2\x86\x91"     /* ↑ */
#define S_DOWN   "\xE2\x86\x93"     /* ↓ */
#define S_DASH   "\xE2\x80\x93"     /* – */
#define S_ELLIP  "\xE2\x80\xA6"     /* … */

/* --- canvas -------------------------------------------------------------- */

static SDL_Window   *s_win;
static uint32_t     *s_px;
static int           s_w, s_h;
static int           s_dirty = 1;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;   /* design unit in pixels */

static int px(float u) { return (int)(u * s_u + 0.5f); }

static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }

/* Width of a string, 0 for a face that failed to bake — the layout then
 * degrades to overlapping columns instead of a crash. */
static int tw(const PsxUiFace *f, const char *s) { return f ? psx_ui_font_text_w(f, s) : 0; }
static int imax(int a, int b) { return a > b ? a : b; }

typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* Text inside a band, clipped to it. */
static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}

static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    const int w = tw(f, s);
    psx_ui_text(&s_cv, r->x + (r->w - w) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* Right-aligned text; returns its left edge. */
static int text_right(int right, int baseline, const char *s, uint32_t col, const PsxUiFace *f)
{
    const int x = right - tw(f, s);
    psx_ui_text(&s_cv, x, baseline, s, col, f);
    return x;
}

/* The duelist's FREE DUEL portrait in a rounded well. First choice is the
 * 48x48 tile decoded from the player's own disc (psx_duelist_portraits.c),
 * which every player has. Failing that, the compile-time bake or the
 * drawn-screen capture: those 38x38 icons carry transparent corners, and the
 * toolkit's scaled blit treats its source as opaque, so they are composed
 * over the row's own colour first. Nothing at all gets a plain plate. */
static void draw_icon(int x, int y, int size, int duelist, uint32_t bg)
{
    const uint32_t *disc = psx_duelist_portraits_get(duelist);
    if (disc) {
        psx_ui_blit_scaled(&s_cv, x, y, size, size, (float)px(2.5f),
                           disc, PSX_PORTRAIT_W, PSX_PORTRAIT_W);
        return;
    }
    const uint32_t *src = (duelist >= 0 && duelist < PSX_DUELIST_ICON_N)
                              ? PSX_DUELIST_ICONS[duelist] : 0;
    if (!src) src = psx_duelist_icon_cache_get(duelist);
    if (!src) {
        psx_ui_round_rect(&s_cv, x, y, size, size, (float)px(2.5f), COL_BTN);
        return;
    }
    static uint32_t tmp[PSX_DUELIST_ICON_W * PSX_DUELIST_ICON_H];
    for (int i = 0; i < PSX_DUELIST_ICON_W * PSX_DUELIST_ICON_H; i++)
        tmp[i] = (src[i] >> 24) ? (0xFF000000u | (src[i] & 0x00FFFFFFu)) : bg;
    psx_ui_blit_scaled(&s_cv, x, y, size, size, (float)px(2.5f),
                       tmp, PSX_DUELIST_ICON_W, PSX_DUELIST_ICON_H);
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
static int  s_hover_btn  = -1;   /* top-bar button under the pointer */

/* The search caret blinks; this is its current phase, flipped from tick. */
static int  s_caret_on = 1;

/* Weight-cell editing: which right-pane row (absolute index into s_rows) has
 * the number box open, and what has been typed into it. -1 = not editing. */
static int  s_edit_row = -1;
static char s_edit_buf[6];
static int  s_edit_len;

/* One-line status ("Saved", "Edit refused: ..."), shown in the right pane's
 * header until its deadline passes. */
static char     s_msg[80];
static uint32_t s_msg_until;

/* Right-click context menu (also the LOAD button's file list): a handful of
 * actions on whatever was under the pointer. One level, no submenus — band
 * choices are spelled out as items. */
enum { CM_NONE = 0, CM_ADD, CM_EDIT_WEIGHT, CM_MOVE_BAND, CM_REMOVE,
       CM_LOAD_FILE, CM_EXPORT };
#define CMENU_MAX 12
static struct {
    char label[64];
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

/* --- layout ---------------------------------------------------------------
 *
 * Everything is derived from the drawable size, because the window is the
 * player's to resize and drag between displays. Recomputed at the top of
 * every draw and every pointer event (it is a few dozen additions and some
 * cached text measurements), so drawing and hit-testing read the same
 * numbers and cannot drift apart.
 */

typedef struct {
    Rect bar, tab_cards, tab_duel, search, btn_save, btn_load, btn_third;
    int  mod_x;                     /* left edge of the mod indicator        */
    Rect pane[2];                   /* the two panels                        */
    Rect title[2];                  /* what is listed, per panel             */
    Rect cols[2];                   /* the column names                      */
    Rect rows[2];                   /* the rows                              */
    Rect sb[2];                     /* scrollbar tracks                      */
    int  row_h, nrows;
    int  foot_y, foot_h;            /* the help / unsaved line at the bottom */
    /* Left pane, BY CARD: each column's left and right edge. */
    int c_id_x, c_id_r, c_name_x, c_name_r, c_type_x, c_type_r,
        c_atk_x, c_atk_r, c_def_x, c_def_r, c_drop_x, c_drop_r;
    /* Left pane, BY DUELIST. */
    int d_icon_x, d_name_x, d_name_r, d_drops_x, d_drops_r;
    /* Right pane. */
    int r_dot_x, r_icon_x, r_id_x, r_id_r, r_name_x, r_name_r,
        r_rank_x, r_rank_r, r_weight_x, r_weight_r, r_chance_x, r_chance_r;
} Layout;
static Layout s_L;

static void layout_compute(void)
{
    Layout *L = &s_L;
    memset(L, 0, sizeof *L);
    const PsxUiFace *ft = face_title(), *fb = face_bold(), *fr = face_body(), *fs = face_small();
    const int gap = px(U_GAP), pad = px(U_PAD), cg = px(12.0f);

    /* Top bar: title, the two view tabs, the search box; from the right the
     * mod indicator, then the view-dependent button, Load and Save. */
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };
    const int bh = px(U_BTN_H), by = (L->bar.h - bh) / 2;
    int x = px(8.0f) + tw(ft, "Drop Table Manager") + px(14.0f);
    L->tab_cards = (Rect){ x, by, tw(fb, "By card") + px(18.0f), bh };
    x += L->tab_cards.w + px(4.0f);
    L->tab_duel = (Rect){ x, by, tw(fb, "By duelist") + px(18.0f), bh };
    x += L->tab_duel.w + px(12.0f);
    int rx = s_w - px(8.0f);
    L->mod_x = rx - tw(fs, "Drop missing cards: off");
    rx = L->mod_x - px(14.0f);
    int w = tw(fb, s_view == VIEW_DUELISTS ? "Defaults" : "All CPU") + px(18.0f);
    L->btn_third = (Rect){ rx - w, by, w, bh };  rx -= w + px(4.0f);
    w = tw(fb, "Load" S_ELLIP) + px(18.0f);
    L->btn_load = (Rect){ rx - w, by, w, bh };   rx -= w + px(4.0f);
    w = tw(fb, "Save") + px(18.0f);
    L->btn_save = (Rect){ rx - w, by, w, bh };   rx -= w + px(10.0f);
    w = rx - x;
    if (w > px(170.0f)) w = px(170.0f);
    if (w < px(50.0f))  w = px(50.0f);
    L->search = (Rect){ x, (L->bar.h - px(16.0f)) / 2, w, px(16.0f) };

    /* Two panels. The views want the space split differently: a card list
     * needs room for long names AND four numeric columns, a duelist list is
     * short names and one count. */
    const int top = L->bar.h + gap;
    L->foot_h = px(U_FOOT_H);
    L->foot_y = s_h - gap - L->foot_h;
    const int ph = L->foot_y - gap - top;
    const int avail = s_w - 3 * gap;
    int lw = avail * (s_view == VIEW_CARDS ? 60 : 38) / 100;
    if (lw > avail - px(260.0f)) lw = avail - px(260.0f);
    if (lw < px(200.0f)) lw = px(200.0f);
    if (lw > avail - px(40.0f)) lw = avail - px(40.0f);
    L->pane[0] = (Rect){ gap, top, lw, ph };
    L->pane[1] = (Rect){ gap + lw + gap, top, avail - lw, ph };
    L->row_h = px(U_ROW_H);
    for (int p = 0; p < 2; p++) {
        const Rect *P = &L->pane[p];
        L->title[p] = (Rect){ P->x + pad, P->y + px(3.0f), P->w - 2 * pad, px(U_TITLE_H) };
        L->cols[p]  = (Rect){ P->x + pad, L->title[p].y + L->title[p].h, P->w - 2 * pad, px(U_HDR_H) };
        const int ry = L->cols[p].y + L->cols[p].h;
        L->rows[p]  = (Rect){ P->x + px(4.0f), ry, P->w - px(4.0f) - px(10.0f), P->y + P->h - ry - px(4.0f) };
    }
    L->nrows = L->rows[0].h / L->row_h;
    if (L->nrows < 1) L->nrows = 1;
    for (int p = 0; p < 2; p++)
        L->sb[p] = (Rect){ L->pane[p].x + L->pane[p].w - px(8.0f), L->rows[p].y, px(U_SB_W), L->nrows * L->row_h };

    /* Left pane columns, from the right, each at its widest text; NAME takes
     * what is left. The type column fits "Beast-Warrior", the longest name in
     * the game's own table. */
    {
        const int x0 = L->rows[0].x + px(6.0f), xr = L->rows[0].x + L->rows[0].w - px(6.0f);
        int cw;
        L->c_id_x = x0;  L->c_id_r = x0 + tw(fr, "722");
        cw = imax(tw(fs, "Drops " S_DOWN), tw(fr, "39"));
        L->c_drop_r = xr;                    L->c_drop_x = L->c_drop_r - cw;
        cw = imax(tw(fs, "DEF " S_DOWN), tw(fr, "9999"));
        L->c_def_r = L->c_drop_x - cg;       L->c_def_x = L->c_def_r - cw;
        cw = imax(tw(fs, "ATK " S_DOWN), tw(fr, "9999"));
        L->c_atk_r = L->c_def_x - cg;        L->c_atk_x = L->c_atk_r - cw;
        cw = imax(tw(fs, "Type " S_DOWN), tw(fr, "Beast-Warrior"));
        L->c_type_r = L->c_atk_x - cg;       L->c_type_x = L->c_type_r - cw;
        L->c_name_x = L->c_id_r + px(10.0f); L->c_name_r = L->c_type_x - cg;
        if (L->c_name_r < L->c_name_x + px(40.0f)) L->c_name_r = L->c_name_x + px(40.0f);

        L->d_icon_x = x0;
        L->d_name_x = x0 + px(U_ICON) + px(8.0f);
        cw = imax(tw(fs, "Drops " S_DOWN), tw(fr, "999"));
        L->d_drops_r = xr;                   L->d_drops_x = xr - cw;
        L->d_name_r = L->d_drops_x - cg;
    }
    /* Right pane: an edited dot in the gutter, then the portrait or the id,
     * the name, and RANK / WEIGHT / CHANCE at their widest text. */
    {
        const int xr = L->rows[1].x + L->rows[1].w - px(6.0f);
        L->r_dot_x = L->rows[1].x + px(7.0f);
        const int cx = L->rows[1].x + px(14.0f);
        int cw;
        cw = imax(tw(fs, "Chance " S_DOWN), tw(fr, "100.00%"));
        L->r_chance_r = xr;                  L->r_chance_x = xr - cw;
        cw = imax(tw(fs, "Weight " S_DOWN), tw(fr, "2048"));
        L->r_weight_r = L->r_chance_x - cg;  L->r_weight_x = L->r_weight_r - cw;
        cw = imax(tw(fs, "Rank " S_DOWN), tw(fr, "S/A POW"));
        L->r_rank_r = L->r_weight_x - cg;    L->r_rank_x = L->r_rank_r - cw;
        L->r_icon_x = cx;
        L->r_id_x = cx;  L->r_id_r = cx + tw(fr, "722");
        L->r_name_x = (s_view == VIEW_CARDS) ? cx + px(U_ICON) + px(8.0f) : L->r_id_r + px(8.0f);
        L->r_name_r = L->r_rank_x - cg;
        if (L->r_name_r < L->r_name_x + px(40.0f)) L->r_name_r = L->r_name_x + px(40.0f);
    }
}

static int list_rows(void) { return s_L.nrows > 0 ? s_L.nrows : 1; }

/* Which panel the point is in, -1 for neither. */
static int pane_at(int x, int y)
{
    for (int p = 0; p < 2; p++)
        if (in_rect(&s_L.pane[p], x, y)) return p;
    return -1;
}

/* Which visible row of panel `p` the point is on, -1 for none. Any x inside
 * the panel counts, so a click in the margin beside a name still lands on
 * its row — the scrollbar is tested before this, by the caller. */
static int row_at(int p, int x, int y)
{
    if (p < 0) return -1;
    const Rect *P = &s_L.pane[p], *R = &s_L.rows[p];
    if (x < P->x || x >= P->x + P->w) return -1;
    if (y < R->y || y >= R->y + s_L.nrows * s_L.row_h) return -1;
    return (y - R->y) / s_L.row_h;
}

/* --- editing -------------------------------------------------------------- */

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 3500u;
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
        say("Edit refused: the edit table is full");
        return 0;
    }
    static uint16_t tmp[NCARDS];
    for (int t = 0; t < NTIER; t++) {
        const int rc = eff_tier(d, t, tmp);
        if (rc == 1 || rc == -1) continue;   /* transformed, or nothing to do */
        if (had) (void)psx_drop_edits_set(d, card, old);
        else     (void)psx_drop_edits_unset(d, card);
        say(rc == -4 ? "Edit refused: a band would exceed 1984"
                     : "Edit refused: that band cannot renormalize");
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
        say("Edit refused: the most a weight can be is 1984");
        return;
    }
    uint16_t v[NTIER];
    for (int t = 0; t < NTIER; t++) v[t] = s_eff[r->duelist][t][r->card - 1];
    v[r->tier] = (uint16_t)weight;
    if (commit_vector(r->duelist, r->card, v))
        say(weight ? "Edited. Save to keep it." : "Removed from the band. Save to keep it.");
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
        say("Moved to another band. Save to keep it.");
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
        say("Removed from the band. Save to keep it.");
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
        say("Already in that band. Edit its weight instead.");
        return 0;
    }
    v[band] = ADD_WEIGHT;
    if (!commit_vector(d, card, v)) return 0;
    say("Added at weight 20. Save to keep it.");
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

static int cmenu_row_h(void) { return s_L.row_h + px(2.0f); }

static Rect cmenu_rect(void)
{
    const PsxUiFace *fr = face_body();
    int wide = 0;
    for (int i = 0; i < s_cmenu_n; i++) wide = imax(wide, tw(fr, s_cmenu[i].label));
    Rect r = { s_cmenu_x, s_cmenu_y, wide + 2 * px(U_PAD) + px(6.0f), s_cmenu_n * cmenu_row_h() + px(6.0f) };
    if (r.w < px(150.0f)) r.w = px(150.0f);
    if (r.x + r.w > s_w - px(4.0f)) r.x = s_w - px(4.0f) - r.w;
    if (r.y + r.h > s_h - px(4.0f)) r.y = s_h - px(4.0f) - r.h;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

static int cmenu_item_at(int x, int y)
{
    if (!s_cmenu_n) return -1;
    const Rect r = cmenu_rect();
    if (!in_rect(&r, x, y)) return -1;
    const int i = (y - r.y - px(3.0f)) / cmenu_row_h();
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
            char m[80];
            snprintf(m, sizeof m, "Loaded %d entr%s. Save to keep them.", n, n == 1 ? "y" : "ies");
            say(m);
        } else {
            say("Load failed");
        }
        break;
    }
    case CM_EXPORT: {
        char name[64];
        if (psx_drop_edits_export(name, sizeof name)) {
            char m[80];
            snprintf(m, sizeof m, "Exported %.40s", name);
            say(m);
        } else {
            say("Export failed");
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
    cmenu_add("Export the current table to drop_tables", CM_EXPORT, 0, 0, 0);
    for (int i = 0; i < n; i++) {
        char label[64];
        snprintf(label, sizeof label, "Load %.56s", names[i]);
        cmenu_add_s(label, CM_LOAD_FILE, names[i]);
    }
    if (!n)
        cmenu_add("No .ini files in drop_tables yet", CM_NONE, 0, 0, 0);
    s_dirty = 1;
}

/* --- scrollbars -----------------------------------------------------------
 *
 * 722 cards is a lot of wheel. Each pane gets a real scrollbar in its right
 * gutter: proportional thumb, draggable, and the track pages on click. Drawn
 * only when the content overflows, same rule as the N-M of K indicator.
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

/* The thumb of one pane's scrollbar; 0 when everything fits. */
static int sb_thumb(int pane, int *thumb_y, int *thumb_h)
{
    const int total = pane_total(pane), page = list_rows();
    if (total <= page || page <= 0) return 0;
    const Rect *T = &s_L.sb[pane];
    int h = T->h * page / total;
    if (h < px(12.0f)) h = px(12.0f);
    if (h > T->h) h = T->h;
    *thumb_h = h;
    const int range = total - page;
    const int sc = *pane_scroll(pane);
    *thumb_y = T->y + (range > 0 ? (int)((long long)(T->h - h) * sc / range) : 0);
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
        int tyv, hh;
        if (!sb_thumb(pane, &tyv, &hh)) continue;
        const Rect *T = &s_L.sb[pane];
        const int slack = px(4.0f);            /* a whisker either side */
        if (x < T->x - slack || x >= T->x + T->w + slack || y < T->y || y >= T->y + T->h)
            continue;
        if (y >= tyv && y < tyv + hh) {
            s_sb_drag = pane + 1;
            s_sb_grab = y - tyv;
        } else {
            const int page = list_rows();
            set_scroll(pane, *pane_scroll(pane) + (y < tyv ? -page : page));
        }
        s_dirty = 1;
        return 1;
    }
    return 0;
}

static void sb_drag_to(int y)
{
    const int pane = s_sb_drag - 1;
    int tyv, hh;
    if (pane < 0 || !sb_thumb(pane, &tyv, &hh)) return;
    const Rect *T = &s_L.sb[pane];
    const int total = pane_total(pane), page = list_rows();
    const int span = T->h - hh;
    if (span <= 0) return;
    int top = y - s_sb_grab - T->y;
    if (top < 0) top = 0;
    if (top > span) top = span;
    set_scroll(pane, (int)(((long long)top * (total - page) + span / 2) / span));
}

static void pct(char *out, unsigned cap, int weight)
{
    /* Weights are out of 2048 by construction, checked at bake time, so this
     * is the real chance and not an approximation of one. */
    const int hundredths = (weight * 10000 + PSX_DROP_DB_TOTAL / 2)
                           / PSX_DROP_DB_TOTAL;
    snprintf(out, cap, "%d.%02d%%", hundredths / 100, hundredths % 100);
}

/* --- drawing -------------------------------------------------------------- */

static void draw_button(const Rect *r, const char *label, int primary, int hover)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, primary ? COL_BTN_ON : COL_BTN);
    if (hover) psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_HOVER);
    text_centered(r, label, COL_TEXT, face_bold());
}

static void draw_bar(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fr = face_body(), *fs = face_small();
    psx_ui_fill(&s_cv, 0, 0, s_w, L->bar.h, COL_BAR);
    psx_ui_fill(&s_cv, 0, L->bar.h - 1, s_w, 1, 0x40FFFFFFu);
    Rect t = { px(8.0f), 0, L->tab_cards.x - px(8.0f), L->bar.h };
    text_in(&t, 0, "Drop Table Manager", COL_ACCENT, ft);

    draw_button(&L->tab_cards, "By card", s_view == VIEW_CARDS, s_hover_btn == 0);
    draw_button(&L->tab_duel, "By duelist", s_view == VIEW_DUELISTS, s_hover_btn == 1);

    /* The search box always has key focus by design, so the caret is always
     * there — blinking, because a solid bar reads as a glyph. */
    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, L->search.h * 0.5f, COL_EDIT_BG);
    if (s_search[0]) text_in(&L->search, px(8.0f), s_search, COL_TEXT, fr);
    else             text_in(&L->search, px(8.0f), "Type to search" S_ELLIP, COL_DIM, fr);
    if (s_caret_on && s_edit_row < 0) {
        const int cx = L->search.x + px(8.0f) + (s_search[0] ? tw(fr, s_search) : 0) + 1;
        psx_ui_fill(&s_cv, cx, L->search.y + px(3.0f), imax(1, px(1.2f)), L->search.h - px(6.0f), COL_ACCENT);
    }

    /* Save is lit while there are unsaved edits; Load opens the drop_tables
     * share menu. The last slot is view-dependent: Defaults scopes to the
     * BY DUELIST selection, All CPU pads the BY CARD droppers list out to
     * the whole roster for drag-and-drop. */
    draw_button(&L->btn_save, "Save", psx_drop_edits_dirty(), s_hover_btn == 2);
    draw_button(&L->btn_load, "Load" S_ELLIP, 0, s_hover_btn == 3);
    if (s_view == VIEW_DUELISTS) draw_button(&L->btn_third, "Defaults", 0, s_hover_btn == 4);
    else                         draw_button(&L->btn_third, "All CPU", s_all_cpu, s_hover_btn == 4);

    const int on = psx_drop_missing_enabled();
    Rect m = { L->mod_x, 0, s_w - px(8.0f) - L->mod_x, L->bar.h };
    text_in(&m, 0, on ? "Drop missing cards: on" : "Drop missing cards: off", on ? COL_ACCENT : COL_DIM, fs);
}

/* A panel with its title band: what is listed on the left, and at most ONE
 * piece of side text on the right — the status line while it lives, else the
 * N-M of K scroll position. The title is CLIPPED short of the side text,
 * because a long card name drawn under it is soup. */
static void draw_panel(int p, const char *title, uint32_t title_col, const char *side, uint32_t side_col)
{
    const Layout *L = &s_L;
    const Rect *P = &L->pane[p], *T = &L->title[p];
    const PsxUiFace *fb = face_bold(), *fs = face_small();
    psx_ui_round_rect(&s_cv, P->x, P->y, P->w, P->h, (float)px(U_R_PANEL), COL_PANEL);
    int right = T->x + T->w;
    if (side && side[0]) {
        const int base = psx_ui_baseline_in(T->y, T->h, fs);
        right = text_right(T->x + T->w, base, side, side_col, fs) - px(10.0f);
    }
    psx_ui_text_clip(&s_cv, T->x, psx_ui_baseline_in(T->y, T->h, fb), title, title_col, fb, right - T->x);
}

/* A sortable column name, with the direction marker when it is the active
 * key. `hot`/`desc` are passed in rather than read from s_sort, because three
 * different sorts own headers (card list, duelist list, right pane). */
static void draw_col(int x, int right, int y, int h, const char *label, int hot, int desc, int align_right)
{
    const PsxUiFace *fs = face_small();
    char buf[24];
    if (hot) snprintf(buf, sizeof buf, "%s %s", label, desc ? S_DOWN : S_UP);
    else     snprintf(buf, sizeof buf, "%s", label);
    const int base = psx_ui_baseline_in(y, h, fs);
    const uint32_t col = hot ? COL_ACCENT : COL_DIM;
    if (align_right) text_right(right, base, buf, col, fs);
    else             psx_ui_text_clip(&s_cv, x, base, buf, col, fs, right - x);
}

static void draw_scrollbar(int pane)
{
    int tyv, hh;
    if (!sb_thumb(pane, &tyv, &hh)) return;
    const Rect *T = &s_L.sb[pane];
    psx_ui_round_rect(&s_cv, T->x, T->y, T->w, T->h, T->w * 0.5f, COL_TRACK);
    psx_ui_round_rect(&s_cv, T->x, tyv, T->w, hh, T->w * 0.5f, s_sb_drag == pane + 1 ? COL_ACCENT : COL_THUMB);
}

/* The right pane's rows: who drops the selected card, or what the selected
 * duelist drops. */
static void draw_drop_rows(int name_of_card)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body();
    const Rect *C = &L->cols[1], *R = &L->rows[1];
    if (name_of_card) {
        draw_col(L->r_id_x, L->r_id_r + px(4.0f), C->y, C->h, "ID", 0, 0, 0);
        draw_col(L->r_name_x, L->r_name_r, C->y, C->h, "Card", 0, 0, 0);
    } else {
        draw_col(L->r_name_x, L->r_name_r, C->y, C->h, "Duelist", 0, 0, 0);
    }
    draw_col(L->r_rank_x, L->r_rank_r, C->y, C->h, "Rank", s_rsort == RSORT_RANK, s_rdesc, 0);
    draw_col(L->r_weight_x, L->r_weight_r, C->y, C->h, "Weight", s_rsort == RSORT_WEIGHT, s_rdesc, 1);
    draw_col(L->r_chance_x, L->r_chance_r, C->y, C->h, "Chance", s_rsort == RSORT_WEIGHT, s_rdesc, 1);

    const int icon = px(U_ICON);
    for (int r = 0; r < L->nrows; r++) {
        const int i = s_scroll_right + r;
        if (i >= s_rows_n) break;
        const DropRow *d = &s_rows[i];
        const int y = R->y + r * L->row_h;
        const Rect row = { R->x, y, R->w, L->row_h };
        uint32_t bg = COL_PANEL;
        if (s_hover_pane == 1 && r == s_hover_row) {
            psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_HOVER);
        }
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        /* A grey row is the ALL CPU padding: this duelist does not drop the
         * card. It exists to be a drop target (and a quick-add on click), so
         * it draws dimmed with no numbers. */
        const int grey = d->tier < 0;
        if (name_of_card) {
            char idb[8];
            snprintf(idb, sizeof idb, "%d", d->card);
            text_right(L->r_id_r, base, idb, COL_DIM, fr);
            psx_ui_text_clip(&s_cv, L->r_name_x, base, psx_card_db_name(d->card), COL_TEXT, fr, L->r_name_r - L->r_name_x);
        } else {
            draw_icon(L->r_icon_x, y + (L->row_h - icon) / 2, icon, d->duelist, bg);
            psx_ui_text_clip(&s_cv, L->r_name_x, base, PSX_DROP_DB[d->duelist].name,
                             grey ? COL_DIM : COL_TEXT, fr, L->r_name_r - L->r_name_x);
        }
        if (grey) {
            psx_ui_text(&s_cv, L->r_rank_x, base, S_DASH, COL_DIM, fr);
            continue;
        }
        /* An edited (duelist, card) carries a green dot in the gutter, so
         * the player can see which rows are theirs. */
        if (psx_drop_edits_get(d->duelist, d->card, 0)) {
            const int dot = px(6.0f);
            psx_ui_round_rect(&s_cv, L->r_dot_x - dot / 2, y + (L->row_h - dot) / 2, dot, dot, dot * 0.5f, COL_EDITED);
        }
        psx_ui_text(&s_cv, L->r_rank_x, base, PSX_DROP_TIER_NAMES[d->tier], COL_DIM, fr);
        char buf[16];
        if (i == s_edit_row) {
            /* The weight cell as a number box: typed digits and a caret. */
            const Rect eb = { L->r_weight_x - px(6.0f), y + px(1.0f), L->r_weight_r - L->r_weight_x + px(12.0f), L->row_h - px(2.0f) };
            psx_ui_round_rect(&s_cv, eb.x, eb.y, eb.w, eb.h, (float)px(U_R_BOX), COL_EDIT_BG);
            psx_ui_round_rect_line(&s_cv, eb.x, eb.y, eb.w, eb.h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
            const int ex = psx_ui_text(&s_cv, eb.x + px(5.0f), base, s_edit_buf, COL_TEXT, fr);
            if (s_caret_on)
                psx_ui_fill(&s_cv, ex + 1, eb.y + px(3.0f), imax(1, px(1.2f)), eb.h - px(6.0f), COL_ACCENT);
        } else {
            snprintf(buf, sizeof buf, "%d", d->weight);
            text_right(L->r_weight_r, base, buf, COL_DIM, fr);
        }
        pct(buf, sizeof buf, d->weight);
        text_right(L->r_chance_r, base, buf, COL_TEXT, fr);
    }
    if (!s_rows_n) {
        const Rect e = { L->r_name_x, R->y, L->r_chance_r - L->r_name_x, L->row_h };
        text_in(&e, 0, psx_drop_missing_enabled()
                           ? "Nothing here."
                           : "No duelist drops this card. Try Mods > Drop missing cards.",
                COL_DIM, fr);
    }
}

/* The side text shared by both views: the status line while it lives, else
 * the right pane's scroll position. */
static const char *right_side(char *buf, unsigned cap, uint32_t *col)
{
    const int page = list_rows();
    if (s_msg[0] && SDL_GetTicks() < s_msg_until) { *col = COL_WARN; return s_msg; }
    if (s_rows_n > page) {
        const int hi = (s_scroll_right + page < s_rows_n) ? s_scroll_right + page : s_rows_n;
        snprintf(buf, cap, "%d" S_DASH "%d of %d", s_scroll_right + 1, hi, s_rows_n);
        *col = COL_DIM;
        return buf;
    }
    return NULL;
}

static const char *left_side(char *buf, unsigned cap)
{
    const int page = list_rows();
    const int ltot = (s_view == VIEW_CARDS) ? s_order_n : NDUEL;
    if (ltot <= page) return NULL;
    const int hi = (s_scroll + page < ltot) ? s_scroll + page : ltot;
    snprintf(buf, cap, "%d" S_DASH "%d of %d", s_scroll + 1, hi, ltot);
    return buf;
}

static void draw_cards_view(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body();
    char title[80], sel[96], ls[32], rs[32];
    if (s_search[0]) snprintf(title, sizeof title, "%d of %d cards", s_order_n, NCARDS);
    else             snprintf(title, sizeof title, "%d cards", NCARDS);
    snprintf(sel, sizeof sel, "Card %03d " S_DASH " %s", s_sel_card, psx_card_db_name(s_sel_card));
    uint32_t rcol = COL_DIM;
    const char *rside = right_side(rs, sizeof rs, &rcol);
    draw_panel(0, title, COL_TEXT, left_side(ls, sizeof ls), COL_DIM);
    draw_panel(1, sel, COL_ACCENT, rside, rcol);

    const Rect *C = &L->cols[0], *R = &L->rows[0];
    draw_col(L->c_id_x, L->c_id_r + px(6.0f), C->y, C->h, "ID", s_sort == SORT_ID, s_desc, 0);
    draw_col(L->c_name_x, L->c_name_r, C->y, C->h, "Name", s_sort == SORT_NAME, s_desc, 0);
    draw_col(L->c_type_x, L->c_type_r, C->y, C->h, "Type", s_sort == SORT_TYPE, s_desc, 0);
    draw_col(L->c_atk_x, L->c_atk_r, C->y, C->h, "ATK", s_sort == SORT_ATK, s_desc, 1);
    draw_col(L->c_def_x, L->c_def_r, C->y, C->h, "DEF", s_sort == SORT_DEF, s_desc, 1);
    draw_col(L->c_drop_x, L->c_drop_r, C->y, C->h, "Drops", s_sort == SORT_DROPS, s_desc, 1);

    for (int r = 0; r < L->nrows; r++) {
        const int i = s_scroll + r;
        if (i >= s_order_n) break;
        const int id = s_order[i];
        const int y = R->y + r * L->row_h;
        const int selected = id == s_sel_card;
        if (selected) psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_SEL_BG);
        else if (s_hover_pane == 0 && r == s_hover_row)
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_HOVER);
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", id);
        text_right(L->c_id_r, base, buf, COL_DIM, fr);
        psx_ui_text_clip(&s_cv, L->c_name_x, base, psx_card_db_name(id), selected ? COL_ACCENT : COL_TEXT, fr, L->c_name_r - L->c_name_x);
        int atk = 0, def = 0, ty = 0;
        if (psx_card_db_stats(id, &atk, &def, &ty)) {
            psx_ui_text_clip(&s_cv, L->c_type_x, base, psx_card_db_type_name(ty), COL_DIM, fr, L->c_type_r - L->c_type_x);
            snprintf(buf, sizeof buf, "%d", atk);
            text_right(L->c_atk_r, base, buf, COL_TEXT, fr);
            snprintf(buf, sizeof buf, "%d", def);
            text_right(L->c_def_r, base, buf, COL_TEXT, fr);
        }
        snprintf(buf, sizeof buf, "%d", s_drop_count[id]);
        text_right(L->c_drop_r, base, buf, s_drop_count[id] ? COL_TEXT : COL_DIM, fr);
    }
    draw_drop_rows(0);
}

static void draw_duelists_view(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body();
    char sel[96], ls[32], rs[32];
    const int ec = psx_drop_edits_count(s_sel_duelist);
    if (ec) snprintf(sel, sizeof sel, "%s " S_DASH " %d edit%s", PSX_DROP_DB[s_sel_duelist].name, ec, ec == 1 ? "" : "s");
    else    snprintf(sel, sizeof sel, "%s", PSX_DROP_DB[s_sel_duelist].name);
    uint32_t rcol = COL_DIM;
    const char *rside = right_side(rs, sizeof rs, &rcol);
    draw_panel(0, "39 duelists", COL_TEXT, left_side(ls, sizeof ls), COL_DIM);
    draw_panel(1, sel, COL_ACCENT, rside, rcol);

    const Rect *C = &L->cols[0], *R = &L->rows[0];
    draw_col(L->d_name_x, L->d_name_r, C->y, C->h, "Duelist", s_dsort == DSORT_NAME, s_ddesc, 0);
    draw_col(L->d_drops_x, L->d_drops_r, C->y, C->h, "Drops", s_dsort == DSORT_DROPS, s_ddesc, 1);

    const int icon = px(U_ICON);
    for (int r = 0; r < L->nrows; r++) {
        const int i = s_scroll + r;
        if (i >= NDUEL) break;
        const int d = s_duel_order[i];
        const int y = R->y + r * L->row_h;
        const int selected = d == s_sel_duelist;
        uint32_t bg = COL_PANEL;
        if (selected) {
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_SEL_BG);
            bg = COL_SEL_BG;
        } else if (s_hover_pane == 0 && r == s_hover_row) {
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_HOVER);
        }
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        draw_icon(L->d_icon_x, y + (L->row_h - icon) / 2, icon, d, bg);
        psx_ui_text_clip(&s_cv, L->d_name_x, base, PSX_DROP_DB[d].name, selected ? COL_ACCENT : COL_TEXT, fr, L->d_name_r - L->d_name_x);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", s_duel_total[d]);
        text_right(L->d_drops_r, base, buf, COL_DIM, fr);
    }
    draw_drop_rows(1);
}

static void draw_cmenu(void)
{
    if (!s_cmenu_n) return;
    const Rect r = cmenu_rect();
    const int rh = cmenu_row_h();
    const PsxUiFace *fr = face_body();
    psx_ui_round_rect_shadow(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_PANEL, px(5.0f));
    psx_ui_round_rect(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_EDIT_BG);
    psx_ui_round_rect_line(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
    for (int i = 0; i < s_cmenu_n; i++) {
        const Rect row = { r.x + px(3.0f), r.y + px(3.0f) + i * rh, r.w - px(6.0f), rh };
        const int inert = s_cmenu[i].action == CM_NONE;
        if (i == s_cmenu_hover && !inert)
            psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, rh * 0.5f, COL_HOVER);
        text_in(&row, px(8.0f), s_cmenu[i].label, inert ? COL_DIM : COL_TEXT, fr);
    }
}

static void draw_ghost(void)
{
    char buf[80];
    if (!s_drag_live) return;
    if (s_drag_kind == DRAG_CARD) {
        snprintf(buf, sizeof buf, "%d  %.32s", s_drag_card, psx_card_db_name(s_drag_card));
    } else if (s_drag_row >= 0 && s_drag_row < s_rows_n) {
        snprintf(buf, sizeof buf, "%.28s " S_DASH " drop outside the table to remove",
                 psx_card_db_name(s_rows[s_drag_row].card));
    } else {
        return;
    }
    const PsxUiFace *fb = face_bold();
    const int h = px(U_BTN_H);
    Rect g = { s_mouse_x + px(6.0f), s_mouse_y + px(6.0f), tw(fb, buf) + px(20.0f), h };
    if (g.x + g.w > s_w) g.x = s_w - g.w;
    if (g.y + g.h > s_h) g.y = s_h - g.h;
    psx_ui_round_rect_shadow(&s_cv, g.x, g.y, g.w, g.h, h * 0.5f, COL_PANEL, px(4.0f));
    psx_ui_round_rect(&s_cv, g.x, g.y, g.w, g.h, h * 0.5f, COL_BTN_ON);
    text_centered(&g, buf, COL_TEXT, fb);
}

/* The bottom line: unsaved edits while there are some, else how to use the
 * window — the things a first look would not guess (typing into a weight,
 * dragging a card, the right-click menu). */
static void draw_footer(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fs = face_small();
    const Rect f = { L->pane[0].x + px(4.0f), L->foot_y, s_w - 2 * L->pane[0].x - px(8.0f), L->foot_h };
    if (psx_drop_edits_dirty())
        text_in(&f, 0, "Unsaved edits. Save writes drop_table_edits.ini in your player-data folder; the game rolls what you save.", COL_WARN, fs);
    else
        text_in(&f, 0, "Click a weight to type a new one, Enter keeps it. Click a rank to move it between bands. Drag a card from the list onto a duelist to add it. Right-click a row for more.", COL_DIM, fs);
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    if (!psx_card_db_ready()) {
        draw_panel(0, "Waiting for the game to load its card table" S_ELLIP, COL_DIM, NULL, COL_DIM);
        draw_panel(1, "", COL_DIM, NULL, COL_DIM);
        draw_footer();
        return;
    }
    if (s_view == VIEW_CARDS) draw_cards_view();
    else                      draw_duelists_view();
    draw_scrollbar(0);
    draw_scrollbar(1);
    draw_footer();
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

static void set_view(int view)
{
    if (s_view == view) return;
    s_view = view;
    s_scroll = 0;
    s_scroll_right = 0;
    rebuild_rows();
    s_dirty = 1;
}

/* Which top-bar button the point is on: 0/1 the tabs, 2 Save, 3 Load, 4 the
 * view-dependent slot; -1 none. */
static int button_at(int x, int y)
{
    const Layout *L = &s_L;
    if (in_rect(&L->tab_cards, x, y)) return 0;
    if (in_rect(&L->tab_duel, x, y))  return 1;
    if (in_rect(&L->btn_save, x, y))  return 2;
    if (in_rect(&L->btn_load, x, y))  return 3;
    if (in_rect(&L->btn_third, x, y)) return 4;
    return -1;
}

/* The boundary between two columns: halfway across the gap. */
static int mid(int left_r, int right_x) { return (left_r + right_x) / 2; }

static void click(int x, int y)
{
    const Layout *L = &s_L;
    /* A click lands somewhere else: whatever number box was open is done.
     * (Clicking a weight cell reopens one right after.) */
    edit_end();
    if (in_rect(&L->bar, x, y)) {
        switch (button_at(x, y)) {
        case 0: set_view(VIEW_CARDS); break;
        case 1: set_view(VIEW_DUELISTS); break;
        case 2: say(psx_drop_edits_save() ? "Saved" : "Save failed"); break;
        case 3: open_load_menu(L->btn_load.x, L->bar.h); break;
        case 4:
            if (s_view == VIEW_DUELISTS) {
                /* Return to default, scoped to the duelist on screen. The
                 * default is whatever the layers underneath produce: stock,
                 * plus the mod when its row is on. */
                if (psx_drop_edits_clear(s_sel_duelist)) {
                    invalidate();
                    say("Edits cleared. Save to keep it.");
                } else {
                    say("No edits for this duelist");
                }
            } else {
                s_all_cpu = !s_all_cpu;
                s_scroll_right = 0;
                rebuild_rows();
                s_dirty = 1;
            }
            break;
        default: break;
        }
        return;
    }
    const int p = pane_at(x, y);
    if (p < 0) return;
    if (y >= L->cols[p].y && y < L->cols[p].y + L->cols[p].h) {
        if (p == 1) {
            /* RANK is left-aligned; everything right of it is the weight in
             * one scaling or another. */
            if (x >= mid(L->r_name_r, L->r_rank_x))
                set_rsort(x < mid(L->r_rank_r, L->r_weight_x) ? RSORT_RANK : RSORT_WEIGHT);
        } else if (s_view == VIEW_CARDS) {
            if (x < mid(L->c_id_r, L->c_name_x))        set_sort(SORT_ID);
            else if (x < mid(L->c_name_r, L->c_type_x)) set_sort(SORT_NAME);
            else if (x < mid(L->c_type_r, L->c_atk_x))  set_sort(SORT_TYPE);
            else if (x < mid(L->c_atk_r, L->c_def_x))   set_sort(SORT_ATK);
            else if (x < mid(L->c_def_r, L->c_drop_x))  set_sort(SORT_DEF);
            else                                        set_sort(SORT_DROPS);
        } else {
            set_dsort(x < mid(L->d_name_r, L->d_drops_x) ? DSORT_NAME : DSORT_DROPS);
        }
        return;
    }
    const int r = row_at(p, x, y);
    if (r < 0) return;
    if (p == 0) {
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
    const Layout *L = &s_L;
    const int r = row_at(1, x, y);
    if (r < 0) return;
    const int i = s_scroll_right + r;
    if (i < 0 || i >= s_rows_n) return;
    /* A grey ALL CPU row has no cells to act on — adds happen by DRAG or
     * right-click there, and a plain click falls through to the cross-jump
     * like the name of any other row. */
    if (s_rows[i].tier >= 0) {
        if (x >= mid(L->r_rank_r, L->r_weight_x)) { edit_begin(i); return; }
        if (x >= mid(L->r_name_r, L->r_rank_x))   { cycle_rank(i); return; }
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
    const int p = pane_at(x, y);
    const int r = row_at(p, x, y);
    if (r < 0) return;
    char buf[64];
    s_cmenu_x = x; s_cmenu_y = y; s_cmenu_n = 0; s_cmenu_hover = -1;
    if (p == 1) {
        const int i = s_scroll_right + r;
        if (i >= 0 && i < s_rows_n && s_rows[i].tier < 0) {
            /* grey ALL CPU row: the duelist does not drop the card yet */
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "Add to %.24s (%s)",
                         PSX_DROP_DB[s_rows[i].duelist].name, PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, s_rows[i].duelist, s_rows[i].card, t);
            }
        } else if (i >= 0 && i < s_rows_n) {
            cmenu_add("Edit weight", CM_EDIT_WEIGHT, i, 0, 0);
            for (int t = 0; t < NTIER; t++) {
                if (t == s_rows[i].tier) continue;
                snprintf(buf, sizeof buf, "Move to %s", PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_MOVE_BAND, i, t, 0);
            }
            cmenu_add("Remove from the band", CM_REMOVE, i, 0, 0);
        } else if (s_view == VIEW_DUELISTS) {
            /* Empty space in a duelist's table: offer to add the card that
             * is selected in the BY CARD view. */
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "Add %d %.24s (%s)", s_sel_card,
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
                snprintf(buf, sizeof buf, "Add to %.24s (%s)",
                         PSX_DROP_DB[s_sel_duelist].name, PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, s_sel_duelist, s_sel_card, t);
            }
        }
    } else {
        const int i = s_scroll + r;
        if (i >= 0 && i < NDUEL) {
            const int d = s_duel_order[i];
            for (int t = 0; t < NTIER; t++) {
                snprintf(buf, sizeof buf, "Add %d %.24s (%s)", s_sel_card,
                         psx_card_db_name(s_sel_card), PSX_DROP_TIER_NAMES[t]);
                cmenu_add(buf, CM_ADD, d, s_sel_card, t);
            }
        }
    }
    if (s_cmenu_n) s_dirty = 1;
}

/* Where a live drag ends. A card dropped on a duelist (or one of their drop
 * rows, which also names the band) is an add; a table row dropped anywhere
 * outside the right pane's rows is a removal; anything else cancels. */
static void drop_at(int x, int y)
{
    const int p = pane_at(x, y);
    const int r = row_at(p, x, y);
    if (s_drag_kind == DRAG_CARD) {
        if (p == 1 && r >= 0) {
            const int i = s_scroll_right + r;
            if (s_view == VIEW_CARDS && i >= 0 && i < s_rows_n)
                (void)add_card(s_rows[i].duelist, s_drag_card,
                               s_rows[i].tier < 0 ? 0 : s_rows[i].tier);
            else if (s_view == VIEW_DUELISTS)
                (void)add_card(s_sel_duelist, s_drag_card,
                               (i >= 0 && i < s_rows_n) ? s_rows[i].tier : 0);
        } else if (p == 0 && r >= 0 && s_view == VIEW_DUELISTS) {
            const int i = s_scroll + r;
            if (i >= 0 && i < NDUEL)
                (void)add_card(s_duel_order[i], s_drag_card, 0);
        }
    } else if (s_drag_kind == DRAG_ROW) {
        if (p != 1 || y < s_L.rows[1].y) remove_row_band(s_drag_row);
    }
    s_dirty = 1;
}

/* While dragging a card, hovering a view tab switches views — the way to
 * carry a card from the BY CARD list onto any duelist in the other view. */
static void spring_tabs(int x, int y)
{
    if (s_drag_kind != DRAG_CARD) return;
    if (in_rect(&s_L.tab_cards, x, y)) set_view(VIEW_CARDS);
    else if (in_rect(&s_L.tab_duel, x, y)) set_view(VIEW_DUELISTS);
}

static void scroll_by(int x, int amount)
{
    const int right = x >= s_L.pane[1].x;
    int *s = right ? &s_scroll_right : &s_scroll;
    const int n = right ? s_rows_n : (s_view == VIEW_CARDS ? s_order_n : NDUEL);
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

/* Which pane and visible row (or top-bar button) the pointer is over. Only a
 * change redraws, so mouse motion over the same row costs a few compares. */
static void hover_move(int x, int y)
{
    s_mouse_x = x;
    s_mouse_y = y;
    if (s_cmenu_n) {
        const int it = cmenu_item_at(x, y);
        if (it != s_cmenu_hover) { s_cmenu_hover = it; s_dirty = 1; }
        return;                     /* the menu owns hover while it is open */
    }
    const int btn = in_rect(&s_L.bar, x, y) ? button_at(x, y) : -1;
    int pane = pane_at(x, y);
    int row = row_at(pane, x, y);
    if (row < 0) pane = -1;
    if (pane != s_hover_pane || row != s_hover_row || btn != s_hover_btn) {
        s_hover_pane = pane;
        s_hover_row = row;
        s_hover_btn = btn;
        s_dirty = 1;
    }
}

static void hover_clear(void)
{
    if (s_hover_pane != -1 || s_hover_row != -1 || s_hover_btn != -1) {
        s_hover_pane = s_hover_row = s_hover_btn = -1;
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
        layout_compute();
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
        layout_compute();
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
        layout_compute();
        hover_move(x, y);
        if (s_sb_drag) {
            sb_drag_to(y);
        } else if (s_down && s_drag_kind != DRAG_NONE && !s_drag_live) {
            const int dx = x - s_down_x, dy = y - s_down_y;
            if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > s_L.row_h / 2) {
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
        layout_compute();
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
            set_view(s_view == VIEW_CARDS ? VIEW_DUELISTS : VIEW_CARDS);
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
    case SDL_WINDOWEVENT_EXPOSED:     /* uncovered or restored: paint again */
    case SDL_WINDOWEVENT_RESIZED:
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (ev->window.windowID != id) return 0;
        s_dirty = 1;
        return 1;
    default:
        break;
    }
    return 0;
}

/* --- window lifecycle ---------------------------------------------------- */

static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
/* The game's own GL window/context, captured when the tool window opens (the
 * emulation thread owns it, and that is the thread we run on). Every SDL
 * renderer call on the tool window can make ITS context current, and the
 * game's next GL call would then run against the wrong one: black game
 * window, then a driver fault at present. So after any renderer work the
 * game's context is put back. This SDL3/Wayland build has no plain window
 * framebuffer, so a renderer is the only way to show pixels at all. */
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;

static void gl_capture(void)
{
    s_gl_win = SDL_GL_GetCurrentWindow();
    s_gl_ctx = SDL_GL_GetCurrentContext();
}
static int s_ren_software;      /* the window draws through its own surface: no context to put back */
static void gl_restore(void)
{
    if (s_ren_software) return;
    if (s_gl_ctx && s_gl_win && SDL_GL_GetCurrentContext() != s_gl_ctx)
        SDL_GL_MakeCurrent(s_gl_win, s_gl_ctx);
}

static int ensure_canvas(int w, int h)
{
    if (w == s_w && h == s_h && s_px && s_tex) return 1;
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    free(s_px);
    s_px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!s_px) { s_w = s_h = 0; gl_restore(); return 0; }
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    gl_restore();
    if (!s_tex) { free(s_px); s_px = NULL; s_w = s_h = 0; return 0; }
    s_w = w; s_h = h;
    /* The design unit follows the drawable: a drag onto the 4K display
     * rescales the next frame, the same path as any resize. */
    s_u = (float)h / 480.0f;
    if (s_u < 1.0f) s_u = 1.0f;
    if (s_u > 8.0f) s_u = 8.0f;
    s_dirty = 1;
    return 1;
}

/* Upload + present, then give the GL context back to the game. */
static int s_present_fail;
/* Show the canvas. When presenting keeps failing on one backend, the
 * window is redrawn through the other one (the choice is logged). */
static void present_canvas(void)
{
    if (!s_ren || !s_tex) return;
    const int ok = psx_tool_present(s_ren, s_tex, s_px, s_w, s_h, "Drop Table Manager");
    gl_restore();
    if (ok) { s_present_fail = 0; return; }
    if (++s_present_fail < 3) { s_dirty = 1; return; }
    psx_tool_log("Drop Table Manager: switching to the %s renderer after %d failed presents", s_ren_software ? "accelerated" : "software", s_present_fail);
    gl_capture();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    SDL_DestroyRenderer(s_ren);
    s_ren = psx_tool_renderer_create(s_win, "Drop Table Manager", s_ren_software ? 1 : 0, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) { psx_drop_viewer_close(); return; }
    const int w = s_w, h = s_h; s_w = s_h = 0;
    if (!ensure_canvas(w, h)) { psx_drop_viewer_close(); return; }
    s_dirty = 1;
}

void psx_drop_viewer_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("Drop Table Manager",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("Drop table manager: no window", 2000); return; }
    gl_capture();
    /* The software renderer first: it draws through the window's own surface
     * and touches no GL context, which is what a second window wants beside
     * the game's OpenGL. Wayland has no window surfaces, so there it fails
     * and the accelerated renderer is used with the context put back after
     * every call; on Windows a GL renderer on this window was reported to
     * leave it blank white, and the software one avoids that. */
    s_ren = psx_tool_renderer_create(s_win, "Drop Table Manager", -1, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) {
        SDL_DestroyWindow(s_win); s_win = NULL;
        host_osd_push("Drop table manager: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_drop_viewer_close(); return; }
    layout_compute();
    invalidate();
    SDL_StartTextInput(s_win);
}

void psx_drop_viewer_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_pane = s_hover_row = s_hover_btn = -1;
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
    /* A card renamed in the Card Manager (or a card set switched) shows
     * here at once: names come live from the card table, so a resort and a
     * redraw are all it takes. */
    {
        static unsigned seen_gen = (unsigned)-1;
        const unsigned gen = psx_card_packs_generation();
        if (gen != seen_gen) {
            seen_gen = gen;
            if (psx_card_db_ready()) { rebuild_order(); rebuild_rows(); }
            s_dirty = 1;
        }
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
    /* The disc portraits decode on the first frame the disc is readable;
     * the plates drawn before that need one more pass. */
    static int last_portraits = -1;
    const int portraits = psx_duelist_portraits_ready();
    if (portraits != last_portraits) {
        last_portraits = portraits;
        s_dirty = 1;
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) {
        s_msg[0] = 0;
        s_dirty = 1;
    }
    if (s_dirty) {
        draw();
        s_dirty = 0;
        present_canvas();
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
    layout_compute();
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

static unsigned rect_json(char *out, unsigned cap, const char *key, const Rect *r)
{
    return (unsigned)snprintf(out, cap, ",\"%s\":[%d,%d,%d,%d]", key, r->x, r->y, r->w, r->h);
}

int psx_drop_viewer_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    if (s_win) layout_compute();
    const Layout *L = &s_L;
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"view\":\"%s\",\"sort\":%d,\"desc\":%d,"
        "\"rsort\":%d,\"rdesc\":%d,\"dsort\":%d,\"ddesc\":%d,"
        "\"search\":\"%s\",\"cards_listed\":%d,\"rows\":%d,"
        "\"sel_card\":%d,\"sel_duelist\":%d,\"modded\":%d,\"ready\":%d,"
        "\"canvas\":[%d,%d],\"split_x\":%d,\"list_rows\":%d,\"unit\":%.3f,"
        "\"hover\":[%d,%d],\"hover_btn\":%d,\"edit_row\":%d,\"edit_buf\":\"%s\","
        "\"edits_dirty\":%d,\"msg\":\"%s\",\"menu\":%d,\"menu_hover\":%d,"
        "\"drag\":%d,\"drag_live\":%d,\"scroll\":[%d,%d],\"all_cpu\":%d,\"disc_portraits\":%d",
        s_win != NULL, s_view == VIEW_CARDS ? "cards" : "duelists",
        s_sort, s_desc, s_rsort, s_rdesc, s_dsort, s_ddesc,
        s_search, s_order_n, s_rows_n,
        s_sel_card, s_sel_duelist, s_eff_modded, psx_card_db_ready(),
        s_w, s_h, s_win ? L->pane[1].x : 0, s_win ? list_rows() : 0, s_u,
        s_hover_pane, s_hover_row, s_hover_btn, s_edit_row, s_edit_buf,
        psx_drop_edits_dirty(), s_msg, s_cmenu_n, s_cmenu_hover,
        s_drag_kind, s_drag_live, s_scroll, s_scroll_right, s_all_cpu, psx_duelist_portraits_ready());
    if (!s_win || n >= cap) return n < cap;
    /* Geometry, so a script can click what it sees without knowing the
     * layout's arithmetic. Rects are [x, y, w, h] in canvas pixels. */
    n += (unsigned)snprintf(out + n, cap - n, ",\"geom\":{\"row_h\":%d", L->row_h);
    if (n < cap) n += rect_json(out + n, cap - n, "bar", &L->bar);
    if (n < cap) n += rect_json(out + n, cap - n, "tab_cards", &L->tab_cards);
    if (n < cap) n += rect_json(out + n, cap - n, "tab_duelists", &L->tab_duel);
    if (n < cap) n += rect_json(out + n, cap - n, "search", &L->search);
    if (n < cap) n += rect_json(out + n, cap - n, "save", &L->btn_save);
    if (n < cap) n += rect_json(out + n, cap - n, "load", &L->btn_load);
    if (n < cap) n += rect_json(out + n, cap - n, "third", &L->btn_third);
    if (n < cap) n += rect_json(out + n, cap - n, "left", &L->pane[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right", &L->pane[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "left_cols", &L->cols[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right_cols", &L->cols[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "left_rows", &L->rows[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right_rows", &L->rows[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "left_sb", &L->sb[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right_sb", &L->sb[1]);
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n,
        ",\"card_cols\":{\"id\":[%d,%d],\"name\":[%d,%d],\"type\":[%d,%d],\"atk\":[%d,%d],\"def\":[%d,%d],\"drops\":[%d,%d]}"
        ",\"duelist_cols\":{\"name\":[%d,%d],\"drops\":[%d,%d]}"
        ",\"drop_cols\":{\"name\":[%d,%d],\"rank\":[%d,%d],\"weight\":[%d,%d],\"chance\":[%d,%d]}}",
        L->c_id_x, L->c_id_r, L->c_name_x, L->c_name_r, L->c_type_x, L->c_type_r,
        L->c_atk_x, L->c_atk_r, L->c_def_x, L->c_def_r, L->c_drop_x, L->c_drop_r,
        L->d_name_x, L->d_name_r, L->d_drops_x, L->d_drops_r,
        L->r_name_x, L->r_name_r, L->r_rank_x, L->r_rank_r, L->r_weight_x, L->r_weight_r,
        L->r_chance_x, L->r_chance_r);
    return n < cap;
}

/* The canvas as a binary PPM — the window is a host surface the game's
 * screenshot commands cannot reach, so this is how a script sees it. */
int psx_drop_viewer_shot(const char *path)
{
    if (!s_win || !s_px || !path) return 0;
    if (s_dirty) { draw(); s_dirty = 0; }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
    for (int i = 0; i < s_w * s_h; i++) {
        const uint32_t c = s_px[i];
        const unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

PSX_MOD_CONSTRUCTOR(psx_drop_viewer_install)
{
    psx_drop_viewer_register_menu();
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
