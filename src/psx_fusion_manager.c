/* psx_fusion_manager.c -- see psx_fusion_manager.h.
 *
 * A reference window beside the game, like the Drop Table Manager and the
 * Dialogue Manager: what fuses with what, both directions, over the game's
 * own tables rather than a transcribed FAQ -- and, since those tables can be
 * replaced, over the table the game is about to read.
 *
 * The data and every edit belong to psx_fusion_table.c: it reads the stock
 * bytes off the disc (so this window works from boot, with no duel needed),
 * overlays the player's changes, packs them back into the game's format and
 * installs the result as a disc-sector override. This file is the view and
 * the typing.
 *
 * Same skeleton as psx_dialogue_manager.c: created on open and destroyed on
 * close, the F10 menu's toolkit and palette, sizes in design units (one 480th
 * of the window height), the software renderer on Windows and the accelerated
 * one elsewhere with the game's GL context put back after every renderer call
 * (PSX_TOOL_RENDERER overrides). Nothing here calls SDL from a thread other
 * than the one the runtime pumps events on.
 */

#include "psx_textfile.h"
#include "psx_fusion_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_tool_window.h"
#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_fusion_db.h"
#include "psx_fusion_table.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

/* --- look ---------------------------------------------------------------- */

#define WIN_W  1480
#define WIN_H   860

#define COL_BG        0xFF0F1219u
#define COL_BAR       0xFF141826u
#define COL_PANEL     0xFF1E2233u
#define COL_TEXT      0xFFC9CFDDu
#define COL_DIM       0xFF7C8598u
#define COL_ACCENT    0xFF7FA6FFu
#define COL_SEL_BG    0xFF2C3B60u
#define COL_HOVER     0x14FFFFFFu
#define COL_EDIT_BG   0xFF0E1119u
#define COL_EQUIP     0xFFE0A46Au      /* equips are not fusions; say so in ink */
#define COL_RESULT    0xFF8BD48Bu
#define COL_EDITED    0xFFE79FD0u      /* the player changed this pair */
#define COL_GLITCH    0xFFB08CD8u
#define COL_BTN       0xFF2A3147u
#define COL_BTN_ON    0xFF3D5A9Cu
#define COL_TRACK     0x40FFFFFFu
#define COL_THUMB     0xFF7C8598u
#define COL_WARN      0xFFE8C36Au

#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_ROW_H     14.0f
#define U_HDR_H     16.0f
#define U_TITLE_H   17.0f
#define U_BTN_H     17.0f
#define U_EDIT_H    20.0f
#define U_FOOT_H    14.0f
#define U_SB_W       4.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f

#define S_ELLIP  "\xE2\x80\xA6"     /* … */
#define S_DASH   "\xE2\x80\x93"     /* – */
#define S_UP     "\xE2\x86\x91"     /* ↑ */
#define S_DOWN   "\xE2\x86\x93"     /* ↓ */
#define S_ARROW  "\xE2\x86\x92"     /* → */

#define MAXID PSX_FUSION_TABLE_CARDS

/* --- canvas -------------------------------------------------------------- */

static SDL_Window   *s_win;
static uint32_t     *s_px;
static int           s_w, s_h;
static int           s_dirty = 1;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;

static int px(float u) { return (int)(u * s_u + 0.5f); }
static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }
static int tw(const PsxUiFace *f, const char *s) { return f ? psx_ui_font_text_w(f, s) : 0; }

typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y) { return r->w > 0 && r->h > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}
static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text(&s_cv, r->x + (r->w - tw(f, s)) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* A column is a left and a right edge; every table below is laid out as an
 * array of these, which is what lets one header-drawing and one
 * click-to-sort routine serve all four of them. */
typedef struct { int x, r; } Col;

/* Lay `n` columns into [x, right). A positive width is design units; a
 * negative one is a share of whatever the fixed columns leave, weighted by
 * its magnitude, so the name columns grow with the window and the numbers
 * do not.
 *
 * The window is resizable, so "the fixed columns do not fit" is a state a
 * player can reach by dragging an edge. Squeeze them proportionally when
 * that happens rather than letting the last of them run out of the panel
 * and paint over whatever is beside it: a clipped number is a small
 * complaint, a number floating in the gap between two panels is a bug. */
static void cols_layout(Col *out, const float *wid, int n, int x, int right, int gap)
{
    int fixed = 0;
    float share = 0.0f;
    for (int i = 0; i < n; i++) { if (wid[i] > 0.0f) fixed += px(wid[i]); else share += -wid[i]; }
    int room = right - x - gap * (n - 1);
    if (room < 0) room = 0;
    const int num = fixed > room ? room : fixed, den = fixed > 0 ? fixed : 1;
    int flex = room - fixed;
    if (flex < 0) flex = 0;
    int cx = x, used = 0;
    float acc = 0.0f;
    for (int i = 0; i < n; i++) {
        int cw;
        if (wid[i] > 0.0f) {
            cw = px(wid[i]) * num / den;
        } else {
            acc += -wid[i];
            const int upto = share > 0.0f ? (int)((float)flex * (acc / share) + 0.5f) : 0;
            cw = upto - used;
            used = upto;
        }
        if (cw < 0) cw = 0;
        out[i].x = cx;
        out[i].r = cx + cw;
        cx += cw + gap;
    }
}

static int col_at(const Col *c, int n, int x)
{
    for (int i = 0; i < n - 1; i++) if (x < c[i].r + px(U_GAP) / 2) return i;
    return n - 1;
}

static void text_right(int right, int baseline, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text(&s_cv, right - tw(f, s), baseline, s, col, f);
}

/* --- the index ------------------------------------------------------------
 *
 * None of it is ours. psx_fusion_table.c reads the stock table off the disc,
 * applies the player's edits, packs the result and republishes both arrays;
 * its generation counter is how this file knows to re-derive the per-card
 * counts and the views built from them. Nothing here reads guest memory, so
 * the window is as useful at the title screen as it is mid-duel. */

static const PsxFusionRecipe *s_rec;
static int                    s_rec_n;
static const PsxFusionEquip  *s_eq;
static int                    s_eq_n, s_eq_groups;
static unsigned               s_seen_gen;
static unsigned               s_seen_pack_gen;   /* card packs: an equip list edited anywhere */
/* The equip pairings the game actually answers: the disc's groups, except
 * that an equip card whose card.ini names its own list (ids, types or all)
 * replaces its group. psx_fusion_table_equips() is the disc alone. */
static PsxFusionEquip *s_eq_buf;
static int             s_eq_buf_cap;

static uint16_t s_n_makes[MAXID + 1];   /* partners, equip links included */
static uint16_t s_n_from[MAXID + 1];    /* pairs that produce this card */

/* Card stats, snapshotted so a qsort over twenty thousand recipes does not
 * turn into a million guest reads. */
static int16_t s_atk[MAXID + 1], s_def[MAXID + 1];
static int8_t  s_type[MAXID + 1];
static int     s_stats_ready;

static void stats_refresh(void)
{
    s_stats_ready = psx_card_db_ready();
    for (int id = 1; id <= MAXID; id++) {
        int a = -1, d = -1, t = -1;
        if (!s_stats_ready || !psx_card_db_stats(id, &a, &d, &t)) { a = d = -1; t = -1; }
        s_atk[id] = (int16_t)a;
        s_def[id] = (int16_t)d;
        s_type[id] = (int8_t)t;
    }
}

static const char *nm(int id) { return (id >= 1 && id <= MAXID) ? psx_card_packs_display_name(id) : ""; }   /* the Card Manager's name when it renamed the card */
static const char *ty(int id) { return (id >= 1 && id <= MAXID && s_type[id] >= 0) ? psx_card_db_type_name(s_type[id]) : ""; }
static int index_have(void) { return s_rec_n > 0; }
static const char *waiting_line(void)
{
    return "Waiting for the disc " S_DASH " the table is read from it, no duel needed";
}

/* --- state --------------------------------------------------------------- */

enum { VIEW_CARD = 0, VIEW_RECIPES = 1 };
static int s_view = VIEW_CARD;

static int   s_open_req;
static char  s_search[64];
static int   s_sel;                /* selected card id, 0 = none */
static int   s_scroll, s_mk_scroll, s_fr_scroll;
static int   s_hover_pane = -1, s_hover_row = -1, s_hover_btn = -1;
static char  s_msg[1024];
static Uint32 s_msg_until;
static int   s_caret_on = 1;
static int   s_stats_seen = -1;

/* Where the reader came from, so following a card name is reversible. */
static uint16_t s_hist[64];
static int      s_hist_n;

enum { SORT_ID = 0, SORT_NAME, SORT_TYPE, SORT_ATK, SORT_DEF, SORT_MAKES, SORT_FROM };
static int s_sort = SORT_ID, s_desc;

enum { RSORT_A = 0, RSORT_B, RSORT_RESULT, RSORT_TYPE, RSORT_ATK, RSORT_DEF };
static int s_rsort = RSORT_ATK, s_rdesc = 1;

static int  s_order[MAXID + 1];
static int  s_order_n;
static int *s_ridx;                /* filtered recipe indices */
static int  s_ridx_n, s_ridx_cap;

/* One row of the FUSES WITH panel. `result` is what stands after the summon,
 * which for an equip is the monster itself, powered up by `bonus`. */
enum { MK_FUSION = 0, MK_GLITCH = 1, MK_EQUIP = 2 };
typedef struct { uint16_t partner, result, stock; uint8_t kind, edited; int16_t bonus; } MakeRow;
static MakeRow *s_mk;
static int      s_mk_n, s_mk_cap;
static int     *s_fr;              /* recipe indices producing s_sel */
static int      s_fr_n, s_fr_cap;

/* The edit line: two number boxes under the FUSES WITH panel. One widget
 * covers all three operations -- a partner the card does not have yet is an
 * add, one it has is a change, and a result of 0 is a removal -- which is a
 * great deal less UI than an editor per cell, and the only shape a script can
 * drive exactly the way a player does. */
enum { ED_NONE = 0, ED_PARTNER = 1, ED_RESULT = 2 };
static int  s_ed_focus;
static char s_ed_partner[8], s_ed_result[8];

/* Right-click menu. The edit line below the panel is the fast path for
 * someone who knows the card ids; this is the one for everybody else, and it
 * is what makes "delete this" and "add one" findable at all. */
enum { CMA_NONE = 0, CMA_GOTO, CMA_CHANGE, CMA_DELETE, CMA_ADD, CMA_CLEARCARD, CMA_CLEARALL, CMA_SEP,
       CMA_EQUIP_DEL, CMA_EQUIP_ADD, CMA_EQUIP_BATCH, CMA_EQUIP_STOCK };
enum { CM_MAX = 12 };
typedef struct { char label[96]; uint8_t action; uint16_t a, b; } CmItem;
static CmItem s_cm[CM_MAX];
static int    s_cm_n, s_cm_x, s_cm_y, s_cm_hover = -1;
static int    s_cm_open;

/* Card chooser. Two card ids are two searches through 722 names, so the
 * chooser IS the editor as far as a player is concerned -- the typed line
 * only ever fills in what this would have. */
enum { PICK_NONE = 0, PICK_PARTNER = 1, PICK_RESULT = 2,
       PICK_EQUIP_MON = 3,      /* a monster for the equip in s_pick_a */
       PICK_EQUIP_EQ = 4,       /* an equip for the monster in s_pick_a */
       PICK_EQUIP_BATCH = 5 };  /* pending complete monster list for one equip */
static int  s_pick_mode;
static int  s_pick_a, s_pick_b;      /* the pair being built */
static char s_pick_search[48];
static int  s_pick_order[MAXID + 1];
static int  s_pick_n, s_pick_scroll, s_pick_hover = -1;
static uint8_t s_pick_selected[MAXID + 1];
static int     s_pick_selected_n;
/* PICK_RESULT offers "makes nothing" as a row above the cards, so deleting a
 * fusion is the same gesture as changing one. */
#define PICK_NOTHING (-1)

enum { BTN_BYCARD = 0, BTN_RECIPES, BTN_IMPORT, BTN_EXPORT, BTN_CLEAR, BTN_CLEAR_EQUIPS, BTN_RESTORE, BTN_COUNT };
static const char *const BTN_LABEL[BTN_COUNT] = { "By card", "Recipes", "Import" S_ELLIP, "Export" S_ELLIP,
                                                  "Clear fusions" S_ELLIP, "Clear equips" S_ELLIP, "Restore fusions" S_ELLIP };

/* A modal ask, for the one action that cannot be undone from inside the
 * window. Everything else here is additive or reversible; throwing away a
 * whole edit set on a misclick is not, so it gets a dialog and a backup
 * file rather than one of the two. */
enum { DLG_NONE = 0, DLG_RESTORE = 1, DLG_CLEAR = 2, DLG_CLEARCARD = 3,
       DLG_CLEAR_EQUIPS = 4 };
static int s_dlg_card;             /* which card DLG_CLEARCARD is about */
static int s_dlg;
static int s_hover_dlg = -1;       /* 0 cancel, 1 confirm */

/* the file dialog's answer, consumed on the emulation thread */
static char s_pick_path[1200];
static int  s_pick_kind;           /* 1 export, 2 import */
static char s_pick_err[200];       /* why the dialog would not open, until tick() has said so */
static int  s_pick_err_kind;

enum { NC_CARD = 7, NC_REC = 9, NC_MAKE = 7, NC_FROM = 4 };
enum { PANE_LIST = 0, PANE_MK = 1, PANE_FR = 2, PANE_COUNT = 3 };

typedef struct {
    Rect cm;                                   /* the right-click menu */
    Rect pick, pick_search, pick_rows, pick_sb;
    Rect pick_select, pick_remove, pick_clear, pick_cancel, pick_apply;
    Rect dlg, dlg_ok, dlg_cancel;
    Rect bar, search, btn[BTN_COUNT];
    Rect list, list_hdr, list_rows, list_sb;
    Rect mk, mk_title, mk_hdr, mk_rows, mk_sb, mk_edit, ed_partner, ed_result;
    Rect fr, fr_title, fr_hdr, fr_rows, fr_sb;
    Rect foot;
    int  row_h;
    Col  c[NC_CARD];       /* the card list, in BY CARD */
    Col  p[NC_REC];        /* the recipe list, in RECIPES */
    Col  m[NC_MAKE];
    Col  f[NC_FROM];
} Layout;
static Layout s_L;

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 9000u;
    s_dirty = 1;
}

/* --- filtering and ordering ---------------------------------------------- */

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static int card_matches(int id, const char *needle)
{
    char idbuf[8];
    if (!needle[0]) return 1;
    snprintf(idbuf, sizeof idbuf, "%d", id);
    return ci_contains(nm(id), needle) || !strcmp(idbuf, needle);
}

static int card_cmp(const void *pa, const void *pb)
{
    const int ia = *(const int *)pa, ib = *(const int *)pb;
    int r = 0;
    switch (s_sort) {
    case SORT_NAME:  r = strcmp(nm(ia), nm(ib)); break;
    case SORT_TYPE:  r = strcmp(ty(ia), ty(ib)); break;
    case SORT_ATK:   r = s_atk[ia] - s_atk[ib]; break;
    case SORT_DEF:   r = s_def[ia] - s_def[ib]; break;
    case SORT_MAKES: r = (int)s_n_makes[ia] - (int)s_n_makes[ib]; break;
    case SORT_FROM:  r = (int)s_n_from[ia] - (int)s_n_from[ib]; break;
    default:         r = ia - ib; break;
    }
    if (r == 0) r = ia - ib;          /* id is the tie-break, so order is stable */
    return s_desc ? -r : r;
}

static void rebuild_cards(void)
{
    s_order_n = 0;
    for (int id = 1; id <= MAXID; id++) {
        if (!card_matches(id, s_search)) continue;
        s_order[s_order_n++] = id;
    }
    qsort(s_order, (size_t)s_order_n, sizeof(int), card_cmp);
    s_dirty = 1;
}

static int recipe_cmp(const void *pa, const void *pb)
{
    const PsxFusionRecipe *x = &s_rec[*(const int *)pa], *y = &s_rec[*(const int *)pb];
    int r = 0;
    switch (s_rsort) {
    case RSORT_A:      r = strcmp(nm(x->a), nm(y->a)); break;
    case RSORT_B:      r = strcmp(nm(x->b), nm(y->b)); break;
    case RSORT_RESULT: r = strcmp(nm(x->r), nm(y->r)); break;
    case RSORT_TYPE:   r = strcmp(ty(x->r), ty(y->r)); break;
    case RSORT_ATK:    r = s_atk[x->r] - s_atk[y->r]; break;
    default:           r = s_def[x->r] - s_def[y->r]; break;
    }
    if (r == 0) r = x->r != y->r ? x->r - y->r : (x->a != y->a ? x->a - y->a : x->b - y->b);
    return s_rdesc ? -r : r;
}

