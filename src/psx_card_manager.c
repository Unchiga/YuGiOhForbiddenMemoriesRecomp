/* psx_card_manager.c — see psx_card_manager.h.
 *
 * The window is the front end of psx_card_packs.c and owns no game state of
 * its own: every field it shows is either the stock value read from the
 * game's tables or the player's edit read back from that module, and SAVE
 * goes through psx_card_packs_save(), the same path a hand-edited card.ini
 * takes. The art preview is decoded from the very sectors the game will
 * stream, so what the preview shows is what the password screen draws.
 *
 * DRAWN LIKE THE F10 MENU: the same typeface (psx_ui_font), the same
 * antialiased panels and pills (psx_ui_draw), the same palette, and a layout
 * in DESIGN UNITS (1 unit = 1/480 of the window height) so a bigger window
 * gets more detail rather than bigger blocks. Everything is measured, never
 * assumed: text that would run past its box is wrapped or ellipsised.
 *
 * WINDOW PLUMBING: an SDL renderer is the only way this SDL3/Wayland build
 * can put pixels in a second window (no plain window framebuffer), and every
 * renderer call may make ITS GL context current on the emulation thread. So
 * the game's context is captured when the window opens and put back after
 * each renderer call; without that the game window goes black and the
 * driver faults at present. */

#include "psx_card_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_card_share.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_video_menu.h"

/* --- window ---------------------------------------------------------------- */
#define WIN_W  1480
#define WIN_H   820

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static uint32_t     *s_px;
static int           s_w, s_h, s_dirty;
static int           s_open_req;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;          /* design unit in pixels */
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;

/* --- palette (the F10 menu's, opaque where this window is opaque) ----------- */
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

/* --- design units ------------------------------------------------------------ */
#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_LIST_W   190.0f
#define U_ROW_H     14.0f
#define U_HDR_H     16.0f
#define U_FIELD_H   18.0f
#define U_BOX_H     15.0f
#define U_LABEL_W   64.0f
#define U_BTN_H     17.0f
#define U_STEP_W    15.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f

static int px(float u) { return (int)(u * s_u + 0.5f); }

static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }

/* --- list ------------------------------------------------------------------ */
#define CARDS 722
static int  s_order[CARDS];
static int  s_order_n;
static char s_search[32];
static int  s_scroll;
static int  s_hover_row = -1;
static int  s_sel = 1;
static int  s_sb_drag, s_sb_grab;

/* --- editor ---------------------------------------------------------------- */
enum { F_NAME, F_DESC, F_ATK, F_DEF, F_STAR1, F_STAR2, F_TYPE, F_LEVEL, F_ATTR, F_PRICE, F_PASSWORD,
       F_EFFECT, F_AMOUNT, F_TARGET, F_TERRAIN, F_RITUAL, F_EQUIP_BONUS, F_EQUIPS, F_BOOST, F_TRAP_MAX, F_COUNT };
#define F_FX_FIRST F_EFFECT
static const char *const FIELD_LABEL[F_COUNT] = {
    "Name", "Description", "Attack", "Defense", "Star 1", "Star 2", "Type", "Level", "Attribute", "Price", "Password",
    "Effect", "Amount", "Target type", "Terrain", "Recipe", "Equip bonus", "Equips", "Boosts", "Trap ATK max"
};
enum { B_SAVE, B_RESTORE, B_FOLDER, B_ART, B_THUMB, B_TITLE, B_EXPORT, B_IMPORT, B_COUNT };
static const char *const BTN_LABEL[B_COUNT] = { "Save", "Restore stock", "Open folder", "Pick art\xE2\x80\xA6", "Pick thumbnail\xE2\x80\xA6", "Pick title\xE2\x80\xA6",
                                                "Export\xE2\x80\xA6", "Import\xE2\x80\xA6" };
#define FTEXT 2048                    /* the longest field text (an equip list) */

static int          s_stock_ok;
static int          s_has_pack;      /* the card has a folder (is edited) */
static int          s_changed;
static int          s_focus = -1;
static char         s_buf[FTEXT];
static int          s_caret_on = 1;
static char         s_msg[128];
static unsigned     s_msg_until;
static unsigned     s_seen_gen;
static int          s_hover_btn = -1;

static uint8_t  s_art[102 * 96 * 3], s_thumb[40 * 32 * 3];
static uint32_t s_art_argb[102 * 96], s_thumb_argb[40 * 32];
static int      s_art_ok, s_thumb_ok, s_art_id = -1;
static unsigned s_art_gen;

static char s_pick_path[1024];
static int  s_pick_kind;              /* 1 art, 2 thumb, 3 title, 4 export target, 5 import source */
static unsigned s_present_count;

/* the import preview: what the chosen file would do, shown before it does it */
static int  s_modal;
static PsxCardShareInfo s_share;
static char s_share_path[1024];
static int  s_modal_hover = -1;

/* --- geometry, recomputed from the window size on every draw and click ------- */
typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

typedef struct {
    Rect bar, search, list, list_rows, sb, ed;
    int  row_h, rows;
    Rect value[F_COUNT], step_l[F_COUNT], step_r[F_COUNT], clear[F_COUNT], label[F_COUNT];
    Rect btn[B_COUNT];
    Rect art, thumb;
    int  info_x, info_y;
    int  status_y;
    int  fx_note_y;                   /* the effects note line, 0 = none */
    Rect modal, modal_ok, modal_cancel;
} Layout;
static Layout s_L;

static int field_is_enum(int f) { return f == F_STAR1 || f == F_STAR2 || f == F_TYPE || f == F_ATTR || f == F_EFFECT || f == F_TARGET || f == F_TERRAIN; }
static int field_is_set(int f);
static PsxCardPack  s_edit;
static PsxCardStock s_stock;
/* The type the game will see: the edit when set, else stock. Magic, Trap,
 * Ritual and Equip (20..23) have no ATK/DEF, stars, level or attribute
 * anywhere the game draws them, so those fields are monster-only. */
static int effective_type(void) { return s_edit.type >= 0 ? s_edit.type : s_stock.type; }
static int is_monster(void) { return effective_type() < 20; }
/* Cards the game's magic dispatcher knows: 301..350, 651..700, 721. */
static int magic_dispatchable(int id) { return (id >= 301 && id <= 350) || (id >= 651 && id <= 700) || id == 721; }
static int eff_effect(void) { return s_edit.effect >= 0 ? s_edit.effect : s_stock.effect; }
static int field_applies(int f)
{
    const int t = effective_type();
    switch (f) {
    case F_ATK: case F_DEF: case F_STAR1: case F_STAR2: case F_LEVEL: case F_ATTR: return is_monster();
    case F_EFFECT:  return (t == 20 || t == 22) && magic_dispatchable(s_sel);
    case F_AMOUNT: {
        if (!field_applies(F_EFFECT)) return 0;
        const int e = eff_effect();
        return e == PSX_CARD_FX_HEAL || e == PSX_CARD_FX_DAMAGE || e == PSX_CARD_FX_DESTROY_ATK || e == PSX_CARD_FX_WEAKEN;
    }
    case F_TARGET:  return field_applies(F_EFFECT) && eff_effect() == PSX_CARD_FX_DESTROY_TYPE;
    case F_TERRAIN: return field_applies(F_EFFECT) && eff_effect() == PSX_CARD_FX_FIELD;
    case F_RITUAL:  return field_applies(F_EFFECT) && eff_effect() == PSX_CARD_FX_RITUAL;
    case F_EQUIP_BONUS: case F_EQUIPS: return t == 23;
    case F_BOOST:   return s_sel >= 330 && s_sel <= 335;
    case F_TRAP_MAX: return s_sel >= 681 && s_sel <= 686;
    default: return 1;
    }
}

