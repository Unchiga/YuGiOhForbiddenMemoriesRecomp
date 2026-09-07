/* psx_cpu_manager.c — see psx_cpu_manager.h.
 *
 * The window is the Drop Table Manager's shape on purpose: two panes, the
 * duelists on the left and the selected one's detail on the right, the F10
 * menu's palette and design units, the same number boxes and the same Save /
 * Import / Export. A player who has used one has used this one.
 *
 * DECKS shows the pool the opponent's 40 cards are drawn from, out of 2048,
 * and edits it. The game draws AT MOST THREE copies of any card (measured:
 * a pool with one card at 1984 of 2048 still dealt exactly three of it), so
 * a big weight buys frequency, not a solitaire deck.
 *
 * AI shows the nine bytes gDuel_aOpponentData holds for that duelist, stock
 * beside live, and edits them. Only the first has a name in the
 * decompilation; the rest are shown as themselves rather than guessed at.
 *
 * The record on the title line is the save's own WIN / LOSS, editable in
 * place. It is written to the save struct in RAM, so it counts as a save
 * edit, not a preference -- the window says so when no save is loaded.
 */

#include "psx_cpu_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_tool_window.h"
#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_cpu_data.h"
#include "psx_drop_db.h"
#include "psx_duelist_icon_cache.h"
#include "psx_duelist_icons.h"
#include "psx_duelist_portraits.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

#define WIN_W  1280
#define WIN_H   760

#define COL_BG        0xFF0F1219u
#define COL_BAR       0xFF141826u
#define COL_PANEL     0xFF1E2233u
#define COL_TEXT      0xFFC9CFDDu
#define COL_DIM       0xFF7C8598u
#define COL_ACCENT    0xFF7FA6FFu
#define COL_SEL_BG    0xFF2C3B60u
#define COL_HOVER     0x14FFFFFFu
#define COL_EDIT_BG   0xFF0E1119u
#define COL_EDITED    0xFF8BD48Bu
#define COL_BTN       0xFF2A3147u
#define COL_BTN_ON    0xFF3D5A9Cu
#define COL_TRACK     0x40FFFFFFu
#define COL_THUMB     0xFF7C8598u
#define COL_WARN      0xFFE8C36Au

#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_ROW_H     14.0f
#define U_TITLE_H   18.0f
#define U_HDR_H     16.0f
#define U_BTN_H     17.0f
#define U_FOOT_H    14.0f
#define U_SB_W       4.0f
#define U_ICON      12.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f

#define S_DASH   "\xE2\x80\x93"
#define S_ELLIP  "\xE2\x80\xA6"

#define NDUEL   PSX_DROP_DB_DUELISTS
#define NCARDS  PSX_DROP_DB_CARDS
#define TOTAL   PSX_DROP_DB_TOTAL

enum { VIEW_DECKS = 0, VIEW_AI = 1 };

/* --- canvas --------------------------------------------------------------- */

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint32_t     *s_px;
static int           s_w, s_h;
static int           s_dirty = 1;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;
static int           s_ren_software;
static int           s_present_fail;
static volatile int  s_open_req;

static int px(float u) { return (int)(u * s_u + 0.5f); }
static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }
static int tw(const PsxUiFace *f, const char *s) { return f ? psx_ui_font_text_w(f, s) : 0; }
static int imax(int a, int b) { return a > b ? a : b; }

typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}
static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text(&s_cv, r->x + (r->w - tw(f, s)) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}
static int text_right(int right, int baseline, const char *s, uint32_t col, const PsxUiFace *f)
{
    const int x = right - tw(f, s);
    psx_ui_text(&s_cv, x, baseline, s, col, f);
    return x;
}

/* The duelist's portrait, disc tile first, then the bake or the screen
 * capture -- the Drop Table Manager's own ladder. */
static void draw_icon(int x, int y, int size, int duelist, uint32_t bg)
{
    const uint32_t *disc = psx_duelist_portraits_get(duelist);
    if (disc) {
        psx_ui_blit_scaled(&s_cv, x, y, size, size, (float)px(2.5f), disc, PSX_PORTRAIT_W, PSX_PORTRAIT_W);
        return;
    }
    const uint32_t *src = (duelist >= 0 && duelist < PSX_DUELIST_ICON_N) ? PSX_DUELIST_ICONS[duelist] : 0;
    if (!src) src = psx_duelist_icon_cache_get(duelist);
    if (!src) { psx_ui_round_rect(&s_cv, x, y, size, size, (float)px(2.5f), COL_BTN); return; }
    static uint32_t tmp[PSX_DUELIST_ICON_W * PSX_DUELIST_ICON_H];
    for (int i = 0; i < PSX_DUELIST_ICON_W * PSX_DUELIST_ICON_H; i++)
        tmp[i] = (src[i] >> 24) ? (0xFF000000u | (src[i] & 0x00FFFFFFu)) : bg;
    psx_ui_blit_scaled(&s_cv, x, y, size, size, (float)px(2.5f), tmp, PSX_DUELIST_ICON_W, PSX_DUELIST_ICON_H);
}

/* --- state ---------------------------------------------------------------- */

static int  s_view = VIEW_DECKS;
static int  s_sel;                       /* duelist 0..38 */
static char s_search[32];
static int  s_scroll, s_scroll_right;
static int  s_hover_pane = -1, s_hover_row = -1, s_hover_btn = -1;
static char s_msg[160];
static uint32_t s_msg_until;

/* the right pane's rows: the selected duelist's pool, or every card when
 * ALL CARDS is on -- which is how a card that is NOT in the deck gets added,
 * the same trick the Drop Table Manager's ALL CPU row uses. */
static uint16_t s_card[NCARDS], s_weight[NCARDS];
static int      s_rows_n;
static int      s_all_cards;
static unsigned s_seen_gen;

/* Right-click menu: a handful of actions on whatever is under the pointer.
 * One level, no submenus. */
enum { CM_NONE = 0, CM_EDIT, CM_ADD, CM_REMOVE, CM_DECK_STOCK, CM_AI_STOCK,
       CM_ALL_STOCK, CM_RECORD_CLEAR, CM_SELECT, CM_PORTRAIT, CM_PORTRAIT_STOCK };
#define CMENU_MAX 10
static struct { char label[64]; int action, a, b; } s_cm[CMENU_MAX];
static int s_cm_n, s_cm_x, s_cm_y, s_cm_hover = -1;

/* the number box: which row (or -1), and which field it edits */
enum { ED_NONE = 0, ED_DECK, ED_AI, ED_WINS, ED_LOSSES };
static int  s_edit_kind, s_edit_row;
static char s_edit_buf[8];
static int  s_edit_len;
static int  s_caret_on = 1;

/* the file dialogs' answer */
static char s_pick_path[1200];
static int  s_pick_kind;                 /* 1 export, 2 import, 3 portrait */
static char s_pick_err[200];
static int  s_pick_err_kind;

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 3500u;
    s_dirty = 1;
}

static void edit_end(void) { if (s_edit_kind) { s_edit_kind = ED_NONE; s_edit_row = -1; s_dirty = 1; } }

