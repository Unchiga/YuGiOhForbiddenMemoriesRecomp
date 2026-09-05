/* psx_dialogue_manager.c -- see psx_dialogue_manager.h.
 *
 * A reference window beside the game, like the Drop Table Manager: it lists
 * the game's texts (psx_dialogue.c's runs) with their original and current
 * wording, finds one by any word in it, and drives Export... / Import...
 * of the translation file. Editing happens in that file with a text editor;
 * the window is for seeing what is there and what a translation changed.
 *
 * Same skeleton as psx_drop_viewer.c: created on open and destroyed on
 * close, the F10 menu's toolkit and palette, sizes in design units (one
 * 480th of the window height), the software renderer on Windows and the
 * accelerated one elsewhere with the game's GL context put back after every
 * renderer call (PSX_TOOL_RENDERER overrides). */

#include "psx_dialogue_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_dialogue.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

/* --- look ---------------------------------------------------------------- */

#define WIN_W  1400
#define WIN_H   800

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
#define U_HDR_H     16.0f
#define U_BTN_H     17.0f
#define U_FOOT_H    14.0f
#define U_SB_W       4.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f
#define U_DETAIL_H 150.0f          /* the bottom panel with the selected text */

#define S_ELLIP  "\xE2\x80\xA6"

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
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}
static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text(&s_cv, r->x + (r->w - tw(f, s)) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* --- state --------------------------------------------------------------- */

static int   s_open_req;
static char  s_search[64];
static int  *s_order;             /* filtered run indices */
static int   s_order_n, s_order_cap;
static int   s_sel = -1;          /* run index */
static int   s_scroll;            /* first visible row */
static int   s_hover_row = -1, s_hover_btn = -1;
static int   s_detail_scroll;
static char  s_msg[512];
static Uint32 s_msg_until;
static unsigned s_seen_gen = (unsigned)-1;
static int   s_caret_on = 1;

enum { BTN_EXPORT = 0, BTN_IMPORT, BTN_ORIGINAL, BTN_COUNT };
static const char *const BTN_LABEL[BTN_COUNT] = { "Export" S_ELLIP, "Import" S_ELLIP, "Back to original" };

/* the file dialog's answer, consumed on the emulation thread */
static char  s_pick_path[1200];
static int   s_pick_kind;         /* 1 export, 2 import */

typedef struct {
    Rect bar, search, btn[BTN_COUNT], list, hdr, rows, sb, detail, foot;
    int  row_h;
    int  c_key, c_ids, c_orig, c_cur, c_end;   /* column x positions */
} Layout;
static Layout s_L;

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 6000u;
    s_dirty = 1;
}