static void layout_compute(void)
{
    Layout *L = &s_L;
    memset(L, 0, sizeof *L);
    const int gap = px(U_GAP), pad = px(U_PAD);
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };
    L->search = (Rect){ px(118.0f), (L->bar.h - px(16.0f)) / 2, px(130.0f), px(16.0f) };
    const int top = L->bar.h + gap;
    L->list = (Rect){ gap, top, px(U_LIST_W), s_h - top - gap };
    L->row_h = px(U_ROW_H);
    L->list_rows = (Rect){ L->list.x, L->list.y + px(U_HDR_H), L->list.w - px(10.0f), L->list.h - px(U_HDR_H) - px(4.0f) };
    L->rows = L->list_rows.h / L->row_h; if (L->rows < 1) L->rows = 1;
    L->sb = (Rect){ L->list.x + L->list.w - px(8.0f), L->list_rows.y, px(4.0f), L->rows * L->row_h };
    L->ed = (Rect){ L->list.x + L->list.w + gap, top, s_w - (L->list.x + L->list.w + gap) - gap, s_h - top - gap };

    const int ex = L->ed.x + pad;
    int y = L->ed.y + pad + px(16.0f) + px(8.0f);        /* below the header line */
    /* previews */
    const int art_h = px(72.0f), art_w = art_h * 102 / 96;
    L->art = (Rect){ ex, y, art_w, art_h };
    const int th_h = px(24.0f), th_w = th_h * 40 / 32;
    L->thumb = (Rect){ ex + art_w + pad, y, th_w, th_h };
    L->info_x = L->thumb.x + th_w + pad;
    L->info_y = y;
    y += art_h + px(12.0f);
    /* fields */
    const int label_w = px(U_LABEL_W), box_h = px(U_BOX_H), step_w = px(U_STEP_W), sgap = px(3.0f);
    const int right = L->ed.x + L->ed.w - pad;
    const int desc_h = psx_ui_font_line_height(face_body()) * 6 + px(6.0f);   /* the game's six lines */
    for (int f = 0; f < F_COUNT; f++) {
        if (!field_applies(f)) {
            /* the monster-only rows collapse to one note line for a spell */
            if (f == F_ATK) { L->label[f] = (Rect){ ex, y, label_w, box_h }; y += px(U_FIELD_H); }
            continue;
        }
        if (f == F_FX_FIRST || (f > F_FX_FIRST && !L->fx_note_y)) {
            y += px(4.0f);
            L->fx_note_y = y;
            y += psx_ui_font_line_height(face_small()) + px(4.0f);
        }
        const int fh = (f == F_DESC) ? desc_h + px(3.0f) : px(U_FIELD_H);
        L->label[f] = (Rect){ ex, y, label_w, box_h };
        int vx = ex + label_w, vw;
        if (field_is_enum(f)) {
            L->step_l[f] = (Rect){ vx, y, step_w, box_h };
            vx += step_w + sgap;
            vw = (f == F_EFFECT) ? px(190.0f) : px(90.0f);
            L->value[f] = (Rect){ vx, y, vw, box_h };
            L->step_r[f] = (Rect){ vx + vw + sgap, y, step_w, box_h };
            L->clear[f] = (Rect){ L->step_r[f].x + step_w + sgap * 2, y, step_w, box_h };
        } else {
            const int wide = (f == F_DESC || f == F_EQUIPS || f == F_BOOST || f == F_RITUAL);
            vw = (f == F_NAME) ? px(170.0f) : wide ? (right - vx - step_w - sgap * 2) : px(70.0f);
            if (vw < px(40.0f)) vw = px(40.0f);
            L->value[f] = (Rect){ vx, y, vw, (f == F_DESC) ? fh - px(3.0f) : box_h };
            L->clear[f] = (Rect){ vx + vw + sgap * 2, y, step_w, box_h };
        }
        y += fh;
    }
    y += px(6.0f);
    /* buttons: flow layout, wrapping to a new line when the panel runs out */
    {
        const PsxUiFace *fb = face_bold();
        int bx = ex, bh = px(U_BTN_H);
        for (int b = 0; b < B_COUNT; b++) {
            const int bw = psx_ui_font_text_w(fb, BTN_LABEL[b]) + px(18.0f);
            if (bx + bw > right && bx > ex) { bx = ex; y += bh + px(5.0f); }
            L->btn[b] = (Rect){ bx, y, bw, bh };
            bx += bw + px(5.0f);
        }
        y += bh + px(8.0f);
    }
    L->status_y = y;
    /* the import preview panel, centred */
    {
        const int mw = px(360.0f), mh = px(200.0f);
        L->modal = (Rect){ (s_w - mw) / 2, (s_h - mh) / 2, mw, mh };
        const int bw = px(80.0f), bh = px(U_BTN_H);
        L->modal_ok = (Rect){ L->modal.x + L->modal.w - pad - bw * 2 - px(6.0f), L->modal.y + L->modal.h - pad - bh, bw, bh };
        L->modal_cancel = (Rect){ L->modal.x + L->modal.w - pad - bw, L->modal.y + L->modal.h - pad - bh, bw, bh };
    }
}

/* --- text helpers -------------------------------------------------------------- */
/* Greedy word wrap of `s` into at most `max_lines` lines that each fit
 * `max_w`; a word longer than the box is split. Returns the line count. */
static int wrap_text(const PsxUiFace *f, const char *s, int max_w, char lines[][256], int max_lines)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p, *last_space = NULL;
        while (*q) {
            if (*q == ' ') last_space = q;
            if (psx_ui_font_text_w_n(f, p, (int)(q - p) + 1) > max_w) break;
            q++;
        }
        if (*q && last_space && last_space > p) q = last_space;
        if (q == p) q = p + 1;                       /* never stall on a too-narrow box */
        int len = (int)(q - p); if (len > 255) len = 255;
        memcpy(lines[n], p, (size_t)len); lines[n][len] = 0;
        n++;
        p = q;
    }
    return n;
}

static int draw_wrapped(int x, int y, int max_w, const char *s, uint32_t col, const PsxUiFace *f, int max_lines)
{
    char lines[8][256];
    if (max_lines > 8) max_lines = 8;
    const int n = wrap_text(f, s, max_w, lines, max_lines);
    const int lh = psx_ui_font_line_height(f);
    for (int i = 0; i < n; i++)
        psx_ui_text(&s_cv, x, y + i * lh + psx_ui_font_ascent(f), lines[i], col, f);
    return y + n * lh;
}

static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}

static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    const int w = psx_ui_font_text_w(f, s);
    psx_ui_text(&s_cv, r->x + (r->w - w) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* --- helpers ------------------------------------------------------------------ */
static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 3500u;
    s_dirty = 1;
}

static int ci_contains(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && (hay[i] | 32) == (needle[i] | 32)) i++;
        if (i == n) return 1;
    }
    return 0;
}

static void clamp_scroll(void)
{
    int m = s_order_n - s_L.rows; if (m < 0) m = 0;
    if (s_scroll > m) s_scroll = m;
    if (s_scroll < 0) s_scroll = 0;
}

static void rebuild_order(void)
{
    s_order_n = 0;
    const int numeric = s_search[0] >= '0' && s_search[0] <= '9';
    for (int id = 1; id <= CARDS; id++) {
        if (numeric) {
            char idbuf[8]; snprintf(idbuf, sizeof idbuf, "%d", id);
            if (strncmp(idbuf, s_search, strlen(s_search)) != 0) continue;
        } else if (!ci_contains(psx_card_db_name(id), s_search)) {
            continue;
        }
        s_order[s_order_n++] = id;
    }
    clamp_scroll();
    s_dirty = 1;
}

static void load_editor(void)
{
    s_focus = -1;
    s_has_pack = psx_card_packs_get(s_sel, &s_edit);
    if (!s_has_pack) {
        memset(&s_edit, 0, sizeof s_edit);
        s_edit.id = s_sel;
        s_edit.attack = s_edit.defense = s_edit.star1 = s_edit.star2 = s_edit.type =
            s_edit.level = s_edit.attribute = s_edit.price = -1;
        psx_card_packs_effects_reset(&s_edit);
    }
    s_stock_ok = psx_card_packs_stock(s_sel, &s_stock);
    s_changed = 0;
    s_art_id = -1;
    s_dirty = 1;
}

static void select_card(int id)
{
    if (id < 1 || id > CARDS) return;
    s_sel = id;
    load_editor();
}

static void refresh_preview(void)
{
    const unsigned gen = psx_card_packs_generation();
    if (s_art_id == s_sel && s_art_gen == gen) return;
    s_art_ok = psx_card_packs_art_rgb(s_sel, s_art);
    s_thumb_ok = psx_card_packs_thumb_rgb(s_sel, s_thumb);
    for (int i = 0; i < 102 * 96; i++) s_art_argb[i] = 0xFF000000u | ((uint32_t)s_art[i*3] << 16) | ((uint32_t)s_art[i*3+1] << 8) | s_art[i*3+2];
    for (int i = 0; i < 40 * 32; i++) s_thumb_argb[i] = 0xFF000000u | ((uint32_t)s_thumb[i*3] << 16) | ((uint32_t)s_thumb[i*3+1] << 8) | s_thumb[i*3+2];
    s_art_id = s_sel; s_art_gen = gen;
    s_dirty = 1;
}