static void rebuild_recipes(void)
{
    if (s_rec_n > s_ridx_cap) {
        int *p = (int *)realloc(s_ridx, sizeof(int) * (size_t)s_rec_n);
        if (!p) { s_ridx_n = 0; return; }
        s_ridx = p;
        s_ridx_cap = s_rec_n;
    }
    s_ridx_n = 0;
    for (int i = 0; i < s_rec_n; i++) {
        const PsxFusionRecipe *r = &s_rec[i];
        if (s_search[0] && !card_matches(r->a, s_search) && !card_matches(r->b, s_search)
            && !card_matches(r->r, s_search))
            continue;
        s_ridx[s_ridx_n++] = i;
    }
    qsort(s_ridx, (size_t)s_ridx_n, sizeof(int), recipe_cmp);
    s_dirty = 1;
}

static void push_make(uint16_t partner, uint16_t result, int kind, int bonus)
{
    if (s_mk_n == s_mk_cap) {
        const int cap = s_mk_cap ? s_mk_cap * 2 : 128;
        MakeRow *p = (MakeRow *)realloc(s_mk, sizeof(MakeRow) * (size_t)cap);
        if (!p) return;
        s_mk = p;
        s_mk_cap = cap;
    }
    MakeRow *m = &s_mk[s_mk_n++];
    m->partner = partner;
    m->result = result;
    m->kind = (uint8_t)kind;
    m->bonus = (int16_t)bonus;
    m->stock = 0;
    m->edited = 0;
    if (kind != MK_EQUIP) {
        m->stock = (uint16_t)psx_fusion_table_stock_result(s_sel, partner);
        m->edited = (uint8_t)psx_fusion_table_is_edited(s_sel, partner);
    }
}

static void push_from(int idx)
{
    if (s_fr_n == s_fr_cap) {
        const int cap = s_fr_cap ? s_fr_cap * 2 : 128;
        int *p = (int *)realloc(s_fr, sizeof(int) * (size_t)cap);
        if (!p) return;
        s_fr = p;
        s_fr_cap = cap;
    }
    s_fr[s_fr_n++] = idx;
}

/* Fusions before equips, then the strongest result first: the question this
 * panel answers is "what is the best thing this card makes". */
static int make_cmp(const void *pa, const void *pb)
{
    const MakeRow *x = (const MakeRow *)pa, *y = (const MakeRow *)pb;
    if ((x->kind == MK_EQUIP) != (y->kind == MK_EQUIP)) return (x->kind == MK_EQUIP) - (y->kind == MK_EQUIP);
    const int ax = s_atk[x->result] + x->bonus, ay = s_atk[y->result] + y->bonus;
    if (ax != ay) return ay - ax;
    if (x->result != y->result) return x->result - y->result;
    return x->partner - y->partner;
}

static int from_cmp(const void *pa, const void *pb)
{
    const PsxFusionRecipe *x = &s_rec[*(const int *)pa], *y = &s_rec[*(const int *)pb];
    if (x->a != y->a) return x->a - y->a;
    return x->b - y->b;
}

static void rebuild_sel(void)
{
    s_mk_n = 0;
    s_fr_n = 0;
    s_mk_scroll = 0;
    s_fr_scroll = 0;
    s_dirty = 1;
    if (s_sel < 1 || s_sel > MAXID) return;
    for (int i = 0; i < s_rec_n; i++) {
        const PsxFusionRecipe *r = &s_rec[i];
        if (r->a == s_sel || r->b == s_sel)
            push_make(r->a == s_sel ? r->b : r->a, r->r, r->glitch ? MK_GLITCH : MK_FUSION, 0);
        if (r->r == s_sel) push_from(i);
    }
    for (int i = 0; i < s_eq_n; i++) {
        const PsxFusionEquip *e = &s_eq[i];
        /* the monster survives either way round, so the result is the
         * monster and the equip is the partner -- see psx_fusion_db.h */
        if (e->mon == s_sel)   push_make(e->equip, e->mon, MK_EQUIP, psx_fusion_db_equip_bonus(e->equip));
        if (e->equip == s_sel) push_make(e->mon, e->mon, MK_EQUIP, psx_fusion_db_equip_bonus(e->equip));
    }
    qsort(s_mk, (size_t)s_mk_n, sizeof(MakeRow), make_cmp);
    qsort(s_fr, (size_t)s_fr_n, sizeof(int), from_cmp);
}

static void rebuild_all(void)
{
    rebuild_cards();
    rebuild_recipes();
    rebuild_sel();
}

/* Pull the published arrays and re-derive everything built from them. */
static int card_type(int id)
{
    int a = 0, d = 0, ty = -1;
    if (id < 1 || id > MAXID || !psx_card_db_stats(id, &a, &d, &ty)) return -1;
    return ty;
}
static int is_monster_id(int id) { const int t = card_type(id); return t >= 0 && t < 20; }
static int is_equip_id(int id)   { return card_type(id) == 23; }

/* A stock equip remains an equip-list authoring target even if the same
 * card.ini also changes its displayed type. Clear-all must preserve that
 * unrelated type edit while still clearing the disc's recipe group. */
static int is_stock_equip_id(int id)
{
    int n = 0, groups = 0;
    const PsxFusionEquip *eq = psx_fusion_table_equips(&n, &groups);
    (void)groups;
    for (int i = 0; eq && i < n; i++) if (eq[i].equip == id) return 1;
    return 0;
}

/* Does this pack's equip list take monster m? (ids, type bits, attribute
 * bits, or every monster), the same test hook_equip in psx_card_effects.c
 * answers the game with. */
static int pack_equips_monster(const PsxCardPack *pk, int m)
{
    /* one test for the game (hook_equip), the duel assistant and this window */
    const int r = psx_card_effects_equip_fits(pk->id, m);
    return r > 0;
}

static int eq_push(int n, int equip, int mon)
{
    if (n >= s_eq_buf_cap) {
        const int cap = s_eq_buf_cap ? s_eq_buf_cap * 2 : 8192;
        PsxFusionEquip *nb = (PsxFusionEquip *)realloc(s_eq_buf, sizeof(PsxFusionEquip) * (size_t)cap);
        if (!nb) return n;
        s_eq_buf = nb; s_eq_buf_cap = cap;
    }
    s_eq_buf[n].equip = (uint16_t)equip;
    s_eq_buf[n].mon = (uint16_t)mon;
    return n + 1;
}

static void build_effective_equips(void)
{
    int n_stock = 0, groups = 0;
    const PsxFusionEquip *st = psx_fusion_table_equips(&n_stock, &groups);
    static uint8_t overridden[MAXID + 1];
    memset(overridden, 0, sizeof overridden);
    int n = 0;
    s_eq_groups = 0;
    for (int id = 1; id <= MAXID; id++) {
        PsxCardPack pk;
        if (!psx_card_packs_get(id, &pk) || !(pk.equips_set || pk.equip_types)) continue;
        overridden[id] = 1;
        int any = 0;
        for (int m = 1; m <= MAXID; m++) {
            if (!is_monster_id(m) || !pack_equips_monster(&pk, m)) continue;
            n = eq_push(n, id, m); any = 1;
        }
        if (any) s_eq_groups++;
    }
    int last_key = 0;
    for (int i = 0; st && i < n_stock; i++) {
        if (overridden[st[i].equip]) continue;
        if (st[i].equip != last_key) { s_eq_groups++; last_key = st[i].equip; }
        n = eq_push(n, st[i].equip, st[i].mon);
    }
    s_eq = s_eq_buf;
    s_eq_n = n;
}

static void refresh_index(int force)
{
    const unsigned gen = psx_fusion_table_generation();
    const unsigned pgen = psx_card_packs_generation();
    if (!force && gen == s_seen_gen && pgen == s_seen_pack_gen && s_rec) return;
    s_seen_gen = gen;
    s_seen_pack_gen = pgen;
    s_rec = psx_fusion_table_recipes(&s_rec_n);
    build_effective_equips();
    if (!s_rec) s_rec_n = 0;
    memset(s_n_makes, 0, sizeof s_n_makes);
    memset(s_n_from, 0, sizeof s_n_from);
    for (int i = 0; i < s_rec_n; i++) {
        const PsxFusionRecipe *r = &s_rec[i];
        if (r->a >= 1 && r->a <= MAXID) s_n_makes[r->a]++;
        if (r->b >= 1 && r->b <= MAXID && r->b != r->a) s_n_makes[r->b]++;
        if (r->r >= 1 && r->r <= MAXID) s_n_from[r->r]++;
    }
    for (int i = 0; i < s_eq_n; i++) {
        if (s_eq[i].equip >= 1 && s_eq[i].equip <= MAXID) s_n_makes[s_eq[i].equip]++;
        if (s_eq[i].mon >= 1 && s_eq[i].mon <= MAXID) s_n_makes[s_eq[i].mon]++;
    }
    rebuild_all();
}

static int list_count(void) { return s_view == VIEW_CARD ? s_order_n : s_ridx_n; }

/* Bringing the selection into view needs a computed layout, and the debug
 * server can select a card before the window it would scroll even exists.
 * So it is a request, honoured in the frame tick once the layout is real. */
static int s_scroll_pending;

static void scroll_to_selection(void)
{
    if (s_view != VIEW_CARD || s_sel < 1) return;
    const int rows = s_L.row_h > 0 ? s_L.list_rows.h / s_L.row_h : 1;
    for (int i = 0; i < s_order_n; i++) {
        if (s_order[i] != s_sel) continue;
        if (i < s_scroll || i >= s_scroll + rows) {
            s_scroll = i - rows / 2;
            if (s_scroll < 0) s_scroll = 0;
        }
        return;
    }
}

/* Follow a card. `remember` pushes where we were, so Backspace comes back. */
static void select_card(int id, int remember)
{
    if (id < 1 || id > MAXID || id == s_sel) return;
    if (remember && s_sel >= 1 && s_hist_n < (int)(sizeof s_hist / sizeof s_hist[0]))
        s_hist[s_hist_n++] = (uint16_t)s_sel;
    s_sel = id;
    rebuild_sel();
    s_scroll_pending = 1;
}

static void go_back(void)
{
    if (!s_hist_n) { say("Nothing to go back to"); return; }
    s_sel = s_hist[--s_hist_n];
    rebuild_sel();
    s_scroll_pending = 1;
}

/* --- layout -------------------------------------------------------------- */

static void layout_compute(void)
{
    Layout *L = &s_L;
    const int gap = px(U_GAP), pad = px(U_PAD);
    L->row_h = px(U_ROW_H);
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };

    int x = s_w - pad;
    const PsxUiFace *fb = face_body();
    for (int b = BTN_COUNT - 1; b >= 0; b--) {
        const int w = tw(fb, BTN_LABEL[b]) + px(16.0f);
        x -= w;
        L->btn[b] = (Rect){ x, (L->bar.h - px(U_BTN_H)) / 2, w, px(U_BTN_H) };
        x -= gap;
    }
    const int sw = px(200.0f);
    L->search = (Rect){ x - sw - gap, (L->bar.h - px(U_BTN_H)) / 2, sw, px(U_BTN_H) };

    const int foot_h = px(U_FOOT_H);
    L->foot = (Rect){ 0, s_h - foot_h, s_w, foot_h };
    const int top = L->bar.h + gap, bottom = L->foot.y - gap;

    if (s_view == VIEW_RECIPES) {
        L->list = (Rect){ pad, top, s_w - pad * 2, bottom - top };
        L->mk = L->mk_title = L->mk_hdr = L->mk_rows = L->mk_sb = (Rect){ 0, 0, 0, 0 };
        L->mk_edit = L->ed_partner = L->ed_result = (Rect){ 0, 0, 0, 0 };
        L->fr = L->fr_title = L->fr_hdr = L->fr_rows = L->fr_sb = (Rect){ 0, 0, 0, 0 };
    } else {
        /* the list keeps a little under half: the two panels beside it carry
         * two card names apiece and need the room more than it does */
        const int lw = (s_w - pad * 2 - gap) * 44 / 100;
        L->list = (Rect){ pad, top, lw, bottom - top };
        const int rx = L->list.x + L->list.w + gap, rw = s_w - pad - rx;
        const int mkh = (bottom - top - gap) * 58 / 100;
        L->mk = (Rect){ rx, top, rw, mkh };
        L->fr = (Rect){ rx, top + mkh + gap, rw, bottom - (top + mkh + gap) };
    }

    const int sbw = px(U_SB_W), hdr = px(U_HDR_H), tit = px(U_TITLE_H), edh = px(U_EDIT_H);

    L->list_hdr = (Rect){ L->list.x, L->list.y, L->list.w, hdr };
    L->list_rows = (Rect){ L->list.x, L->list.y + hdr, L->list.w - sbw - gap, L->list.h - hdr };
    L->list_sb = (Rect){ L->list.x + L->list.w - sbw, L->list_rows.y, sbw, L->list_rows.h };

    if (s_view == VIEW_CARD) {
        L->mk_title = (Rect){ L->mk.x, L->mk.y, L->mk.w, tit };
        L->mk_hdr = (Rect){ L->mk.x, L->mk.y + tit, L->mk.w, hdr };
        L->mk_edit = (Rect){ L->mk.x, L->mk.y + L->mk.h - edh, L->mk.w, edh };
        L->mk_rows = (Rect){ L->mk.x, L->mk.y + tit + hdr, L->mk.w - sbw - gap, L->mk.h - tit - hdr - edh };
        L->mk_sb = (Rect){ L->mk.x + L->mk.w - sbw, L->mk_rows.y, sbw, L->mk_rows.h };
        L->fr_title = (Rect){ L->fr.x, L->fr.y, L->fr.w, tit };
        L->fr_hdr = (Rect){ L->fr.x, L->fr.y + tit, L->fr.w, hdr };
        L->fr_rows = (Rect){ L->fr.x, L->fr.y + tit + hdr, L->fr.w - sbw - gap, L->fr.h - tit - hdr };
        L->fr_sb = (Rect){ L->fr.x + L->fr.w - sbw, L->fr_rows.y, sbw, L->fr_rows.h };
        /* two number boxes on the edit line, after the "Partner" label */
        const int bw = px(52.0f), by = L->mk_edit.y + (edh - px(U_BTN_H)) / 2;
        const int bx = L->mk_edit.x + px(8.0f) + tw(face_small(), "Partner") + px(8.0f);
        L->ed_partner = (Rect){ bx, by, bw, px(U_BTN_H) };
        L->ed_result = (Rect){ bx + bw + px(8.0f) + tw(face_small(), "makes") + px(8.0f), by, bw, px(U_BTN_H) };
    }

    {   /* the card chooser, centred, tall enough to be worth scrolling.
         * Equip batch mode reserves a real action row: selection stays
         * pending until Apply, so Clear/Remove can still be cancelled. */
        const int pw = px(470.0f), ph = px(360.0f);
        L->pick = (Rect){ (s_w - pw) / 2, (s_h - ph) / 2, pw, ph };
        const int pad2 = px(12.0f), sbw2 = px(U_SB_W);
        L->pick_search = (Rect){ L->pick.x + pad2, L->pick.y + px(30.0f), pw - pad2 * 2, px(U_BTN_H) };
        const int ry = L->pick_search.y + L->pick_search.h + px(8.0f);
        const int action_h = s_pick_mode == PICK_EQUIP_BATCH ? px(30.0f) : 0;
        L->pick_rows = (Rect){ L->pick.x + pad2, ry, pw - pad2 * 2 - sbw2 - px(4.0f), L->pick.y + ph - pad2 - action_h - ry };
        L->pick_sb = (Rect){ L->pick.x + pw - pad2 - sbw2, ry, sbw2, L->pick_rows.h };
        const int by2 = L->pick.y + ph - pad2 - px(20.0f), bg = px(6.0f);
        int bx2 = L->pick.x + pad2;
        int bw2 = tw(face_body(), "Select filtered") + px(16.0f);
        L->pick_select = (Rect){ bx2, by2, bw2, px(20.0f) }; bx2 += bw2 + bg;
        bw2 = tw(face_body(), "Remove filtered") + px(16.0f);
        L->pick_remove = (Rect){ bx2, by2, bw2, px(20.0f) }; bx2 += bw2 + bg;
        bw2 = tw(face_body(), "Clear all") + px(16.0f);
        L->pick_clear = (Rect){ bx2, by2, bw2, px(20.0f) };
        bw2 = tw(face_body(), "Apply") + px(18.0f);
        L->pick_apply = (Rect){ L->pick.x + pw - pad2 - bw2, by2, bw2, px(20.0f) };
        bw2 = tw(face_body(), "Cancel") + px(16.0f);
        L->pick_cancel = (Rect){ L->pick_apply.x - bg - bw2, by2, bw2, px(20.0f) };
    }
    {   /* the right-click menu, at the pointer, nudged to stay on screen */
        const int iw = px(150.0f);
        int w = iw;
        for (int i = 0; i < s_cm_n; i++) {
            const int t = tw(face_body(), s_cm[i].label) + px(28.0f);
            if (t > w) w = t;
        }
        int h = px(6.0f);
        for (int i = 0; i < s_cm_n; i++) h += s_cm[i].action == CMA_SEP ? px(5.0f) : px(17.0f);
        h += px(6.0f);
        int mx = s_cm_x, my = s_cm_y;
        if (mx + w > s_w - px(4.0f)) mx = s_w - px(4.0f) - w;
        if (my + h > s_h - px(4.0f)) my = s_h - px(4.0f) - h;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        L->cm = (Rect){ mx, my, w, h };
    }
    {   /* the modal, centred, sized to its longest line */
        const int dw = px(430.0f), dh = px(112.0f);
        L->dlg = (Rect){ (s_w - dw) / 2, (s_h - dh) / 2, dw, dh };
        const int bh = px(20.0f), bm = px(12.0f);
        const int okw = tw(face_body(), "Restore stock") + px(20.0f);   /* the widest of the three */
        const int cw = tw(face_body(), "Cancel") + px(20.0f);
        L->dlg_ok = (Rect){ L->dlg.x + L->dlg.w - bm - okw, L->dlg.y + L->dlg.h - bm - bh, okw, bh };
        L->dlg_cancel = (Rect){ L->dlg_ok.x - px(8.0f) - cw, L->dlg_ok.y, cw, bh };
    }

    const int in = px(6.0f);
    {
        static const float W[NC_CARD] = { 22.0f, -1.0f, 54.0f, 30.0f, 30.0f, 32.0f, 32.0f };
        cols_layout(L->c, W, NC_CARD, L->list_rows.x + in, L->list_rows.x + L->list_rows.w - in, gap);
    }
    {
        static const float W[NC_REC] = { 22.0f, -1.0f, 22.0f, -1.0f, 22.0f, -1.1f, 54.0f, 30.0f, 30.0f };
        cols_layout(L->p, W, NC_REC, L->list_rows.x + in, L->list_rows.x + L->list_rows.w - in, gap);
    }
    if (s_view == VIEW_CARD) {
        static const float WM[NC_MAKE] = { 22.0f, -1.0f, 22.0f, -1.0f, 30.0f, 30.0f, 56.0f };
        static const float WF[NC_FROM] = { 22.0f, -1.0f, 22.0f, -1.0f };
        cols_layout(L->m, WM, NC_MAKE, L->mk_rows.x + in, L->mk_rows.x + L->mk_rows.w - in, gap);
        cols_layout(L->f, WF, NC_FROM, L->fr_rows.x + in, L->fr_rows.x + L->fr_rows.w - in, gap);
    }
}