/* --- the list ------------------------------------------------------------ */

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k]) {
            char a = p[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

static void rebuild_order(void)
{
    const int n = psx_dialogue_count();
    if (n > s_order_cap) { s_order_cap = n; s_order = (int *)realloc(s_order, sizeof(int) * (size_t)n); }
    s_order_n = 0;
    for (int i = 0; i < n; i++) {
        PsxDialogueRun r;
        if (!psx_dialogue_run(i, &r) || !r.story) continue;
        if (s_search[0]) {
            char key[16]; snprintf(key, sizeof key, "%04X", r.key);
            int hit = ci_contains(key, s_search) || ci_contains(r.plain_stock, s_search) || ci_contains(r.plain_current, s_search);
            for (int k = 0; !hit && k < r.nids; k++) { char id[16]; snprintf(id, sizeof id, "%d", r.ids[k]); hit = !strcmp(id, s_search); }
            if (!hit) continue;
        }
        s_order[s_order_n++] = i;
    }
    if (s_scroll > s_order_n - 1) s_scroll = s_order_n > 0 ? s_order_n - 1 : 0;
    if (s_scroll < 0) s_scroll = 0;
    s_dirty = 1;
}

/* One line of a text for a list cell: newlines become " / ", control codes stay. */
static void flatten(const char *s, char *out, unsigned cap)
{
    unsigned n = 0;
    for (const char *p = s; *p && n + 4 < cap; p++) {
        if (*p == '\n') { if (n + 3 < cap) { out[n++] = ' '; out[n++] = '/'; out[n++] = ' '; } }
        else out[n++] = *p;
    }
    out[n] = 0;
}

static void ids_text(const PsxDialogueRun *r, char *out, unsigned cap)
{
    unsigned n = 0; out[0] = 0;
    for (int k = 0; k < r->nids && n + 8 < cap; k++) n += (unsigned)snprintf(out + n, cap - n, "%s%d", k ? " " : "", r->ids[k]);
    if (!r->nids) snprintf(out, cap, "-");
}

/* --- layout -------------------------------------------------------------- */

static void layout_compute(void)
{
    Layout *L = &s_L;
    const int gap = px(U_GAP), pad = px(U_PAD);
    L->row_h = px(U_ROW_H);
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };
    /* right-aligned buttons, then the search box left of them */
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
    const int det_h = px(U_DETAIL_H);
    L->detail = (Rect){ pad, L->foot.y - gap - det_h, s_w - pad * 2, det_h };
    L->list = (Rect){ pad, L->bar.h + gap, s_w - pad * 2, L->detail.y - gap - (L->bar.h + gap) };
    L->hdr = (Rect){ L->list.x, L->list.y, L->list.w, px(U_HDR_H) };
    L->rows = (Rect){ L->list.x, L->hdr.y + L->hdr.h, L->list.w - px(U_SB_W) - gap, L->list.h - L->hdr.h };
    L->sb = (Rect){ L->list.x + L->list.w - px(U_SB_W), L->rows.y, px(U_SB_W), L->rows.h };
    L->c_key = L->rows.x + px(6.0f);
    L->c_ids = L->c_key + px(44.0f);
    L->c_orig = L->c_ids + px(70.0f);
    L->c_end = L->rows.x + L->rows.w - px(6.0f);
    L->c_cur = L->c_orig + (L->c_end - L->c_orig) / 2;
}

static int list_rows(void) { const int n = s_L.rows.h / (s_L.row_h > 0 ? s_L.row_h : 1); return n > 0 ? n : 1; }

static int row_at(int x, int y)
{
    if (!in_rect(&s_L.rows, x, y)) return -1;
    const int r = s_scroll + (y - s_L.rows.y) / s_L.row_h;
    return r < s_order_n ? r : -1;
}

static int button_at(int x, int y)
{
    for (int b = 0; b < BTN_COUNT; b++) if (in_rect(&s_L.btn[b], x, y)) return b;
    return -1;
}

static void set_scroll(int v)
{
    const int max = s_order_n - list_rows();
    if (v > max) v = max;
    if (v < 0) v = 0;
    if (v != s_scroll) { s_scroll = v; s_dirty = 1; }
}

/* --- drawing ------------------------------------------------------------- */

static void draw_button(const Rect *r, const char *label, int hover)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, U_R_BOX * s_u, hover ? COL_BTN_ON : COL_BTN);
    text_centered(r, label, COL_TEXT, face_body());
}

static void draw_bar(void)
{
    const Layout *L = &s_L;
    psx_ui_fill(&s_cv, L->bar.x, L->bar.y, L->bar.w, L->bar.h, COL_BAR);
    char title[128];
    if (psx_dialogue_ready()) {
        int n = 0, tr = 0;
        for (int i = 0; i < psx_dialogue_count(); i++) { PsxDialogueRun r; if (psx_dialogue_run(i, &r) && r.story) { n++; tr += r.translated; } }
        snprintf(title, sizeof title, "Dialogue Manager   %d texts, %d translated", n, tr);
    }
    else snprintf(title, sizeof title, "Dialogue Manager");
    psx_ui_text(&s_cv, px(U_PAD), psx_ui_baseline_in(L->bar.y, L->bar.h, face_title()), title, COL_TEXT, face_title());
    /* search box */
    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, U_R_BOX * s_u, COL_EDIT_BG);
    if (s_search[0]) {
        char shown[80]; snprintf(shown, sizeof shown, "%s%s", s_search, s_caret_on ? "|" : "");
        text_in(&L->search, px(5.0f), shown, COL_TEXT, face_body());
    } else text_in(&L->search, px(5.0f), "Type to search", COL_DIM, face_body());
    for (int b = 0; b < BTN_COUNT; b++) draw_button(&L->btn[b], BTN_LABEL[b], s_hover_btn == b);
}