static int name_matches(const char *name, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *p = name; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void rebuild_rows(void)
{
    uint16_t c[NCARDS], w[NCARDS];
    const int n = psx_cpu_deck_list(s_sel, c, w, NCARDS);
    s_rows_n = 0;
    if (s_all_cards) {
        /* every card, with the pool's weight beside it and 0 for the ones
         * this duelist cannot draw: type a weight on one of those and it
         * joins the deck. */
        for (int id = 1; id <= NCARDS; id++) {
            if (s_search[0] && !name_matches(psx_card_packs_display_name(id), s_search)) continue;
            s_card[s_rows_n] = (uint16_t)id;
            s_weight[s_rows_n] = (uint16_t)psx_cpu_deck_weight(s_sel, id);
            s_rows_n++;
        }
        if (s_scroll_right > s_rows_n - 1) s_scroll_right = 0;
        s_dirty = 1;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (s_search[0] && !name_matches(psx_card_packs_display_name(c[i]), s_search)) continue;
        s_card[s_rows_n] = c[i];
        s_weight[s_rows_n] = w[i];
        s_rows_n++;
    }
    /* Heaviest first: what a deck IS, is its common cards. */
    for (int i = 1; i < s_rows_n; i++) {
        const uint16_t kc = s_card[i], kw = s_weight[i];
        int j = i - 1;
        while (j >= 0 && s_weight[j] < kw) { s_card[j + 1] = s_card[j]; s_weight[j + 1] = s_weight[j]; j--; }
        s_card[j + 1] = kc; s_weight[j + 1] = kw;
    }
    if (s_scroll_right > s_rows_n - 1) s_scroll_right = 0;
    s_dirty = 1;
}

static void invalidate(void) { s_seen_gen = 0; rebuild_rows(); s_dirty = 1; }

/* --- layout --------------------------------------------------------------- */

typedef struct {
    Rect bar, tab_decks, tab_ai, search, btn_save, btn_import, btn_export, btn_default, btn_all;
    Rect pane[2], title[2], cols[2], rows[2], sb[2];
    int  row_h, nrows;
    int  foot_y, foot_h;
    int  d_icon_x, d_name_x, d_name_r, d_rec_x, d_rec_r, d_deck_x, d_deck_r;
    int  r_id_x, r_id_r, r_name_x, r_name_r, r_weight_x, r_weight_r, r_share_x, r_share_r;
    Rect rec_win, rec_loss;              /* the editable record cells */
} Layout;
static Layout s_L;

static int list_rows(void) { return s_L.nrows; }

static void layout_compute(void)
{
    Layout *L = &s_L;
    memset(L, 0, sizeof *L);
    const PsxUiFace *ft = face_title(), *fb = face_bold(), *fr = face_body(), *fs = face_small();
    const int gap = px(U_GAP), pad = px(U_PAD), cg = px(12.0f);
    (void)fr;

    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };
    const int bh = px(U_BTN_H), by = (L->bar.h - bh) / 2;
    int x = px(8.0f) + tw(ft, "CPU Manager") + px(14.0f);
    L->tab_decks = (Rect){ x, by, tw(fb, "Decks") + px(18.0f), bh };  x += L->tab_decks.w + px(4.0f);
    L->tab_ai    = (Rect){ x, by, tw(fb, "AI") + px(18.0f), bh };     x += L->tab_ai.w + px(12.0f);
    int rx = s_w - px(8.0f);
    int w = tw(fb, "Defaults") + px(18.0f);
    L->btn_default = (Rect){ rx - w, by, w, bh }; rx -= w + px(4.0f);
    w = tw(fb, "All cards") + px(18.0f);
    L->btn_all = (Rect){ rx - w, by, w, bh };     rx -= w + px(4.0f);
    w = tw(fb, "Export" S_ELLIP) + px(18.0f);
    L->btn_export = (Rect){ rx - w, by, w, bh };  rx -= w + px(4.0f);
    w = tw(fb, "Import" S_ELLIP) + px(18.0f);
    L->btn_import = (Rect){ rx - w, by, w, bh };  rx -= w + px(4.0f);
    w = tw(fb, "Save") + px(18.0f);
    L->btn_save = (Rect){ rx - w, by, w, bh };    rx -= w + px(10.0f);
    w = rx - x;
    if (w > px(170.0f)) w = px(170.0f);
    if (w < px(50.0f))  w = px(50.0f);
    L->search = (Rect){ x, (L->bar.h - px(16.0f)) / 2, w, px(16.0f) };

    const int top = L->bar.h + gap;
    L->foot_h = px(U_FOOT_H);
    L->foot_y = s_h - gap - L->foot_h;
    const int ph = L->foot_y - gap - top;
    const int avail = s_w - 3 * gap;
    int lw = avail * 42 / 100;
    if (lw < px(230.0f)) lw = px(230.0f);
    if (lw > avail - px(260.0f)) lw = avail - px(260.0f);
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

    {   /* left: portrait, name, record, deck size */
        const int x0 = L->rows[0].x + px(6.0f), xr = L->rows[0].x + L->rows[0].w - px(6.0f);
        L->d_icon_x = x0;
        L->d_name_x = x0 + px(U_ICON) + px(6.0f);
        int cw = imax(tw(fs, "Cards"), tw(fr, "999"));
        L->d_deck_r = xr;                  L->d_deck_x = L->d_deck_r - cw;
        cw = imax(tw(fs, "Record"), tw(fr, "999 - 999"));
        L->d_rec_r = L->d_deck_x - cg;     L->d_rec_x = L->d_rec_r - cw;
        L->d_name_r = L->d_rec_x - cg;
    }
    {   /* right: id, card, weight, share */
        const int x0 = L->rows[1].x + px(6.0f), xr = L->rows[1].x + L->rows[1].w - px(6.0f);
        L->r_id_x = x0; L->r_id_r = x0 + tw(fr, "722");
        int cw = imax(tw(fs, "Share"), tw(fr, "100.0%"));
        L->r_share_r = xr;                     L->r_share_x = L->r_share_r - cw;
        cw = imax(tw(fs, "Weight"), tw(fr, "2048"));
        L->r_weight_r = L->r_share_x - cg;     L->r_weight_x = L->r_weight_r - cw;
        L->r_name_x = L->r_id_r + px(10.0f);   L->r_name_r = L->r_weight_x - cg;
    }
    {   /* The record cells sit in the right pane's title line, each with its
         * label to the left of it: WIN [n]   LOSS [n]. */
        const int h = L->title[1].h;
        const int cw = imax(tw(fb, "999"), tw(fb, "000")) + px(12.0f);
        const int lw = tw(fs, "LOSS") + px(6.0f);
        L->rec_loss = (Rect){ L->title[1].x + L->title[1].w - cw, L->title[1].y, cw, h };
        L->rec_win  = (Rect){ L->rec_loss.x - lw - cw - px(12.0f), L->title[1].y, cw, h };
    }
}

/* --- scrollbars, the pointer half ----------------------------------------- */

static int  s_sb_drag, s_sb_grab;

static int pane_total(int p) { return p == 0 ? NDUEL : (s_view == VIEW_AI ? PSX_CPU_AI_BYTES : s_rows_n); }
static int *pane_scroll(int p) { return p == 0 ? &s_scroll : &s_scroll_right; }

