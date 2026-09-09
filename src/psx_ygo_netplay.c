/* psx_ygo_netplay.c -- see psx_ygo_netplay.h.
 *
 * Three things live here.
 *
 * 1. The session query every writing layer gates on (psx_ygo_netplay_session).
 *
 * 2. The 2P rules screen is forced to OPEN CARD. The game's own 2P mode was
 *    built for one television: its CARD INDICATE default, DECK NUMBER, draws
 *    every hand as numbered backs so the player beside you cannot read it,
 *    and the owner is expected to know their deck by number. Online each
 *    player has their own screen, so the hand is drawn open in guest state
 *    and hidden per machine below. The choice is a widget byte in the main
 *    menu overlay (D_801845BC[2], mirrored into [0]/[1] for the star), which
 *    the screen copies into D_8009B230 at START and the duel copies into
 *    each duellist's +0x1F "keep my hand face down" byte. Forcing the widget
 *    is a pure function of guest state and runs on both peers at the same
 *    simulated vblank, so the machines stay bit-identical.
 *
 * 3. The hidden-information cover. A present-time overlay (never guest VRAM,
 *    never guest RAM, so it cannot show up in a state digest) that, on the
 *    machine whose seat is NOT the side acting, paints card backs over:
 *      - the five hand sprites of the acting side (52x60 quads at the
 *        object's +0x30/+0x32, which follow the hand as it slides in, lifts
 *        for a fusion pick and flies to the field),
 *      - the card detail panel (name, ATK/DEF, type) whenever it describes a
 *        card the local player is not entitled to see: any hand card of the
 *        acting side, or one of their face-down field cards,
 *      - the enlarged card shown while a card is being played, because at
 *        that moment nobody knows yet whether it lands face down.
 *    Face-down field cards themselves are already backs for everyone; that
 *    is game state and needs no help.
 *
 *    Every address below was read off the decomp (memories-decomp, the duel
 *    display object layout and func_80023144). They are listed once here so
 *    the geometry can be re-checked against one table.
 */
#include "psx_ygo_netplay.h"

#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"

#if defined(PSX_HAS_RECOMP_NET) && PSX_HAS_RECOMP_NET
#include "psx_netplay.h"
#endif

int psx_ygo_netplay_session(void)
{
#if defined(PSX_HAS_RECOMP_NET) && PSX_HAS_RECOMP_NET
    return psx_netplay_active() ? 1 : 0;
#else
    return 0;
#endif
}

int psx_ygo_netplay_local_slot(void)
{
#if defined(PSX_HAS_RECOMP_NET) && PSX_HAS_RECOMP_NET
    if (!psx_netplay_active()) return -1;
    return psx_netplay_local_slot();
#else
    return -1;
#endif
}

/* ---- guest addresses --------------------------------------------------- */
#define A_SCENE         0x8009B26Cu  /* main scene, & 0x1F: 3 duel, 1 3D battle, 0x10 2P rules */
#define A_SIDE          0x8009B1D5u  /* side acting, 0 or 1 */
#define A_OPPONENT      0x8009B361u  /* s8, < 0 = 2P / free duel */
#define A_CAMPAIGN      0x8009B360u  /* s8, < 0 = not a campaign duel */
#define A_PHASE         0x8009B23Au  /* u16 duel phase, & 0xF */
#define A_PANEL_MODE    0x8009B34Eu  /* u8, 0 = detail panel off */
#define A_SELECTED_CARD 0x8009B338u  /* s16, the card the panel describes */
#define A_CURSOR_RECORD 0x8009B1B4u  /* ptr: hand or field selection record */
#define A_HAND_CURSOR   0x800E9F10u  /* + side*0x70 = that side's hand selection record */
#define A_HAND_OBJECTS  0x800EA030u  /* 5 x 0xC, +0 = display object of hand slot i */
#define A_CARD_RECORDS  0x801A7AD8u  /* 30 x 0x1C: side*15 + 0..4 hand, 5..14 field */
#define A_ZOOM_OBJECTS  0x800E9EF0u  /* [1] != 0 while the card-play zoom is up */
#define A_DUELISTS      0x800E9FF0u  /* + side*0x20, +0x1F = keep hand face down */
#define A_RULES_WIDGET  0x801845BCu  /* [0],[1] star per pad, [2] the choice: 1 = OPEN CARD */
#define A_TEXTBOX       0x800EB0F8u  /* + kind*0x64: +0x3C x, +0x3E w, +0x40 y, +0x42 h */
#define TB_STRIDE       0x64u
#define TB_HAND         0            /* bottom strip: the acting side's hand-card info */
#define TB_FIELD        2            /* upper strip: the card under the field cursor */

#define REC_SIZE        0x1C
#define REC_CARD        0x0C
#define REC_FLAGS       0x16
#define REC_FACE_DOWN   0x1000u
#define REC_OCCUPIED    0x8000u

#define CARD_W          52
#define CARD_H          60
#define PANEL_W         288
#define PANEL_H         64
#define ZOOM_X          90
#define ZOOM_Y          22
#define ZOOM_W          140
#define ZOOM_H          196