static void draw_col(int x, int right, int y, int h, const char *label)
{
    Rect r = { x, y, right - x, h };
    text_in(&r, 0, label, COL_DIM, face_small());
}

static void draw_list(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->list.x, L->list.y, L->list.w, L->list.h, U_R_PANEL * s_u, COL_PANEL);
    draw_col(L->c_key, L->c_ids, L->hdr.y, L->hdr.h, "Where");
    draw_col(L->c_ids, L->c_orig, L->hdr.y, L->hdr.h, "Ids");
    draw_col(L->c_orig, L->c_cur, L->hdr.y, L->hdr.h, "Original");
    draw_col(L->c_cur, L->c_end, L->hdr.y, L->hdr.h, "Current");
    if (!psx_dialogue_ready()) {
        Rect r = { L->rows.x, L->rows.y, L->rows.w, L->row_h * 2 };
        text_in(&r, px(6.0f), "Waiting for the game to load its text" S_ELLIP, COL_DIM, face_body());
        return;
    }
    const int nrows = list_rows();
    static char flat[512], flat2[512], ids[128], key[16];
    for (int i = 0; i < nrows && s_scroll + i < s_order_n; i++) {
        const int row = s_scroll + i;
        PsxDialogueRun r;
        if (!psx_dialogue_run(s_order[row], &r)) continue;
        const int y = L->rows.y + i * L->row_h;
        if (s_order[row] == s_sel) psx_ui_round_rect(&s_cv, L->rows.x, y, L->rows.w, L->row_h, U_R_BOX * s_u, COL_SEL_BG);
        else if (row == s_hover_row) psx_ui_round_rect(&s_cv, L->rows.x, y, L->rows.w, L->row_h, U_R_BOX * s_u, COL_HOVER);
        snprintf(key, sizeof key, "@%04X", r.key);
        ids_text(&r, ids, sizeof ids);
        flatten(r.plain_stock, flat, sizeof flat);
        flatten(r.plain_current, flat2, sizeof flat2);
        const uint32_t col = r.translated ? COL_EDITED : COL_TEXT;
        Rect c1 = { L->c_key, y, L->c_ids - L->c_key - px(4.0f), L->row_h };  text_in(&c1, 0, key, COL_DIM, face_body());
        Rect c2 = { L->c_ids, y, L->c_orig - L->c_ids - px(4.0f), L->row_h }; text_in(&c2, 0, ids, COL_DIM, face_body());
        Rect c3 = { L->c_orig, y, L->c_cur - L->c_orig - px(6.0f), L->row_h }; text_in(&c3, 0, flat, COL_TEXT, face_body());
        Rect c4 = { L->c_cur, y, L->c_end - L->c_cur, L->row_h };             text_in(&c4, 0, flat2, col, r.translated ? face_bold() : face_body());
    }
    /* scrollbar */
    if (s_order_n > nrows) {
        psx_ui_round_rect(&s_cv, L->sb.x, L->sb.y, L->sb.w, L->sb.h, U_SB_W * s_u, COL_TRACK);
        const int th = L->sb.h * nrows / s_order_n;
        const int ty = L->sb.y + (L->sb.h - th) * s_scroll / (s_order_n - nrows);
        psx_ui_round_rect(&s_cv, L->sb.x, ty, L->sb.w, th > px(8.0f) ? th : px(8.0f), U_SB_W * s_u, COL_THUMB);
    }
}

/* The selected text in full, original left and current right, one file
 * line per row, scrolled together with the wheel over the panel. */