static const Rect *pane_rows(int p)
{
    return p == PANE_LIST ? &s_L.list_rows : (p == PANE_MK ? &s_L.mk_rows : &s_L.fr_rows);
}
static const Rect *pane_sb(int p)
{
    return p == PANE_LIST ? &s_L.list_sb : (p == PANE_MK ? &s_L.mk_sb : &s_L.fr_sb);
}
static int pane_total(int p)
{
    return p == PANE_LIST ? list_count() : (p == PANE_MK ? s_mk_n : s_fr_n);
}
static int *pane_scroll(int p)
{
    return p == PANE_LIST ? &s_scroll : (p == PANE_MK ? &s_mk_scroll : &s_fr_scroll);
}
static int pane_visible(int p)
{
    const int h = pane_rows(p)->h;
    const int n = s_L.row_h > 0 ? h / s_L.row_h : 0;
    return n > 0 ? n : 1;
}

static void set_scroll(int p, int v)
{
    const int max = pane_total(p) - pane_visible(p);
    if (v > max) v = max;
    if (v < 0) v = 0;
    int *s = pane_scroll(p);
    if (v != *s) { *s = v; s_dirty = 1; }
}

static void clamp_scrolls(void)
{
    for (int p = 0; p < PANE_COUNT; p++) {
        if (pane_rows(p)->h <= 0) continue;
        set_scroll(p, *pane_scroll(p));
    }
}

static int pane_at(int x, int y)
{
    for (int p = 0; p < PANE_COUNT; p++) {
        const Rect *r = pane_rows(p);
        if (r->h > 0 && x >= r->x && x < r->x + r->w + px(U_SB_W) + px(U_GAP) && y >= r->y && y < r->y + r->h)
            return p;
    }
    return -1;
}

static int row_at(int p, int x, int y)
{
    const Rect *r = pane_rows(p);
    if (!in_rect(r, x, y)) return -1;
    const int i = *pane_scroll(p) + (y - r->y) / s_L.row_h;
    return i < pane_total(p) ? i : -1;
}

static int button_at(int x, int y)
{
    for (int b = 0; b < BTN_COUNT; b++) if (in_rect(&s_L.btn[b], x, y)) return b;
    return -1;
}

/* --- drawing ------------------------------------------------------------- */

static void draw_button(const Rect *r, const char *label, int on, int hover)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, U_R_BOX * s_u, (on || hover) ? COL_BTN_ON : COL_BTN);
    text_centered(r, label, (on || hover) ? COL_TEXT : COL_DIM, on ? face_bold() : face_body());
}

static void draw_bar(void)
{
    const Layout *L = &s_L;
    psx_ui_fill(&s_cv, L->bar.x, L->bar.y, L->bar.w, L->bar.h, COL_BAR);
    char title[256];
    if (!psx_fusion_table_ready()) {
        snprintf(title, sizeof title, "Fusion Manager   " S_DASH "   waiting for the disc");
    } else {
        int used = 0, cap = 0, pairs = 0;
        psx_fusion_table_budget(&used, &cap, &pairs);
        const int ed = psx_fusion_table_edit_count();
        if (psx_fusion_table_cleared())
            snprintf(title, sizeof title, "Fusion Manager   TABLE EMPTIED   " S_DASH "   %d recipe%s, %s   " S_DASH "   %d/%d bytes",
                     s_rec_n, s_rec_n == 1 ? "" : "s",
                     psx_fusion_table_applied() ? "ON" : "OFF (MODS " S_ARROW " Fusion edits)", used, cap);
        else if (ed)
            snprintf(title, sizeof title, "Fusion Manager   %d recipes   " S_DASH "   %d edit%s, %s   " S_DASH "   %d/%d bytes",
                     s_rec_n, ed, ed == 1 ? "" : "s",
                     psx_fusion_table_applied() ? "ON" : "OFF (MODS " S_ARROW " Fusion edits)", used, cap);
        else
            snprintf(title, sizeof title, "Fusion Manager   %d recipes, %d equip cards (%d links)   " S_DASH "   %d/%d bytes",
                     s_rec_n, s_eq_groups, s_eq_n, used, cap);
    }
    /* the title grows with the edit count and the byte figures, and the
     * search box is fixed to the right of it: clip rather than collide */
    psx_ui_text_clip(&s_cv, px(U_PAD), psx_ui_baseline_in(L->bar.y, L->bar.h, face_title()), title,
                     COL_TEXT, face_title(), L->search.x - px(U_PAD) - px(10.0f));

    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, U_R_BOX * s_u, COL_EDIT_BG);
    if (s_search[0]) {
        char shown[80];
        snprintf(shown, sizeof shown, "%s%s", s_search, (s_caret_on && !s_ed_focus) ? "|" : "");
        text_in(&L->search, px(5.0f), shown, COL_TEXT, face_body());
    } else {
        text_in(&L->search, px(5.0f), "Type to search", COL_DIM, face_body());
    }
    for (int b = 0; b < BTN_COUNT; b++) {
        const int on = (b == BTN_BYCARD && s_view == VIEW_CARD) || (b == BTN_RECIPES && s_view == VIEW_RECIPES);
        draw_button(&L->btn[b], BTN_LABEL[b], on, s_hover_btn == b);
    }
}

static void draw_col(const Col *c, const Rect *band, const char *label, int hot, int desc, int right)
{
    const PsxUiFace *fs = face_small();
    char buf[32];
    if (hot) snprintf(buf, sizeof buf, "%s %s", label, desc ? S_DOWN : S_UP);
    else     snprintf(buf, sizeof buf, "%s", label);
    const int base = psx_ui_baseline_in(band->y, band->h, fs);
    const uint32_t col = hot ? COL_ACCENT : COL_DIM;
    if (right) text_right(c->r, base, buf, col, fs);
    else       psx_ui_text_clip(&s_cv, c->x, base, buf, col, fs, c->r - c->x);
}

/* --- scrollbars -----------------------------------------------------------
 *
 * Four lists here can overflow: the three panes and the card chooser. Each
 * has a bar in its right gutter, and the bar is a control, not a picture --
 * the thumb drags and the track pages, the same as the Drop Table Manager's.
 * SB_PICK addresses the chooser, which keeps its own scroll because it is
 * modal over whichever pane is underneath.
 */
static int pick_total(void);
static int pick_visible(void);

#define SB_PICK PANE_COUNT

static const Rect *sb_rect(int t)  { return t == SB_PICK ? &s_L.pick_sb : pane_sb(t); }
static int         sb_total(int t) { return t == SB_PICK ? pick_total() : pane_total(t); }
static int         sb_visible(int t) { return t == SB_PICK ? pick_visible() : pane_visible(t); }
static int        *sb_scroll(int t) { return t == SB_PICK ? &s_pick_scroll : pane_scroll(t); }

static void sb_set_scroll(int t, int v)
{
    if (t != SB_PICK) { set_scroll(t, v); return; }
    const int max = pick_total() - pick_visible();
    if (v > max) v = max;
    if (v < 0) v = 0;
    if (v != s_pick_scroll) { s_pick_scroll = v; s_dirty = 1; }
}

/* Where the thumb sits, or 0 when the list fits and there is no bar. Drawing
 * and hit-testing both come through here, so the two cannot drift apart. */
static int sb_thumb(int t, int *out_y, int *out_h)
{
    const Rect *sb = sb_rect(t);
    const int total = sb_total(t), vis = sb_visible(t);
    if (sb->h <= 0 || total <= vis) return 0;
    int th = sb->h * vis / total;
    const int minh = px(10.0f);
    if (th < minh) th = minh;
    if (th > sb->h) th = sb->h;
    *out_h = th;
    *out_y = sb->y + (int)((long long)(sb->h - th) * *sb_scroll(t) / (total - vis));
    return 1;
}

static int s_sb_drag;              /* 0 none, else the target + 1 */
static int s_sb_grab;              /* pointer offset inside the thumb */

static void draw_scrollbar(int t)
{
    int ty, th;
    if (!sb_thumb(t, &ty, &th)) return;
    const Rect *sb = sb_rect(t);
    psx_ui_round_rect(&s_cv, sb->x, sb->y, sb->w, sb->h, sb->w * 0.5f, COL_TRACK);
    psx_ui_round_rect(&s_cv, sb->x, ty, sb->w, th, sb->w * 0.5f,
                      s_sb_drag == t + 1 ? COL_ACCENT : COL_THUMB);
}

/* A press on a bar: the thumb starts a drag, the track pages. Only the
 * chooser's bar is live while the chooser is up, since it is modal. Returns
 * 1 when the press was consumed. */
static int sb_press(int x, int y)
{
    const int first = s_pick_mode ? SB_PICK : 0;
    const int last  = s_pick_mode ? SB_PICK : PANE_COUNT - 1;
    for (int t = first; t <= last; t++) {
        int ty, th;
        if (!sb_thumb(t, &ty, &th)) continue;
        const Rect *sb = sb_rect(t);
        /* The bar is a few pixels wide, so a whisker either side counts as
         * on it -- otherwise it is a pixel-hunt with a mouse. */
        const int slack = px(4.0f);
        if (x < sb->x - slack || x >= sb->x + sb->w + slack) continue;
        if (y < sb->y || y >= sb->y + sb->h) continue;
        if (y >= ty && y < ty + th) {
            s_sb_drag = t + 1;
            s_sb_grab = y - ty;
        } else {
            const int page = sb_visible(t);
            sb_set_scroll(t, *sb_scroll(t) + (y < ty ? -page : page));
        }
        s_dirty = 1;
        return 1;
    }
    return 0;
}

static void sb_drag_to(int y)
{
    const int t = s_sb_drag - 1;
    int ty, th;
    if (t < 0 || !sb_thumb(t, &ty, &th)) return;
    const Rect *sb = sb_rect(t);
    const int span = sb->h - th;
    if (span <= 0) return;
    int top = y - s_sb_grab - sb->y;
    if (top < 0) top = 0;
    if (top > span) top = span;
    sb_set_scroll(t, (int)(((long long)top * (sb_total(t) - sb_visible(t))
                            + span / 2) / span));
}

/* One id + name pair, the shape every table here repeats. */
static void draw_card_cell(const Col *cid, const Col *cname, int y, int h, int id, uint32_t col, const PsxUiFace *f)
{
    char buf[8];
    const int base = psx_ui_baseline_in(y, h, f);
    snprintf(buf, sizeof buf, "%03d", id);
    text_right(cid->r, base, buf, COL_DIM, f);
    psx_ui_text_clip(&s_cv, cname->x, base, nm(id), col, f, cname->r - cname->x);
}

static void draw_num(const Col *c, int y, int h, int v, uint32_t col, const PsxUiFace *f)
{
    char buf[16];
    if (v < 0) snprintf(buf, sizeof buf, S_DASH);
    else       snprintf(buf, sizeof buf, "%d", v);
    text_right(c->r, psx_ui_baseline_in(y, h, f), buf, col, f);
}

static void draw_row_bg(const Rect *rows, int y, int h, int selected, int hovered)
{
    if (selected)     psx_ui_round_rect(&s_cv, rows->x, y, rows->w, h, U_R_BOX * s_u, COL_SEL_BG);
    else if (hovered) psx_ui_round_rect(&s_cv, rows->x, y, rows->w, h, U_R_BOX * s_u, COL_HOVER);
}

static void draw_empty(const Rect *rows, const char *what)
{
    Rect r = { rows->x, rows->y, rows->w, s_L.row_h * 2 };
    text_in(&r, px(6.0f), what, COL_DIM, face_body());
}

static void draw_card_list(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->list.x, L->list.y, L->list.w, L->list.h, U_R_PANEL * s_u, COL_PANEL);
    draw_col(&L->c[0], &L->list_hdr, "ID",    s_sort == SORT_ID,    s_desc, 1);
    draw_col(&L->c[1], &L->list_hdr, "Card",  s_sort == SORT_NAME,  s_desc, 0);
    draw_col(&L->c[2], &L->list_hdr, "Type",  s_sort == SORT_TYPE,  s_desc, 0);
    draw_col(&L->c[3], &L->list_hdr, "ATK",   s_sort == SORT_ATK,   s_desc, 1);
    draw_col(&L->c[4], &L->list_hdr, "DEF",   s_sort == SORT_DEF,   s_desc, 1);
    draw_col(&L->c[5], &L->list_hdr, "Fuses", s_sort == SORT_MAKES, s_desc, 1);
    draw_col(&L->c[6], &L->list_hdr, "Made",  s_sort == SORT_FROM,  s_desc, 1);
    if (!s_order_n) { draw_empty(&L->list_rows, s_search[0] ? "No card matches that" : "Waiting for the game's card table" S_ELLIP); return; }

    const int vis = pane_visible(PANE_LIST);
    const PsxUiFace *fb = face_body();
    for (int i = 0; i < vis && s_scroll + i < s_order_n; i++) {
        const int row = s_scroll + i, id = s_order[row];
        const int y = L->list_rows.y + i * L->row_h;
        draw_row_bg(&L->list_rows, y, L->row_h, id == s_sel, s_hover_pane == PANE_LIST && s_hover_row == row);
        draw_card_cell(&L->c[0], &L->c[1], y, L->row_h, id, id == s_sel ? COL_ACCENT : COL_TEXT, fb);
        Rect t = { L->c[2].x, y, L->c[2].r - L->c[2].x, L->row_h };
        text_in(&t, 0, ty(id), COL_DIM, fb);
        draw_num(&L->c[3], y, L->row_h, s_atk[id], COL_TEXT, fb);
        draw_num(&L->c[4], y, L->row_h, s_def[id], COL_TEXT, fb);
        draw_num(&L->c[5], y, L->row_h, s_n_makes[id], s_n_makes[id] ? COL_TEXT : COL_DIM, fb);
        draw_num(&L->c[6], y, L->row_h, s_n_from[id], s_n_from[id] ? COL_RESULT : COL_DIM, fb);
    }
    draw_scrollbar(PANE_LIST);
}