static int field_is_set(int f)
{
    switch (f) {
    case F_NAME: return s_edit.name[0] != 0;
    case F_DESC: return s_edit.description[0] != 0;
    case F_ATK: return s_edit.attack >= 0;
    case F_DEF: return s_edit.defense >= 0;
    case F_STAR1: return s_edit.star1 >= 1;
    case F_STAR2: return s_edit.star2 >= 1;
    case F_TYPE: return s_edit.type >= 0;
    case F_LEVEL: return s_edit.level >= 0;
    case F_ATTR: return s_edit.attribute >= 0;
    case F_PRICE: return s_edit.price >= 0;
    case F_PASSWORD: return s_edit.password[0] != 0;
    case F_EFFECT: return s_edit.effect >= 0;
    case F_AMOUNT: return s_edit.amount != -1;
    case F_TARGET: return s_edit.target >= 0;
    case F_TERRAIN: return s_edit.terrain >= 1;
    case F_RITUAL: return s_edit.ritual_set;
    case F_EQUIP_BONUS: return s_edit.equip_bonus >= 0;
    case F_EQUIPS: return s_edit.equips_set || s_edit.equip_types != 0;
    case F_BOOST: return s_edit.boost_set;
    case F_TRAP_MAX: return s_edit.trap_atk_max >= 0;
    }
    return 0;
}

static void field_clear(int f)
{
    switch (f) {
    case F_NAME: s_edit.name[0] = 0; break;
    case F_DESC: s_edit.description[0] = 0; break;
    case F_ATK: s_edit.attack = -1; break;
    case F_DEF: s_edit.defense = -1; break;
    case F_STAR1: s_edit.star1 = -1; break;
    case F_STAR2: s_edit.star2 = -1; break;
    case F_TYPE: s_edit.type = -1; break;
    case F_LEVEL: s_edit.level = -1; break;
    case F_ATTR: s_edit.attribute = -1; break;
    case F_PRICE: s_edit.price = -1; break;
    case F_PASSWORD: s_edit.password[0] = 0; break;
    case F_EFFECT: s_edit.effect = -1; break;
    case F_AMOUNT: s_edit.amount = -1; break;
    case F_TARGET: s_edit.target = -1; break;
    case F_TERRAIN: s_edit.terrain = -1; break;
    case F_RITUAL: s_edit.ritual_set = 0; s_edit.ritual_mat[0] = s_edit.ritual_mat[1] = s_edit.ritual_mat[2] = s_edit.ritual_result = -1; break;
    case F_EQUIP_BONUS: s_edit.equip_bonus = -1; break;
    case F_EQUIPS: s_edit.equips_set = 0; s_edit.equip_types = 0; s_edit.equip_n = 0; break;
    case F_BOOST: s_edit.boost_set = 0; for (int t = 0; t < 20; t++) s_edit.boost[t] = PSX_CARD_PACK_BOOST_UNSET; break;
    case F_TRAP_MAX: s_edit.trap_atk_max = -1; break;
    }
    s_changed = 1; s_dirty = 1;
}

static void field_text(int f, int stock, char *out, size_t cap)
{
    const int set = field_is_set(f) && !stock;
    if (!s_stock_ok && stock) { snprintf(out, cap, "-"); return; }
    switch (f) {
    case F_NAME: snprintf(out, cap, "%s", set ? s_edit.name : s_stock.name); break;
    case F_DESC: snprintf(out, cap, "%s", set ? s_edit.description : s_stock.description); break;
    case F_ATK: snprintf(out, cap, "%d", set ? s_edit.attack : s_stock.attack); break;
    case F_DEF: snprintf(out, cap, "%d", set ? s_edit.defense : s_stock.defense); break;
    case F_STAR1: snprintf(out, cap, "%s", psx_card_packs_star_name(set ? s_edit.star1 : s_stock.star1)); break;
    case F_STAR2: snprintf(out, cap, "%s", psx_card_packs_star_name(set ? s_edit.star2 : s_stock.star2)); break;
    case F_TYPE: snprintf(out, cap, "%s", psx_card_packs_type_name(set ? s_edit.type : s_stock.type)); break;
    case F_LEVEL: snprintf(out, cap, "%d", set ? s_edit.level : s_stock.level); break;
    case F_ATTR: snprintf(out, cap, "%s", psx_card_packs_attribute_name(set ? s_edit.attribute : s_stock.attribute)); break;
    case F_PRICE: snprintf(out, cap, "%d", set ? s_edit.price : s_stock.price); break;
    case F_PASSWORD: snprintf(out, cap, "%s", set ? s_edit.password : (s_stock.password[0] ? s_stock.password : "none")); break;
    case F_EFFECT: {
        const int e = set ? s_edit.effect : s_stock.effect;
        snprintf(out, cap, "%s", e >= 0 ? psx_card_packs_effect_label(e) : "code only");
        break;
    }
    case F_AMOUNT:
        if (set) snprintf(out, cap, "%d", s_edit.amount);
        else if (s_stock.amount >= 0 && s_stock.effect == eff_effect()) snprintf(out, cap, "%d", s_stock.amount);
        else snprintf(out, cap, "%d", eff_effect() == PSX_CARD_FX_DESTROY_ATK ? 1500 : 500);   /* the layer's default */
        break;
    case F_TARGET: snprintf(out, cap, "%s", psx_card_packs_type_name(set ? s_edit.target : (s_stock.effect == PSX_CARD_FX_DESTROY_TYPE && s_stock.amount >= 0 ? s_stock.amount : 3))); break;
    case F_TERRAIN: snprintf(out, cap, "%s", psx_card_packs_terrain_name(set ? s_edit.terrain : (s_stock.effect == PSX_CARD_FX_FIELD && s_stock.amount >= 1 ? s_stock.amount : 1))); break;
    case F_RITUAL:
        if (set) psx_card_packs_format_ritual(&s_edit, out, (unsigned)cap);
        else if (s_stock.ritual_result >= 1) snprintf(out, cap, "%d, %d, %d -> %d", s_stock.ritual_mat[0], s_stock.ritual_mat[1], s_stock.ritual_mat[2], s_stock.ritual_result);
        else snprintf(out, cap, "none");
        break;
    case F_EQUIP_BONUS: snprintf(out, cap, "%d", set ? s_edit.equip_bonus : s_stock.equip_bonus); break;
    case F_EQUIPS:
        if (set) psx_card_packs_format_equips(&s_edit, out, (unsigned)cap);
        else snprintf(out, cap, "stock list");
        break;
    case F_BOOST:
        if (set) psx_card_packs_format_boost(&s_edit, out, (unsigned)cap);
        else { PsxCardPack tmp; memset(&tmp, 0, sizeof tmp); memcpy(tmp.boost, s_stock.boost, sizeof tmp.boost); psx_card_packs_format_boost(&tmp, out, (unsigned)cap); }
        break;
    case F_TRAP_MAX: snprintf(out, cap, "%d", set ? s_edit.trap_atk_max : s_stock.trap_atk_max); break;
    }
}

static void field_step(int f, int dir)
{
    int *v, lo, hi, stock;
    switch (f) {
    case F_STAR1: v = &s_edit.star1; lo = 1; hi = 10; stock = s_stock.star1; break;
    case F_STAR2: v = &s_edit.star2; lo = 1; hi = 10; stock = s_stock.star2; break;
    case F_TYPE:  v = &s_edit.type;  lo = 0; hi = 23; stock = s_stock.type; break;
    case F_ATTR:  v = &s_edit.attribute; lo = 0; hi = 7; stock = s_stock.attribute; break;
    case F_EFFECT: v = &s_edit.effect; lo = 0; hi = PSX_CARD_FX_COUNT - 1; stock = s_stock.effect >= 0 ? s_stock.effect : 0; break;
    case F_TARGET: v = &s_edit.target; lo = 0; hi = 19; stock = (s_stock.effect == PSX_CARD_FX_DESTROY_TYPE && s_stock.amount >= 0) ? s_stock.amount : 3; break;
    case F_TERRAIN: v = &s_edit.terrain; lo = 1; hi = 6; stock = (s_stock.effect == PSX_CARD_FX_FIELD && s_stock.amount >= 1) ? s_stock.amount : 1; break;
    default: return;
    }
    int cur = (*v >= lo) ? *v : stock;
    cur += dir;
    if (cur > hi) cur = lo;
    if (cur < lo) cur = hi;
    *v = cur;
    s_changed = 1; s_dirty = 1;
}