static void draw_detail(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->detail.x, L->detail.y, L->detail.w, L->detail.h, U_R_PANEL * s_u, COL_PANEL);
    PsxDialogueRun r;
    if (s_sel < 0 || !psx_dialogue_run(s_sel, &r)) {
        Rect t = { L->detail.x, L->detail.y, L->detail.w, px(U_HDR_H) };
        text_in(&t, px(6.0f), "Select a text to read it in full", COL_DIM, face_body());
        return;
    }
    const int half = L->detail.w / 2;
    char head[160], ids[128];
    ids_text(&r, ids, sizeof ids);
    snprintf(head, sizeof head, "Original   [%04X]   ids %s   %d bytes", r.key, ids, r.bytes);
    Rect h1 = { L->detail.x, L->detail.y, half, px(U_HDR_H) };
    Rect h2 = { L->detail.x + half, L->detail.y, half, px(U_HDR_H) };
    text_in(&h1, px(6.0f), head, COL_DIM, face_small());
    text_in(&h2, px(6.0f), r.translated ? "Current (translated)" : "Current (the original)", r.translated ? COL_EDITED : COL_DIM, face_small());
    const int lh = psx_ui_font_line_height(face_body());
    const int top = L->detail.y + px(U_HDR_H), bottom = L->detail.y + L->detail.h - px(3.0f);
    const char *cols[2] = { r.plain_stock, r.plain_current };
    for (int c = 0; c < 2; c++) {
        int y = top, line = 0;
        const int x = L->detail.x + c * half + px(6.0f), w = half - px(12.0f);
        for (const char *p = cols[c]; ; ) {
            const char *e = strchr(p, '\n');
            const size_t len = e ? (size_t)(e - p) : strlen(p);
            if (line >= s_detail_scroll) {
                if (y + lh > bottom) break;
                char buf[512]; snprintf(buf, sizeof buf, "%.*s", (int)(len < 500 ? len : 500), p);
                psx_ui_text_clip(&s_cv, x, y + psx_ui_font_ascent(face_body()), buf, c ? (r.translated ? COL_EDITED : COL_TEXT) : COL_TEXT, face_body(), w);
                y += lh;
            }
            line++;
            if (!e) break;
            p = e + 1;
        }
    }
}

static void draw_footer(void)
{
    const Layout *L = &s_L;
    const char *m = s_msg[0] ? s_msg : "Wheel scrolls, type to search, Escape clears the search or closes. Edit the exported file with a text editor, then Import it.";
    Rect r = { px(U_PAD), L->foot.y, s_w - px(U_PAD) * 2, L->foot.h };
    text_in(&r, 0, m, s_msg[0] ? COL_WARN : COL_DIM, face_small());
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    draw_list();
    draw_detail();
    draw_footer();
}

/* --- actions ------------------------------------------------------------- */

#if defined(PSX_SDL3)
static void SDLCALL pick_cb(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    if (!filelist || !filelist[0]) return;
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
    s_pick_kind = (int)(intptr_t)userdata;
}
#endif

static void do_export(void)
{
    if (!psx_dialogue_ready()) { say("The game's text is not loaded yet"); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Text files", "txt" } };
    static char def[1200];
    snprintf(def, sizeof def, "%s/dialogue-export.txt", psx_mod_player_data_dir());
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)1, s_win, filters, 1, def);
#else
    say("No file dialog in this build: use the debug command dialogue_export");
#endif
}

static void do_import(void)
{
    if (!psx_dialogue_ready()) { say("The game's text is not loaded yet"); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Text files", "txt" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)2, s_win, filters, 1, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build: use the debug command dialogue_import");
#endif
}

static void do_original(void)
{
    if (!psx_dialogue_translated_count()) { say("The game already shows its original text"); return; }
    psx_dialogue_clear();
    say("Back to the original text; the kept translation file was removed");
}

static void run_button(int b)
{
    if (b == BTN_EXPORT) do_export();
    else if (b == BTN_IMPORT) do_import();
    else if (b == BTN_ORIGINAL) do_original();
}

static void click(int x, int y)
{
    const int b = button_at(x, y);
    if (b >= 0) { run_button(b); return; }
    const int row = row_at(x, y);
    if (row >= 0) { s_sel = s_order[row]; s_detail_scroll = 0; s_dirty = 1; }
}

static void select_step(int d)
{
    int pos = -1;
    for (int i = 0; i < s_order_n; i++) if (s_order[i] == s_sel) { pos = i; break; }
    pos += d;
    if (pos < 0) pos = 0;
    if (pos >= s_order_n) pos = s_order_n - 1;
    if (pos < 0) return;
    s_sel = s_order[pos]; s_detail_scroll = 0;
    if (pos < s_scroll) set_scroll(pos);
    if (pos >= s_scroll + list_rows()) set_scroll(pos - list_rows() + 1);
    s_dirty = 1;
}