static int sb_thumb(int p, int *ty, int *th)
{
    const int total = pane_total(p), page = list_rows();
    if (total <= page || page <= 0) return 0;
    const Rect *T = &s_L.sb[p];
    int h = T->h * page / total;
    if (h < px(12.0f)) h = px(12.0f);
    if (h > T->h) h = T->h;
    *th = h;
    const int range = total - page;
    *ty = T->y + (range > 0 ? (int)((long long)(T->h - h) * *pane_scroll(p) / range) : 0);
    return 1;
}

static void set_scroll(int p, int v)
{
    const int total = pane_total(p), page = list_rows();
    if (v > total - page) v = total - page;
    if (v < 0) v = 0;
    if (*pane_scroll(p) != v) { *pane_scroll(p) = v; s_dirty = 1; }
}

static int sb_press(int x, int y)
{
    for (int p = 0; p < 2; p++) {
        int ty, th;
        if (!sb_thumb(p, &ty, &th)) continue;
        const Rect *T = &s_L.sb[p];
        const int slack = px(4.0f);
        if (x < T->x - slack || x >= T->x + T->w + slack || y < T->y || y >= T->y + T->h) continue;
        if (y >= ty && y < ty + th) { s_sb_drag = p + 1; s_sb_grab = y - ty; }
        else set_scroll(p, *pane_scroll(p) + (y < ty ? -list_rows() : list_rows()));
        s_dirty = 1;
        return 1;
    }
    return 0;
}

static void sb_drag_to(int y)
{
    const int p = s_sb_drag - 1;
    int ty, th;
    if (p < 0 || !sb_thumb(p, &ty, &th)) return;
    const Rect *T = &s_L.sb[p];
    const int span = T->h - th, total = pane_total(p), page = list_rows();
    if (span <= 0) return;
    int top = y - s_sb_grab - T->y;
    if (top < 0) top = 0;
    if (top > span) top = span;
    set_scroll(p, (int)(((long long)top * (total - page) + span / 2) / span));
}

/* --- drawing --------------------------------------------------------------- */

static void draw_button(const Rect *r, const char *label, int on, int hover)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, on ? COL_BTN_ON : COL_BTN);
    if (hover) psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_HOVER);
    text_centered(r, label, COL_TEXT, face_bold());
}

static void draw_col(int x, int r, int y, int h, const char *label, int right)
{
    const PsxUiFace *fs = face_small();
    const int base = psx_ui_baseline_in(y, h, fs);
    if (right) text_right(r, base, label, COL_DIM, fs);
    else       psx_ui_text(&s_cv, x, base, label, COL_DIM, fs);
}

static void draw_scrollbar(int p)
{
    int ty, th;
    if (!sb_thumb(p, &ty, &th)) return;
    const Rect *T = &s_L.sb[p];
    psx_ui_round_rect(&s_cv, T->x, T->y, T->w, T->h, T->w * 0.5f, COL_TRACK);
    psx_ui_round_rect(&s_cv, T->x, ty, T->w, th, T->w * 0.5f, s_sb_drag == p + 1 ? COL_ACCENT : COL_THUMB);
}

static void draw_bar(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fr = face_body(), *fs = face_small();
    psx_ui_fill(&s_cv, 0, 0, s_w, L->bar.h, COL_BAR);
    psx_ui_fill(&s_cv, 0, L->bar.h - 1, s_w, 1, 0x40FFFFFFu);
    Rect t = { px(8.0f), 0, L->tab_decks.x - px(8.0f), L->bar.h };
    text_in(&t, 0, "CPU Manager", COL_ACCENT, ft);
    draw_button(&L->tab_decks, "Decks", s_view == VIEW_DECKS, s_hover_btn == 0);
    draw_button(&L->tab_ai, "AI", s_view == VIEW_AI, s_hover_btn == 1);

    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, L->search.h * 0.5f, COL_EDIT_BG);
    if (s_search[0]) text_in(&L->search, px(8.0f), s_search, COL_TEXT, fr);
    else             text_in(&L->search, px(8.0f), "Type to search" S_ELLIP, COL_DIM, fr);
    if (s_caret_on && !s_edit_kind) {
        const int cx = L->search.x + px(8.0f) + (s_search[0] ? tw(fr, s_search) : 0) + 1;
        psx_ui_fill(&s_cv, cx, L->search.y + px(3.0f), imax(1, px(1.2f)), L->search.h - px(6.0f), COL_ACCENT);
    }
    draw_button(&L->btn_save, "Save", psx_cpu_dirty(), s_hover_btn == 2);
    draw_button(&L->btn_import, "Import" S_ELLIP, 0, s_hover_btn == 3);
    draw_button(&L->btn_export, "Export" S_ELLIP, 0, s_hover_btn == 4);
    draw_button(&L->btn_all, "All cards", s_all_cards, s_hover_btn == 6);
    draw_button(&L->btn_default, "Defaults", 0, s_hover_btn == 5);
    (void)fs;
}

static void draw_panel(int p, const char *title, uint32_t col, const char *side, uint32_t side_col)
{
    const Layout *L = &s_L;
    const Rect *P = &L->pane[p], *T = &L->title[p];
    const PsxUiFace *fb = face_bold(), *fs = face_small();
    psx_ui_round_rect(&s_cv, P->x, P->y, P->w, P->h, (float)px(U_R_PANEL), COL_PANEL);
    int clip = T->w;
    if (side && side[0]) {
        const int sw = tw(fs, side);
        text_right(T->x + T->w, psx_ui_baseline_in(T->y, T->h, fs), side, side_col, fs);
        clip = T->w - sw - px(10.0f);
    }
    psx_ui_text_clip(&s_cv, T->x, psx_ui_baseline_in(T->y, T->h, fb), title, col, fb, clip);
}