static void focus_begin(int f)
{
    s_focus = f;
    if (field_is_set(f)) field_text(f, 0, s_buf, sizeof s_buf);
    else s_buf[0] = 0;
    s_dirty = 1;
}

static void focus_commit(void)
{
    const int f = s_focus;
    s_focus = -1;
    s_dirty = 1;
    if (f < 0) return;
    if (!s_buf[0]) { field_clear(f); return; }
    const int v = atoi(s_buf);
    switch (f) {
    case F_NAME: snprintf(s_edit.name, sizeof s_edit.name, "%s", s_buf); break;
    case F_DESC: snprintf(s_edit.description, sizeof s_edit.description, "%s", s_buf); break;
    case F_ATK: if (v < 0 || v > 5110) { say("Attack is 0 to 5110"); return; } s_edit.attack = v / 10 * 10; break;
    case F_DEF: if (v < 0 || v > 5110) { say("Defense is 0 to 5110"); return; } s_edit.defense = v / 10 * 10; break;
    case F_LEVEL: if (v < 0 || v > 12) { say("Level is 0 to 12"); return; } s_edit.level = v; break;
    case F_PRICE: if (v < 0 || v > 999999) { say("Price is 0 to 999999"); return; } s_edit.price = v; break;
    case F_PASSWORD: {
        int ok = strlen(s_buf) == 8;
        for (int i = 0; ok && i < 8; i++) if (s_buf[i] < '0' || s_buf[i] > '9') ok = 0;
        if (!ok) { say("A password is 8 digits"); return; }
        memcpy(s_edit.password, s_buf, 9);
        break;
    }
    case F_AMOUNT: {
        const int e = eff_effect();
        if (e == PSX_CARD_FX_HEAL && (v < 0 || v > 25500)) { say("Heal is 0 to 25500, in steps of 100"); return; }
        if (e == PSX_CARD_FX_DAMAGE && (v < 0 || v > 2550)) { say("Damage is 0 to 2550, in steps of 10"); return; }
        if (e == PSX_CARD_FX_DESTROY_ATK && (v < 210 || v > 2550)) { say("The ATK threshold is 210 to 2550"); return; }
        if (e == PSX_CARD_FX_WEAKEN && (v < -9990 || v > 9990)) { say("Weaken is -9990 to 9990 (negative strengthens)"); return; }
        s_edit.amount = (e == PSX_CARD_FX_HEAL) ? v / 100 * 100 : v / 10 * 10;
        break;
    }
    case F_EQUIP_BONUS: if (v < 0 || v > 9990) { say("The equip bonus is 0 to 9990"); return; } s_edit.equip_bonus = v / 10 * 10; break;
    case F_TRAP_MAX: if (v < 0 || v > 25500) { say("The ceiling is 0 to 25500, in steps of 100"); return; } s_edit.trap_atk_max = v / 100 * 100; break;
    case F_RITUAL: { char err[96]; if (!psx_card_packs_parse_ritual(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    case F_EQUIPS: { char err[96]; if (!psx_card_packs_parse_equips(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    case F_BOOST:  { char err[96]; if (!psx_card_packs_parse_boost(s_buf, &s_edit, err, sizeof err)) { say(err); return; } break; }
    default: return;
    }
    s_changed = 1;
}

static void do_save(void)
{
    if (s_focus >= 0) focus_commit();
    if (!is_monster()) {
        /* nothing the game would draw; do not carry stale monster numbers */
        s_edit.attack = s_edit.defense = s_edit.star1 = s_edit.star2 = s_edit.level = s_edit.attribute = -1;
    }
    /* effect fields the card cannot use are dropped rather than saved blind */
    if (!field_applies(F_EFFECT)) { s_edit.effect = s_edit.amount = s_edit.target = s_edit.terrain = -1; s_edit.ritual_set = 0; }
    if (!field_applies(F_EQUIPS)) { s_edit.equip_bonus = -1; s_edit.equips_set = 0; s_edit.equip_types = 0; s_edit.equip_n = 0; }
    if (!field_applies(F_BOOST))  s_edit.boost_set = 0;
    if (!field_applies(F_TRAP_MAX)) s_edit.trap_atk_max = -1;
    int any = 0;
    for (int f = 0; f < F_COUNT; f++) any |= field_is_set(f);
    if (!any && !s_edit.has_art && !s_edit.has_thumb && !s_edit.has_title) {
        if (s_has_pack) { psx_card_packs_remove(s_sel); say("Nothing left to keep; the card is stock again"); }
        else say("Nothing to save");
        load_editor();
        return;
    }
    if (psx_card_packs_save(&s_edit)) say("Saved. It shows on the next screen that draws the card.");
    else say("Save failed: could not write the card folder");
    load_editor();
    rebuild_order();
}

static void do_restore(void)
{
    if (!s_has_pack) { say("This card is already stock"); return; }
    psx_card_packs_remove(s_sel);
    say("Card restored to stock");
    load_editor();
    rebuild_order();
}

static void card_folder(char *out, size_t cap, int create)
{
    const char *dir = psx_card_packs_dir();
    if (create) MKDIR(dir);
    snprintf(out, cap, "%s/%d", dir, s_sel);
    if (create) MKDIR(out);
}

static void do_open_folder(void)
{
    char path[1200];
    if (!psx_card_packs_dir()[0]) { say("The game has not booted yet"); return; }
    card_folder(path, sizeof path, 1);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    const pid_t pid = fork();
    if (pid == 0) { execlp("xdg-open", "xdg-open", path, (char *)NULL); _exit(127); }
#endif
    say("Opened the card's folder");
}

static int install_pick(const char *src, int kind)
{
    static const char *const names[4] = { "", "art.png", "thumb.png", "title.png" };
    char dst[1200];
    card_folder(dst, sizeof dst, 1);
    const size_t n = strlen(dst);
    snprintf(dst + n, sizeof dst - n, "/%s", names[kind]);
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
    fclose(in); fclose(out);
    return 1;
}

#if defined(PSX_SDL3)
static void SDLCALL pick_cb(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    if (!filelist || !filelist[0]) return;
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
    s_pick_kind = (int)(intptr_t)userdata;
}
#endif

static void do_pick(int kind)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "PNG images", "png" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)kind, s_win, filters, 1, NULL, false);
#else
    (void)kind;
    say("No file dialog in this build; drop a PNG in the card's folder");
#endif
}

static void do_export(void)
{
    int n = 0;
    for (int id = 1; id <= CARDS; id++) n += psx_card_packs_get(id, NULL) != 0;
    if (!n) { say("No edited cards to export yet"); return; }
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Edited cards", PSX_CARD_SHARE_EXT } };
    static char def[1200];
    snprintf(def, sizeof def, "%s/edited-cards.%s", psx_mod_player_data_dir(), PSX_CARD_SHARE_EXT);
    SDL_ShowSaveFileDialog(pick_cb, (void *)(intptr_t)4, s_win, filters, 1, def);
#else
    say("No file dialog in this build");
#endif
}

static void do_import(void)
{
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "Edited cards", PSX_CARD_SHARE_EXT }, { "Zip archives", "zip" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)5, s_win, filters, 2, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build");
#endif
}

/* The chosen file, once the dialog answers: export writes it, import shows
 * the preview and waits for the player. */
static void begin_export(const char *path)
{
    char p[1200]; snprintf(p, sizeof p, "%s", path);
    const char *dot = strrchr(p, '.'), *slash = strrchr(p, '/');
    if (!dot || (slash && dot < slash)) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".%s", PSX_CARD_SHARE_EXT); }
    char msg[160];
    psx_card_share_export(p, msg, sizeof msg);
    say(msg);
}

static void begin_import(const char *path)
{
    snprintf(s_share_path, sizeof s_share_path, "%s", path);
    if (!psx_card_share_inspect(path, &s_share)) { say(s_share.error); return; }
    if (!s_share.card_n && !s_share.has_drops) { say("That file holds no edited cards"); return; }
    s_modal = 1; s_modal_hover = -1; s_dirty = 1;
}

