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
 *    machine whose seat is NOT the side acting, paints card backs over the
 *    CARDS the acting player is entitled to see and this player is not:
 *      - the five hand sprites of the acting side (52x60 quads at the
 *        object's +0x30/+0x32, which follow the hand as it slides in, lifts
 *        for a fusion pick and flies to the field),
 *      - the same hand as the 3D placement view draws it: for the first
 *        substate of duel phase 7 the 2D objects are already freed and the
 *        3D path paints the hand itself at a fixed row (x 14+60i, y 160),
 *        open, for a dozen frames -- measured 2026-09-09 with a screenshot
 *        burst against a 25 Hz sample of the duel state,
 *      - the enlarged card shown while a card is being played (phase 6),
 *        because at that moment nobody knows yet whether it lands face down,
 *      - the full card view (TRIANGLE, duel effect kind 2) when it shows a
 *        card that is not face-up on a field.
 *    plus the strip under the hand, which names the selected hand card with
 *    its ATK/DEF, type and stars: it is covered by a "<name> IS CHOOSING"
 *    bar (the seat's lobby display name when known). The upper strip, which
 *    names the card under the field cursor, stays: what it names is on the
 *    field. The first cut covered the whole board with a panel because it
 *    misread D_8009B34E (an effect scroll offset, not a "panel up" flag).
 *    Face-down field cards are backs for everyone already; that is game
 *    state and needs no help.
 *
 *    Every address below was read off the decomp (memories-decomp, the duel
 *    display object layout and func_80023144). They are listed once here so
 *    the geometry can be re-checked against one table.
 */
#include "psx_ygo_netplay.h"

#include <stdio.h>
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

int psx_ygo_netplay_seat_name(int slot, char *out, size_t cap)
{
#if defined(PSX_HAS_RECOMP_NET) && PSX_HAS_RECOMP_NET
    return psx_netplay_seat_name(slot, out, cap);
#else
    (void)slot;
    if (out && cap) out[0] = '\0';
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
#define A_SUBSTATE      0x8009B174u  /* u8, hand/placement substate; & 0x7F == 1 in phase 7 = 3D hand */
#define A_EFFECT        0x8009B254u  /* u8, duel effect kind (| 0x80 while running); 2 = full card view */
#define A_VIEWER_CARD   0x8009B246u  /* u16 gDuel_wViewerCardID, the card the full view shows */
#define A_HAND_OBJECTS  0x800EA030u  /* 5 x 0xC, +0 = display object of hand slot i */
#define A_CARD_RECORDS  0x801A7AD8u  /* 30 x 0x1C: side*15 + 0..4 hand, 5..14 field */
#define A_ZOOM_OBJECTS  0x800E9EF0u  /* [1] != 0 while the card-play zoom is up */
#define A_DUELISTS      0x800E9FF0u  /* + side*0x20, +0x1F = keep hand face down */
#define A_RULES_WIDGET  0x801845BCu  /* [0],[1] star per pad, [2] the choice: 1 = OPEN CARD */
#define A_TEXTBOX       0x800EB0F8u  /* + kind*0x64: +0x3C x, +0x3E w, +0x40 y, +0x42 h */
#define TB_STRIDE       0x64u        /* 4 slots, handed out dynamically */

#define REC_SIZE        0x1C
#define REC_CARD        0x0C
#define REC_FLAGS       0x16
#define REC_FACE_DOWN   0x1000u
#define REC_OCCUPIED    0x8000u

#define CARD_W          52
#define CARD_H          60
#define ZOOM_X          90
#define ZOOM_Y          22
#define ZOOM_W          140
#define ZOOM_H          196
#define HAND3D_X        14           /* phase-7 hand row: x = HAND3D_X + HAND3D_DX * i */
#define HAND3D_DX       60
#define HAND3D_Y        160
#define STRIP_W         288          /* the hand-card strip's text box: 288 wide, top at 210 */
#define STRIP_Y         210
#define STRIP_H         24           /* drawn: box top - 2 .. + 24 */

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
#define COL_TEXT   0xFFDCE3F8u
#define COL_STRIP  0xFF0C1220u

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

/* "<name> IS CHOOSING" over the hand-card strip: the strip names the card
 * under the acting player's cursor, with its ATK/DEF, type and stars, which
 * is the hand's contents one card at a time. The name is the seat's lobby
 * display name when the runtime knows one, else PLAYER 1 / PLAYER 2. */
static void strip_cover(int side)
{
    /* The text-box slots are handed out dynamically (the strip was slot 0 on
     * one turn and slot 2 the next), so find it by shape: 288 wide with its
     * top at row 210. The box records x/w/y as drawn but its +0x42 "height"
     * (64) is not the drawn strip: measured off the framebuffer, the dark
     * strip runs from two rows above the box's top for about 20 rows. */
    int x = 0, y = 0, w = 0, h = STRIP_H, k;
    char name[64], label[96];
    for (k = 0; k < 4; k++) {
        const uint32_t b = A_TEXTBOX + (uint32_t)k * TB_STRIDE;
        if (s16_at(b + 0x3E) == STRIP_W && s16_at(b + 0x40) == STRIP_Y) {
            x = s16_at(b + 0x3C) - 2; w = STRIP_W + 4; y = STRIP_Y - 2;
            break;
        }
    }
    if (w == 0) return;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (!psx_ygo_netplay_seat_name(side, name, sizeof name) || !name[0])
        snprintf(name, sizeof name, "PLAYER %d", side + 1);
    snprintf(label, sizeof label, "%s IS CHOOSING", name);
    psx_ui_round_rect(&s_cv, x, y, w, h, 3.0f, COL_STRIP);
    psx_ui_round_rect_line(&s_cv, x + 1, y + 1, w - 2, h - 2, 2.5f, COL_EDGE, 1.0f);
    {
        const PsxUiFace *f = psx_ui_font_face(11.0f, PSX_UI_FONT_SEMIBOLD);
        if (f) {
            const int tw = psx_ui_font_text_w(f, label);
            psx_ui_text(&s_cv, x + (w - tw) / 2, psx_ui_baseline_in(y, h, f), label, COL_TEXT, f);
        }
    }
    s_have = 1;
}

static unsigned rec_flags(int rec)
{
    return psx_mod_read_half(A_CARD_RECORDS + (uint32_t)rec * REC_SIZE + REC_FLAGS);
}

/* The one thing that is public during a turn is a card sitting FACE-UP on
 * either field. Everything else a viewer could name is the acting player's
 * private information. */
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
    const char *who = side == 0 ? "PLAYER 1 IS VIEWING A CARD" : "PLAYER 2 IS VIEWING A CARD";
    const int phase = psx_mod_read_half(A_PHASE) & 0xF;
    /* In DECK NUMBER mode (+0x1F != 0) the hand is numbered backs already. */
    const int hand_open = psx_mod_read_byte(A_DUELISTS + (uint32_t)side * 0x20u + 0x1Fu) == 0;

    /* Hand sprites of the acting side, wherever the game has moved them. A
     * sprite that already points at a field record is the card in flight:
     * cover it unless that slot is face-up, which is public the moment it
     * lands. */
    if (hand_open) {
        for (int i = 0; i < 5; i++) {
            const uint32_t obj = psx_mod_read_word(A_HAND_OBJECTS + (uint32_t)i * 0xCu);
            if (!ram_ptr(obj)) continue;
            if ((psx_mod_read_half(obj + 0x08) & 0xC0) != 0xC0) continue;   /* not renderable */
            const int rec = psx_mod_read_byte(obj + 0x6A);
            if (rec / 15 != side) continue;
            if (rec % 15 > 4) {
                const unsigned fl = rec_flags(rec);
                if ((fl & REC_OCCUPIED) && !(fl & REC_FACE_DOWN)) continue;
            }
            const int x = s16_at(obj + 0x30), y = s16_at(obj + 0x32);
            if (x <= -CARD_W || x >= SCREEN_W || y <= -CARD_H || y >= SCREEN_H) continue;
            card_back(x, y);
            s_have = 1;
        }
    }

    /* The strip under the hand names the selected hand card. Whenever the
     * hand is up (the objects exist) the strip describes a private card. */
    if (hand_open && ram_ptr(psx_mod_read_word(A_HAND_OBJECTS)))
        strip_cover(side);

    /* Phase 7, first substate: the 2D hand objects are gone and the 3D
     * placement view paints the hand itself, open, at a fixed row while the
     * board tilts. Back every occupied hand slot of the acting side there. */
    if (hand_open && phase == 7 && (psx_mod_read_byte(A_SUBSTATE) & 0x7F) == 1) {
        for (int i = 0; i < 5; i++) {
            if ((rec_flags(side * 15 + i) & REC_OCCUPIED) == 0) continue;
            card_back(HAND3D_X + HAND3D_DX * i, HAND3D_Y);
            s_have = 1;
        }
    }

    /* The card being played, enlarged. */
    if (phase == 6 && psx_mod_read_word(A_ZOOM_OBJECTS + 4) != 0) {
        big_back(ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H, NULL);
        s_have = 1;
    }

    /* The full card view (TRIANGLE): art, name, stats and text fill the
     * screen. Cover it whole when the card it shows is not public. */
    if ((psx_mod_read_byte(A_EFFECT) & 0x7F) == 2 && !card_public(s16_at(A_VIEWER_CARD))) {
        big_back(4, 8, SCREEN_W - 8, SCREEN_H - 16, who);
        s_have = 1;
    }

done:
    if (was && !s_have) s_hold = 3;
    else if (s_hold > 0) s_hold--;
}

/* True while this machine is looking at the other player's turn in a 2P
 * netplay duel: the hand on screen is theirs. Render layers that read the
 * hand (the fusion assistant's badges and "No fusions in hand" line) ask
 * this before drawing. */
int psx_ygo_netplay_hand_hidden(void)
{
    if (!psx_ygo_netplay_session() || !psx_mod_game_started()) return 0;
    const int slot = psx_ygo_netplay_local_slot();
    if (slot < 0) return 0;
    if ((psx_mod_read_byte(A_SCENE) & 0x1F) != 3) return 0;
    if (!two_player_duel()) return 0;
    return (psx_mod_read_byte(A_SIDE) & 1) != slot;
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