/* The number box, drawn over a value cell. */
static void draw_number_box(const Rect *cell)
{
    const PsxUiFace *fr = face_body();
    const Rect eb = { cell->x - px(6.0f), cell->y + px(1.0f), cell->w + px(12.0f), cell->h - px(2.0f) };
    psx_ui_round_rect(&s_cv, eb.x, eb.y, eb.w, eb.h, (float)px(U_R_BOX), COL_EDIT_BG);
    psx_ui_round_rect_line(&s_cv, eb.x, eb.y, eb.w, eb.h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
    const int ex = psx_ui_text(&s_cv, eb.x + px(5.0f), psx_ui_baseline_in(cell->y, cell->h, fr), s_edit_buf, COL_TEXT, fr);
    if (s_caret_on) psx_ui_fill(&s_cv, ex + 1, eb.y + px(3.0f), imax(1, px(1.2f)), eb.h - px(6.0f), COL_ACCENT);
}

static void draw_duelists(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body();
    const Rect *C = &L->cols[0], *R = &L->rows[0];
    char side[48];
    int edited = 0;
    for (int d = 0; d < NDUEL; d++)
        edited += psx_cpu_deck_edited(d) || psx_cpu_ai_edit(d, NULL) || psx_cpu_portrait_edited(d);
    if (edited) snprintf(side, sizeof side, "%d edited", edited);
    else        snprintf(side, sizeof side, "stock");
    draw_panel(0, "39 duelists", COL_TEXT, side, edited ? COL_EDITED : COL_DIM);
    draw_col(L->d_name_x, 0, C->y, C->h, "Duelist", 0);
    draw_col(0, L->d_rec_r, C->y, C->h, "Record", 1);
    draw_col(0, L->d_deck_r, C->y, C->h, "Cards", 1);

    const int icon = px(U_ICON);
    for (int r = 0; r < L->nrows; r++) {
        const int d = s_scroll + r;
        if (d >= NDUEL) break;
        const int y = R->y + r * L->row_h;
        const int sel = d == s_sel;
        uint32_t bg = COL_PANEL;
        if (sel) { psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_SEL_BG); bg = COL_SEL_BG; }
        else if (s_hover_pane == 0 && r == s_hover_row)
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_HOVER);
        draw_icon(L->d_icon_x, y + (L->row_h - icon) / 2, icon, d, bg);
        if (psx_cpu_portrait_edited(d)) {
            /* a small corner mark: this portrait is the player's */
            const int m = px(3.0f);
            psx_ui_fill(&s_cv, L->d_icon_x + icon - m, y + (L->row_h - icon) / 2, m, m, COL_EDITED);
        }
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        const int mine = psx_cpu_deck_edited(d) || psx_cpu_ai_edit(d, NULL) || psx_cpu_portrait_edited(d);
        psx_ui_text_clip(&s_cv, L->d_name_x, base, PSX_DROP_DB[d].name,
                         sel ? COL_ACCENT : (mine ? COL_EDITED : COL_TEXT), fr, L->d_name_r - L->d_name_x);
        char buf[24];
        int wins = 0, losses = 0;
        if (psx_cpu_record(d, &wins, &losses)) snprintf(buf, sizeof buf, "%d " S_DASH " %d", wins, losses);
        else                                   snprintf(buf, sizeof buf, S_DASH);
        text_right(L->d_rec_r, base, buf, COL_DIM, fr);
        uint16_t c[NCARDS], w[NCARDS];
        snprintf(buf, sizeof buf, "%d", psx_cpu_deck_list(d, c, w, NCARDS));
        text_right(L->d_deck_r, base, buf, psx_cpu_deck_edited(d) ? COL_EDITED : COL_TEXT, fr);
    }
    draw_scrollbar(0);
}

static void draw_record_cells(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fb = face_bold(), *fs = face_small();
    int wins = 0, losses = 0;
    const int have = psx_cpu_record(s_sel, &wins, &losses);
    char buf[16];
    const int lw = tw(fs, "LOSS") + px(6.0f);
    Rect lab = { L->rec_win.x - lw, L->rec_win.y, lw, L->rec_win.h };
    text_in(&lab, 0, "WIN", COL_DIM, fs);
    lab = (Rect){ L->rec_loss.x - lw, L->rec_loss.y, lw, L->rec_loss.h };
    text_in(&lab, 0, "LOSS", COL_DIM, fs);
    /* Both are drawn as input wells, so they read as something to click
     * rather than as a caption. */
    if (s_edit_kind == ED_WINS) draw_number_box(&L->rec_win);
    else {
        psx_ui_round_rect(&s_cv, L->rec_win.x, L->rec_win.y + px(1.0f), L->rec_win.w,
                          L->rec_win.h - px(2.0f), (float)px(U_R_BOX), COL_EDIT_BG);
        snprintf(buf, sizeof buf, have ? "%d" : S_DASH, wins);
        text_centered(&L->rec_win, buf, have ? COL_TEXT : COL_DIM, fb);
    }
    if (s_edit_kind == ED_LOSSES) draw_number_box(&L->rec_loss);
    else {
        psx_ui_round_rect(&s_cv, L->rec_loss.x, L->rec_loss.y + px(1.0f), L->rec_loss.w,
                          L->rec_loss.h - px(2.0f), (float)px(U_R_BOX), COL_EDIT_BG);
        snprintf(buf, sizeof buf, have ? "%d" : S_DASH, losses);
        text_centered(&L->rec_loss, buf, have ? COL_TEXT : COL_DIM, fb);
    }
}

static void draw_deck(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body();
    const Rect *C = &L->cols[1], *R = &L->rows[1];
    char title[64];
    snprintf(title, sizeof title, "%s%s", PSX_DROP_DB[s_sel].name,
             psx_cpu_deck_edited(s_sel) ? " " S_DASH " edited deck" : "");
    draw_panel(1, title, psx_cpu_deck_edited(s_sel) ? COL_EDITED : COL_ACCENT, NULL, COL_DIM);
    draw_record_cells();
    draw_col(L->r_id_x, 0, C->y, C->h, "ID", 0);
    draw_col(L->r_name_x, 0, C->y, C->h, "Card", 0);
    draw_col(0, L->r_weight_r, C->y, C->h, "Weight", 1);
    draw_col(0, L->r_share_r, C->y, C->h, "Share", 1);

    for (int r = 0; r < L->nrows; r++) {
        const int i = s_scroll_right + r;
        if (i >= s_rows_n) break;
        const int y = R->y + r * L->row_h;
        if (s_hover_pane == 1 && r == s_hover_row)
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_HOVER);
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        char buf[24];
        snprintf(buf, sizeof buf, "%d", s_card[i]);
        text_right(L->r_id_r, base, buf, COL_DIM, fr);
        const int stock = psx_cpu_deck_stock_weight(s_sel, s_card[i]);
        psx_ui_text_clip(&s_cv, L->r_name_x, base, psx_card_packs_display_name(s_card[i]),
                         stock == s_weight[i] ? COL_TEXT : COL_EDITED, fr, L->r_name_r - L->r_name_x);
        const Rect cell = { L->r_weight_x, y, L->r_weight_r - L->r_weight_x, L->row_h };
        if (s_edit_kind == ED_DECK && s_edit_row == i) draw_number_box(&cell);
        else {
            snprintf(buf, sizeof buf, "%d", s_weight[i]);
            text_right(L->r_weight_r, base, buf, COL_TEXT, fr);
        }
        const int hundredths = (s_weight[i] * 10000 + TOTAL / 2) / TOTAL;
        snprintf(buf, sizeof buf, "%d.%02d%%", hundredths / 100, hundredths % 100);
        text_right(L->r_share_r, base, buf, COL_DIM, fr);
    }
    if (!s_rows_n) {
        const Rect e = { L->r_name_x, R->y, L->r_share_r - L->r_name_x, L->row_h };
        text_in(&e, 0, s_search[0] ? "No card in this deck matches that" : "This duelist draws nothing", COL_DIM, fr);
    }
    draw_scrollbar(1);
}