static void finish_import(int go)
{
    s_modal = 0; s_dirty = 1;
    if (!go) { say("Import cancelled; nothing changed"); return; }
    char msg[200];
    psx_card_share_import(s_share_path, msg, sizeof msg);
    say(msg);
    load_editor();
    rebuild_order();
}

/* --- draw ------------------------------------------------------------------- */
static void draw_caret(const Rect *r, int dir, uint32_t col)
{
    const float cx = r->x + r->w * 0.5f, cy = r->y + r->h * 0.5f, s = (float)px(3.0f);
    const float w = (float)px(1.4f);
    if (dir < 0) {
        psx_ui_line(&s_cv, cx + s * 0.5f, cy - s, cx - s * 0.5f, cy, w, col);
        psx_ui_line(&s_cv, cx - s * 0.5f, cy, cx + s * 0.5f, cy + s, w, col);
    } else {
        psx_ui_line(&s_cv, cx - s * 0.5f, cy - s, cx + s * 0.5f, cy, w, col);
        psx_ui_line(&s_cv, cx + s * 0.5f, cy, cx - s * 0.5f, cy + s, w, col);
    }
}

static void draw_cross(const Rect *r, uint32_t col)
{
    const float cx = r->x + r->w * 0.5f, cy = r->y + r->h * 0.5f, s = (float)px(2.6f), w = (float)px(1.4f);
    psx_ui_line(&s_cv, cx - s, cy - s, cx + s, cy + s, w, col);
    psx_ui_line(&s_cv, cx - s, cy + s, cx + s, cy - s, w, col);
}

static void draw_button(const Rect *r, const char *label, int primary, int hover)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, primary ? COL_BTN_ON : COL_BTN);
    if (hover) psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_HOVER);
    text_centered(r, label, COL_TEXT, face_bold());
}

static void draw_bar(void)
{
    const Layout *L = &s_L;
    psx_ui_fill(&s_cv, 0, 0, s_w, L->bar.h, COL_BAR);
    psx_ui_fill(&s_cv, 0, L->bar.h - 1, s_w, 1, 0x40FFFFFFu);
    Rect t = { px(8.0f), 0, px(110.0f), L->bar.h };
    text_in(&t, 0, "Card Manager", COL_ACCENT, face_title());
    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, L->search.h * 0.5f, COL_EDIT_BG);
    char sb[48];
    snprintf(sb, sizeof sb, "%s%s", s_search, (s_focus < 0 && s_caret_on) ? "|" : "");
    if (s_search[0]) text_in(&L->search, px(8.0f), sb, COL_TEXT, face_body());
    else text_in(&L->search, px(8.0f), "Type to search\xE2\x80\xA6", COL_DIM, face_body());
    char n[48]; snprintf(n, sizeof n, "%d cards", s_order_n);
    Rect c = { L->search.x + L->search.w + px(10.0f), 0, px(80.0f), L->bar.h };
    text_in(&c, 0, n, COL_DIM, face_small());
}

static void draw_list(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->list.x, L->list.y, L->list.w, L->list.h, (float)px(U_R_PANEL), COL_PANEL);
    const PsxUiFace *fs = face_small(), *fb = face_body();
    Rect hdr = { L->list.x + px(8.0f), L->list.y, L->list.w, px(U_HDR_H) };
    psx_ui_text(&s_cv, hdr.x + px(4.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "ID", COL_DIM, fs);
    psx_ui_text(&s_cv, hdr.x + px(30.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "Name", COL_DIM, fs);
    psx_ui_text(&s_cv, L->list.x + L->list.w - px(44.0f), psx_ui_baseline_in(hdr.y, hdr.h, fs), "Edited", COL_DIM, fs);
    int y = L->list_rows.y;
    for (int i = 0; i < L->rows; i++, y += L->row_h) {
        const int k = s_scroll + i;
        if (k >= s_order_n) break;
        const int id = s_order[k];
        const Rect row = { L->list.x + px(4.0f), y, L->list_rows.w - px(4.0f), L->row_h };
        if (id == s_sel) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_SEL_BG);
        else if (i == s_hover_row) psx_ui_round_rect(&s_cv, row.x, row.y, row.w, row.h, row.h * 0.5f, COL_HOVER);
        char b[8]; snprintf(b, sizeof b, "%d", id);
        const int base = psx_ui_baseline_in(y, L->row_h, fb);
        const int idw = psx_ui_font_text_w(fb, b);
        psx_ui_text(&s_cv, hdr.x + px(22.0f) - idw, base, b, COL_DIM, fb);
        psx_ui_text_clip(&s_cv, hdr.x + px(30.0f), base, psx_card_db_name(id), id == s_sel ? COL_ACCENT : COL_TEXT, fb,
                         L->list.w - px(30.0f) - px(54.0f));
        if (psx_card_packs_get(id, NULL)) {
            const int cx = L->list.x + L->list.w - px(30.0f);
            psx_ui_round_rect(&s_cv, cx - px(3.0f), y + L->row_h / 2 - px(3.0f), px(6.0f), px(6.0f), (float)px(3.0f), COL_EDITED);
        }
    }
    if (s_order_n > L->rows) {
        psx_ui_round_rect(&s_cv, L->sb.x, L->sb.y, L->sb.w, L->sb.h, L->sb.w * 0.5f, COL_TRACK);
        int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
        const int ty = L->sb.y + (L->sb.h - th) * s_scroll / (s_order_n - L->rows);
        psx_ui_round_rect(&s_cv, L->sb.x, ty, L->sb.w, th, L->sb.w * 0.5f, s_sb_drag ? COL_ACCENT : COL_THUMB);
    }
}

