/* MODS > CONFIRM MENU CIRCLE EXIT.
 *
 * Circle on mode-select (CAMPAIGN / FREE DUEL / BUILD DECK / LIBRARY /
 * PASSWORD / SAVE) drops straight to the title menu, so one stray press loses
 * the player's place. This raises a yes/no prompt instead. Off is stock.
 *
 * Cross confirms, Circle cancels, Left/Right choose, NO is the default.
 *
 * HOW THE SCREEN WORKS
 *
 * Menu ID (0x80184594) is the DESTINATION of a menu leave, not the highlight
 * and not the screen: entering a mode leaves it on the row that launched it.
 * 0x80184596 is what says whether mode-select is up (0 here, non-zero once a
 * mode opens); both are required to raise a prompt.
 *
 * Circle sets a leave-requested flag at 0x80184595. The menu library reads it
 * at the top of a later call, and if the Menu ID is a mode-select row it
 * rewrites that byte to LOAD and returns to the caller, which builds the
 * title screen. The leave lands about 17 frames after the press.
 *
 * NO therefore rewrites the DESTINATION once the leave has landed: the leave
 * still happens, it just re-enters the row it came from, which looks like
 * nothing happening. YES declines to rewrite it. Neither synthesises a
 * transition; the game performs its own.
 *
 * TAKING THE PAD
 *
 * func_8003CC38 reads the raw BIOS pad buffer (0x800EF668: status, id,
 * ACTIVE-LOW buttons at +2) and feeds func_8003CCD8, an auto-repeat engine
 * that derives every downstream mirror and accumulator. Hooking that read is
 * the one point both of mode-select's drivers sit downstream of, so the
 * derived words are born cleared and there is no ordering to lose. Clearing
 * the derived words instead cannot work: the fresh-load scene loop consumes
 * them before the frame routine runs.
 *
 * The hook is opted in through game.toml's mod_function_entry_funcs, which is
 * the one change here that needs a regenerate.
 *
 * Three rules, each load-bearing:
 *
 *   - Never clear what the repeat engine derives new-press from. It computes
 *     `current & ~previous`, so zeroing held state makes every frame a fresh
 *     press and the d-pad accelerates.
 *   - Hold a release latch after the prompt closes. A button still down would
 *     otherwise reappear as a brand-new press on the next frame.
 *   - Eat Circle too while the prompt is up. Answering through the game's own
 *     leave lets a held Circle auto-repeat into a second leave, which raises a
 *     fresh prompt the moment the first is dismissed.
 *
 * KNOWN LIMITATION: the prompt is silent. play_se needs guest context this
 * screen offers no safe point for.
 */

#include <string.h>

#include "cpu_state.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_fusion_font.h"
#include "psx_game_hooks.h"
#include "psx_mode_select_confirm.h"
#include "psx_rank_sprites.h"   /* PsxSprite */
#include "psx_shop_skin.h"
#include "psx_video_menu.h"

#define MENU_ID_ADDR         0x80184594u   /* leave destination */
#define MENU_ID_LOAD         0x01u
#define LEAVE_FLAG_ADDR      0x80184595u
#define SCREEN_GATE_ADDR     0x80184596u   /* 0 = mode-select is up */
#define MENU_ID_MODESEL_LO   0x05u
#define MENU_ID_MODESEL_HI   0x0Au

/* Raw BIOS pad buffer, buttons at +2, ACTIVE LOW. */
#define RAW_PAD_BTN_ADDR     0x800EF66Au
#define RAW_EAT_MASK         0xFFFFu
#define RAW_CIRCLE           0x2000u

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }


/* Frames a confirmed YES keeps asserting Circle. Bounded because the pad is
 * held away from the screen meanwhile, so a leave that never lands must not
 * freeze input permanently. */
#define YES_DRIVE_FRAMES 90
#define NP_RIGHT  0x2000u
#define NP_LEFT   0x8000u
#define NP_CROSS  0x0040u
#define NP_CIRCLE 0x0020u