static void draw_recipe_list(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->list.x, L->list.y, L->list.w, L->list.h, U_R_PANEL * s_u, COL_PANEL);
    draw_col(&L->p[0], &L->list_hdr, "ID",     0,                       0,       1);
    draw_col(&L->p[1], &L->list_hdr, "Card",   s_rsort == RSORT_A,      s_rdesc, 0);
    draw_col(&L->p[2], &L->list_hdr, "ID",     0,                       0,       1);
    draw_col(&L->p[3], &L->list_hdr, "Plus",   s_rsort == RSORT_B,      s_rdesc, 0);
    draw_col(&L->p[4], &L->list_hdr, "ID",     0,                       0,       1);
    draw_col(&L->p[5], &L->list_hdr, "Makes",  s_rsort == RSORT_RESULT, s_rdesc, 0);
    draw_col(&L->p[6], &L->list_hdr, "Type",   s_rsort == RSORT_TYPE,   s_rdesc, 0);
    draw_col(&L->p[7], &L->list_hdr, "ATK",    s_rsort == RSORT_ATK,    s_rdesc, 1);
    draw_col(&L->p[8], &L->list_hdr, "DEF",    s_rsort == RSORT_DEF,    s_rdesc, 1);
    if (!s_ridx_n) {
        draw_empty(&L->list_rows, index_have() ? "No recipe matches that" : waiting_line());
        return;
    }
    const int vis = pane_visible(PANE_LIST);
    const PsxUiFace *fb = face_body();
    for (int i = 0; i < vis && s_scroll + i < s_ridx_n; i++) {
        const int row = s_scroll + i;
        const PsxFusionRecipe *r = &s_rec[s_ridx[row]];
        const int y = L->list_rows.y + i * L->row_h;
        const int hit = r->a == s_sel || r->b == s_sel || r->r == s_sel;
        const int ed = psx_fusion_table_is_edited(r->a, r->b);
        draw_row_bg(&L->list_rows, y, L->row_h, hit, s_hover_pane == PANE_LIST && s_hover_row == row);
        draw_card_cell(&L->p[0], &L->p[1], y, L->row_h, r->a, r->a == s_sel ? COL_ACCENT : COL_TEXT, fb);
        draw_card_cell(&L->p[2], &L->p[3], y, L->row_h, r->b, r->b == s_sel ? COL_ACCENT : COL_TEXT, fb);
        draw_card_cell(&L->p[4], &L->p[5], y, L->row_h, r->r,
                       ed ? COL_EDITED : (r->glitch ? COL_GLITCH : COL_RESULT), fb);
        Rect t = { L->p[6].x, y, L->p[6].r - L->p[6].x, L->row_h };
        text_in(&t, 0, ty(r->r), COL_DIM, fb);
        draw_num(&L->p[7], y, L->row_h, s_atk[r->r], COL_TEXT, fb);
        draw_num(&L->p[8], y, L->row_h, s_def[r->r], COL_TEXT, fb);
    }
    draw_scrollbar(PANE_LIST);
}

static void panel_title(const Rect *panel, const Rect *title, const char *left, const char *right)
{
    psx_ui_round_rect(&s_cv, panel->x, panel->y, panel->w, panel->h, U_R_PANEL * s_u, COL_PANEL);
    Rect t = { title->x + px(8.0f), title->y, title->w - px(16.0f), title->h };
    text_in(&t, 0, left, COL_TEXT, face_bold());
    if (right && right[0]) {
        const PsxUiFace *fs = face_small();
        text_right(title->x + title->w - px(8.0f), psx_ui_baseline_in(title->y, title->h, fs), right, COL_DIM, fs);
    }
}

/* "Partner [   ] makes [   ]  <what those two numbers mean>". Enter applies. */
static void draw_edit_line(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fs = face_small(), *fb = face_body();
    const int base = psx_ui_baseline_in(L->mk_edit.y, L->mk_edit.h, fs);
    if (s_sel < 1) {
        psx_ui_text(&s_cv, L->mk_edit.x + px(8.0f), base, "Pick a card to change what it fuses into", COL_DIM, fs);
        return;
    }
    psx_ui_text(&s_cv, L->mk_edit.x + px(8.0f), base, "Partner", COL_DIM, fs);
    psx_ui_text(&s_cv, L->ed_partner.x + L->ed_partner.w + px(8.0f), base, "makes", COL_DIM, fs);
    for (int i = 0; i < 2; i++) {
        const Rect *b = i ? &L->ed_result : &L->ed_partner;
        const char *v = i ? s_ed_result : s_ed_partner;
        const int on = s_ed_focus == (i ? ED_RESULT : ED_PARTNER);
        psx_ui_round_rect(&s_cv, b->x, b->y, b->w, b->h, U_R_BOX * s_u, COL_EDIT_BG);
        if (on) psx_ui_round_rect_line(&s_cv, b->x, b->y, b->w, b->h, U_R_BOX * s_u, COL_ACCENT, 1.5f * s_u);
        char shown[16];
        snprintf(shown, sizeof shown, "%s%s", v, (on && s_caret_on) ? "|" : "");
        if (v[0] || on) text_in(b, px(5.0f), shown, COL_TEXT, fb);
        else            text_in(b, px(5.0f), i ? "id" : "id", COL_DIM, fb);
    }
    /* say what the two numbers name, so a typo is visible before Enter */
    const int pid = atoi(s_ed_partner), rid = atoi(s_ed_result);
    char note[224];
    uint32_t col = COL_DIM;
    if (!s_ed_partner[0])
        snprintf(note, sizeof note, "two card ids, then Enter " S_DASH " a result of 0 removes the fusion, double-click a row to load it");
    else if (pid < 1 || pid > MAXID) { snprintf(note, sizeof note, "%d is not a card id", pid); col = COL_WARN; }
    else if (!s_ed_result[0])
        snprintf(note, sizeof note, "%s + %s " S_ARROW " ?", nm(s_sel), nm(pid));
    else if (rid == 0)
        snprintf(note, sizeof note, "%s + %s makes NOTHING   " S_DASH "   Enter applies", nm(s_sel), nm(pid));
    else if (rid > MAXID) { snprintf(note, sizeof note, "%d is not a card id", rid); col = COL_WARN; }
    else
        snprintf(note, sizeof note, "%s + %s " S_ARROW " %s   " S_DASH "   Enter applies", nm(s_sel), nm(pid), nm(rid));
    const int nx = L->ed_result.x + L->ed_result.w + px(10.0f);
    psx_ui_text_clip(&s_cv, nx, base, note, col, fs, L->mk_edit.x + L->mk_edit.w - px(8.0f) - nx);
}

static void draw_makes(void)
{
    const Layout *L = &s_L;
    char head[192], count[48];
    if (s_sel >= 1) snprintf(head, sizeof head, "Fuses with " S_DASH " %s", nm(s_sel));
    else            snprintf(head, sizeof head, "Fuses with");
    snprintf(count, sizeof count, "%d", s_mk_n);
    panel_title(&L->mk, &L->mk_title, head, s_sel >= 1 ? count : "");
    draw_col(&L->m[0], &L->mk_hdr, "ID",      0, 0, 1);
    draw_col(&L->m[1], &L->mk_hdr, "Partner", 0, 0, 0);
    draw_col(&L->m[2], &L->mk_hdr, "ID",      0, 0, 1);
    draw_col(&L->m[3], &L->mk_hdr, "Makes",   0, 0, 0);
    draw_col(&L->m[4], &L->mk_hdr, "ATK",     0, 0, 1);
    draw_col(&L->m[5], &L->mk_hdr, "DEF",     0, 0, 1);
    draw_col(&L->m[6], &L->mk_hdr, "How",     0, 0, 0);
    draw_edit_line();
    if (s_sel < 1)   { draw_empty(&L->mk_rows, "Pick a card on the left"); return; }
    if (!s_mk_n)     { draw_empty(&L->mk_rows, index_have() ? "This card combines with nothing " S_DASH " add a pair below"
                                                            : waiting_line()); return; }
    const int vis = pane_visible(PANE_MK);
    const PsxUiFace *fb = face_body();
    for (int i = 0; i < vis && s_mk_scroll + i < s_mk_n; i++) {
        const int row = s_mk_scroll + i;
        const MakeRow *m = &s_mk[row];
        const int y = L->mk_rows.y + i * L->row_h;
        const int eq = m->kind == MK_EQUIP;
        const uint32_t col = m->edited ? COL_EDITED : (eq ? COL_EQUIP : (m->kind == MK_GLITCH ? COL_GLITCH : COL_RESULT));
        draw_row_bg(&L->mk_rows, y, L->row_h, 0, s_hover_pane == PANE_MK && s_hover_row == row);
        draw_card_cell(&L->m[0], &L->m[1], y, L->row_h, m->partner, eq ? COL_EQUIP : COL_TEXT, fb);
        draw_card_cell(&L->m[2], &L->m[3], y, L->row_h, m->result, col, fb);
        draw_num(&L->m[4], y, L->row_h, s_atk[m->result] < 0 ? -1 : s_atk[m->result] + m->bonus, COL_TEXT, fb);
        draw_num(&L->m[5], y, L->row_h, s_def[m->result] < 0 ? -1 : s_def[m->result] + m->bonus, COL_TEXT, fb);
        char how[48];
        if (eq)                         snprintf(how, sizeof how, "equip +%d", m->bonus);
        else if (m->edited && m->stock) snprintf(how, sizeof how, "was %03d", m->stock);
        else if (m->edited)             snprintf(how, sizeof how, "added");
        else if (m->kind == MK_GLITCH)  snprintf(how, sizeof how, "glitch");
        else                            snprintf(how, sizeof how, "fusion");
        Rect t = { L->m[6].x, y, L->m[6].r - L->m[6].x, L->row_h };
        text_in(&t, 0, how, m->edited ? COL_EDITED : (eq ? COL_EQUIP : (m->kind == MK_GLITCH ? COL_GLITCH : COL_DIM)), face_small());
    }
    draw_scrollbar(PANE_MK);
}

static void draw_from(void)
{
    const Layout *L = &s_L;
    char head[192], count[48];
    if (s_sel >= 1) snprintf(head, sizeof head, "Made from " S_DASH " %s", nm(s_sel));
    else            snprintf(head, sizeof head, "Made from");
    snprintf(count, sizeof count, "%d", s_fr_n);
    panel_title(&L->fr, &L->fr_title, head, s_sel >= 1 ? count : "");
    draw_col(&L->f[0], &L->fr_hdr, "ID",   0, 0, 1);
    draw_col(&L->f[1], &L->fr_hdr, "Card", 0, 0, 0);
    draw_col(&L->f[2], &L->fr_hdr, "ID",   0, 0, 1);
    draw_col(&L->f[3], &L->fr_hdr, "Plus", 0, 0, 0);
    if (s_sel < 1) { draw_empty(&L->fr_rows, "Pick a card on the left"); return; }
    if (!s_fr_n)   { draw_empty(&L->fr_rows, index_have() ? "Nothing fuses into this card" : waiting_line()); return; }
    const int vis = pane_visible(PANE_FR);
    const PsxUiFace *fb = face_body();
    for (int i = 0; i < vis && s_fr_scroll + i < s_fr_n; i++) {
        const int row = s_fr_scroll + i;
        const PsxFusionRecipe *r = &s_rec[s_fr[row]];
        const int y = L->fr_rows.y + i * L->row_h;
        const uint32_t col = psx_fusion_table_is_edited(r->a, r->b) ? COL_EDITED : COL_TEXT;
        draw_row_bg(&L->fr_rows, y, L->row_h, 0, s_hover_pane == PANE_FR && s_hover_row == row);
        draw_card_cell(&L->f[0], &L->f[1], y, L->row_h, r->a, col, fb);
        draw_card_cell(&L->f[2], &L->f[3], y, L->row_h, r->b, col, fb);
    }
    draw_scrollbar(PANE_FR);
}

static void draw_footer(void)
{
    const Layout *L = &s_L;
    const char *hint =
        s_view == VIEW_CARD
        ? "RIGHT-CLICK a row to add, change or delete a fusion. Click a name to follow it, Backspace goes back, Tab switches view, Ctrl+Z restores stock."
        : "RIGHT-CLICK a row to change or delete it. Click any card name to open it in BY CARD, headers sort, Tab switches view.";
    const char *m = s_msg[0] ? s_msg : hint;
    Rect r = { px(U_PAD), L->foot.y, s_w - px(U_PAD) * 2, L->foot.h };
    text_in(&r, 0, m, s_msg[0] ? COL_WARN : COL_DIM, face_small());
}

/* --- the card chooser ----------------------------------------------------- */

static int pick_extra(void);
static int equip_pair_set(int equip, int mon, int on, char *err, unsigned cap);
static int equip_list_save(int equip, const uint8_t selected[MAXID + 1],
                           char *err, unsigned cap);
static int equip_stock(int equip, char *err, unsigned cap);

static void pick_rebuild(void)
{
    s_pick_n = 0;
    for (int id = 1; id <= MAXID; id++) {
        if (!card_matches(id, s_pick_search)) continue;
        if ((s_pick_mode == PICK_EQUIP_MON || s_pick_mode == PICK_EQUIP_BATCH) && !is_monster_id(id)) continue;
        if (s_pick_mode == PICK_EQUIP_EQ && !is_equip_id(id)) continue;
        s_pick_order[s_pick_n++] = id;
    }
    if (s_pick_scroll > s_pick_n - 1) s_pick_scroll = s_pick_n > 0 ? s_pick_n - 1 : 0;
    if (s_pick_scroll < 0) s_pick_scroll = 0;
    /* Highlight the first CARD, never the "remove this fusion" row that sits
     * above it: typing a name and pressing Enter must set that card, not
     * delete the fusion. Showing the highlight is half the fix -- the other
     * half is that Enter now has nothing to guess. */
    s_pick_hover = (s_pick_n > 0) ? pick_extra() : -1;
    s_dirty = 1;
}

static void pick_open(int mode, int a, int b)
{
    s_pick_mode = mode;
    s_pick_a = a;
    s_pick_b = b;
    s_pick_search[0] = 0;
    s_pick_scroll = 0;
    if (mode == PICK_EQUIP_BATCH) {
        memset(s_pick_selected, 0, sizeof s_pick_selected);
        s_pick_selected_n = 0;
        refresh_index(0);
        for (int i = 0; i < s_eq_n; i++) {
            const int mon = s_eq[i].mon;
            if (s_eq[i].equip != a || mon < 1 || mon > MAXID || s_pick_selected[mon]) continue;
            s_pick_selected[mon] = 1;
            s_pick_selected_n++;
        }
    }
    pick_rebuild();
}

static void pick_close(void) { s_pick_mode = PICK_NONE; s_dirty = 1; }

/* How many card rows fit, and where the "makes nothing" row sits. */
static int pick_extra(void) { return s_pick_mode == PICK_RESULT ? 1 : 0; }
static int pick_total(void) { return s_pick_n + pick_extra(); }
static int pick_visible(void)
{
    const int n = s_L.row_h > 0 ? s_L.pick_rows.h / s_L.row_h : 0;
    return n > 0 ? n : 1;
}

static void pick_choose(int row)
{
    char err[512];
    const int extra = pick_extra();
    int id;
    if (row < 0 || row >= pick_total()) return;
    if (extra && row == 0) id = PICK_NOTHING;
    else {
        const int i = row - extra;
        if (i < 0 || i >= s_pick_n) return;
        id = s_pick_order[i];
    }
    if (s_pick_mode == PICK_EQUIP_BATCH) {
        if (id < 1 || id > MAXID) return;
        if (s_pick_selected[id]) { s_pick_selected[id] = 0; s_pick_selected_n--; }
        else                     { s_pick_selected[id] = 1; s_pick_selected_n++; }
        s_dirty = 1;
        return;
    }
    if (s_pick_mode == PICK_PARTNER) {
        if (id == PICK_NOTHING) return;
        pick_open(PICK_RESULT, s_pick_a, id);
        return;
    }
    if (s_pick_mode == PICK_EQUIP_MON || s_pick_mode == PICK_EQUIP_EQ) {
        const int equip = s_pick_mode == PICK_EQUIP_MON ? s_pick_a : id;
        const int mon   = s_pick_mode == PICK_EQUIP_MON ? id : s_pick_a;
        pick_close();
        (void)equip_pair_set(equip, mon, 1, err, sizeof err);
        say(err);
        return;
    }
    /* PICK_RESULT: this is the edit */
    {
        const int a = s_pick_a, b = s_pick_b, r = (id == PICK_NOTHING) ? 0 : id;
        pick_close();
        if (!psx_fusion_table_edit(a, b, r, err, sizeof err)) { say(err); return; }
        refresh_index(1);
        rebuild_sel();
        if (r) snprintf(err, sizeof err, "%s + %s " S_ARROW " %s", nm(a), nm(b), nm(r));
        else   snprintf(err, sizeof err, "%s + %s now makes nothing", nm(a), nm(b));
        say(err);
    }
}

static void pick_batch_filtered(int on)
{
    if (s_pick_mode != PICK_EQUIP_BATCH) return;
    for (int i = 0; i < s_pick_n; i++) {
        const int id = s_pick_order[i];
        if (!!s_pick_selected[id] == !!on) continue;
        s_pick_selected[id] = on ? 1 : 0;
        s_pick_selected_n += on ? 1 : -1;
    }
    s_dirty = 1;
}

static void pick_batch_clear(void)
{
    if (s_pick_mode != PICK_EQUIP_BATCH) return;
    memset(s_pick_selected, 0, sizeof s_pick_selected);
    s_pick_selected_n = 0;
    s_dirty = 1;
}