static void draw_editor(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fb = face_body(), *fs = face_small();
    psx_ui_round_rect(&s_cv, L->ed.x, L->ed.y, L->ed.w, L->ed.h, (float)px(U_R_PANEL), COL_PANEL);
    const int ex = L->ed.x + px(U_PAD), right = L->ed.x + L->ed.w - px(U_PAD);
    /* header */
    {
        char h[96]; snprintf(h, sizeof h, "Card %03d", s_sel);
        int x = psx_ui_text(&s_cv, ex, L->ed.y + px(U_PAD) + psx_ui_font_ascent(ft), h, COL_ACCENT, ft);
        x += px(8.0f);
        psx_ui_text_clip(&s_cv, x, L->ed.y + px(U_PAD) + psx_ui_font_ascent(ft), psx_card_db_name(s_sel), COL_TEXT, ft, right - x - px(60.0f));
        if (s_has_pack) {
            const int cw = psx_ui_font_text_w(fs, "edited") + px(12.0f), ch = px(13.0f);
            Rect chip = { right - cw, L->ed.y + px(U_PAD) + px(2.0f), cw, ch };
            psx_ui_round_rect(&s_cv, chip.x, chip.y, chip.w, chip.h, ch * 0.5f, COL_SEL_BG);
            text_centered(&chip, "edited", COL_EDITED, fs);
        }
    }
    /* previews */
    psx_ui_round_rect(&s_cv, L->art.x - 2, L->art.y - 2, L->art.w + 4, L->art.h + 4, (float)px(U_R_BOX), COL_EDIT_BG);
    if (s_art_ok) psx_ui_blit_scaled(&s_cv, L->art.x, L->art.y, L->art.w, L->art.h, (float)px(4.0f), s_art_argb, 102, 96);
    psx_ui_round_rect(&s_cv, L->thumb.x - 2, L->thumb.y - 2, L->thumb.w + 4, L->thumb.h + 4, (float)px(4.0f), COL_EDIT_BG);
    if (s_thumb_ok) psx_ui_blit_scaled(&s_cv, L->thumb.x, L->thumb.y, L->thumb.w, L->thumb.h, (float)px(3.0f), s_thumb_argb, 40, 32);
    psx_ui_text(&s_cv, L->thumb.x, L->thumb.y + L->thumb.h + px(4.0f) + psx_ui_font_ascent(fs), "duel", COL_DIM, fs);
    {
        const int lh = psx_ui_font_line_height(fs);
        int y = L->info_y;
        const int iw = right - L->info_x;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_art ? "Face art: yours" : "Face art: stock", s_edit.has_art ? COL_EDITED : COL_DIM, fs); y += lh;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_thumb ? "Duel thumbnail: yours" : "Duel thumbnail: stock", s_edit.has_thumb ? COL_EDITED : COL_DIM, fs); y += lh;
        psx_ui_text(&s_cv, L->info_x, y + psx_ui_font_ascent(fs), s_edit.has_title ? "Title strip: yours" : "Title strip: from the name", s_edit.has_title ? COL_EDITED : COL_DIM, fs); y += lh + px(4.0f);
        draw_wrapped(L->info_x, y, iw, "Any PNG works for the face; it becomes 102x96 in 256 colours. The duel thumbnail is 40x32 in 64 colours and is made from the face unless you pick one. A change shows on the next screen that draws the card.", COL_DIM, fs, 5);
    }
    /* fields */
    for (int f = 0; f < F_COUNT; f++) {
        const Rect *v = &L->value[f];
        const int set = field_is_set(f);
        if (!field_applies(f)) {
            /* a spell, trap, ritual or equip card: the game never draws these */
            if (f == F_ATK && !is_monster())
                psx_ui_text(&s_cv, L->label[f].x, psx_ui_baseline_in(L->label[f].y, L->label[f].h, fs), "ATK, DEF, stars, level and attribute are not used by a Magic, Trap, Ritual or Equip card", 0x99C9CFDDu, fs);
            continue;
        }
        if (f == F_FX_FIRST || (f > F_FX_FIRST && L->fx_note_y && L->label[f].y == L->fx_note_y + psx_ui_font_line_height(fs) + px(4.0f))) {
            const char *note = psx_card_effects_note(s_sel, effective_type());
            psx_ui_text_clip(&s_cv, ex, L->fx_note_y + psx_ui_font_ascent(fs), note, COL_ACCENT, fs, right - ex);
        }
        psx_ui_text(&s_cv, L->label[f].x, psx_ui_baseline_in(L->label[f].y, L->label[f].h, fb), FIELD_LABEL[f], COL_DIM, fb);
        psx_ui_round_rect(&s_cv, v->x, v->y, v->w, v->h, (float)px(U_R_BOX), s_focus == f ? COL_EDIT_BG : COL_BTN);
        if (s_focus == f) psx_ui_round_rect_line(&s_cv, v->x, v->y, v->w, v->h, (float)px(U_R_BOX), COL_ACCENT, 1.0f);
        char t[PSX_CARD_PACK_DESC_MAX + 8];
        if (s_focus == f) {
            char tb[PSX_CARD_PACK_DESC_MAX + 8];
            snprintf(tb, sizeof tb, "%s%s", s_buf, s_caret_on ? "|" : "");
            if (f == F_DESC) {
                char lines[16][256];
                const int lh = psx_ui_font_line_height(fb);
                const int fitl = (v->h - px(4.0f)) / lh;
                const int n = wrap_text(fb, tb, v->w - px(12.0f), lines, 16);
                const int first = n > fitl ? n - fitl : 0;
                for (int i = first; i < n; i++)
                    psx_ui_text(&s_cv, v->x + px(6.0f), v->y + px(2.0f) + (i - first) * lh + psx_ui_font_ascent(fb), lines[i], COL_TEXT, fb);
            } else {
                const char *tail = tb;
                while (*tail && psx_ui_font_text_w(fb, tail) > v->w - px(12.0f)) tail++;
                psx_ui_text(&s_cv, v->x + px(6.0f), psx_ui_baseline_in(v->y, v->h, fb), tail, COL_TEXT, fb);
            }
        } else {
            field_text(f, 0, t, sizeof t);
            if (f == F_DESC) {
                /* "|" is a line break in the game; show it as one */
                char shown[PSX_CARD_PACK_DESC_MAX + 8]; snprintf(shown, sizeof shown, "%s", t);
                const int lh = psx_ui_font_line_height(fb);
                const int fitl = (v->h - px(4.0f)) / lh;
                char *p = shown; int line = 0;
                while (p && *p && line < fitl) {
                    char *bar = strchr(p, '|');
                    if (bar) *bar = 0;
                    char lines[4][256];
                    const int n = wrap_text(fb, p, v->w - px(12.0f), lines, 4);
                    for (int i = 0; i < n && line < fitl; i++, line++)
                        psx_ui_text(&s_cv, v->x + px(6.0f), v->y + px(2.0f) + line * lh + psx_ui_font_ascent(fb), lines[i], set ? COL_EDITED : COL_TEXT, fb);
                    if (!n) line++;
                    p = bar ? bar + 1 : NULL;
                }
            } else {
                text_in(v, px(6.0f), t, set ? COL_EDITED : COL_TEXT, fb);
            }
        }
        if (field_is_enum(f)) {
            const Rect *l = &L->step_l[f], *r = &L->step_r[f];
            psx_ui_round_rect(&s_cv, l->x, l->y, l->w, l->h, l->h * 0.5f, COL_BTN); draw_caret(l, -1, COL_TEXT);
            psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, r->h * 0.5f, COL_BTN); draw_caret(r, +1, COL_TEXT);
        }
        if (set) {
            const Rect *c = &L->clear[f];
            psx_ui_round_rect(&s_cv, c->x, c->y, c->w, c->h, c->h * 0.5f, COL_BTN); draw_cross(c, COL_TEXT);
            if (f != F_DESC) {
                char st[PSX_CARD_PACK_DESC_MAX + 8]; field_text(f, 1, st, sizeof st);
                char s2[PSX_CARD_PACK_DESC_MAX + 16]; snprintf(s2, sizeof s2, "stock: %s", st);
                const int sx = c->x + c->w + px(8.0f);
                psx_ui_text_clip(&s_cv, sx, psx_ui_baseline_in(v->y, v->h, fs), s2, COL_DIM, fs, right - sx);
            }
        }
    }
    for (int b = 0; b < B_COUNT; b++)
        draw_button(&L->btn[b], BTN_LABEL[b], b == B_SAVE && s_changed, s_hover_btn == b);
    /* status + help, wrapped to the panel */
    {
        int y = L->status_y;
        const int w = right - ex;
        if (s_msg[0]) y = draw_wrapped(ex, y, w, s_msg, COL_WARN, fb, 2);
        else if (s_changed) y = draw_wrapped(ex, y, w, "Unsaved changes. Save writes card.ini in the card's folder and applies it right away.", COL_DIM, fb, 2);
        else {
            char p[1200]; snprintf(p, sizeof p, "%s/%d/", psx_card_packs_dir(), s_sel);
            y = draw_wrapped(ex, y, w, p, COL_DIM, fs, 2);
        }
        y += px(3.0f);
        draw_wrapped(ex, y, w, "Green is your edit; x puts a value back to stock. Click a value to type, Enter keeps it, Esc cancels. In the description a | starts a new line (20 columns, six lines); without one the text is wrapped for you. Export writes every edited card to one .ygocards file; Import reads one and shows what it will replace first.", COL_DIM, fs, 5);
    }
}

static void draw_modal(void)
{
    const Layout *L = &s_L;
    const PsxUiFace *ft = face_title(), *fb = face_body(), *fs = face_small();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, 0xB8000000u);
    psx_ui_round_rect_shadow(&s_cv, L->modal.x, L->modal.y, L->modal.w, L->modal.h, (float)px(U_R_PANEL), COL_PANEL, px(6.0f));
    psx_ui_round_rect(&s_cv, L->modal.x, L->modal.y, L->modal.w, L->modal.h, (float)px(U_R_PANEL), COL_PANEL);
    const int ex = L->modal.x + px(U_PAD), w = L->modal.w - px(U_PAD) * 2;
    int y = L->modal.y + px(U_PAD);
    psx_ui_text(&s_cv, ex, y + psx_ui_font_ascent(ft), "Import edited cards", COL_ACCENT, ft); y += psx_ui_font_line_height(ft) + px(4.0f);
    const char *base = strrchr(s_share_path, '/'); base = base ? base + 1 : s_share_path;
    psx_ui_text_clip(&s_cv, ex, y + psx_ui_font_ascent(fs), base, COL_DIM, fs, w); y += psx_ui_font_line_height(fs) + px(6.0f);
    char line[512];
    snprintf(line, sizeof line, "%d edited card%s in this file%s.", s_share.card_n, s_share.card_n == 1 ? "" : "s",
             s_share.has_drops ? ", plus drop table edits" : "");
    y = draw_wrapped(ex, y, w, line, COL_TEXT, fb, 2);
    if (s_share.replace_n) {
        unsigned n = (unsigned)snprintf(line, sizeof line, "%d replace%s a card you already edited: ", s_share.replace_n, s_share.replace_n == 1 ? "s" : "");
        for (int i = 0; i < s_share.replace_n && n + 48 < sizeof line; i++) {
            if (i == 12) { n += (unsigned)snprintf(line + n, sizeof line - n, " and %d more", s_share.replace_n - i); break; }
            n += (unsigned)snprintf(line + n, sizeof line - n, "%s%d %s", i ? ", " : "", s_share.replace_ids[i], psx_card_db_name(s_share.replace_ids[i]));
        }
        y = draw_wrapped(ex, y, w, line, COL_WARN, fb, 4);
    } else {
        y = draw_wrapped(ex, y, w, "None of them replaces a card you edited.", COL_DIM, fb, 1);
    }
    if (s_share.has_drops) y = draw_wrapped(ex, y, w, s_share.drops_here ? "The drop table edits in the file replace yours." : "The file's drop table edits are installed too.", COL_WARN, fb, 2);
    (void)y;
    draw_button(&L->modal_ok, "Import", 1, s_modal_hover == 0);
    draw_button(&L->modal_cancel, "Cancel", 0, s_modal_hover == 1);
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    draw_list();
    draw_editor();
    if (s_modal) draw_modal();
}