/* ---- state --------------------------------------------------------------- */
static int      g_enabled = 1;    /* MODS > CONFIRM MENU CIRCLE EXIT, on */
static int      s_open;           /* the prompt is up */
static int      s_have_prev;      /* s_prev_id holds a real prior sample */
static uint8_t  s_prev_id;        /* destination byte observed last tick */
static int      s_dirty;          /* the overlay needs a present */
static uint16_t s_latch;          /* buttons the entry hook took, ACTIVE HIGH */
static int      s_leaving;        /* frames left driving a confirmed YES */
static uint16_t s_hold_raw;       /* raw bits masked until the player lets go */

/* YES first, NO second, matching the game's own LOAD? popup. */
#define SEL_YES 0
#define SEL_NO  1
static int      s_sel = SEL_NO;   /* NO by default: a stray Circle cannot exit */

static int mode_select_row(uint8_t id) {
    return id >= MENU_ID_MODESEL_LO && id <= MENU_ID_MODESEL_HI;
}

/* Are we actually looking at mode-select? */
static int on_mode_select(uint8_t id) {
    return mode_select_row(id) && psx_mod_read_byte(SCREEN_GATE_ADDR) == 0u;
}

/* Has a mode opened underneath the prompt? The gate byte also reads non-zero
 * for the few frames a leave is in flight; the leave flag separates them. */
static int mode_entered(void) {
    return psx_mod_read_byte(SCREEN_GATE_ADDR) != 0u &&
           psx_mod_read_byte(LEAVE_FLAG_ADDR)  == 0u;
}

static void prompt_close(void) {
    if (!s_open) return;
    s_open = 0;
    s_latch = 0;
    s_leaving = 0;
    /* Arm the release latch: a button still held must not reappear as a
     * fresh press the frame after the prompt closes. */
    s_hold_raw = 0xFFFFu;
    s_dirty = 1;   /* one more present, to take the box back off the screen */
}

/* ---- the decision -------------------------------------------------------- */
/* Steer the leave by its destination once it lands. Never clear the leave
 * flag: denied its leave the library falls through to its normal dispatcher
 * and accepts the highlighted row. */
/* Entry of the routine that reads the raw pad buffer, in guest context and
 * upstream of everything derived from it. */
static void on_frame_entry(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (s_leaving) {
        /* Hand the screen a Circle and nothing else; the repeat engine makes
         * it a genuine new-press and the screen runs its own leave. */
        psx_mod_write_half(RAW_PAD_BTN_ADDR, (uint16_t)(0xFFFFu & ~RAW_CIRCLE));
        return;
    }
    if (s_open) {
        const uint16_t hw    = psx_mod_read_half(RAW_PAD_BTN_ADDR);
        const uint16_t taken = (uint16_t)(~hw & RAW_EAT_MASK);
        if (taken) {
            /* Byte-swapped, matching the layout used below. */
            s_latch |= swap16(taken);
            psx_mod_write_half(RAW_PAD_BTN_ADDR, (uint16_t)(hw | RAW_EAT_MASK));
        }
        return;
    }
    if (s_hold_raw) {
        /* Keep only the bits still held, reporting them released. */
        const uint16_t hw = psx_mod_read_half(RAW_PAD_BTN_ADDR);
        s_hold_raw = (uint16_t)(s_hold_raw & ~hw);
        if (s_hold_raw)
            psx_mod_write_half(RAW_PAD_BTN_ADDR, (uint16_t)(hw | s_hold_raw));
    }
}