static int on_event(const void *evp)
{
    const SDL_Event *ev = (const SDL_Event *)evp;
    if (!s_win) return 0;
    const Uint32 id = SDL_GetWindowID(s_win);
    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN: {
        if (ev->button.windowID != id) return 0;
        if (ev->button.button != SDL_BUTTON_LEFT) return 1;
        layout_compute();
        click((int)ev->button.x, (int)ev->button.y);
        return 1;
    }
    case SDL_MOUSEBUTTONUP:
        return ev->button.windowID == id;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        layout_compute();
        const int r = row_at((int)ev->motion.x, (int)ev->motion.y), b = button_at((int)ev->motion.x, (int)ev->motion.y);
        if (r != s_hover_row || b != s_hover_btn) { s_hover_row = r; s_hover_btn = b; s_dirty = 1; }
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int my = (int)ev->wheel.mouse_y;
#else
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
#endif
        layout_compute();
        if (my >= s_L.detail.y) { s_detail_scroll += ev->wheel.y > 0 ? -2 : 2; if (s_detail_scroll < 0) s_detail_scroll = 0; s_dirty = 1; }
        else set_scroll(s_scroll + (ev->wheel.y > 0 ? -3 : 3));
        return 1;
    }
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (key == SDLK_ESCAPE) {
            if (s_search[0]) { s_search[0] = 0; rebuild_order(); }
            else psx_dialogue_manager_close();
        } else if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = 0; s_scroll = 0; rebuild_order(); }
        } else if (key == SDLK_PAGEUP) set_scroll(s_scroll - list_rows());
        else if (key == SDLK_PAGEDOWN) set_scroll(s_scroll + list_rows());
        else if (key == SDLK_UP) select_step(-1);
        else if (key == SDLK_DOWN) select_step(1);
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        for (const char *p = ev->text.text; *p; p++) {
            if ((unsigned char)*p >= 32u && (unsigned char)*p < 127u) {
                const size_t n = strlen(s_search);
                if (n + 1 < sizeof s_search) { s_search[n] = *p; s_search[n + 1] = 0; }
            }
        }
        s_scroll = 0;
        rebuild_order();
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_dialogue_manager_close();
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

static void present_canvas(void)
{
    if (!s_ren || !s_tex) return;
    SDL_UpdateTexture(s_tex, NULL, s_px, s_w * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    gl_restore();
}

void psx_dialogue_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("Dialogue Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("Dialogue manager: no window", 2000); return; }
    gl_capture();
    s_ren = NULL;
    s_ren_software = 0;
    {
        const char *pick = getenv("PSX_TOOL_RENDERER");
#if defined(_WIN32)
        int want_software = 1;
#else
        int want_software = 0;
#endif
        if (pick && *pick) want_software = !strcmp(pick, "software");
#if defined(PSX_SDL3)
        if (pick && *pick && strcmp(pick, "software") && strcmp(pick, "accelerated")) {
            const SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, pick);
            SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, s_win);
            s_ren = SDL_CreateRendererWithProperties(props);
            SDL_DestroyProperties(props);
        } else
#endif
        if (want_software) { s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE); s_ren_software = s_ren != NULL; }
    }
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    gl_restore();
#if defined(PSX_SDL3)
    if (s_ren) fprintf(stderr, "%s: renderer %s%s\n", SDL_GetWindowTitle(s_win), SDL_GetRendererName(s_ren), s_ren_software ? " (window surface)" : "");
#endif
    if (!s_ren) {
        SDL_DestroyWindow(s_win); s_win = NULL;
        host_osd_push("Dialogue manager: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_dialogue_manager_close(); return; }
    layout_compute();
    rebuild_order();
    SDL_StartTextInput(s_win);
}

void psx_dialogue_manager_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_row = s_hover_btn = -1;
}

int psx_dialogue_manager_is_open(void) { return s_win != NULL; }