/* --- input -------------------------------------------------------------------- */
static void set_scroll_from_thumb(int y)
{
    const Layout *L = &s_L;
    if (s_order_n <= L->rows) return;
    int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
    const int range = L->sb.h - th;
    if (range <= 0) return;
    int t = y - s_sb_grab - L->sb.y;
    if (t < 0) t = 0;
    if (t > range) t = range;
    s_scroll = (int)((long)t * (s_order_n - L->rows) / range);
    clamp_scroll();
    s_dirty = 1;
}

static void click(int x, int y, int button)
{
    layout_compute();
    const Layout *L = &s_L;
    if (s_modal) {
        if (in_rect(&L->modal_ok, x, y)) finish_import(1);
        else if (in_rect(&L->modal_cancel, x, y)) finish_import(0);
        return;
    }
    if (s_focus >= 0) focus_commit();
    if (in_rect(&L->list, x, y)) {
        if (s_order_n > L->rows && x >= L->sb.x - px(4.0f) && y >= L->sb.y && y < L->sb.y + L->sb.h) {
            int th = L->sb.h * L->rows / s_order_n; if (th < px(12.0f)) th = px(12.0f);
            const int ty = L->sb.y + (L->sb.h - th) * s_scroll / (s_order_n - L->rows);
            s_sb_drag = 1;
            if (y >= ty && y < ty + th) s_sb_grab = y - ty;
            else { s_sb_grab = th / 2; set_scroll_from_thumb(y); }
            s_dirty = 1;
            return;
        }
        if (in_rect(&L->list_rows, x, y)) {
            const int i = (y - L->list_rows.y) / L->row_h;
            if (i >= 0 && i < L->rows && s_scroll + i < s_order_n) select_card(s_order[s_scroll + i]);
        }
        return;
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (!field_applies(f)) continue;
        if (in_rect(&L->value[f], x, y)) {
            if (field_is_enum(f)) field_step(f, button == 3 ? -1 : +1);
            else focus_begin(f);
            return;
        }
        if (field_is_enum(f)) {
            if (in_rect(&L->step_l[f], x, y)) { field_step(f, -1); return; }
            if (in_rect(&L->step_r[f], x, y)) { field_step(f, +1); return; }
        }
        if (field_is_set(f) && in_rect(&L->clear[f], x, y)) { field_clear(f); return; }
    }
    for (int b = 0; b < B_COUNT; b++) {
        if (!in_rect(&L->btn[b], x, y)) continue;
        switch (b) {
        case B_SAVE: do_save(); break;
        case B_RESTORE: do_restore(); break;
        case B_FOLDER: do_open_folder(); break;
        case B_ART: do_pick(1); break;
        case B_THUMB: do_pick(2); break;
        case B_TITLE: do_pick(3); break;
        case B_EXPORT: do_export(); break;
        case B_IMPORT: do_import(); break;
        }
        return;
    }
}

static void scroll_by(int amount)
{
    s_scroll += amount;
    clamp_scroll();
    s_dirty = 1;
}

static int on_event(const void *evp)
{
    const SDL_Event *ev = (const SDL_Event *)evp;
    if (!s_win) return 0;
    const Uint32 id = SDL_GetWindowID(s_win);
    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.windowID != id) return 0;
        click((int)ev->button.x, (int)ev->button.y, ev->button.button);
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.windowID != id) return 0;
        if (s_sb_drag) { s_sb_drag = 0; s_dirty = 1; }
        return 1;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        const int x = (int)ev->motion.x, y = (int)ev->motion.y;
        if (s_sb_drag) { set_scroll_from_thumb(y); return 1; }
        int row = -1, btn = -1;
        if (s_modal) {
            const int mh = in_rect(&s_L.modal_ok, x, y) ? 0 : in_rect(&s_L.modal_cancel, x, y) ? 1 : -1;
            if (mh != s_modal_hover) { s_modal_hover = mh; s_dirty = 1; }
            return 1;
        }
        if (in_rect(&s_L.list_rows, x, y)) {
            row = (y - s_L.list_rows.y) / s_L.row_h;
            if (row >= s_L.rows) row = -1;
        }
        for (int b = 0; b < B_COUNT; b++) if (in_rect(&s_L.btn[b], x, y)) btn = b;
        if (row != s_hover_row || btn != s_hover_btn) { s_hover_row = row; s_hover_btn = btn; s_dirty = 1; }
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int mx = (int)ev->wheel.mouse_x;
#else
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
#endif
        if (mx < s_L.list.x + s_L.list.w) scroll_by(ev->wheel.y > 0 ? -3 : 3);
        return 1;
    }
    case SDL_KEYUP:
        return ev->key.windowID == id;
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (s_modal) {
            if (key == SDLK_ESCAPE) finish_import(0);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) finish_import(1);
            return 1;
        }
        if (s_focus >= 0) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) focus_commit();
            else if (key == SDLK_ESCAPE) { s_focus = -1; s_dirty = 1; }
            else if (key == SDLK_BACKSPACE) { const size_t n = strlen(s_buf); if (n) s_buf[n - 1] = 0; s_dirty = 1; }
            else if (key == SDLK_TAB) { const int f = s_focus; focus_commit(); int nf = f + 1; while (nf < F_COUNT && (field_is_enum(nf) || !field_applies(nf))) nf++; if (nf < F_COUNT) focus_begin(nf); }
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            if (s_search[0]) { s_search[0] = 0; rebuild_order(); }
            else psx_card_manager_close();
        } else if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = 0; s_scroll = 0; rebuild_order(); }
        } else if (key == SDLK_PAGEUP) scroll_by(-s_L.rows);
        else if (key == SDLK_PAGEDOWN) scroll_by(s_L.rows);
        else if (key == SDLK_DOWN || key == SDLK_UP) {
            int k = 0;
            for (; k < s_order_n && s_order[k] != s_sel; k++) {}
            k += key == SDLK_DOWN ? 1 : -1;
            if (k >= 0 && k < s_order_n) {
                select_card(s_order[k]);
                if (k < s_scroll) s_scroll = k;
                if (k >= s_scroll + s_L.rows) s_scroll = k - s_L.rows + 1;
            }
        } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && s_changed) do_save();
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        for (const char *p = ev->text.text; *p; p++) {
            const unsigned char ch = (unsigned char)*p;
            if (ch < 32u || ch >= 127u) continue;
            if (s_focus >= 0) {
                const size_t n = strlen(s_buf);
                const size_t cap = s_focus == F_NAME ? PSX_CARD_PACK_NAME_MAX : s_focus == F_DESC ? PSX_CARD_PACK_DESC_MAX :
                                   s_focus == F_PASSWORD ? 8 : s_focus == F_EQUIPS ? FTEXT - 8 : s_focus == F_BOOST ? 500 : s_focus == F_RITUAL ? 40 : 6;
                if (n < cap) { s_buf[n] = (char)ch; s_buf[n + 1] = 0; }
            } else {
                const size_t n = strlen(s_search);
                if (n + 1 < sizeof s_search) { s_search[n] = (char)ch; s_search[n + 1] = 0; s_scroll = 0; rebuild_order(); }
            }
            s_dirty = 1;
        }
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_card_manager_close();
        return 1;
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