static void guard_tick(void) {
    if (!psx_mod_game_started()) { prompt_close(); s_have_prev = 0; return; }

    const uint8_t id = psx_mod_read_byte(MENU_ID_ADDR);

    if (s_leaving > 0 && --s_leaving == 0) {
        /* The leave never landed; give the pad back. */
        prompt_close();
    }

    if (!s_have_prev) { s_prev_id = id; s_have_prev = 1; return; }

    /* The leave landing: destination was a mode-select row and now reads
     * LOAD. The one instant that decides anything. */
    const int landed = mode_select_row(s_prev_id) && id == MENU_ID_LOAD;

    if (s_open) {
        if (landed) {
            /* Any landing here is the answer. YES declines to rewrite the
             * destination, so the game reaches the title by its own path. */
            if (s_sel == SEL_NO) {
                psx_mod_write_byte(MENU_ID_ADDR, s_prev_id);
            } else {
                s_prev_id = id;
            }
            prompt_close();
            return;
        }
        if (mode_entered() || (!mode_select_row(id) && id != MENU_ID_LOAD)) {
            /* A mode opened underneath us, or a savestate restore. */
            prompt_close();
            s_prev_id = id;
            return;
        }
        /* Not pinned: the Menu ID is written FROM the widget's cursor, so
         * writing it back cannot hold the highlight. Track it instead. */
        s_prev_id = id;

        /* Selection, from what the hook took. Left/Right only; everything
         * else is eaten but unbound here. */
        const uint16_t hit = s_latch;
        s_latch = 0;
        const int was = s_sel;
        if (hit & NP_LEFT)  s_sel = SEL_YES;
        if (hit & NP_RIGHT) s_sel = SEL_NO;
        if (s_sel != was) s_dirty = 1;

        if (hit & NP_CIRCLE) { prompt_close(); return; }   /* cancel */
        if (hit & NP_CROSS) {
            if (s_sel == SEL_YES) {
                /* Cross cannot make this screen leave; ask the hook for a
                 * Circle. The prompt stays up until the leave lands. */
                s_leaving = YES_DRIVE_FRAMES;
            } else {
                prompt_close();
            }
        }
        return;
    }

    if (g_enabled && landed && on_mode_select(s_prev_id)) {
        /* Circle's exit landed. Send it back and hold the decision open. */
        psx_mod_write_byte(MENU_ID_ADDR, s_prev_id);
        s_open          = 1;
        s_sel           = SEL_NO;
        s_dirty         = 1;
        return;
    }

    s_prev_id = id;
}

/* ---- drawing ------------------------------------------------------------- */
#define BOX_W 160
#define BOX_H 76
/* Centred on the 320x240 guest picture. */
#define BOX_X ((320 - BOX_W) / 2)
#define BOX_Y ((240 - BOX_H) / 2)

static uint32_t s_px[BOX_W * BOX_H];

/* ---- drawing ------------------------------------------------------------
 * The password screen's box furniture (psx_shop_skin, baked off the player's
 * own disc) and the game's own text font (psx_fusion_font, baked out of VRAM).
 * Neither is invented art, which is what lets a host overlay sit next to the
 * game's own windows without announcing itself.
 *
 * psx_card_shop.c has its own copy of these, authored against its own canvas.
 * They are duplicated rather than shared on purpose: factoring them out would
 * mean 130 lines of churn in a file that already ships, for no behaviour
 * change. If a third overlay ever wants them, that is the moment to extract
 * them properly -- on their own merits, in their own change. */

static void px_fill(int x0, int y0, int w, int h, uint32_t c) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (int y = y0; y < y0 + h && y < BOX_H; y++)
        for (int x = x0; x < x0 + w && x < BOX_W; x++)
            s_px[y * BOX_W + x] = c;
}

static void skin_blit(const PsxSprite *sp, int dx, int dy) {
    if (!sp->px) return;
    for (int y = 0; y < sp->h; y++)
        for (int x = 0; x < sp->w; x++) {
            const int px = dx + x, py = dy + y;
            if (px < 0 || py < 0 || px >= BOX_W || py >= BOX_H) continue;
            const uint32_t c = sp->px[y * sp->w + x];
            if (c >> 24) s_px[py * BOX_W + px] = c;
        }
}

static void skin_blit_rect(const PsxSprite *sp, int dx, int dy,
                           int sx, int w, int h) {
    if (!sp->px) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int px = dx + x, py = dy + y;
            if (px < 0 || py < 0 || px >= BOX_W || py >= BOX_H) continue;
            const uint32_t c = sp->px[y * sp->w + (sx + x)];
            if (c >> 24) s_px[py * BOX_W + px] = c;
        }
}