static void draw_ai(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fr = face_body(), *fs = face_small();
    const Rect *C = &L->cols[1], *R = &L->rows[1];
    uint8_t live[PSX_CPU_AI_BYTES] = {0}, stock[PSX_CPU_AI_BYTES] = {0};
    const int have_live = psx_cpu_ai_live(s_sel, live);
    const int have_stock = psx_cpu_ai_stock(s_sel, stock);
    const int edited = psx_cpu_ai_edit(s_sel, NULL);
    char title[64];
    snprintf(title, sizeof title, "%s%s", PSX_DROP_DB[s_sel].name, edited ? " " S_DASH " edited AI" : "");
    draw_panel(1, title, edited ? COL_EDITED : COL_ACCENT, NULL, COL_DIM);
    draw_record_cells();
    draw_col(L->r_id_x, 0, C->y, C->h, "#", 0);
    draw_col(L->r_name_x, 0, C->y, C->h, "Field", 0);
    draw_col(0, L->r_weight_r, C->y, C->h, "Value", 1);
    draw_col(0, L->r_share_r, C->y, C->h, "Stock", 1);

    for (int r = 0; r < L->nrows; r++) {
        const int f = s_scroll_right + r;
        if (f >= PSX_CPU_AI_BYTES) break;
        const int y = R->y + r * L->row_h;
        if (s_hover_pane == 1 && r == s_hover_row)
            psx_ui_round_rect(&s_cv, R->x, y, R->w, L->row_h, L->row_h * 0.5f, COL_HOVER);
        const int base = psx_ui_baseline_in(y, L->row_h, fr);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", f);
        text_right(L->r_id_r, base, buf, COL_DIM, fr);
        const int changed = have_live && have_stock && live[f] != stock[f];
        psx_ui_text_clip(&s_cv, L->r_name_x, base, psx_cpu_ai_label(f),
                         changed ? COL_EDITED : COL_TEXT, fr, L->r_name_r - L->r_name_x);
        const Rect cell = { L->r_weight_x, y, L->r_weight_r - L->r_weight_x, L->row_h };
        if (s_edit_kind == ED_AI && s_edit_row == f) draw_number_box(&cell);
        else {
            snprintf(buf, sizeof buf, have_live ? "%d" : S_DASH, live[f]);
            text_right(L->r_weight_r, base, buf, COL_TEXT, fr);
        }
        snprintf(buf, sizeof buf, have_stock ? "%d" : S_DASH, stock[f]);
        text_right(L->r_share_r, base, buf, COL_DIM, fr);
    }
    /* the hint for whatever the pointer is on */
    if (s_hover_pane == 1 && s_hover_row >= 0 && s_scroll_right + s_hover_row < PSX_CPU_AI_BYTES) {
        const Rect h = { L->rows[1].x + px(6.0f), L->rows[1].y + PSX_CPU_AI_BYTES * L->row_h + px(6.0f),
                         L->rows[1].w - px(12.0f), L->row_h };
        text_in(&h, 0, psx_cpu_ai_hint(s_scroll_right + s_hover_row), COL_DIM, fs);
    }
    draw_scrollbar(1);
}

static void draw_footer(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *fs = face_small();
    const Rect f = { L->pane[0].x + px(4.0f), L->foot_y, s_w - 2 * L->pane[0].x - px(8.0f), L->foot_h };
    if (s_msg[0]) { text_in(&f, 0, s_msg, COL_WARN, fs); return; }
    if (psx_cpu_dirty()) {
        text_in(&f, 0, "Unsaved edits. Save writes cpu_manager.ini in your player-data folder; a deck goes back to the disc through a sector override.", COL_WARN, fs);
        return;
    }
    text_in(&f, 0, s_view == VIEW_AI
            ? "Click a value to type a new one, Enter keeps it. Right-click for the menu. These nine bytes are what the duel AI reads about this opponent."
            : (s_all_cards
               ? "Every card: type a weight on one at 0 to add it to the deck, or right-click it. Weights are out of 2048."
               : "Click a weight to type a new one, Enter keeps it. Right-click a row for more, or turn on All cards to add one. At most three copies of a card are ever dealt."),
            COL_DIM, fs);
}

static void draw_cmenu(void);

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    draw_duelists();
    if (s_view == VIEW_AI) draw_ai(); else draw_deck();
    draw_footer();
    draw_cmenu();
}

/* --- the right-click menu -------------------------------------------------- */

static void cm_close(void) { if (s_cm_n) { s_cm_n = 0; s_cm_hover = -1; s_dirty = 1; } }

static void cm_add(const char *label, int action, int a, int b)
{
    if (s_cm_n >= CMENU_MAX) return;
    snprintf(s_cm[s_cm_n].label, sizeof s_cm[0].label, "%s", label);
    s_cm[s_cm_n].action = action;
    s_cm[s_cm_n].a = a;
    s_cm[s_cm_n].b = b;
    s_cm_n++;
}

static int cm_row_h(void) { return s_L.row_h + px(2.0f); }

static Rect cm_rect(void)
{
    const PsxUiFace *fr = face_body();
    int wide = 0;
    for (int i = 0; i < s_cm_n; i++) wide = imax(wide, tw(fr, s_cm[i].label));
    Rect r = { s_cm_x, s_cm_y, wide + 2 * px(U_PAD) + px(6.0f), s_cm_n * cm_row_h() + px(6.0f) };
    if (r.w < px(150.0f)) r.w = px(150.0f);
    if (r.x + r.w > s_w - px(4.0f)) r.x = s_w - px(4.0f) - r.w;
    if (r.y + r.h > s_h - px(4.0f)) r.y = s_h - px(4.0f) - r.h;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

static int cm_item_at(int x, int y)
{
    if (!s_cm_n) return -1;
    const Rect r = cm_rect();
    if (!in_rect(&r, x, y)) return -1;
    const int i = (y - r.y - px(3.0f)) / cm_row_h();
    return (i >= 0 && i < s_cm_n) ? i : -1;
}

static void draw_cmenu(void)
{
    if (!s_cm_n) return;
    const Rect r = cm_rect();
    const PsxUiFace *fr = face_body();
    psx_ui_round_rect_shadow(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_BAR, px(4.0f));
    psx_ui_round_rect(&s_cv, r.x, r.y, r.w, r.h, (float)px(U_R_BOX), COL_BAR);
    for (int i = 0; i < s_cm_n; i++) {
        const Rect row = { r.x + px(3.0f), r.y + px(3.0f) + i * cm_row_h(), r.w - px(6.0f), cm_row_h() };
        if (i == s_cm_hover) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_SEL_BG);
        text_in(&row, px(6.0f), s_cm[i].label, s_cm[i].action == CM_NONE ? COL_DIM : COL_TEXT, fr);
    }
}

/* --- input ----------------------------------------------------------------- */

/* The box opens EMPTY, the way the Drop Table Manager's does: what is typed
 * replaces the value rather than being appended to it. (It once opened with
 * the old number in it, and typing 300 over an 11 asked for 11300.) */
static void edit_begin(int kind, int row, int value)
{
    (void)value;
    s_edit_kind = kind;
    s_edit_row = row;
    s_edit_buf[0] = 0;
    s_edit_len = 0;
    s_dirty = 1;
}

static void edit_commit(void)
{
    if (!s_edit_kind) return;
    const int v = s_edit_len ? atoi(s_edit_buf) : -1;
    const int kind = s_edit_kind, row = s_edit_row;
    edit_end();
    if (v < 0) return;
    if (kind == ED_DECK) {
        if (row < 0 || row >= s_rows_n) return;
        if (!psx_cpu_deck_set(s_sel, s_card[row], v)) {
            say("That weight cannot be balanced into 2048; nothing changed");
            return;
        }
        invalidate();
        say("Deck edited. Save to keep it.");
    } else if (kind == ED_AI) {
        if (!psx_cpu_ai_set(s_sel, row, v > 255 ? 255 : v)) { say("That field could not be set"); return; }
        s_dirty = 1;
        say("AI edited. Save to keep it.");
    } else if (kind == ED_WINS || kind == ED_LOSSES) {
        int wins = 0, losses = 0;
        if (!psx_cpu_record(s_sel, &wins, &losses)) { say("No save is loaded, so there is no record to edit"); return; }
        if (kind == ED_WINS) wins = v; else losses = v;
        psx_cpu_record_set(s_sel, wins, losses);
        s_dirty = 1;
        say("Record written to the save in memory");
    }
}