static void pick_batch_apply(void)
{
    if (s_pick_mode != PICK_EQUIP_BATCH) return;
    char err[512];
    const int equip = s_pick_a;
    if (!equip_list_save(equip, s_pick_selected, err, sizeof err)) { say(err); return; }
    pick_close();
    say(err);
}

static void draw_picker(void)
{
    const Layout *L = &s_L;
    if (!s_pick_mode) return;
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, 0xA0080A10u);
    psx_ui_round_rect_shadow(&s_cv, L->pick.x, L->pick.y, L->pick.w, L->pick.h, U_R_PANEL * s_u, 0x80000000u, px(14.0f));
    psx_ui_round_rect(&s_cv, L->pick.x, L->pick.y, L->pick.w, L->pick.h, U_R_PANEL * s_u, COL_PANEL);
    psx_ui_round_rect_line(&s_cv, L->pick.x, L->pick.y, L->pick.w, L->pick.h, U_R_PANEL * s_u, 0x30FFFFFFu, 1.0f * s_u);

    char head[192];
    if (s_pick_mode == PICK_PARTNER)        snprintf(head, sizeof head, "What does %s fuse with?", nm(s_pick_a));
    else if (s_pick_mode == PICK_EQUIP_MON) snprintf(head, sizeof head, "Which monster does %s fit?", nm(s_pick_a));
    else if (s_pick_mode == PICK_EQUIP_EQ)  snprintf(head, sizeof head, "Which equip fits %s?", nm(s_pick_a));
    else if (s_pick_mode == PICK_EQUIP_BATCH) snprintf(head, sizeof head, "%s usable monsters  %s  %d selected", nm(s_pick_a), S_DASH, s_pick_selected_n);
    else                                    snprintf(head, sizeof head, "%s + %s makes" S_ELLIP, nm(s_pick_a), nm(s_pick_b));
    Rect t = { L->pick.x + px(12.0f), L->pick.y + px(8.0f), L->pick.w - px(24.0f), px(18.0f) };
    text_in(&t, 0, head, COL_TEXT, face_title());

    psx_ui_round_rect(&s_cv, L->pick_search.x, L->pick_search.y, L->pick_search.w, L->pick_search.h, U_R_BOX * s_u, COL_EDIT_BG);
    psx_ui_round_rect_line(&s_cv, L->pick_search.x, L->pick_search.y, L->pick_search.w, L->pick_search.h, U_R_BOX * s_u, COL_ACCENT, 1.5f * s_u);
    if (s_pick_search[0]) {
        char shown[64];
        snprintf(shown, sizeof shown, "%s%s", s_pick_search, s_caret_on ? "|" : "");
        text_in(&L->pick_search, px(6.0f), shown, COL_TEXT, face_body());
    } else {
        text_in(&L->pick_search, px(6.0f), "Type a name to find it" S_ELLIP, COL_DIM, face_body());
    }

    const int vis = pick_visible(), extra = pick_extra();
    const PsxUiFace *fb = face_body();
    for (int i = 0; i < vis && s_pick_scroll + i < pick_total(); i++) {
        const int row = s_pick_scroll + i;
        const int y = L->pick_rows.y + i * L->row_h;
        if (row == s_pick_hover)
            psx_ui_round_rect(&s_cv, L->pick_rows.x, y, L->pick_rows.w, L->row_h, U_R_BOX * s_u, COL_SEL_BG);
        if (extra && row == 0) {
            Rect r = { L->pick_rows.x + px(6.0f), y, L->pick_rows.w - px(12.0f), L->row_h };
            text_in(&r, 0, "Nothing " S_DASH " remove this fusion", COL_WARN, fb);
            continue;
        }
        const int id = s_pick_order[row - extra];
        const int base = psx_ui_baseline_in(y, L->row_h, fb);
        char idb[8];
        snprintf(idb, sizeof idb, "%03d", id);
        int tx = L->pick_rows.x + px(6.0f);
        if (s_pick_mode == PICK_EQUIP_BATCH) {
            const int bs = px(8.0f), by = y + (L->row_h - bs) / 2;
            psx_ui_round_rect_line(&s_cv, tx, by, bs, bs, px(1.5f),
                                   s_pick_selected[id] ? COL_ACCENT : COL_DIM, 1.0f * s_u);
            if (s_pick_selected[id]) psx_ui_fill(&s_cv, tx + px(2.0f), by + px(2.0f),
                                                 bs - px(4.0f), bs - px(4.0f), COL_ACCENT);
            tx += px(14.0f);
        }
        psx_ui_text(&s_cv, tx, base, idb, COL_DIM, fb);
        psx_ui_text_clip(&s_cv, tx + px(32.0f), base, nm(id), COL_TEXT, fb,
                         L->pick_rows.x + L->pick_rows.w - (tx + px(32.0f)) - px(116.0f));
        char st[48];
        snprintf(st, sizeof st, "%d / %d", s_atk[id] < 0 ? 0 : s_atk[id], s_def[id] < 0 ? 0 : s_def[id]);
        text_right(L->pick_rows.x + L->pick_rows.w - px(6.0f), base, st, COL_DIM, face_small());
    }
    if (!pick_total()) {
        Rect r = { L->pick_rows.x + px(6.0f), L->pick_rows.y, L->pick_rows.w, L->row_h };
        text_in(&r, 0, "No card matches that", COL_DIM, fb);
    }
    draw_scrollbar(SB_PICK);
    if (s_pick_mode == PICK_EQUIP_BATCH) {
        draw_button(&L->pick_select, "Select filtered", 0, 0);
        draw_button(&L->pick_remove, "Remove filtered", 0, 0);
        draw_button(&L->pick_clear, "Clear all", 0, 0);
        draw_button(&L->pick_cancel, "Cancel", 0, 0);
        draw_button(&L->pick_apply, "Apply", 1, 0);
    }
}

/* --- the right-click menu -------------------------------------------------- */

static void cm_add(const char *label, int action, int a, int b)
{
    if (s_cm_n >= CM_MAX) return;
    CmItem *it = &s_cm[s_cm_n++];
    snprintf(it->label, sizeof it->label, "%s", label);
    it->action = (uint8_t)action;
    it->a = (uint16_t)a;
    it->b = (uint16_t)b;
}

static void cm_close(void) { s_cm_open = 0; s_cm_hover = -1; s_dirty = 1; }

/* Row rect of item i, for hit-testing and drawing alike. */
static int cm_item_y(int i)
{
    int y = s_L.cm.y + px(6.0f);
    for (int k = 0; k < i; k++) y += s_cm[k].action == CMA_SEP ? px(5.0f) : px(17.0f);
    return y;
}

static int cm_at(int x, int y)
{
    if (!s_cm_open || !in_rect(&s_L.cm, x, y)) return -1;
    for (int i = 0; i < s_cm_n; i++) {
        if (s_cm[i].action == CMA_SEP) continue;
        const int iy = cm_item_y(i);
        if (y >= iy && y < iy + px(17.0f)) return i;
    }
    return -1;
}

static void dialog_open(int kind);   /* the confirm these destructive items raise */
static int equip_clear_all_now(char *err, unsigned cap);

static void cm_run(int i)
{
    if (i < 0 || i >= s_cm_n) return;
    const CmItem it = s_cm[i];
    cm_close();
    switch (it.action) {
    case CMA_GOTO:   select_card(it.a, 1); break;
    case CMA_ADD:    if (it.a >= 1) { select_card(it.a, 1); pick_open(PICK_PARTNER, it.a, 0); } break;
    case CMA_CHANGE: pick_open(PICK_RESULT, it.a, it.b); break;
    case CMA_EQUIP_ADD: if (it.a >= 1) { select_card(it.a, 1); pick_open(is_equip_id(it.a) ? PICK_EQUIP_MON : PICK_EQUIP_EQ, it.a, 0); } break;
    case CMA_EQUIP_BATCH: if (it.a >= 1 && is_equip_id(it.a)) { select_card(it.a, 1); pick_open(PICK_EQUIP_BATCH, it.a, 0); } break;
    case CMA_EQUIP_DEL: { char err[256]; (void)equip_pair_set(it.a, it.b, 0, err, sizeof err); say(err); break; }
    case CMA_EQUIP_STOCK: { char err[256]; (void)equip_stock(it.a, err, sizeof err); say(err); break; }
    case CMA_CLEARCARD: s_dlg_card = it.a; dialog_open(DLG_CLEARCARD); break;
    case CMA_CLEARALL:  dialog_open(DLG_CLEAR); break;
    case CMA_DELETE: {
        char err[512];
        if (!psx_fusion_table_edit(it.a, it.b, 0, err, sizeof err)) { say(err); return; }
        refresh_index(1);
        rebuild_sel();
        snprintf(err, sizeof err, "%s + %s now makes nothing", nm(it.a), nm(it.b));
        say(err);
        break;
    }
    default: break;
    }
}

static void draw_context_menu(void)
{
    const Layout *L = &s_L;
    if (!s_cm_open) return;
    psx_ui_round_rect_shadow(&s_cv, L->cm.x, L->cm.y, L->cm.w, L->cm.h, U_R_BOX * s_u, 0x90000000u, px(10.0f));
    psx_ui_round_rect(&s_cv, L->cm.x, L->cm.y, L->cm.w, L->cm.h, U_R_BOX * s_u, 0xFF272C3Fu);
    psx_ui_round_rect_line(&s_cv, L->cm.x, L->cm.y, L->cm.w, L->cm.h, U_R_BOX * s_u, 0x30FFFFFFu, 1.0f * s_u);
    for (int i = 0; i < s_cm_n; i++) {
        const int y = cm_item_y(i);
        if (s_cm[i].action == CMA_SEP) {
            psx_ui_fill(&s_cv, L->cm.x + px(8.0f), y + px(2.0f), L->cm.w - px(16.0f), 1, 0x28FFFFFFu);
            continue;
        }
        const int h = px(17.0f);
        if (i == s_cm_hover)
            psx_ui_round_rect(&s_cv, L->cm.x + px(3.0f), y, L->cm.w - px(6.0f), h, U_R_BOX * s_u, COL_BTN_ON);
        Rect r = { L->cm.x + px(10.0f), y, L->cm.w - px(20.0f), h };
        text_in(&r, 0, s_cm[i].label, s_cm[i].action == CMA_DELETE ? COL_WARN : COL_TEXT, face_body());
    }
}

/* Build the menu for whatever is under the pointer. */
static void right_click(int x, int y)
{
    const Layout *L = &s_L;
    char buf[96];
    s_cm_n = 0;
    s_cm_x = x;
    s_cm_y = y;

    const int p = pane_at(x, y);
    const int row = p >= 0 ? row_at(p, x, y) : -1;

    if (p == PANE_MK && row >= 0 && row < s_mk_n) {
        const MakeRow *m = &s_mk[row];
        if (m->kind == MK_EQUIP) {
            /* the row is an equip pairing: from the monster's side the partner
             * is the equip, from the equip's side the partner is the monster */
            const int equip = is_equip_id(s_sel) ? s_sel : m->partner;
            const int mon   = is_equip_id(s_sel) ? m->partner : s_sel;
            snprintf(buf, sizeof buf, "%.24s no longer fits %.24s", nm(equip), nm(mon));
            cm_add(buf, CMA_EQUIP_DEL, (uint16_t)equip, (uint16_t)mon);
            cm_add("", CMA_SEP, 0, 0);
        } else {
            snprintf(buf, sizeof buf, "Change what %.20s + %.20s makes" S_ELLIP, nm(s_sel), nm(m->partner));
            cm_add(buf, CMA_CHANGE, s_sel, m->partner);
            cm_add("Delete this fusion", CMA_DELETE, s_sel, m->partner);
            cm_add("", CMA_SEP, 0, 0);
        }
        cm_add("Add a fusion" S_ELLIP, CMA_ADD, s_sel, 0);
        if (is_equip_id(s_sel)) {
            cm_add("Batch edit usable monsters" S_ELLIP, CMA_EQUIP_BATCH, s_sel, 0);
            snprintf(buf, sizeof buf, "Add a monster %.30s fits" S_ELLIP, nm(s_sel));
            cm_add(buf, CMA_EQUIP_ADD, s_sel, 0);
            PsxCardPack pk;
            if (psx_card_packs_get(s_sel, &pk) && (pk.equips_set || pk.equip_types))
                cm_add("Equip list back to the disc's own", CMA_EQUIP_STOCK, s_sel, 0);
        } else if (is_monster_id(s_sel)) {
            snprintf(buf, sizeof buf, "Add an equip that fits %.28s" S_ELLIP, nm(s_sel));
            cm_add(buf, CMA_EQUIP_ADD, s_sel, 0);
        }
        snprintf(buf, sizeof buf, "Delete all %d fusions for %.28s" S_ELLIP, s_mk_n, nm(s_sel));
        cm_add(buf, CMA_CLEARCARD, s_sel, 0);
        cm_add("Delete every fusion in the game" S_ELLIP, CMA_CLEARALL, 0, 0);
        cm_add("", CMA_SEP, 0, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(m->partner));
        cm_add(buf, CMA_GOTO, m->partner, 0);
        if (m->result != m->partner) {
            snprintf(buf, sizeof buf, "Go to %.40s", nm(m->result));
            cm_add(buf, CMA_GOTO, m->result, 0);
        }
    } else if (p == PANE_FR && row >= 0 && row < s_fr_n) {
        const PsxFusionRecipe *r = &s_rec[s_fr[row]];
        snprintf(buf, sizeof buf, "Change what %.20s + %.20s makes" S_ELLIP, nm(r->a), nm(r->b));
        cm_add(buf, CMA_CHANGE, r->a, r->b);
        cm_add("Delete this fusion", CMA_DELETE, r->a, r->b);
        cm_add("", CMA_SEP, 0, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(r->a));
        cm_add(buf, CMA_GOTO, r->a, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(r->b));
        cm_add(buf, CMA_GOTO, r->b, 0);
    } else if (p == PANE_LIST && s_view == VIEW_RECIPES && row >= 0 && row < s_ridx_n) {
        const PsxFusionRecipe *r = &s_rec[s_ridx[row]];
        snprintf(buf, sizeof buf, "Change what %.20s + %.20s makes" S_ELLIP, nm(r->a), nm(r->b));
        cm_add(buf, CMA_CHANGE, r->a, r->b);
        cm_add("Delete this fusion", CMA_DELETE, r->a, r->b);
        cm_add("", CMA_SEP, 0, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(r->a));
        cm_add(buf, CMA_GOTO, r->a, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(r->b));
        cm_add(buf, CMA_GOTO, r->b, 0);
        snprintf(buf, sizeof buf, "Go to %.40s", nm(r->r));
        cm_add(buf, CMA_GOTO, r->r, 0);
    } else if (p == PANE_LIST && s_view == VIEW_CARD && row >= 0 && row < s_order_n) {
        const int id = s_order[row];
        snprintf(buf, sizeof buf, "Add a fusion for %.40s" S_ELLIP, nm(id));
        cm_add(buf, CMA_ADD, id, 0);
        if (is_equip_id(id)) {
            cm_add("Batch edit usable monsters" S_ELLIP, CMA_EQUIP_BATCH, id, 0);
            snprintf(buf, sizeof buf, "Add a monster %.30s fits" S_ELLIP, nm(id));
            cm_add(buf, CMA_EQUIP_ADD, id, 0);
        } else if (is_monster_id(id)) {
            snprintf(buf, sizeof buf, "Add an equip that fits %.28s" S_ELLIP, nm(id));
            cm_add(buf, CMA_EQUIP_ADD, id, 0);
        }
        if (s_n_makes[id]) {
            snprintf(buf, sizeof buf, "Delete every fusion for %.36s" S_ELLIP, nm(id));
            cm_add(buf, CMA_CLEARCARD, id, 0);
        }
        cm_add("", CMA_SEP, 0, 0);
        cm_add("Delete every fusion in the game" S_ELLIP, CMA_CLEARALL, 0, 0);
    } else if (s_sel >= 1 && (in_rect(&L->mk, x, y) || in_rect(&L->mk_edit, x, y))) {
        cm_add("Add a fusion" S_ELLIP, CMA_ADD, s_sel, 0);
        if (is_equip_id(s_sel))
            cm_add("Batch edit usable monsters" S_ELLIP, CMA_EQUIP_BATCH, s_sel, 0);
        cm_add("Delete every fusion in the game" S_ELLIP, CMA_CLEARALL, 0, 0);
    } else {
        return;                          /* nothing sensible to offer here */
    }
    s_cm_open = 1;
    s_cm_hover = -1;
    layout_compute();
    s_dirty = 1;
}