/* Field, side edges, then the top/bottom strips whose ends carry the corners
 * -- the way the password screen's own draw list assembles a box, at our
 * width. The field is STRETCHED, not tiled: its art carries the box's own
 * light-to-dark shading, so tiling would put the dark bottom mid-box. */
static void skin_box(int x0, int y0, int w, int h) {
    const PsxSprite *f = &psx_spr_shop_field;
    const int iw = w - 8, ih = h - 8;
    if (f->px && iw > 1 && ih > 1)
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                const int sy = y * (f->h - 1) / (ih - 1);
                const int sx = x * (f->w - 1) / (iw - 1);
                const int px = x0 + 4 + x, py = y0 + 4 + y;
                if (px >= 0 && py >= 0 && px < BOX_W && py < BOX_H)
                    s_px[py * BOX_W + px] = f->px[sy * f->w + sx] | 0xFF000000u;
            }
    const PsxSprite *le = &psx_spr_shop_left, *re = &psx_spr_shop_right;
    for (int y = 8; y < h - 8; y += le->h) {
        const int hh = (h - 8 - y) < le->h ? (h - 8 - y) : le->h;
        skin_blit_rect(le, x0, y0 + y, 0, le->w, hh);
        skin_blit_rect(re, x0 + w - re->w, y0 + y, 0, re->w, hh);
    }
    const PsxSprite *ts = &psx_spr_shop_top, *bs = &psx_spr_shop_bot;
    const int end = ts->w / 2;               /* each half owns a corner */
    for (int pass = 0; pass < 2; pass++) {
        const PsxSprite *sp = pass ? bs : ts;
        const int dy = pass ? y0 + h - sp->h : y0;
        skin_blit_rect(sp, x0, dy, 0, end, sp->h);
        skin_blit_rect(sp, x0 + w - end, dy, sp->w - end, end, sp->h);
        for (int x = end; x < w - end; ) {
            const int seg = (w - end - x) < 136 ? (w - end - x) : 136;
            skin_blit_rect(sp, x0 + x, dy, 20, seg, sp->h);
            x += seg;
        }
    }
}

/* The font stores a 4-bit coverage value per pixel, so tinting ramps the core
 * and keeps the game's own dark outline -- no CLUT to track. */
static int put_glyph(int cell, int x0, int y0, uint32_t tint) {
    const PsxFusionFont *f = &psx_fusion_font;
    if (cell < 0) return 4;
    const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
    int hi = 0;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++) {
            const uint8_t v = g[y * f->w + x];
            if (!v) continue;
            if (x > hi) hi = x;
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= BOX_W || py >= BOX_H) continue;
            if (v == 1) { s_px[py * BOX_W + px] = 0xFF101010u; continue; }
            const uint32_t k = v * 17u > 255u ? 255u : v * 17u;
            const uint32_t r = ((tint >> 16 & 0xFFu) * k) / 255u;
            const uint32_t gg = ((tint >> 8 & 0xFFu) * k) / 255u;
            const uint32_t b = ((tint & 0xFFu) * k) / 255u;
            s_px[py * BOX_W + px] = 0xFF000000u | r << 16 | gg << 8 | b;
        }
    return hi + 1;
}

static int put_text(const char *t, int x, int y, uint32_t tint) {
    for (; *t; t++) {
        if (*t == ' ') { x += 4; continue; }
        const int w = put_glyph(psx_fusion_font_cell((unsigned char)*t),
                                x, y, tint);
        x += (w > 2 ? w : 4) + 1;
    }
    return x;
}

/* Advance-width without drawing, mirroring put_text's metrics, so a
 * variable-width label can be centred. */
static int text_width(const char *t) {
    const PsxFusionFont *f = &psx_fusion_font;
    int x = 0;
    for (; *t; t++) {
        if (*t == ' ') { x += 4; continue; }
        const int cell = psx_fusion_font_cell((unsigned char)*t);
        if (cell < 0) { x += 5; continue; }
        const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
        int hi = 0;
        for (int yy = 0; yy < f->h; yy++)
            for (int xx = 0; xx < f->w; xx++)
                if (g[yy * f->w + xx] && xx > hi) hi = xx;
        x += ((hi + 1) > 2 ? (hi + 1) : 4) + 1;
    }
    return x;
}