static void do_portrait(void);

static void cm_run(int i)
{
    if (i < 0 || i >= s_cm_n) return;
    const int action = s_cm[i].action, a = s_cm[i].a, b = s_cm[i].b;
    cm_close();
    switch (action) {
    case CM_SELECT:
        if (a != s_sel) { s_sel = a; s_scroll_right = 0; invalidate(); }
        break;
    case CM_EDIT:
        edit_begin(s_view == VIEW_AI ? ED_AI : ED_DECK, a, 0);
        break;
    case CM_ADD:
        /* a starter weight, the way the drop editor adds a card at 20 */
        if (psx_cpu_deck_set(s_sel, a, b)) {
            invalidate();
            say("Card added to the deck. Save to keep it.");
        } else say("That weight cannot be balanced into 2048");
        break;
    case CM_REMOVE:
        if (psx_cpu_deck_set(s_sel, a, 0)) { invalidate(); say("Card removed from the deck. Save to keep it."); }
        break;
    case CM_DECK_STOCK:
        if (psx_cpu_deck_clear(a)) { invalidate(); say("Deck back to the disc's own. Save to keep it."); }
        else say("That deck is already stock");
        break;
    case CM_AI_STOCK:
        if (psx_cpu_ai_clear(a)) { s_dirty = 1; say("AI back to stock. Save to keep it."); }
        else say("That AI profile is already stock");
        break;
    case CM_ALL_STOCK: {
        const int x = psx_cpu_deck_clear(a), y = psx_cpu_ai_clear(a);
        if (x || y) { invalidate(); say("Deck and AI back to stock. Save to keep it."); }
        else say("That duelist is already stock");
        break;
    }
    case CM_PORTRAIT:
        if (a != s_sel) { s_sel = a; invalidate(); }
        do_portrait();
        break;
    case CM_PORTRAIT_STOCK:
        if (psx_cpu_portrait_clear(a)) { s_dirty = 1; say("Portrait back to the disc's own"); }
        break;
    case CM_RECORD_CLEAR:
        if (psx_cpu_record_set(a, 0, 0)) { s_dirty = 1; say("Record cleared in the save"); }
        else say("No save is loaded");
        break;
    default: break;
    }
}

static int pane_at(int x, int y)
{
    for (int p = 0; p < 2; p++) if (in_rect(&s_L.pane[p], x, y)) return p;
    return -1;
}

static int row_at(int p, int x, int y)
{
    if (p < 0) return -1;
    const Rect *P = &s_L.pane[p], *R = &s_L.rows[p];
    if (x < P->x || x >= P->x + P->w) return -1;
    if (y < R->y || y >= R->y + s_L.nrows * s_L.row_h) return -1;
    return (y - R->y) / s_L.row_h;
}

static int button_at(int x, int y)
{
    const Layout *L = &s_L;
    if (in_rect(&L->tab_decks, x, y))  return 0;
    if (in_rect(&L->tab_ai, x, y))     return 1;
    if (in_rect(&L->btn_save, x, y))   return 2;
    if (in_rect(&L->btn_import, x, y)) return 3;
    if (in_rect(&L->btn_export, x, y)) return 4;
    if (in_rect(&L->btn_default, x, y)) return 5;
    if (in_rect(&L->btn_all, x, y))     return 6;
    return -1;
}

static void export_default_path(char *out, unsigned cap)
{
    char dir[1024];
    psx_cpu_share_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/cpu-duelists.ini", dir);
}

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
    if (!filelist[0]) return;
    s_pick_kind = kind;
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
}
#endif

static void do_export(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "CPU duelists", "ini" } };
    static char def[1200];
    export_default_path(def, sizeof def);
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)1, s_win, filters, 1, def);
#else
    say("No file dialog in this build: use the debug command cpu_data with export:<path>");
#endif
}

static void do_portrait(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Pictures", "png;jpg;jpeg;bmp" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)3, s_win, filters, 1, NULL, false);
#else
    say("No file dialog in this build: use the debug command cpu_data with portrait:<path>");
#endif
}

static void do_import(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "CPU duelists", "ini" } };
    static char dir[1024];
    psx_cpu_share_dir(dir, sizeof dir);
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)2, s_win, filters, 1, dir, false);
#else
    say("No file dialog in this build: use the debug command cpu_data with import:<path>");
#endif
}

static void finish_pick(int kind, const char *path)
{
    char msg[200];
    if (kind == 1) (void)psx_cpu_export_file(path, msg, sizeof msg);
    else if (kind == 3) { (void)psx_cpu_portrait_set(s_sel, path, msg, sizeof msg); s_dirty = 1; }
    else if (psx_cpu_import_file(path, msg, sizeof msg)) invalidate();
    say(msg);
}

/* Right-click: what can be done to whatever is under the pointer. */
static void rclick(int x, int y)
{
    edit_end();
    cm_close();
    const int p = pane_at(x, y);
    const int r = row_at(p, x, y);
    if (r < 0) return;
    s_cm_x = x; s_cm_y = y; s_cm_n = 0; s_cm_hover = -1;
    char buf[64];
    if (p == 0) {
        const int d = s_scroll + r;
        if (d >= NDUEL) return;
        if (d != s_sel) {
            snprintf(buf, sizeof buf, "Show %.24s", PSX_DROP_DB[d].name);
            cm_add(buf, CM_SELECT, d, 0);
        }
        cm_add("Replace the portrait" S_ELLIP, CM_PORTRAIT, d, 0);
        if (psx_cpu_portrait_edited(d)) cm_add("Portrait back to stock", CM_PORTRAIT_STOCK, d, 0);
        cm_add("Deck back to stock", CM_DECK_STOCK, d, 0);
        cm_add("AI back to stock", CM_AI_STOCK, d, 0);
        cm_add("Both back to stock", CM_ALL_STOCK, d, 0);
        cm_add("Clear the win / loss record", CM_RECORD_CLEAR, d, 0);
    } else if (p == 1 && s_view == VIEW_AI) {
        const int f = s_scroll_right + r;
        if (f >= PSX_CPU_AI_BYTES) return;
        cm_add("Type a value", CM_EDIT, f, 0);
        cm_add("AI back to stock", CM_AI_STOCK, s_sel, 0);
    } else if (p == 1) {
        const int i = s_scroll_right + r;
        if (i >= s_rows_n) return;
        if (s_weight[i]) {
            cm_add("Type a weight", CM_EDIT, i, 0);
            cm_add("Remove from the deck", CM_REMOVE, s_card[i], 0);
        } else {
            /* an ALL CARDS row this duelist cannot draw */
            snprintf(buf, sizeof buf, "Add %.24s at 20", psx_card_packs_display_name(s_card[i]));
            cm_add(buf, CM_ADD, s_card[i], 20);
            snprintf(buf, sizeof buf, "Add it at 100");
            cm_add(buf, CM_ADD, s_card[i], 100);
        }
        cm_add("Deck back to stock", CM_DECK_STOCK, s_sel, 0);
    }
    if (s_cm_n) s_dirty = 1;
}