#define SCREEN_W        320
#define SCREEN_H        240

static int ram_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }
static int s16_at(uint32_t a)  { return (int)(int16_t)psx_mod_read_half(a); }

/* ---- 2P duel? which side is the local player? -------------------------- */

static int two_player_duel(void)
{
    return (int8_t)psx_mod_read_byte(A_OPPONENT) < 0 &&
           (int8_t)psx_mod_read_byte(A_CAMPAIGN) < 0;
}

/* ---- 2. rules screen: OPEN CARD ---------------------------------------- */

static void rules_tick(void)
{
    if (!psx_ygo_netplay_session() || !psx_mod_game_started()) return;
    if ((psx_mod_read_byte(A_SCENE) & 0x1F) != 0x10) return;
    if (psx_mod_read_byte(A_RULES_WIDGET + 2) == 1) return;
    psx_mod_write_byte(A_RULES_WIDGET + 2, 1);
    if (psx_mod_read_byte(A_RULES_WIDGET + 0) < 2) psx_mod_write_byte(A_RULES_WIDGET + 0, 1);
    if (psx_mod_read_byte(A_RULES_WIDGET + 1) < 2) psx_mod_write_byte(A_RULES_WIDGET + 1, 1);
}

/* ---- 3. the cover ------------------------------------------------------ */

static uint32_t   s_px[SCREEN_W * SCREEN_H];
static PsxUiCanvas s_cv = { s_px, SCREEN_W, SCREEN_H, 0, 0, 0, 0 };
static int        s_have;          /* the canvas holds something this frame */
static int        s_hold;          /* presents to keep asking for after it empties */

#define COL_BACK   0xFF1B2545u
#define COL_EDGE   0xFF7C8BC4u
#define COL_INNER  0xFF2A386Au
#define COL_MARK   0xFF9FB0E6u
#define COL_PANEL  0xFF101828u
#define COL_TEXT   0xFFDCE3F8u

static void card_back(int x, int y)
{
    psx_ui_round_rect(&s_cv, x, y, CARD_W, CARD_H, 3.0f, COL_BACK);
    psx_ui_round_rect_line(&s_cv, x + 1, y + 1, CARD_W - 2, CARD_H - 2, 2.5f, COL_EDGE, 1.0f);
    psx_ui_round_rect(&s_cv, x + 6, y + 6, CARD_W - 12, CARD_H - 12, 2.0f, COL_INNER);
    /* a diamond, the way the disc's own back carries a centred motif */
    const float cx = x + CARD_W * 0.5f, cy = y + CARD_H * 0.5f;
    psx_ui_line(&s_cv, cx, cy - 12, cx + 10, cy, 1.5f, COL_MARK);
    psx_ui_line(&s_cv, cx + 10, cy, cx, cy + 12, 1.5f, COL_MARK);
    psx_ui_line(&s_cv, cx, cy + 12, cx - 10, cy, 1.5f, COL_MARK);
    psx_ui_line(&s_cv, cx - 10, cy, cx, cy - 12, 1.5f, COL_MARK);
}

static void big_back(int x, int y, int w, int h, const char *label)
{
    psx_ui_round_rect(&s_cv, x, y, w, h, 4.0f, COL_BACK);
    psx_ui_round_rect_line(&s_cv, x + 2, y + 2, w - 4, h - 4, 3.0f, COL_EDGE, 1.5f);
    psx_ui_round_rect(&s_cv, x + 10, y + 10, w - 20, h - 20, 3.0f, COL_INNER);
    if (label) {
        const PsxUiFace *f = psx_ui_font_face(11.0f, PSX_UI_FONT_SEMIBOLD);
        if (f) {
            const int tw = psx_ui_font_text_w(f, label);
            psx_ui_text(&s_cv, x + (w - tw) / 2, psx_ui_baseline_in(y, h, f), label, COL_TEXT, f);
        }
    }
}

static void panel_cover(int x, int y, int w, int h, const char *label)
{
    psx_ui_round_rect(&s_cv, x, y, w, h, 3.0f, COL_PANEL);
    psx_ui_round_rect_line(&s_cv, x + 1, y + 1, w - 2, h - 2, 2.5f, COL_EDGE, 1.0f);
    const PsxUiFace *f = label ? psx_ui_font_face(12.0f, PSX_UI_FONT_SEMIBOLD) : NULL;
    if (f) {
        const int tw = psx_ui_font_text_w(f, label);
        int by = psx_ui_baseline_in(y, h, f);
        if (by > SCREEN_H - 4) by = psx_ui_baseline_in(y, SCREEN_H - y, f);   /* label into the visible part */
        psx_ui_text(&s_cv, x + (w - tw) / 2, by, label, COL_TEXT, f);
    }
}

/* The one thing that is public during a turn is a card sitting FACE-UP on
 * either field. The card-info strips key off gDuel_wSelectedCardID, which
 * follows the acting player's cursor over their own hand and their own
 * face-down cards, so a strip is safe to show only when the id it names is
 * face-up on a field somewhere. Everything else it could name is the acting
 * player's private information. */