/* The card shop's palette, for the same reason it uses it: these are the
 * colours the password screen's own box furniture was authored against. */
#define C_GOLD   0xFFE0B84Cu
#define C_WHITE  0xFFF0F0F0u
#define C_GREY   0xFFB0B4C0u
#define C_SEL    0xFF2A3454u

static void put_centred(const char *s, int cx, int y, uint32_t tint) {
    (void)put_text(s, cx - text_width(s) / 2, y, tint);
}

static void draw_prompt(void) {
    memset(s_px, 0, sizeof s_px);
    skin_box(0, 0, BOX_W, BOX_H);

    put_centred("RETURN TO TITLE?", BOX_W / 2, 13, C_GOLD);

    /* The two choices, each centred in its own half. The selected one sits on
     * the shop panel's highlight plate, which is tall enough to contain a
     * glyph rather than ruling a line through it, and is flanked by the
     * password screen's own arrows -- the game's idiom for "this steers". */
    static const char *const CHOICE[2] = { "YES", "NO" };
    for (int i = 0; i < 2; i++) {
        const int cx = BOX_W / 4 + i * (BOX_W / 2);
        const int w  = text_width(CHOICE[i]);
        if (s_sel == i) px_fill(cx - w / 2 - 6, 30, w + 12, 16, C_SEL);
        put_centred(CHOICE[i], cx, 32, s_sel == i ? C_GOLD : C_GREY);
    }

    /* CROSS confirms, CIRCLE cancels -- drawn with the buttons themselves,
     * the same two sprites the password screen labels its OK/END row with. */
    const PsxSprite *xb = &psx_spr_shop_xbtn, *ob = &psx_spr_shop_obtn;
    const int wx = xb->w + 3 + text_width("OK");
    const int wo = ob->w + 3 + text_width("CANCEL");
    int x = (BOX_W - (wx + 12 + wo)) / 2;
    skin_blit(xb, x, 50);
    x = put_text("OK", x + xb->w + 3, 52, C_WHITE) + 12;
    skin_blit(ob, x, 50);
    (void)put_text("CANCEL", x + ob->w + 3, 52, C_WHITE);
}

int psx_mode_select_confirm_image(const uint32_t **px, int *w, int *h) {
    if (!s_open) return 0;
    draw_prompt();
    *px = s_px; *w = BOX_W; *h = BOX_H;
    return 1;
}

void psx_mode_select_confirm_origin(int *x, int *y) {
    *x = BOX_X; *y = BOX_Y;
}

/* Mode-select is a still screen: without this the prompt would not be
 * composited until something else happened to force a present. Latched, so a
 * close gets the one present that takes the box back off. */
int psx_mode_select_confirm_needs_present(void) {
    const int d = s_dirty; s_dirty = 0; return d;
}

/* ---- menu + debug -------------------------------------------------------- */
static void enabled_changed(int value) {
    g_enabled = value ? 1 : 0;
    /* Turning the row off must not strand an open prompt. */
    if (!g_enabled) prompt_close();
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(g_enabled ? "Menu Circle exit: confirm"
                            : "Menu Circle exit: normal", 1200);
}

void psx_mode_select_confirm_register(void) {
    static const char *const ONOFF[] = { "OFF", "ON" };
    static const char *const HINTS[] = {
        "CIRCLE DROPS CAMPAIGN/FREE DUEL/ETC STRAIGHT TO TITLE, AS STOCK",
        "CIRCLE ASKS BEFORE DROPPING CAMPAIGN/FREE DUEL/ETC TO TITLE",
    };
    (void)psx_game_add_vblank_hook(guard_tick);
    /* One hook: the entry of the routine that reads the raw pad buffer,
     * upstream of every derived mirror. */
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.mode_select_confirm.rawpad", 0x8003CC38u, on_frame_entry);
    const int row = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "CONFIRM MENU CIRCLE EXIT", HINTS[1],
        ONOFF, 2, "confirm_menu_circle_exit", 1, enabled_changed);
    psx_video_menu_set_row_hints(row, HINTS);
}