/* --- window lifecycle ------------------------------------------------------------ */
static void gl_capture(void)
{
    s_gl_win = SDL_GL_GetCurrentWindow();
    s_gl_ctx = SDL_GL_GetCurrentContext();
}
static void gl_restore(void)
{
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
    s_present_count++;
}

void psx_card_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    s_win = SDL_CreateWindow("Card Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!s_win) { host_osd_push("Card manager: no window", 2000); return; }
    gl_capture();
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    gl_restore();
    if (!s_ren) {
        SDL_DestroyWindow(s_win); s_win = NULL;
        host_osd_push("Card manager: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_card_manager_close(); return; }
    rebuild_order();
    load_editor();
    SDL_StartTextInput(s_win);
}

void psx_card_manager_close(void)
{
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win); s_win = NULL; }
    gl_restore();
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    s_hover_row = -1;
    s_hover_btn = -1;
    s_focus = -1;
    s_sb_drag = 0;
}

int psx_card_manager_is_open(void) { return s_win != NULL; }

void psx_card_manager_select(int id)
{
    if (!s_win) return;
    select_card(id);
}

static void tick(void)
{
    const int req = s_open_req;
    if (req) { s_open_req = 0; if (req > 0) psx_card_manager_open(); else psx_card_manager_close(); }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) {
        if (!ensure_canvas(w, h)) { psx_card_manager_close(); return; }
    }
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; s_dirty = 1; }
    }
    if (s_pick_path[0]) {
        const int kind = s_pick_kind;
        char src[1024]; snprintf(src, sizeof src, "%s", s_pick_path);
        s_pick_path[0] = 0;
        if (kind == 4) begin_export(src);
        else if (kind == 5) begin_import(src);
        else if (install_pick(src, kind)) {
            psx_card_packs_reload(s_sel);
            say(kind == 1 ? "Face art installed" : kind == 2 ? "Duel thumbnail installed" : "Title strip installed");
            PsxCardPack p;
            if (psx_card_packs_get(s_sel, &p)) { s_edit.has_art = p.has_art; s_edit.has_thumb = p.has_thumb; s_edit.has_title = p.has_title; s_has_pack = 1; }
            rebuild_order();
        } else say("Could not copy that file into the card's folder");
    }
    static int last_ready = -1;
    const int ready = psx_card_db_ready();
    if (ready != last_ready) { last_ready = ready; rebuild_order(); if (ready) load_editor(); }
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        if (!s_changed && s_focus < 0) load_editor();
        s_dirty = 1;
    }
    refresh_preview();
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    if (s_dirty) {
        draw();
        s_dirty = 0;
        present_canvas();
    }
}

/* --- debug plumbing ---------------------------------------------------------------- */
void psx_card_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

int psx_card_manager_state_json(char *out, unsigned cap)
{
    static char t[F_COUNT][FTEXT];
    for (int f = 0; f < F_COUNT; f++) field_text(f, 0, t[f], sizeof t[f]);
    if (s_win) layout_compute();
    unsigned n = (unsigned)snprintf(out, cap,
        "\"open\":%d,\"card\":%d,\"edited\":%d,\"changed\":%d,\"focus\":%d,\"buf\":\"%s\","
        "\"search\":\"%s\",\"rows\":%d,\"scroll\":%d,\"w\":%d,\"h\":%d,\"unit\":%.2f,\"msg\":\"%s\","
        "\"name\":\"%s\",\"desc\":\"%s\",\"atk\":\"%s\",\"def\":\"%s\",\"star1\":\"%s\",\"star2\":\"%s\",\"type\":\"%s\","
        "\"level\":\"%s\",\"attr\":\"%s\",\"price\":\"%s\",\"password\":\"%s\","
        "\"effect\":\"%s\",\"amount\":\"%s\",\"target\":\"%s\",\"terrain\":\"%s\",\"ritual\":\"%s\",\"equip_bonus\":\"%s\",\"equips\":\"%.200s\",\"boost\":\"%.200s\",\"trap_max\":\"%s\","
        "\"art\":%d,\"thumb\":%d,\"title\":%d,\"presents\":%u,\"modal\":%d,\"geom\":{",
        s_win != NULL, s_sel, s_has_pack, s_changed, s_focus, s_buf, s_search, s_order_n, s_scroll,
        s_w, s_h, s_u, s_msg, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10],
        t[11], t[12], t[13], t[14], t[15], t[16], t[17], t[18], t[19],
        s_edit.has_art, s_edit.has_thumb, s_edit.has_title, s_present_count, s_modal);
    if (s_win && n < cap) {
        n += (unsigned)snprintf(out + n, cap - n, "\"rows\":[%d,%d,%d,%d],\"row_h\":%d,\"visible\":%d,\"sb\":[%d,%d,%d,%d],\"value\":[",
                                s_L.list_rows.x, s_L.list_rows.y, s_L.list_rows.w, s_L.list_rows.h, s_L.row_h, s_L.rows,
                                s_L.sb.x, s_L.sb.y, s_L.sb.w, s_L.sb.h);
        for (int f = 0; f < F_COUNT && n < cap; f++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d,%d,%d]", f ? "," : "", s_L.value[f].x, s_L.value[f].y, s_L.value[f].w, s_L.value[f].h);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"step_r\":[");
        for (int f = 0; f < F_COUNT && n < cap; f++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d]", f ? "," : "", s_L.step_r[f].x + s_L.step_r[f].w / 2, s_L.step_r[f].y + s_L.step_r[f].h / 2);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"btn\":[");
        for (int b = 0; b < B_COUNT && n < cap; b++)
            n += (unsigned)snprintf(out + n, cap - n, "%s[%d,%d]", b ? "," : "", s_L.btn[b].x + s_L.btn[b].w / 2, s_L.btn[b].y + s_L.btn[b].h / 2);
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"applies\":[");
        for (int f = 0; f < F_COUNT && n < cap; f++) n += (unsigned)snprintf(out + n, cap - n, "%s%d", f ? "," : "", field_applies(f));
        if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "],\"modal_ok\":[%d,%d],\"modal_cancel\":[%d,%d]",
                                             s_L.modal_ok.x + s_L.modal_ok.w / 2, s_L.modal_ok.y + s_L.modal_ok.h / 2,
                                             s_L.modal_cancel.x + s_L.modal_cancel.w / 2, s_L.modal_cancel.y + s_L.modal_cancel.h / 2);
    }
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "}");
    return n < cap;
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

int psx_card_manager_button(int x, int y, int button, int down)
{
    return inject_button(x, y, button <= 0 ? SDL_BUTTON_LEFT : button, down);
}

int psx_card_manager_move(int x, int y)
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

int psx_card_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_card_manager_type(const char *text)
{
    if (!s_win || !text) return 0;
    static char keep[64];
    snprintf(keep, sizeof keep, "%s", text);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_TEXTINPUT;
    ev.text.windowID = SDL_GetWindowID(s_win);
#if defined(PSX_SDL3)
    ev.text.text = keep;
#else
    snprintf(ev.text.text, sizeof ev.text.text, "%s", keep);
#endif
    return SDL_PushEvent(&ev) == 1;
}

int psx_card_manager_key(int sdl_key)
{
    if (!s_win) return 0;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.windowID = SDL_GetWindowID(s_win);
#if defined(PSX_SDL3)
    ev.key.key = (SDL_Keycode)sdl_key;
    ev.key.down = true;
#else
    ev.key.keysym.sym = (SDL_Keycode)sdl_key;
    ev.key.state = SDL_PRESSED;
#endif
    return SDL_PushEvent(&ev) == 1;
}

void psx_card_manager_import_preview(const char *path)
{
    if (!s_win || !path || !path[0]) return;
    begin_import(path);
}

void psx_card_manager_search(const char *text)
{
    snprintf(s_search, sizeof s_search, "%s", text ? text : "");
    s_scroll = 0;
    rebuild_order();
}

int psx_card_manager_shot(const char *path)
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

static void row_activate(void) { psx_card_manager_open(); }

PSX_MOD_CONSTRUCTOR(psx_card_manager_install)
{
    (void)psx_video_menu_add_action(PSX_VM_MENU_VIEW, "Card manager",
                                    "Change a card's name, description, art, stats, stars, effects, price and password; export or import them",
                                    row_activate);
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