static void draw_dialog(void)
{
    const Layout *L = &s_L;
    if (!s_dlg) return;
    /* dim what is behind it, so it reads as modal rather than as a panel */
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, 0xA0080A10u);
    psx_ui_round_rect_shadow(&s_cv, L->dlg.x, L->dlg.y, L->dlg.w, L->dlg.h, U_R_PANEL * s_u, 0x80000000u, px(14.0f));
    psx_ui_round_rect(&s_cv, L->dlg.x, L->dlg.y, L->dlg.w, L->dlg.h, U_R_PANEL * s_u, COL_PANEL);
    psx_ui_round_rect_line(&s_cv, L->dlg.x, L->dlg.y, L->dlg.w, L->dlg.h, U_R_PANEL * s_u, 0x30FFFFFFu, 1.0f * s_u);

    const int n = psx_fusion_table_edit_count();
    const int x = L->dlg.x + px(14.0f), wmax = L->dlg.w - px(28.0f);
    int y = L->dlg.y + px(12.0f);

    char head[192], l1[224], l2[224];
    const char *okl = "Restore stock", *cancell = "Cancel";
    if (s_dlg == DLG_CLEAR) {
        okl = "Delete all";
        snprintf(head, sizeof head, "Delete every fusion in the game?");
        snprintf(l1, sizeof l1, "All %d recipes go. Nothing will combine until you add or import some.", s_rec_n);
        snprintf(l2, sizeof l2, "Restore stock brings the game's own table back; your edits are backed up first.");
    } else if (s_dlg == DLG_CLEARCARD) {
        okl = "Delete them";
        snprintf(head, sizeof head, "Delete every fusion for %s?", nm(s_dlg_card));
        snprintf(l1, sizeof l1, "%d recipe%s this card takes part in will be removed.",
                 s_mk_n, s_mk_n == 1 ? "" : "s");
        snprintf(l2, sizeof l2, "Equips are not affected, and Restore stock still undoes the lot.");
    } else if (s_dlg == DLG_CLEAR_EQUIPS) {
        okl = "Clear equips";
        snprintf(head, sizeof head, "Clear every standard equip recipe?");
        snprintf(l1, sizeof l1, "Every Equip card will explicitly fit zero monsters.");
        snprintf(l2, sizeof l2, "Fusion recipes and other card edits stay; each equip's batch editor can restore or replace its list.");
    } else {
        snprintf(head, sizeof head, "Restore the game's own fusion table?");
        if (n || psx_fusion_table_cleared()) {
            snprintf(l1, sizeof l1, "%d edit%s will be dropped and MODS " S_ARROW " Fusion edits switched off.",
                     n, n == 1 ? "" : "s");
            snprintf(l2, sizeof l2, "They are copied to fusion_edits_backup.txt first " S_DASH " Import brings them back.");
        } else {
            snprintf(l1, sizeof l1, "There are no edits. This only switches the override off.");
            l2[0] = 0;
        }
    }
    Rect t = { x, y, wmax, px(16.0f) };
    text_in(&t, 0, head, COL_TEXT, face_title());
    y += px(20.0f);
    Rect a = { x, y, wmax, px(14.0f) };
    text_in(&a, 0, l1, COL_WARN, face_small());
    if (l2[0]) { Rect b = { x, y + px(15.0f), wmax, px(14.0f) }; text_in(&b, 0, l2, COL_DIM, face_small()); }

    draw_button(&L->dlg_cancel, cancell, 0, s_hover_dlg == 0);
    draw_button(&L->dlg_ok, okl, s_hover_dlg == 1, s_hover_dlg == 1);
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    if (s_view == VIEW_CARD) {
        draw_card_list();
        draw_makes();
        draw_from();
    } else {
        draw_recipe_list();
    }
    draw_footer();
    draw_dialog();
    draw_picker();
    draw_context_menu();
}

/* --- equip pairings --------------------------------------------------------
 *
 * Which monsters an equip fits is the equip card's own list in card.ini
 * (`equips = ...`), served to the game by psx_card_effects.c, so an edit here
 * goes through the card pack: the equip's effective list, the monster added
 * or taken out, written back as explicit ids. A list the pack spells as
 * types or "all" is expanded first, and a list past the pack's capacity is
 * refused rather than cut. Nothing here touches the fusion table. */
static void equip_pack_load_or_init(int equip, PsxCardPack *pk)
{
    if (psx_card_packs_get(equip, pk)) return;
    memset(pk, 0, sizeof *pk);
    pk->id = equip;
    pk->attack = pk->defense = pk->star1 = pk->star2 = pk->type =
        pk->level = pk->attribute = pk->price = -1;
    psx_card_packs_effects_reset(pk);
}

/* Save a COMPLETE explicit card-id list. This is the single mutation used by
 * the batch dialog, pair-at-a-time compatibility API, and clear-all action.
 * A byte selection makes duplicate ids unrepresentable; ids are written in
 * stable numeric order for deterministic exports and diffs. */
static int equip_list_save(int equip, const uint8_t selected[MAXID + 1],
                           char *err, unsigned cap)
{
    if (!is_equip_id(equip) && !is_stock_equip_id(equip)) {
        snprintf(err, cap, "%s is not an Equip card", nm(equip));
        return 0;
    }
    PsxCardPack pk;
    equip_pack_load_or_init(equip, &pk);
    pk.equips_set = 1;
    pk.equip_types = 0;
    pk.equip_n = 0;
    for (int id = 1; id <= MAXID; id++) {
        if (!selected[id]) continue;
        if (!is_monster_id(id)) {
            snprintf(err, cap, "%s is not a monster", nm(id));
            return 0;
        }
        if (pk.equip_n >= PSX_CARD_PACK_EQUIP_MAX) {
            snprintf(err, cap, "An equip list holds at most %d monsters", PSX_CARD_PACK_EQUIP_MAX);
            return 0;
        }
        pk.equip_ids[pk.equip_n++] = (uint16_t)id;
    }
    if (!psx_card_packs_save(&pk)) {
        snprintf(err, cap, "Could not write cards/%d/card.ini", equip);
        return 0;
    }
    refresh_index(1);
    rebuild_sel();
    snprintf(err, cap, "%s now fits %d monster%s. Kept in cards/%d/card.ini",
             nm(equip), pk.equip_n, pk.equip_n == 1 ? "" : "s", equip);
    return 1;
}

static int equip_pair_set(int equip, int mon, int on, char *err, unsigned cap)
{
    if (!is_equip_id(equip)) { snprintf(err, cap, "%s is not an Equip card", nm(equip)); return 0; }
    if (!is_monster_id(mon)) { snprintf(err, cap, "%s is not a monster", nm(mon)); return 0; }
    uint8_t selected[MAXID + 1];
    memset(selected, 0, sizeof selected);
    int n = 0, had = 0;
    for (int i = 0; i < s_eq_n; i++) {
        if (s_eq[i].equip != equip) continue;
        if (s_eq[i].mon >= 1 && s_eq[i].mon <= MAXID && !selected[s_eq[i].mon]) {
            selected[s_eq[i].mon] = 1;
            n++;
        }
    }
    had = selected[mon] != 0;
    if (on && had)   { snprintf(err, cap, "%s already fits %s", nm(equip), nm(mon)); return 0; }
    if (!on && !had) { snprintf(err, cap, "%s does not fit %s", nm(equip), nm(mon)); return 0; }
    selected[mon] = on ? 1 : 0;
    if (!equip_list_save(equip, selected, err, cap)) return 0;
    n += on ? 1 : -1;
    if (on) snprintf(err, cap, "%s now fits %s (%d monsters). Kept in cards/%d/card.ini", nm(equip), nm(mon), n, equip);
    else    snprintf(err, cap, "%s no longer fits %s (%d monsters). Kept in cards/%d/card.ini", nm(equip), nm(mon), n, equip);
    return 1;
}

static int equip_clear_all_now(char *err, unsigned cap)
{
    uint8_t none[MAXID + 1];
    memset(none, 0, sizeof none);
    int changed = 0;
    for (int id = 1; id <= MAXID; id++) {
        if (!is_equip_id(id) && !is_stock_equip_id(id)) continue;
        char one[160];
        if (!equip_list_save(id, none, one, sizeof one)) {
            snprintf(err, cap, "Cleared %d Equip card%s, then failed: %s",
                     changed, changed == 1 ? "" : "s", one);
            return 0;
        }
        changed++;
    }
    refresh_index(1);
    rebuild_sel();
    snprintf(err, cap, "Cleared every usable-monster list on %d Equip cards. Fusion recipes and other card edits were preserved.", changed);
    return changed > 0;
}

static int equip_restore_all_now(char *err, unsigned cap)
{
    int changed = 0;
    for (int id = 1; id <= MAXID; id++) {
        PsxCardPack pk;
        if (!psx_card_packs_get(id, &pk) ||
            !(pk.equips_set || pk.equip_types)) continue;
        pk.equips_set = 0;
        pk.equip_types = 0;
        pk.equip_n = 0;
        if (!psx_card_packs_save(&pk)) {
            snprintf(err, cap, "Restored %d Equip card%s, then could not write cards/%d/card.ini",
                     changed, changed == 1 ? "" : "s", id);
            return 0;
        }
        changed++;
    }
    refresh_index(1);
    rebuild_sel();
    snprintf(err, cap, changed ? "Restored the disc's usable-monster lists on %d Equip cards."
                                : "Every Equip card already uses the disc's list.", changed);
    return 1;
}

/* The equip's list back to the disc's own. */
static int equip_stock(int equip, char *err, unsigned cap)
{
    PsxCardPack pk;
    if (!psx_card_packs_get(equip, &pk) || !(pk.equips_set || pk.equip_types)) { snprintf(err, cap, "%s already fits the disc's own monsters", nm(equip)); return 0; }
    pk.equips_set = 0; pk.equip_types = 0; pk.equip_n = 0;
    if (!psx_card_packs_save(&pk)) { snprintf(err, cap, "Could not write cards/%d/card.ini", equip); return 0; }
    refresh_index(1);
    rebuild_sel();
    snprintf(err, cap, "%s fits the disc's own monsters again", nm(equip));
    return 1;
}

int psx_fusion_manager_equip_set(int equip, int mon, int on, char *err, unsigned errcap)
{
    char buf[256];
    refresh_index(0);
    const int ok = equip_pair_set(equip, mon, on, buf, sizeof buf);
    if (err && errcap) snprintf(err, errcap, "%s", buf);
    if (s_win) say(buf);
    return ok;
}

int psx_fusion_manager_equip_replace(int equip, const int *monsters, int count,
                                     char *err, unsigned errcap)
{
    uint8_t selected[MAXID + 1];
    char buf[512];
    memset(selected, 0, sizeof selected);
    refresh_index(0);
    if (count < 0 || count > PSX_CARD_PACK_EQUIP_MAX || (count && !monsters)) {
        snprintf(buf, sizeof buf, "An equip list holds 0 to %d monster IDs", PSX_CARD_PACK_EQUIP_MAX);
        if (err && errcap) snprintf(err, errcap, "%s", buf);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        const int mon = monsters[i];
        if (mon < 1 || mon > MAXID || !is_monster_id(mon)) {
            snprintf(buf, sizeof buf, "Card %d is not a monster", mon);
            if (err && errcap) snprintf(err, errcap, "%s", buf);
            return 0;
        }
        if (selected[mon]) {
            snprintf(buf, sizeof buf, "Monster %d appears more than once", mon);
            if (err && errcap) snprintf(err, errcap, "%s", buf);
            return 0;
        }
        selected[mon] = 1;
    }
    const int ok = equip_list_save(equip, selected, buf, sizeof buf);
    if (err && errcap) snprintf(err, errcap, "%s", buf);
    if (s_win) say(buf);
    return ok;
}

int psx_fusion_manager_equip_clear_all(char *err, unsigned errcap)
{
    char buf[512];
    refresh_index(0);
    const int ok = equip_clear_all_now(buf, sizeof buf);
    if (err && errcap) snprintf(err, errcap, "%s", buf);
    if (s_win) say(buf);
    return ok;
}

int psx_fusion_manager_equip_restore_all(char *err, unsigned errcap)
{
    char buf[512];
    refresh_index(0);
    const int ok = equip_restore_all_now(buf, sizeof buf);
    if (err && errcap) snprintf(err, errcap, "%s", buf);
    if (s_win) say(buf);
    return ok;
}

/* --- editing -------------------------------------------------------------- */

static void ed_clear(void) { s_ed_partner[0] = 0; s_ed_result[0] = 0; s_dirty = 1; }

static void ed_load_row(int row)
{
    if (row < 0 || row >= s_mk_n) return;
    const MakeRow *m = &s_mk[row];
    if (m->kind == MK_EQUIP) { say("An equip pairing: right-click the row to take it away, or the card to add one"); return; }
    snprintf(s_ed_partner, sizeof s_ed_partner, "%d", m->partner);
    snprintf(s_ed_result, sizeof s_ed_result, "%d", m->result);
    s_ed_focus = ED_RESULT;
    s_dirty = 1;
}

static void ed_commit(void)
{
    char err[512];
    if (s_sel < 1) { say("Pick a card first"); return; }
    if (!s_ed_partner[0]) { say("Type the partner's card id"); return; }
    if (!s_ed_result[0]) { say("Type what the pair should make, or 0 to remove the fusion"); return; }
    const int partner = atoi(s_ed_partner), result = atoi(s_ed_result);
    const int sel = s_sel;
    if (!psx_fusion_table_edit(sel, partner, result, err, sizeof err)) { say(err); return; }
    refresh_index(1);
    rebuild_sel();
    if (result == 0) snprintf(err, sizeof err, "%s + %s now makes nothing", nm(sel), nm(partner));
    else             snprintf(err, sizeof err, "%s + %s " S_ARROW " %s", nm(sel), nm(partner), nm(result));
    /* Edits apply on their own now, so this can only mean the player has
     * deliberately switched them off -- worth saying, since the change they
     * just made will not show in a duel until they switch them back on. */
    if (!psx_fusion_table_applied()) {
        const size_t n = strlen(err);
        snprintf(err + n, sizeof err - n, "   " S_DASH "   MODS " S_ARROW " Fusion edits is OFF, so the game is still rolling its own table");
    }
    say(err);
    ed_clear();
    s_ed_focus = ED_PARTNER;
}

static void restore_stock_now(void)
{
    char msg[512];
    psx_fusion_table_restore_stock(msg, sizeof msg);
    refresh_index(1);
    rebuild_sel();
    say(msg);
}

static void dialog_open(int kind) { s_dlg = kind; s_hover_dlg = -1; s_cm_open = 0; s_dirty = 1; }
static void dialog_close(void)    { s_dlg = DLG_NONE; s_hover_dlg = -1; s_dirty = 1; }

static void dialog_confirm(void)
{
    const int kind = s_dlg, card = s_dlg_card;
    char msg[512];
    dialog_close();
    if (kind == DLG_RESTORE) { restore_stock_now(); return; }
    if (kind == DLG_CLEAR) {
        if (!psx_fusion_table_clear_all(msg, sizeof msg)) { say(msg); return; }
    } else if (kind == DLG_CLEARCARD) {
        if (!psx_fusion_table_clear_card(card, msg, sizeof msg)) { say(msg); return; }
    } else if (kind == DLG_CLEAR_EQUIPS) {
        if (!equip_clear_all_now(msg, sizeof msg)) { say(msg); return; }
    } else {
        return;
    }
    refresh_index(1);
    rebuild_sel();
    say(msg);
}

/* --- actions -------------------------------------------------------------- */

#if defined(PSX_SDL3)
/* SDL may call this from another thread: the path goes first and the kind
 * last, since tick() takes the kind as the signal to read both. A dialog
 * that failed to open used to vanish here without a word; now it lands in
 * s_pick_err for tick() to report, and an Export falls back to the default
 * file the way the Card and Drop Table managers' do. */
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

static void export_default_path(char *out, unsigned cap)
{
    snprintf(out, cap, "%s/fusion-recipes.txt", psx_mod_player_data_dir());
}

static void do_export(void)
{
    if (!psx_fusion_table_ready()) { say(waiting_line()); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Text files", "txt" } };
    static char def[1200];
    export_default_path(def, sizeof def);
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)1, s_win, filters, 1, def);
#else
    say("No file dialog in this build: use the debug command fusion_manager with export:<path>");
#endif
}

static void do_import(void)
{
    if (!psx_fusion_table_ready()) { say(waiting_line()); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Recipe lists", "txt;csv" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)2, s_win, filters, 1, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build: use the debug command fusion_manager with import:<path>");
#endif
}

static void set_view(int v)
{
    if (v == s_view) return;
    s_view = v;
    s_scroll = 0;
    s_hover_pane = s_hover_row = -1;
    s_ed_focus = ED_NONE;
    layout_compute();
    if (s_view == VIEW_CARD) s_scroll_pending = 1;
    s_dirty = 1;
}

static void set_sort(int sort)
{
    if (s_sort == sort) s_desc = !s_desc;
    else { s_sort = sort; s_desc = sort == SORT_ATK || sort == SORT_DEF || sort == SORT_MAKES || sort == SORT_FROM; }
    s_scroll = 0;
    rebuild_cards();
    s_scroll_pending = 1;
}

static void set_rsort(int sort)
{
    if (s_rsort == sort) s_rdesc = !s_rdesc;
    else { s_rsort = sort; s_rdesc = sort == RSORT_ATK || sort == RSORT_DEF; }
    s_scroll = 0;
    rebuild_recipes();
}