static void click(int x, int y, int button)
{
    const Layout *L = &s_L;
    edit_end();
    if (s_cm_n) { cm_run(cm_item_at(x, y)); cm_close(); return; }
    if (in_rect(&L->bar, x, y)) {
        switch (button_at(x, y)) {
        case 0: if (s_view != VIEW_DECKS) { s_view = VIEW_DECKS; s_scroll_right = 0; invalidate(); } break;
        case 1: if (s_view != VIEW_AI)    { s_view = VIEW_AI;    s_scroll_right = 0; s_dirty = 1; } break;
        case 2: say(psx_cpu_save() ? "Saved" : "Save failed"); break;
        case 3: do_import(); break;
        case 4: do_export(); break;
        case 6: s_all_cards = !s_all_cards; s_scroll_right = 0; invalidate(); break;
        case 5: {
            const int a = psx_cpu_deck_clear(s_sel), b = psx_cpu_ai_clear(s_sel);
            if (a || b) { invalidate(); say("Back to the disc's own deck and AI. Save to keep it."); }
            else say("This duelist is already stock");
            break;
        }
        default: break;
        }
        return;
    }
    if (in_rect(&L->rec_win, x, y))  { int w = 0, l = 0; if (psx_cpu_record(s_sel, &w, &l)) edit_begin(ED_WINS, 0, w); else say("No save is loaded"); return; }
    if (in_rect(&L->rec_loss, x, y)) { int w = 0, l = 0; if (psx_cpu_record(s_sel, &w, &l)) edit_begin(ED_LOSSES, 0, l); else say("No save is loaded"); return; }

    const int p = pane_at(x, y);
    const int r = row_at(p, x, y);
    if (p == 0 && r >= 0) {
        const int d = s_scroll + r;
        if (d < NDUEL && d != s_sel) { s_sel = d; s_scroll_right = 0; invalidate(); }
        return;
    }
    if (p == 1 && r >= 0) {
        if (s_view == VIEW_AI) {
            const int f = s_scroll_right + r;
            uint8_t live[PSX_CPU_AI_BYTES] = {0};
            if (f < PSX_CPU_AI_BYTES && psx_cpu_ai_live(s_sel, live)) edit_begin(ED_AI, f, live[f]);
            return;
        }
        const int i = s_scroll_right + r;
        if (i >= s_rows_n) return;
        edit_begin(ED_DECK, i, s_weight[i]);
    }
}

/* --- the window ------------------------------------------------------------ */

static void gl_capture(void) { s_gl_win = SDL_GL_GetCurrentWindow(); s_gl_ctx = SDL_GL_GetCurrentContext(); }
static void gl_restore(void)
{
    if (s_ren_software) return;
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

static void present(void)
{
    const int ok = psx_tool_present(s_ren, s_tex, s_px, s_w, s_h, "CPU Manager");
    gl_restore();
    if (ok) { s_present_fail = 0; return; }
    if (++s_present_fail < 3) { s_dirty = 1; return; }
    psx_tool_log("CPU Manager: switching to the %s renderer after %d failed presents",
                 s_ren_software ? "accelerated" : "software", s_present_fail);
    gl_capture();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    SDL_DestroyRenderer(s_ren);
    s_ren = psx_tool_renderer_create(s_win, "CPU Manager", s_ren_software ? 1 : 0, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) { psx_cpu_manager_close(); return; }
    s_tex = NULL;
    if (!ensure_canvas(s_w, s_h)) { psx_cpu_manager_close(); return; }
    s_dirty = 1;
}

void psx_cpu_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("CPU Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("CPU manager: no window", 2000); return; }
    gl_capture();
    s_ren = psx_tool_renderer_create(s_win, "CPU Manager", -1, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) { SDL_DestroyWindow(s_win); s_win = NULL; host_osd_push("CPU manager: no renderer", 2000); return; }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_cpu_manager_close(); return; }
    layout_compute();
    invalidate();
    SDL_StartTextInput(s_win);
}

void psx_cpu_manager_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_pane = s_hover_row = s_hover_btn = -1;
    s_sb_drag = 0;
    cm_close();
    edit_end();
}

int psx_cpu_manager_is_open(void) { return s_win != NULL; }
void psx_cpu_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

static void row_activate(void) { psx_cpu_manager_open(); }

void psx_cpu_manager_register_menu(void)
{
    (void)psx_video_menu_add_action(PSX_VM_MENU_VIEW, "CPU manager",
                                    "Decks, AI and records for every opponent", row_activate);
}

static void tick(void)
{
    const int req = s_open_req;
    if (req) { s_open_req = 0; if (req > 0) psx_cpu_manager_open(); else psx_cpu_manager_close(); }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) { if (!ensure_canvas(w, h)) { psx_cpu_manager_close(); return; } }
    if (s_pick_err[0]) {
        char why[200]; snprintf(why, sizeof why, "%s", s_pick_err);
        const int kind = s_pick_err_kind;
        s_pick_err[0] = 0;
        if (kind == 1) { char def[1200]; export_default_path(def, sizeof def); finish_pick(1, def); }
        else { char m[280]; snprintf(m, sizeof m, "The file dialog could not open: %.200s", why); say(m); }
    }
    if (s_pick_path[0]) {
        const int kind = s_pick_kind;
        char path[1200]; snprintf(path, sizeof path, "%s", s_pick_path);
        s_pick_path[0] = 0;
        finish_pick(kind, path);
    }
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; s_dirty = 1; }
    }
    {   /* the edits, the card names and the save's records all move underneath */
        static unsigned seen_packs;
        const unsigned g = psx_cpu_generation(), pg = psx_card_packs_generation();
        if (g != s_seen_gen || pg != seen_packs) { s_seen_gen = g; seen_packs = pg; rebuild_rows(); }
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    static uint32_t last_record_poll;
    if (SDL_GetTicks() - last_record_poll > 500u) { last_record_poll = SDL_GetTicks(); s_dirty = 1; }
    if (!s_dirty) return;
    draw();
    s_dirty = 0;
    present();
}

static void hover_move(int x, int y)
{
    const int p = pane_at(x, y);
    const int r = row_at(p, x, y);
    const int b = button_at(x, y);
    if (p != s_hover_pane || r != s_hover_row || b != s_hover_btn) {
        s_hover_pane = p; s_hover_row = r; s_hover_btn = b;
        s_dirty = 1;
    }
}

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
        if (ev->button.button == SDL_BUTTON_RIGHT) { rclick(x, y); return 1; }
        if (ev->button.button == SDL_BUTTON_LEFT && !s_cm_n && sb_press(x, y)) return 1;
        click(x, y, ev->button.button);
        return 1;
    }
    case SDL_MOUSEBUTTONUP:
        if (ev->button.windowID != id) return 0;
        if (s_sb_drag) { s_sb_drag = 0; s_dirty = 1; }
        return 1;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        layout_compute();
        if (s_sb_drag) { sb_drag_to((int)ev->motion.y); return 1; }
        if (s_cm_n) {
            const int h = cm_item_at((int)ev->motion.x, (int)ev->motion.y);
            if (h != s_cm_hover) { s_cm_hover = h; s_dirty = 1; }
            return 1;
        }
        hover_move((int)ev->motion.x, (int)ev->motion.y);
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
        int mx = 0, my = 0;