static void tick(void)
{
    const int req = s_open_req;
    if (req) {
        s_open_req = 0;
        if (req > 0) psx_dialogue_manager_open(); else psx_dialogue_manager_close();
    }
    /* a file dialog answered (the callback may run on another thread) */
    if (s_pick_kind) {
        const int kind = s_pick_kind; s_pick_kind = 0;
        char msg[512];
        if (kind == 1) { char p[1200]; snprintf(p, sizeof p, "%s", s_pick_path); if (!strstr(p, ".txt")) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".txt"); } psx_dialogue_export(p, msg, sizeof msg); }
        else psx_dialogue_import(s_pick_path, msg, sizeof msg);
        say(msg);
    }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) { if (!ensure_canvas(w, h)) { psx_dialogue_manager_close(); return; } }
    const unsigned gen = psx_dialogue_generation() + (psx_dialogue_ready() ? 0x10000u : 0u);
    if (gen != s_seen_gen) { s_seen_gen = gen; rebuild_order(); }
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; if (s_search[0]) s_dirty = 1; }
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    if (s_dirty) { draw(); s_dirty = 0; present_canvas(); }
}

/* --- the row ------------------------------------------------------------- */

static void row_activate(void) { psx_dialogue_manager_open(); }

void psx_dialogue_manager_register_menu(void)
{
    (void)psx_video_menu_add_action(PSX_VM_MENU_VIEW, "Dialogue manager",
        "Export the campaign's dialogue as plain text for translation, and import it back",
        row_activate);
}

/* --- debug side ---------------------------------------------------------- */

void psx_dialogue_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

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

int psx_dialogue_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_dialogue_manager_inject_key(int keycode)
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

int psx_dialogue_manager_inject_text(const char *text)
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

int psx_dialogue_manager_shot(const char *path)
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

int psx_dialogue_manager_set(int key, const char *search)
{
    if (search) { snprintf(s_search, sizeof s_search, "%s", search); s_scroll = 0; rebuild_order(); }
    if (key >= 0) {
        for (int i = 0; i < psx_dialogue_count(); i++) {
            PsxDialogueRun r;
            if (psx_dialogue_run(i, &r) && (int)r.key == key) { s_sel = i; s_detail_scroll = 0; break; }
        }
        /* bring it into view */
        for (int i = 0; i < s_order_n; i++) if (s_order[i] == s_sel) { if (i < s_scroll || i >= s_scroll + list_rows()) set_scroll(i); break; }
    }
    s_dirty = 1;
    return s_win != NULL;
}

int psx_dialogue_manager_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    if (s_win) layout_compute();
    const Layout *L = &s_L;
    PsxDialogueRun r; const int has = s_sel >= 0 && psx_dialogue_run(s_sel, &r);
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"ready\":%d,\"search\":\"%s\",\"listed\":%d,\"scroll\":%d,\"sel\":%d,\"sel_key\":\"%04X\",\"sel_translated\":%d,"
        "\"canvas\":[%d,%d],\"unit\":%.3f,\"list_rows\":%d,\"hover_row\":%d,\"hover_btn\":%d,\"msg\":\"%s\"",
        s_win != NULL, psx_dialogue_ready(), s_search, s_order_n, s_scroll, s_sel, has ? r.key : 0u, has ? r.translated : 0,
        s_w, s_h, s_u, s_win ? list_rows() : 0, s_hover_row, s_hover_btn, s_msg);
    if (!s_win || n >= cap) return n < cap;
    n += (unsigned)snprintf(out + n, cap - n, ",\"geom\":{\"row_h\":%d,\"search\":[%d,%d,%d,%d],\"rows\":[%d,%d,%d,%d],\"detail\":[%d,%d,%d,%d]",
        L->row_h, L->search.x, L->search.y, L->search.w, L->search.h, L->rows.x, L->rows.y, L->rows.w, L->rows.h,
        L->detail.x, L->detail.y, L->detail.w, L->detail.h);
    for (int b = 0; b < BTN_COUNT && n < cap; b++)
        n += (unsigned)snprintf(out + n, cap - n, ",\"btn%d\":[%d,%d,%d,%d]", b, L->btn[b].x, L->btn[b].y, L->btn[b].w, L->btn[b].h);
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "}");
    return n < cap;
}

PSX_MOD_CONSTRUCTOR(psx_dialogue_manager_install)
{
    psx_dialogue_manager_register_menu();
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