static void run_button(int b)
{
    switch (b) {
    case BTN_BYCARD:  set_view(VIEW_CARD); break;
    case BTN_RECIPES: set_view(VIEW_RECIPES); break;
    case BTN_IMPORT:  do_import(); break;
    case BTN_EXPORT:  do_export(); break;
    case BTN_CLEAR:   dialog_open(DLG_CLEAR); break;
    case BTN_CLEAR_EQUIPS: dialog_open(DLG_CLEAR_EQUIPS); break;
    case BTN_RESTORE: dialog_open(DLG_RESTORE); break;
    default: break;
    }
}

static void click(int x, int y, int clicks)
{
    const Layout *L = &s_L;
    /* topmost first: the menu floats over the chooser, which is modal over
     * the window, which the restore dialog is modal over in turn */
    if (s_cm_open) {
        const int i = cm_at(x, y);
        if (i >= 0) cm_run(i); else cm_close();
        return;
    }
    if (s_pick_mode) {
        if (s_pick_mode == PICK_EQUIP_BATCH && in_rect(&L->pick_select, x, y)) {
            pick_batch_filtered(1);
        } else if (s_pick_mode == PICK_EQUIP_BATCH && in_rect(&L->pick_remove, x, y)) {
            pick_batch_filtered(0);
        } else if (s_pick_mode == PICK_EQUIP_BATCH && in_rect(&L->pick_clear, x, y)) {
            pick_batch_clear();
        } else if (s_pick_mode == PICK_EQUIP_BATCH && in_rect(&L->pick_cancel, x, y)) {
            pick_close();
        } else if (s_pick_mode == PICK_EQUIP_BATCH && in_rect(&L->pick_apply, x, y)) {
            pick_batch_apply();
        } else if (in_rect(&L->pick_rows, x, y)) {
            pick_choose(s_pick_scroll + (y - L->pick_rows.y) / L->row_h);
        } else if (!in_rect(&L->pick, x, y)) {
            pick_close();
        }
        return;
    }
    if (s_dlg) {
        if (in_rect(&L->dlg_ok, x, y))          dialog_confirm();
        else if (in_rect(&L->dlg_cancel, x, y)) dialog_close();
        else if (!in_rect(&L->dlg, x, y))       dialog_close();   /* outside = cancel */
        return;
    }
    const int b = button_at(x, y);
    if (b >= 0) { run_button(b); return; }

    if (in_rect(&L->ed_partner, x, y)) { s_ed_focus = ED_PARTNER; s_dirty = 1; return; }
    if (in_rect(&L->ed_result, x, y))  { s_ed_focus = ED_RESULT; s_dirty = 1; return; }
    if (in_rect(&L->search, x, y))     { s_ed_focus = ED_NONE; s_dirty = 1; return; }

    /* column headers sort the table under them */
    if (in_rect(&L->list_hdr, x, y)) {
        if (s_view == VIEW_CARD) {
            static const int MAP[NC_CARD] = { SORT_ID, SORT_NAME, SORT_TYPE, SORT_ATK, SORT_DEF, SORT_MAKES, SORT_FROM };
            set_sort(MAP[col_at(L->c, NC_CARD, x)]);
        } else {
            static const int MAP[NC_REC] = { RSORT_A, RSORT_A, RSORT_B, RSORT_B, RSORT_RESULT, RSORT_RESULT, RSORT_TYPE, RSORT_ATK, RSORT_DEF };
            set_rsort(MAP[col_at(L->p, NC_REC, x)]);
        }
        return;
    }

    const int p = pane_at(x, y);
    if (p < 0) return;
    const int row = row_at(p, x, y);
    if (row < 0) return;

    /* a second click on a FUSES WITH row loads it into the edit line rather
     * than following it a second time */
    if (p == PANE_MK && clicks >= 2) { ed_load_row(row); return; }

    if (p == PANE_LIST) {
        if (s_view == VIEW_CARD) {
            select_card(s_order[row], 1);
        } else {
            /* whichever of the three names was clicked opens in BY CARD */
            const PsxFusionRecipe *r = &s_rec[s_ridx[row]];
            const int c = col_at(L->p, NC_REC, x);
            const int id = c <= 1 ? r->a : (c <= 3 ? r->b : r->r);
            select_card(id, 1);
            set_view(VIEW_CARD);
        }
        return;
    }
    if (p == PANE_MK) {
        const MakeRow *m = &s_mk[row];
        select_card(col_at(L->m, NC_MAKE, x) <= 1 ? m->partner : m->result, 1);
        return;
    }
    const PsxFusionRecipe *r = &s_rec[s_fr[row]];
    select_card(col_at(L->f, NC_FROM, x) <= 1 ? r->a : r->b, 1);
}

static void select_step(int d)
{
    if (s_view != VIEW_CARD || !s_order_n) return;
    int pos = -1;
    for (int i = 0; i < s_order_n; i++) if (s_order[i] == s_sel) { pos = i; break; }
    pos += d;
    if (pos < 0) pos = 0;
    if (pos >= s_order_n) pos = s_order_n - 1;
    select_card(s_order[pos], 0);
    if (pos < s_scroll) set_scroll(PANE_LIST, pos);
    if (pos >= s_scroll + pane_visible(PANE_LIST)) set_scroll(PANE_LIST, pos - pane_visible(PANE_LIST) + 1);
}

static int on_event(const void *evp)
{
    const SDL_Event *raw = (const SDL_Event *)evp;
    SDL_Event adjusted;
    if (!s_win) return 0;
    if (psx_fm_editor_filter_event(PSX_FM_PAGE_FUSIONS, raw, &adjusted)) return 1;
    const SDL_Event *ev = &adjusted;
    const Uint32 id = SDL_GetWindowID(s_win);
    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.windowID != id) return 0;
        layout_compute();
        if (ev->button.button == SDL_BUTTON_RIGHT) {
            if (s_cm_open) cm_close();
            else if (!s_pick_mode && !s_dlg) right_click((int)ev->button.x, (int)ev->button.y);
            return 1;
        }
        if (ev->button.button != SDL_BUTTON_LEFT) return 1;
        /* the bars first: a press on one is never a click on the row behind */
        if (!s_cm_open && !s_dlg && sb_press((int)ev->button.x, (int)ev->button.y))
            return 1;
        click((int)ev->button.x, (int)ev->button.y, (int)ev->button.clicks);
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.windowID != id) return 0;
        if (s_sb_drag) { s_sb_drag = 0; s_dirty = 1; }
        return 1;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        layout_compute();
        const int mx = (int)ev->motion.x, my = (int)ev->motion.y;
        if (s_sb_drag) { sb_drag_to(my); return 1; }
        if (s_cm_open) {
            const int h = cm_at(mx, my);
            if (h != s_cm_hover) { s_cm_hover = h; s_dirty = 1; }
            return 1;
        }
        if (s_pick_mode) {
            /* only while actually over the list: moving the pointer away must
             * not clear the row the keyboard put the highlight on */
            if (in_rect(&s_L.pick_rows, mx, my)) {
                const int h = s_pick_scroll + (my - s_L.pick_rows.y) / s_L.row_h;
                const int v = (h >= 0 && h < pick_total()) ? h : s_pick_hover;
                if (v != s_pick_hover) { s_pick_hover = v; s_dirty = 1; }
            }
            return 1;
        }
        if (s_dlg) {
            const int h = in_rect(&s_L.dlg_ok, mx, my) ? 1 : (in_rect(&s_L.dlg_cancel, mx, my) ? 0 : -1);
            if (h != s_hover_dlg) { s_hover_dlg = h; s_dirty = 1; }
            return 1;
        }
        const int p = pane_at(mx, my);
        const int r = p >= 0 ? row_at(p, mx, my) : -1;
        const int b = button_at(mx, my);
        if (p != s_hover_pane || r != s_hover_row || b != s_hover_btn) {
            s_hover_pane = p; s_hover_row = r; s_hover_btn = b;
            s_dirty = 1;
        }
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
        if (s_dlg || s_cm_open) return 1;
        if (s_pick_mode) {
            int v = s_pick_scroll + (ev->wheel.y > 0 ? -3 : 3);
            const int max = pick_total() - pick_visible();
            if (v > max) v = max;
            if (v < 0) v = 0;
            if (v != s_pick_scroll) { s_pick_scroll = v; s_dirty = 1; }
            return 1;
        }
        int mx = 0, my = 0;
#if defined(PSX_SDL3)
        mx = (int)ev->wheel.mouse_x; my = (int)ev->wheel.mouse_y;
#else
        SDL_GetMouseState(&mx, &my);
#endif
        layout_compute();
        int p = pane_at(mx, my);
        if (p < 0) p = PANE_LIST;
        set_scroll(p, *pane_scroll(p) + (ev->wheel.y > 0 ? -3 : 3));
        return 1;
    }
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
        const int ctrl = (ev->key.mod & SDL_KMOD_CTRL) != 0;
#else
        const int key = (int)ev->key.keysym.sym;
        const int ctrl = (ev->key.keysym.mod & KMOD_CTRL) != 0;
#endif
        if (s_cm_open) {
            if (key == SDLK_ESCAPE) cm_close();
            else if (key == SDLK_UP || key == SDLK_DOWN) {
                int i = s_cm_hover;
                do { i += (key == SDLK_DOWN) ? 1 : -1; }
                while (i >= 0 && i < s_cm_n && s_cm[i].action == CMA_SEP);
                if (i < 0) i = 0;
                if (i >= s_cm_n) i = s_cm_n - 1;
                if (s_cm[i].action != CMA_SEP) s_cm_hover = i;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) cm_run(s_cm_hover);
            s_dirty = 1;
            return 1;
        }
        if (s_pick_mode) {
            if (key == SDLK_ESCAPE) pick_close();
            else if (s_pick_mode == PICK_EQUIP_BATCH && ctrl && key == SDLK_a) {
                pick_batch_filtered(1);
            } else if (s_pick_mode == PICK_EQUIP_BATCH && key == SDLK_DELETE) {
                pick_batch_filtered(0);
            }
            else if (key == SDLK_BACKSPACE) {
                const size_t n = strlen(s_pick_search);
                if (n) { s_pick_search[n - 1] = 0; s_pick_scroll = 0; pick_rebuild(); }
            } else if (key == SDLK_UP || key == SDLK_DOWN) {
                int h = s_pick_hover < 0 ? s_pick_scroll - (key == SDLK_DOWN ? 1 : 0) : s_pick_hover;
                h += (key == SDLK_DOWN) ? 1 : -1;
                if (h < 0) h = 0;
                if (h >= pick_total()) h = pick_total() - 1;
                s_pick_hover = h;
                if (h < s_pick_scroll) s_pick_scroll = h;
                if (h >= s_pick_scroll + pick_visible()) s_pick_scroll = h - pick_visible() + 1;
            } else if (key == SDLK_PAGEUP)   { s_pick_scroll -= pick_visible(); if (s_pick_scroll < 0) s_pick_scroll = 0; }
            else if (key == SDLK_PAGEDOWN) {
                const int max = pick_total() - pick_visible();
                s_pick_scroll += pick_visible();
                if (s_pick_scroll > max) s_pick_scroll = max < 0 ? 0 : max;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                pick_choose(s_pick_hover);
            }
            s_dirty = 1;
            return 1;
        }
        if (s_dlg) {
            if (key == SDLK_ESCAPE) dialog_close();
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) dialog_confirm();
            s_dirty = 1;
            return 1;
        }
        if (ctrl && key == SDLK_z) { dialog_open(DLG_RESTORE); s_dirty = 1; return 1; }
        if (key == SDLK_ESCAPE) {
            if (s_ed_focus) { s_ed_focus = ED_NONE; ed_clear(); }
            else if (s_search[0]) { s_search[0] = 0; s_scroll = 0; rebuild_cards(); rebuild_recipes(); }
            else psx_fusion_manager_close();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            ed_commit();
        } else if (key == SDLK_BACKSPACE) {
            if (s_ed_focus) {
                char *v = s_ed_focus == ED_RESULT ? s_ed_result : s_ed_partner;
                const size_t n = strlen(v);
                if (n) v[n - 1] = 0;
            } else {
                /* editing the search first, walking back only once it is
                 * empty -- the footer says so, and the two are never both
                 * wanted */
                const size_t n = strlen(s_search);
                if (n) { s_search[n - 1] = 0; s_scroll = 0; rebuild_cards(); rebuild_recipes(); }
                else go_back();
            }
        } else if (key == SDLK_TAB) {
            if (s_ed_focus) s_ed_focus = s_ed_focus == ED_PARTNER ? ED_RESULT : ED_PARTNER;
            else set_view(s_view == VIEW_CARD ? VIEW_RECIPES : VIEW_CARD);
        } else if (key == SDLK_PAGEUP) {
            set_scroll(PANE_LIST, s_scroll - pane_visible(PANE_LIST));
        } else if (key == SDLK_PAGEDOWN) {
            set_scroll(PANE_LIST, s_scroll + pane_visible(PANE_LIST));
        } else if (key == SDLK_HOME) {
            set_scroll(PANE_LIST, 0);
        } else if (key == SDLK_END) {
            set_scroll(PANE_LIST, list_count());
        } else if (key == SDLK_UP) {
            select_step(-1);
        } else if (key == SDLK_DOWN) {
            select_step(1);
        }
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        if (s_dlg || s_cm_open) return 1;
        if (s_pick_mode) {
            for (const char *p = ev->text.text; *p; p++) {
                if ((unsigned char)*p < 32u || (unsigned char)*p >= 127u) continue;
                const size_t n = strlen(s_pick_search);
                if (n + 1 < sizeof s_pick_search) { s_pick_search[n] = *p; s_pick_search[n + 1] = 0; }
            }
            s_pick_scroll = 0;
            pick_rebuild();
            return 1;
        }
        for (const char *p = ev->text.text; *p; p++) {
            if (s_ed_focus) {
                /* digits only: a stray letter would make atoi read 0 and
                 * quietly delete a fusion instead of setting one */
                if (*p < '0' || *p > '9') continue;
                char *v = s_ed_focus == ED_RESULT ? s_ed_result : s_ed_partner;
                const size_t n = strlen(v);
                if (n + 1 < 5) { v[n] = *p; v[n + 1] = 0; }
                continue;
            }
            /* printable ASCII only, and not the two characters that would
             * have to be escaped to keep the debug server's state line
             * valid JSON -- neither appears in a card name */
            if ((unsigned char)*p >= 32u && (unsigned char)*p < 127u && *p != '"' && *p != '\\') {
                const size_t n = strlen(s_search);
                if (n + 1 < sizeof s_search) { s_search[n] = *p; s_search[n + 1] = 0; }
            }
        }
        if (!s_ed_focus) { s_scroll = 0; rebuild_cards(); rebuild_recipes(); }
        s_dirty = 1;
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_fusion_manager_close();
        return 1;
    case SDL_WINDOWEVENT_EXPOSED:
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
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;
static int           s_ren_software;

static void gl_capture(void) { s_gl_win = SDL_GL_GetCurrentWindow(); s_gl_ctx = SDL_GL_GetCurrentContext(); }
static void gl_restore(void)
{
    if (s_gl_ctx && s_gl_win && SDL_GL_GetCurrentContext() != s_gl_ctx) SDL_GL_MakeCurrent(s_gl_win, s_gl_ctx);
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
    s_u = (float)h / 480.0f;
    if (s_u < 1.0f) s_u = 1.0f;
    if (s_u > 8.0f) s_u = 8.0f;
    s_dirty = 1;
    return 1;
}

static int s_present_fail;
/* Show the canvas. When presenting keeps failing on one backend, the window
 * is redrawn through the other one (the choice is logged). */
static void present_canvas(void)
{
    if (!s_ren || !s_tex) return;
    const int ok = psx_tool_present(s_ren, s_tex, s_px, s_w, s_h, "Fusion Manager");
    gl_restore();
    if (ok) { s_present_fail = 0; return; }
    if (++s_present_fail < 3) { s_dirty = 1; return; }
    psx_tool_log("Fusion Manager: switching to the %s renderer after %d failed presents",
                 s_ren_software ? "accelerated" : "software", s_present_fail);
    gl_capture();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    SDL_DestroyRenderer(s_ren);
    s_ren = psx_tool_renderer_create(s_win, "Fusion Manager", s_ren_software ? 1 : 0, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) { psx_fusion_manager_close(); return; }
    const int w = s_w, h = s_h;
    s_w = s_h = 0;
    if (!ensure_canvas(w, h)) { psx_fusion_manager_close(); return; }
    s_dirty = 1;
}