#if defined(PSX_SDL3)
        mx = (int)ev->wheel.mouse_x; my = (int)ev->wheel.mouse_y;
#else
        SDL_GetMouseState(&mx, &my);
#endif
        layout_compute();
        int p = pane_at(mx, my);
        if (p < 0) p = 0;
        set_scroll(p, *pane_scroll(p) + (ev->wheel.y > 0 ? -3 : 3));
        return 1;
    }
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (s_edit_kind) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { edit_commit(); return 1; }
            if (key == SDLK_ESCAPE) { edit_end(); return 1; }
            if (key == SDLK_BACKSPACE) { if (s_edit_len) s_edit_buf[--s_edit_len] = 0; s_dirty = 1; return 1; }
            return 1;
        }
        if (s_cm_n) { if (key == SDLK_ESCAPE) cm_close(); return 1; }
        if (key == SDLK_ESCAPE) { psx_cpu_manager_close(); return 1; }
        if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = 0; rebuild_rows(); }
            return 1;
        }
        if (key == SDLK_UP || key == SDLK_DOWN) {
            s_sel += key == SDLK_DOWN ? 1 : -1;
            if (s_sel < 0) s_sel = 0;
            if (s_sel >= NDUEL) s_sel = NDUEL - 1;
            if (s_sel < s_scroll) s_scroll = s_sel;
            if (s_sel >= s_scroll + list_rows()) s_scroll = s_sel - list_rows() + 1;
            s_scroll_right = 0;
            invalidate();
            return 1;
        }
        return 1;
    }
    case SDL_TEXTINPUT: {
        if (ev->text.windowID != id) return 0;
        const char *t = ev->text.text;
        if (s_edit_kind) {
            for (; *t; t++)
                if (*t >= '0' && *t <= '9' && s_edit_len + 1 < (int)sizeof s_edit_buf) {
                    s_edit_buf[s_edit_len++] = *t;
                    s_edit_buf[s_edit_len] = 0;
                }
            s_dirty = 1;
            return 1;
        }
        size_t n = strlen(s_search);
        for (; *t; t++) if (n + 1 < sizeof s_search) { s_search[n++] = *t; s_search[n] = 0; }
        rebuild_rows();
        return 1;
    }
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID == id) { psx_cpu_manager_close(); return 1; }
        return 0;
    default: return 0;
    }
}

/* --- debug surface --------------------------------------------------------- */

int psx_cpu_manager_set(int view, int duelist, const char *search)
{
    if (!s_win) return 0;
    if (view >= 0) s_view = view ? VIEW_AI : VIEW_DECKS;
    if (duelist >= 0 && duelist < NDUEL) {
        s_sel = duelist;
        if (s_sel < s_scroll) s_scroll = s_sel;
        if (s_sel >= s_scroll + list_rows()) s_scroll = s_sel - list_rows() + 1;
    }
    if (search) snprintf(s_search, sizeof s_search, "%s", search);
    layout_compute();
    invalidate();
    return 1;
}

static unsigned rect_json(char *out, unsigned cap, const char *key, const Rect *r)
{
    return (unsigned)snprintf(out, cap, ",\"%s\":[%d,%d,%d,%d]", key, r->x, r->y, r->w, r->h);
}

int psx_cpu_manager_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    if (s_win) layout_compute();
    const Layout *L = &s_L;
    int wins = 0, losses = 0;
    const int have = psx_cpu_record(s_sel, &wins, &losses);
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"view\":\"%s\",\"sel\":%d,\"name\":\"%s\",\"rows\":%d,\"search\":\"%s\","
        "\"record\":[%d,%d],\"has_record\":%d,\"deck_edited\":%d,\"ai_edited\":%d,\"dirty\":%d,"
        "\"canvas\":[%d,%d],\"list_rows\":%d,\"hover\":[%d,%d],\"hover_btn\":%d,\"edit\":%d,"
        "\"edit_row\":%d,\"edit_buf\":\"%s\",\"all_cards\":%d,\"menu\":%d,\"msg\":\"%s\"",
        s_win != NULL, s_view == VIEW_AI ? "ai" : "decks", s_sel, PSX_DROP_DB[s_sel].name,
        s_view == VIEW_AI ? PSX_CPU_AI_BYTES : s_rows_n, s_search,
        wins, losses, have, psx_cpu_deck_edited(s_sel), psx_cpu_ai_edit(s_sel, NULL), psx_cpu_dirty(),
        s_w, s_h, s_win ? list_rows() : 0, s_hover_pane, s_hover_row, s_hover_btn,
        s_edit_kind, s_edit_row, s_edit_buf, s_all_cards, s_cm_n, s_msg);
    if (!s_win || n >= cap) return n < cap;
    n += (unsigned)snprintf(out + n, cap - n, ",\"geom\":{\"row_h\":%d", L->row_h);
    if (n < cap) n += rect_json(out + n, cap - n, "bar", &L->bar);
    if (n < cap) n += rect_json(out + n, cap - n, "tab_decks", &L->tab_decks);
    if (n < cap) n += rect_json(out + n, cap - n, "tab_ai", &L->tab_ai);
    if (n < cap) n += rect_json(out + n, cap - n, "search", &L->search);
    if (n < cap) n += rect_json(out + n, cap - n, "save", &L->btn_save);
    if (n < cap) n += rect_json(out + n, cap - n, "import", &L->btn_import);
    if (n < cap) n += rect_json(out + n, cap - n, "export", &L->btn_export);
    if (n < cap) n += rect_json(out + n, cap - n, "defaults", &L->btn_default);
    if (n < cap) n += rect_json(out + n, cap - n, "all_cards", &L->btn_all);
    if (n < cap) n += rect_json(out + n, cap - n, "left", &L->pane[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right", &L->pane[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "left_rows", &L->rows[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right_rows", &L->rows[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "left_sb", &L->sb[0]);
    if (n < cap) n += rect_json(out + n, cap - n, "right_sb", &L->sb[1]);
    if (n < cap) n += rect_json(out + n, cap - n, "rec_win", &L->rec_win);
    if (n < cap) n += rect_json(out + n, cap - n, "rec_loss", &L->rec_loss);
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n,
        ",\"weight_col\":[%d,%d],\"name_col\":[%d,%d]}",
        L->r_weight_x, L->r_weight_r, L->r_name_x, L->r_name_r);
    return n < cap;
}

int psx_cpu_manager_shot(const char *path)
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
    ev.button.x = x; ev.button.y = y;
    return SDL_PushEvent(&ev) == 1;
}

int psx_cpu_manager_move(int x, int y)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(s_win);
    ev.motion.x = x; ev.motion.y = y;
    return SDL_PushEvent(&ev) == 1;
}

int psx_cpu_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_cpu_manager_move(x, y);
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_cpu_manager_press(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_cpu_manager_move(x, y);
    return inject_button(x, y, button, 1);
}

int psx_cpu_manager_release(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    (void)psx_cpu_manager_move(x, y);
    return inject_button(x, y, button, 0);
}

int psx_cpu_manager_key(int keycode)
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

int psx_cpu_manager_text(const char *text)
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

PSX_MOD_CONSTRUCTOR(psx_cpu_manager_install)
{
    psx_cpu_manager_register_menu();
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