static int card_public(int id)
{
    if (id <= 0) return 0;
    for (int s = 0; s < 2; s++) {
        for (int r = s * 15 + 5; r < s * 15 + 15; r++) {   /* field records only */
            const uint32_t a = A_CARD_RECORDS + (uint32_t)r * REC_SIZE;
            const unsigned fl = psx_mod_read_half(a + REC_FLAGS);
            if ((fl & REC_OCCUPIED) == 0 || (fl & REC_FACE_DOWN)) continue;
            if (s16_at(a + REC_CARD) == id) return 1;
        }
    }
    return 0;
}

/* Cover a game text box (by kind) with a solid panel, clipped to the screen.
 * Returns 1 if anything was drawn. */
static int cover_textbox(int kind, const char *label)
{
    const uint32_t b = A_TEXTBOX + (uint32_t)kind * TB_STRIDE;
    int x = s16_at(b + 0x3C), w = s16_at(b + 0x3E);
    int y = s16_at(b + 0x40), h = s16_at(b + 0x42);
    if (w <= 0 || h <= 0 || w > SCREEN_W || h > 320) return 0;
    if (x >= SCREEN_W || y >= SCREEN_H || x + w <= 0 || y + h <= 0) return 0;
    panel_cover(x, y, w, h, label);
    return 1;
}

static void cover_tick(void)
{
    const int was = s_have;
    s_have = 0;
    if (!psx_ygo_netplay_session() || !psx_mod_game_started()) goto done;
    const int slot = psx_ygo_netplay_local_slot();
    if (slot < 0) goto done;
    if ((psx_mod_read_byte(A_SCENE) & 0x1F) != 3) goto done;   /* not the duel board */
    if (!two_player_duel()) goto done;
    const int side = psx_mod_read_byte(A_SIDE) & 1;
    if (side == slot) goto done;                                /* my turn: my hand is mine to see */

    memset(s_px, 0, sizeof s_px);
    const char *who = side == 0 ? "PLAYER 1 IS CHOOSING" : "PLAYER 2 IS CHOOSING";

    /* Hand sprites of the acting side. Only when the game draws them open;
     * in DECK NUMBER mode (+0x1F != 0) they are backs already. */
    if (psx_mod_read_byte(A_DUELISTS + (uint32_t)side * 0x20u + 0x1Fu) == 0) {
        for (int i = 0; i < 5; i++) {
            const uint32_t obj = psx_mod_read_word(A_HAND_OBJECTS + (uint32_t)i * 0xCu);
            if (!ram_ptr(obj)) continue;
            if ((psx_mod_read_half(obj + 0x08) & 0xC0) != 0xC0) continue;   /* not renderable */
            const int rec = psx_mod_read_byte(obj + 0x6A);
            if (rec / 15 != side || rec % 15 > 4) continue;
            const int x = s16_at(obj + 0x30), y = s16_at(obj + 0x32);
            if (x <= -CARD_W || x >= SCREEN_W || y <= -CARD_H || y >= SCREEN_H) continue;
            card_back(x, y);
            s_have = 1;
        }
    }

    /* The card-info strips. The bottom one names the acting player's selected
     * hand card and is always private, so it is covered whenever it is up.
     * The upper one names whatever their cursor points at on the field, which
     * is public only when that card is face-up. Both are the same text boxes
     * the big TRIANGLE card view is built from, so this covers that too. */
    {
        const int sel = s16_at(A_SELECTED_CARD);
        const int public = card_public(sel);
        if (cover_textbox(TB_HAND, who)) s_have = 1;
        if (!public && cover_textbox(TB_FIELD, who)) s_have = 1;
        /* The full-card view (TRIANGLE): panel_mode 1 draws a big card-art
         * image on the left plus a stats box, neither of them the strips
         * above. Cover the whole viewer region when it shows a private card. */
        if (psx_mod_read_byte(A_PANEL_MODE) == 1 && !public) {
            panel_cover(6, 28, 308, 200, who);
            s_have = 1;
        }
    }

    /* The card being played, enlarged. */
    if ((psx_mod_read_half(A_PHASE) & 0xF) == 6 && psx_mod_read_word(A_ZOOM_OBJECTS + 4) != 0) {
        big_back(ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H, who);
        s_have = 1;
    }

done:
    if (was && !s_have) s_hold = 3;
    else if (s_hold > 0) s_hold--;
}

int psx_ygo_netplay_cover_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_have) return 0;
    if (pixels) *pixels = s_px;
    if (w) *w = SCREEN_W;
    if (h) *h = SCREEN_H;
    return 1;
}

void psx_ygo_netplay_cover_origin(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
}

int psx_ygo_netplay_cover_needs_present(void)
{
    return s_have || s_hold > 0;
}

void psx_ygo_netplay_install_hooks(void)
{
    (void)psx_game_add_vblank_hook(rules_tick);
    (void)psx_game_add_frame_hook(cover_tick);
}