void psx_fusion_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = psx_fm_editor_acquire(PSX_FM_PAGE_FUSIONS, WIN_W, WIN_H);
    if (!s_win) { host_osd_push("Fusion manager: no window", 2000); return; }
    /* The shared editor owns the minimum. Its normal 720x458 floor keeps the
     * four panes readable, but on a tightly scaled desktop it may lower that
     * floor by a few pixels so the complete decorated window still fits.
     * Reasserting 720x458 here used to undo that adaptive fit for this page
     * and every tab opened after it. */
    gl_capture();
    s_ren = psx_tool_renderer_create(s_win, "Fusion Manager", -1, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) {
        psx_fm_editor_release(PSX_FM_PAGE_FUSIONS); s_win = NULL;
        host_osd_push("Fusion manager: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_fusion_manager_close(); return; }
    if (!s_stats_ready) stats_refresh();
    layout_compute();
    refresh_index(1);
#if defined(PSX_SDL3)
    SDL_StartTextInput(s_win);
#else
    SDL_StartTextInput();
#endif
}

void psx_fusion_manager_close(void)
{
    const int preserve = psx_fm_editor_is_switching();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { psx_fm_editor_release(PSX_FM_PAGE_FUSIONS); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    if (preserve) return;
    s_hover_pane = s_hover_row = s_hover_btn = -1;
    s_ed_focus = ED_NONE;
    s_dlg = DLG_NONE;
    s_cm_open = 0;
    s_pick_mode = PICK_NONE;
    s_sb_drag = 0;
}

int psx_fusion_manager_is_open(void) { return s_win != NULL; }

static void tick(void)
{
    const int req = s_open_req;
    if (req) {
        s_open_req = 0;
        if (req > 0) psx_fusion_manager_open(); else psx_fusion_manager_close();
    }

    /* a file dialog answered, or could not open at all (the callback may
     * run on another thread) */
    if (s_pick_err[0]) {
        char why[200]; snprintf(why, sizeof why, "%s", s_pick_err);
        const int kind = s_pick_err_kind;
        s_pick_err[0] = 0;
        if (kind == 1) {
            char def[1200], msg[sizeof s_msg];
            export_default_path(def, sizeof def);
            psx_fusion_table_export(def, 0, msg, sizeof msg);
            say(msg);
        } else {
            char m[280]; snprintf(m, sizeof m, "The file dialog could not open: %.200s", why);
            say(m);
        }
    }
    if (s_pick_kind) {
        const int kind = s_pick_kind;
        s_pick_kind = 0;
        char p[1200], msg[sizeof s_msg];
        snprintf(p, sizeof p, "%s", s_pick_path);
        if (kind == 1) {
            if (!strstr(p, ".txt")) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".txt"); }
            psx_fusion_table_export(p, 0, msg, sizeof msg);
        } else {
            psx_fusion_table_import(p, msg, sizeof msg);
            refresh_index(1);
            rebuild_sel();
        }
        say(msg);
    }

    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    h = psx_fm_editor_content_height(h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) { if (!ensure_canvas(w, h)) { psx_fusion_manager_close(); return; } }
    layout_compute();
    /* the table module reads the disc on the first frame it can, so the
     * window may well be open before there is anything to show */
    refresh_index(0);
    /* A card renamed in the Card Manager shows here at once: the lists are
     * filtered by name, so they are rebuilt, then redrawn. */
    {
        static unsigned seen_gen = (unsigned)-1;
        const unsigned gen = psx_card_packs_generation();
        if (gen != seen_gen) { seen_gen = gen; rebuild_cards(); rebuild_recipes(); s_dirty = 1; }
    }
    if (s_scroll_pending) { s_scroll_pending = 0; scroll_to_selection(); }
    clamp_scrolls();
    /* the card table comes up with the EXE, which may be after the window */
    if (psx_card_db_ready() != s_stats_seen) {
        s_stats_seen = psx_card_db_ready();
        stats_refresh();
        rebuild_all();
    }
    {
        const int on = (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS) &&
                       ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; if (s_search[0] || s_ed_focus) s_dirty = 1; }
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    if (s_dirty) { draw(); s_dirty = 0; present_canvas(); }
}

/* --- the row ------------------------------------------------------------- */

void psx_fusion_manager_register_menu(void)
{
    /* Kept for source compatibility; FM Editor owns the single VIEW action. */
}

/* --- debug side ---------------------------------------------------------- */

void psx_fusion_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

int psx_fusion_manager_export(const char *path, char *err, unsigned errcap)
{
    return psx_fusion_table_export(path, 0, err, errcap);
}

int psx_fusion_manager_import(const char *path, char *err, unsigned errcap)
{
    const int ok = psx_fusion_table_import(path, err, errcap);
    refresh_index(1);
    rebuild_sel();
    s_dirty = 1;
    return ok;
}

int psx_fusion_manager_edit(int a, int b, int result, char *err, unsigned errcap)
{
    if (a < 1) a = s_sel;
    if (!psx_fusion_table_edit(a, b, result, err, errcap)) return 0;
    refresh_index(1);
    rebuild_sel();
    s_dirty = 1;
    if (err) snprintf(err, errcap, "%d + %d -> %d", a, b, result);
    return 1;
}

void psx_fusion_manager_undo_all(void)
{
    restore_stock_now();
}

int psx_fusion_manager_clear_all(char *err, unsigned errcap)
{
    if (!psx_fusion_table_clear_all(err, errcap)) return 0;
    refresh_index(1);
    rebuild_sel();
    s_dirty = 1;
    return 1;
}

int psx_fusion_manager_clear_card(int id, char *err, unsigned errcap)
{
    if (id < 1) id = s_sel;
    if (!psx_fusion_table_clear_card(id, err, errcap)) return 0;
    refresh_index(1);
    rebuild_sel();
    s_dirty = 1;
    return 1;
}

/* Raise / answer the confirm from a script, so the dialog itself is testable.
 * `ask` 1 opens it, 0 cancels, 2 confirms. */
void psx_fusion_manager_confirm_restore(int ask)
{
    if (ask == 1) dialog_open(DLG_RESTORE);
    else if (ask == 2) dialog_confirm();
    else dialog_close();
}

void psx_fusion_manager_confirm_clear_equips(int ask)
{
    if (ask == 1) dialog_open(DLG_CLEAR_EQUIPS);
    else if (ask == 2 && s_dlg == DLG_CLEAR_EQUIPS) dialog_confirm();
    else dialog_close();
}

int psx_fusion_manager_equip_batch_open(int equip)
{
    if (!s_win || (!is_equip_id(equip) && !is_stock_equip_id(equip))) return 0;
    select_card(equip, 1);
    pick_open(PICK_EQUIP_BATCH, equip, 0);
    s_dirty = 1;
    return 1;
}

static int inject_button(int x, int y, int button, int down, int clicks)
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
    ev.button.clicks = (Uint8)clicks;
    ev.button.x = x; ev.button.y = psx_fm_editor_window_y(y);
    return SDL_PushEvent(&ev) == 1;
}

int psx_fusion_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    if (!inject_button(x, y, button, 1, 1)) return 0;
    return inject_button(x, y, button, 0, 1);
}

/* The halves of a drag: press, then move waypoints, then release -- what the
 * scrollbar thumb needs and a click cannot express. */
int psx_fusion_manager_press(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    return inject_button(x, y, button, 1, 1);
}

int psx_fusion_manager_release(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    return inject_button(x, y, button, 0, 1);
}

int psx_fusion_manager_double_click(int x, int y)
{
    if (!s_win) return 0;
    if (!inject_button(x, y, SDL_BUTTON_LEFT, 1, 2)) return 0;
    return inject_button(x, y, SDL_BUTTON_LEFT, 0, 2);
}

int psx_fusion_manager_move(int x, int y)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(s_win);
    ev.motion.x = x; ev.motion.y = psx_fm_editor_window_y(y);
    return SDL_PushEvent(&ev) == 1;
}

int psx_fusion_manager_inject_key(int keycode)
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

int psx_fusion_manager_inject_text(const char *text)
{
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

int psx_fusion_manager_shot(const char *path)
{
    if (!s_win || !s_px || !path) return 0;
    if (s_dirty) { draw(); s_dirty = 0; }
    FILE *f = psx_fopen_utf8(path, "wb");
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

int psx_fusion_manager_set(int view, int card, int sort, int desc, const char *search)
{
    refresh_index(0);
    if (search) {
        snprintf(s_search, sizeof s_search, "%s", search);
        for (char *q = s_search; *q; q++) if (*q == '"' || *q == '\\') *q = ' ';   /* keep the state line valid JSON */
        s_scroll = 0;
        rebuild_cards();
        rebuild_recipes();
    }
    if (view >= 0) set_view(view ? VIEW_RECIPES : VIEW_CARD);
    if (sort >= 0) {
        /* the same handle drives whichever table the view is showing, so a
         * script sorts "the list" without knowing which one it is */
        if (s_view == VIEW_CARD) { s_sort = sort; s_desc = desc > 0; rebuild_cards(); }
        else                     { s_rsort = sort; s_rdesc = desc > 0; rebuild_recipes(); }
    } else if (desc >= 0) {
        if (s_view == VIEW_CARD) { s_desc = desc > 0; rebuild_cards(); }
        else                     { s_rdesc = desc > 0; rebuild_recipes(); }
    }
    if (card >= 1) { s_sel = 0; select_card(card, 0); }
    s_dirty = 1;
    return s_win != NULL;
}

int psx_fusion_manager_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    if (s_win) layout_compute();
    refresh_index(0);
    const Layout *L = &s_L;
    int used = 0, bcap = 0, pairs = 0;
    psx_fusion_table_budget(&used, &bcap, &pairs);
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"table_ready\":%d,\"duel_live\":%d,\"recipes\":%d,\"equip_cards\":%d,\"equip_links\":%d,"
        "\"edits\":%d,\"cleared\":%d,\"applied\":%d,\"bytes\":%d,\"bytes_max\":%d,\"packed_pairs\":%d,"
        "\"view\":%d,\"search\":\"%s\",\"listed\":%d,\"scroll\":%d,\"sel\":%d,\"sel_name\":\"%s\","
        "\"makes\":%d,\"made_from\":%d,\"sort\":%d,\"desc\":%d,\"rsort\":%d,\"rdesc\":%d,"
        "\"ed_focus\":%d,\"ed_partner\":\"%s\",\"ed_result\":\"%s\",\"dialog\":%d,\"menu\":%d,\"picker\":%d,\"pick_listed\":%d,\"pick_selected\":%d,\"pick_search\":\"%s\","
        "\"canvas\":[%d,%d],\"unit\":%.3f,\"list_rows\":%d,\"hover_pane\":%d,\"hover_row\":%d,\"hover_btn\":%d,\"msg\":\"%s\","
        "\"mk_scroll\":%d,\"fr_scroll\":%d,\"pick_scroll\":%d,\"sb_drag\":%d",
        s_win != NULL, psx_fusion_table_ready(), psx_fusion_db_ready(), s_rec_n, s_eq_groups, s_eq_n,
        psx_fusion_table_edit_count(), psx_fusion_table_cleared(), psx_fusion_table_applied(), used, bcap, pairs,
        s_view, s_search, list_count(), s_scroll, s_sel, nm(s_sel),
        s_mk_n, s_fr_n, s_sort, s_desc, s_rsort, s_rdesc,
        s_ed_focus, s_ed_partner, s_ed_result, s_dlg, s_cm_open ? s_cm_n : 0, s_pick_mode, pick_total(), s_pick_selected_n, s_pick_search,
        s_w, s_h, s_u, s_win ? pane_visible(PANE_LIST) : 0, s_hover_pane, s_hover_row, s_hover_btn, s_msg,
        s_mk_scroll, s_fr_scroll, s_pick_scroll, s_sb_drag);
    if (!s_win || n >= cap) return n < cap;
    n += (unsigned)snprintf(out + n, cap - n,
        ",\"geom\":{\"row_h\":%d,\"search\":[%d,%d,%d,%d],\"list_hdr\":[%d,%d,%d,%d],\"rows\":[%d,%d,%d,%d]",
        L->row_h, L->search.x, L->search.y, L->search.w, L->search.h,
        L->list_hdr.x, L->list_hdr.y, L->list_hdr.w, L->list_hdr.h,
        L->list_rows.x, L->list_rows.y, L->list_rows.w, L->list_rows.h);
    if (n < cap && s_view == VIEW_CARD)
        n += (unsigned)snprintf(out + n, cap - n,
            ",\"makes_rows\":[%d,%d,%d,%d],\"from_rows\":[%d,%d,%d,%d],\"ed_partner_box\":[%d,%d,%d,%d],\"ed_result_box\":[%d,%d,%d,%d]",
            L->mk_rows.x, L->mk_rows.y, L->mk_rows.w, L->mk_rows.h,
            L->fr_rows.x, L->fr_rows.y, L->fr_rows.w, L->fr_rows.h,
            L->ed_partner.x, L->ed_partner.y, L->ed_partner.w, L->ed_partner.h,
            L->ed_result.x, L->ed_result.y, L->ed_result.w, L->ed_result.h);
    for (int b = 0; b < BTN_COUNT && n < cap; b++)
        n += (unsigned)snprintf(out + n, cap - n, ",\"btn%d\":[%d,%d,%d,%d]", b, L->btn[b].x, L->btn[b].y, L->btn[b].w, L->btn[b].h);
    if (n < cap)
        n += (unsigned)snprintf(out + n, cap - n, ",\"dlg_ok\":[%d,%d,%d,%d],\"dlg_cancel\":[%d,%d,%d,%d]",
            L->dlg_ok.x, L->dlg_ok.y, L->dlg_ok.w, L->dlg_ok.h,
            L->dlg_cancel.x, L->dlg_cancel.y, L->dlg_cancel.w, L->dlg_cancel.h);
    if (n < cap)
        n += (unsigned)snprintf(out + n, cap - n, ",\"cm\":[%d,%d,%d,%d],\"pick_rows\":[%d,%d,%d,%d]",
            L->cm.x, L->cm.y, L->cm.w, L->cm.h,
            L->pick_rows.x, L->pick_rows.y, L->pick_rows.w, L->pick_rows.h);
    if (n < cap)
        n += (unsigned)snprintf(out + n, cap - n,
            ",\"pick_select\":[%d,%d,%d,%d],\"pick_remove\":[%d,%d,%d,%d],\"pick_clear\":[%d,%d,%d,%d],\"pick_cancel\":[%d,%d,%d,%d],\"pick_apply\":[%d,%d,%d,%d]",
            L->pick_select.x, L->pick_select.y, L->pick_select.w, L->pick_select.h,
            L->pick_remove.x, L->pick_remove.y, L->pick_remove.w, L->pick_remove.h,
            L->pick_clear.x, L->pick_clear.y, L->pick_clear.w, L->pick_clear.h,
            L->pick_cancel.x, L->pick_cancel.y, L->pick_cancel.w, L->pick_cancel.h,
            L->pick_apply.x, L->pick_apply.y, L->pick_apply.w, L->pick_apply.h);
    /* The scrollbars, so a script can drag a thumb the way a mouse does. */
    if (n < cap)
        n += (unsigned)snprintf(out + n, cap - n,
            ",\"list_sb\":[%d,%d,%d,%d],\"makes_sb\":[%d,%d,%d,%d],\"from_sb\":[%d,%d,%d,%d],\"pick_sb\":[%d,%d,%d,%d]",
            L->list_sb.x, L->list_sb.y, L->list_sb.w, L->list_sb.h,
            L->mk_sb.x, L->mk_sb.y, L->mk_sb.w, L->mk_sb.h,
            L->fr_sb.x, L->fr_sb.y, L->fr_sb.w, L->fr_sb.h,
            L->pick_sb.x, L->pick_sb.y, L->pick_sb.w, L->pick_sb.h);
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "}");
    return n < cap;
}

/* A card's two panels without a window: the read-back that makes the index
 * checkable from a script. Uses the same builders the window does, so a
 * disagreement between the two is impossible. */
int psx_fusion_manager_card_json(char *out, unsigned cap, int card)
{
    if (!out || cap < 256u) return 0;
    refresh_index(0);
    if (card >= 1 && card <= MAXID && card != s_sel) { s_sel = card; rebuild_sel(); }
    if (s_sel < 1) { snprintf(out, cap, "\"card\":0,\"makes\":[],\"made_from\":[]"); return 1; }
    unsigned n = (unsigned)snprintf(out, cap, "\"card\":%d,\"name\":\"%s\",\"makes\":[", s_sel, nm(s_sel));
    for (int i = 0; i < s_mk_n && n < cap; i++) {
        const MakeRow *m = &s_mk[i];
        n += (unsigned)snprintf(out + n, cap - n,
            "%s{\"partner\":%d,\"result\":%d,\"equip\":%d,\"glitch\":%d,\"edited\":%d,\"stock\":%d,\"bonus\":%d}",
            i ? "," : "", m->partner, m->result, m->kind == MK_EQUIP, m->kind == MK_GLITCH, m->edited, m->stock, m->bonus);
    }
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"made_from\":[");
    for (int i = 0; i < s_fr_n && n < cap; i++) {
        const PsxFusionRecipe *r = &s_rec[s_fr[i]];
        n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d]", i ? "," : "", r->a, r->b);
    }
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

PSX_MOD_CONSTRUCTOR(psx_fusion_manager_install)
{
    psx_fusion_manager_register_menu();
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
