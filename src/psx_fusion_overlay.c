/* psx_fusion_overlay.c — the fusion assistant's line above the hand.
 *
 * See psx_fusion_overlay.h. This file owns three things: turning a card id into
 * readable text, laying that out small enough to stay out of the way, and
 * marking the cards to pick.
 *
 * ---- card names ------------------------------------------------------------
 *
 * The name blob is dictionary-compressed and does not hold ASCII. Its bytes are
 * the text engine's own character codes, a frequency-ordered alphabet where
 * SPACE is 0 and 'e' is 1, and 0xFF terminates. The table below is that
 * alphabet; it was derived by aligning the blob against a published id->name
 * list across all 722 cards and taking the per-code majority, so every code it
 * claims is backed by many names rather than one guess.
 *
 *     name bytes at  0x801D0000 + u16[0x801D5800 + id*2]
 *
 * ---- keeping it small ------------------------------------------------------
 *
 * The game ships exactly ONE alphabet — the 8x12 cells psx_fusion_font bakes,
 * which is what the card-name bar and the results screen both print with. There
 * is no smaller one to switch to; the only other charset in VRAM is a 16x16 cut
 * of the same glyphs. So "smaller" here is bought two other ways:
 *
 *   - the NUMBERS use psx_spr_digit, the 8x8 card-stat digits the rank meter
 *     already bakes. Those are genuinely smaller than the alphabet, and they
 *     are the digits the game prints attack and defence with everywhere else.
 *   - the NAME is set proportionally AND in a derived 6x9 face (see below).
 *     Every glyph is stored in an 8-wide cell with blank columns either side,
 *     so measuring the real ink and advancing by that plus one takes roughly a
 *     quarter off the line before the smaller face takes anything at all.
 *
 * ---- order badges ----------------------------------------------------------
 *
 * A recommendation the player then has to work out the order for is only half
 * an answer, because the fold is order-sensitive. So the recommended line's
 * cards carry 1..5 in the same small digits, over the hand. They are drawn only
 * while nothing is picked: once the player starts picking, the GAME draws its
 * own numbered badges in that exact spot, and two sets of numbers disagreeing
 * would be worse than none.
 */

#include "psx_fusion_overlay.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_fusion_assist.h"
#include "psx_fusion_db.h"
#include "psx_fusion_font.h"
#include "psx_rank_sprites.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#define PSX_FUSION_NAME_PTRS 0x801D5800u
#define PSX_FUSION_NAME_BASE 0x801D0000u
#define PSX_FUSION_NAME_END  0xFFu

static const char k_charset[0x80] = {
    /* 0x00 */ ' ', 'e', 't', 'a', 'o', 'i', 'n', 's',
    /* 0x08 */ 'r', 'h', 'l', '.', 'd', 'u', 'm', 'c',
    /* 0x10 */ 'g', 'y', 'w', 'f', 'p', 'b', 'k',   0,
    /* 0x18 */ 'A', 'v', 'I','\'', 'T', 'S', 'M', ',',
    /* 0x20 */ 'D', 'O', 'W', 'H', 'Y', 'E', 'R',   0,
    /* 0x28 */   0, 'G', 'L', 'C', 'N', 'B',   0, 'P',
    /* 0x30 */ '-', 'F', 'z', 'K', 'j', 'U', 'x', 'q',
    /* 0x38 */ '0', 'V', '2', 'J', '#', '1', 'Q', 'Z',
    /* 0x40 */   0, '3',   0, '&',   0, '7',   0,   0,
};

/* Layout, guest pixels. The canvas spans the text line AND the top of the hand
 * so the order badges can ride on the cards. Everything here is tunable live:
 * placing this against the game's own art is a by-eye job. */
#define FO_W        320
#define FO_H        112
#define FO_X        0
/* Canvas top. The text sits under the FIELD box the game draws at the top
 * left, and the order badges ride the hand a hundred pixels below, so one tall
 * mostly-transparent canvas covers both rather than two composite passes. */
#define FO_Y        52
#define FO_TEXT_X   16
#define FO_TEXT_Y   4     /* the line's top within the canvas */
#define FO_CARD_X   22    /* left edge of hand card 0 */
#define FO_CARD_DX  58    /* card pitch */
/* The badge sits INSIDE each card's top-right corner, where it reads as part
 * of the card rather than as something floating above the hand. */
#define FO_BADGE_DX 36    /* badge left, from the card's left edge */
#define FO_BADGE_DY 99    /* badge top within the canvas = inside the card */

static int s_x        = FO_X;
static int s_y        = FO_Y;
static int s_text_x   = FO_TEXT_X;
static int s_text_y   = FO_TEXT_Y;
static int s_badge_dx = FO_BADGE_DX;
static int s_card_x   = FO_CARD_X;
static int s_card_dx  = FO_CARD_DX;
static int s_badge_dy = FO_BADGE_DY;
static int s_mode     = PSX_FUSION_HINT_FULL;

static uint32_t s_canvas[FO_W * FO_H];
static char     s_text[64];
static char     s_drawn[64];
static uint8_t  s_badges[PSX_FUSION_HAND_MAX];   /* slot -> 1..5, 0 = none */
static uint8_t  s_badges_drawn[PSX_FUSION_HAND_MAX];
static int      s_have_content;
/* Frames to keep asking for a present after the canvas changed. Without it the
 * presenter's static-frame skip leaves the LAST drawn line on screen after the
 * overlay goes empty — so the hint would linger after the hand stopped being
 * pickable, which is exactly when it must be gone. */
static int      s_present_hold;

/* ---- would this selection land on the suggestion? --------------------------
 *
 * Compares the CARD the current picks would summon against the card being
 * suggested — not the pick order against the suggested pick order.
 *
 * The order test was the first cut and it was wrong. The suggestion is the
 * SHORTEST line to a card, but it is rarely the only one: with Flame Viper,
 * Takriminos, Yamatano Dragon Scroll, Ancient Jar and Maiden of the Moonlight
 * in hand the suggestion is two cards, and picking all five left to right also
 * ends on Mystical Sand — the very card being suggested — because the first
 * three fail to fuse and simply hand the fourth along. Marking that red told
 * the player they were wrong while they were about to be right.
 *
 * Neutral until there are two picks: one card fuses with nothing, so there is
 * nothing to be right or wrong about yet. */
enum { FO_TRACK_NEUTRAL = 0, FO_TRACK_ON = 1, FO_TRACK_OFF = 2 };
static int s_track;
static int s_track_drawn;

static const uint32_t k_track_tint[3] = {
    0xFFFFFFu,   /* neutral: the game's own white */
    0x78FF78u,   /* on the recommended line */
    0xFF7878u,   /* off it */
};
static uint32_t s_tint = 0xFFFFFFu;

static uint32_t tint_px(uint32_t argb)
{
    if (s_tint == 0xFFFFFFu) return argb;
    const uint32_t a = argb & 0xFF000000u;
    const uint32_t r = ((argb >> 16) & 0xFFu) * ((s_tint >> 16) & 0xFFu) / 255u;
    const uint32_t g = ((argb >> 8) & 0xFFu) * ((s_tint >> 8) & 0xFFu) / 255u;
    const uint32_t b = (argb & 0xFFu) * (s_tint & 0xFFu) / 255u;
    return a | (r << 16) | (g << 8) | b;
}

/* ---- a smaller cut of the game's alphabet ---------------------------------
 *
 * The game HAS no smaller alphabet — the 8x12 cells are the only one, and the
 * only other charset in VRAM is a 16x16 cut of the same glyphs. So the small
 * face is derived here: each 8x12 cell is reduced to 6x9 by taking the MAXIMUM
 * source value in each destination pixel's footprint rather than the average.
 * Averaging is the textbook filter and the wrong one for this: the strokes are
 * one or two texels wide, and averaging thins them until letters break up,
 * where max-pooling keeps every stroke and just hardens the anti-aliasing.
 *
 * Built once on first use rather than baked, so the generated font table stays
 * a faithful copy of VRAM and this stays visibly a derivation of it. */
#define FO_SMALL_W 6
#define FO_SMALL_H 9

static uint8_t s_small[PSX_FUSION_FONT_CELLS * FO_SMALL_W * FO_SMALL_H];
static int8_t s_glyph_lo[PSX_FUSION_FONT_CELLS];
static int8_t s_glyph_hi[PSX_FUSION_FONT_CELLS];
static int    s_metrics_done;

static void measure_font(void)
{
    const PsxFusionFont *f = &psx_fusion_font;
    for (int c = 0; c < PSX_FUSION_FONT_CELLS; c++) {
        const uint8_t *g = f->px + (size_t)c * (size_t)f->w * (size_t)f->h;
        uint8_t *s = s_small + (size_t)c * FO_SMALL_W * FO_SMALL_H;
        for (int y = 0; y < FO_SMALL_H; y++) {
            const int sy0 = y * f->h / FO_SMALL_H;
            int sy1 = (y + 1) * f->h / FO_SMALL_H;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int x = 0; x < FO_SMALL_W; x++) {
                const int sx0 = x * f->w / FO_SMALL_W;
                int sx1 = (x + 1) * f->w / FO_SMALL_W;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                uint8_t m = 0;
                for (int sy = sy0; sy < sy1 && sy < f->h; sy++)
                    for (int sx = sx0; sx < sx1 && sx < f->w; sx++) {
                        const uint8_t v = g[sy * f->w + sx];
                        if (v > m) m = v;
                    }
                s[y * FO_SMALL_W + x] = m;
            }
        }
        int lo = FO_SMALL_W, hi = -1;
        for (int y = 0; y < FO_SMALL_H; y++)
            for (int x = 0; x < FO_SMALL_W; x++)
                if (s[y * FO_SMALL_W + x]) {
                    if (x < lo) lo = x;
                    if (x > hi) hi = x;
                }
        s_glyph_lo[c] = (int8_t)(hi < 0 ? 0 : lo);
        s_glyph_hi[c] = (int8_t)hi;
    }
    s_metrics_done = 1;
}

static int card_name(uint16_t id, char *out, int cap)
{
    int n = 0;
    if (id < 1 || id > PSX_FUSION_CARD_ID_MAX || cap <= 0) {
        if (cap > 0) out[0] = 0;
        return 0;
    }
    const uint32_t off =
        psx_mod_read_half(PSX_FUSION_NAME_PTRS + (uint32_t)id * 2u);
    const uint32_t p = PSX_FUSION_NAME_BASE + off;
    for (int i = 0; i < cap - 1; i++) {
        const uint8_t b = psx_mod_read_byte(p + (uint32_t)i);
        if (b == PSX_FUSION_NAME_END) break;
        const char c = (b < 0x80u) ? k_charset[b] : 0;
        if (c) out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* ---- rasterising ---------------------------------------------------------- */

static void put_px(int x, int y, uint32_t argb)
{
    if (x < 0 || x >= FO_W || y < 0 || y >= FO_H) return;
    s_canvas[y * FO_W + x] = tint_px(argb);
}

/* The stored 4-bit value IS the brightness the game draws: 1 the dark outline,
 * 15 the white core. Scaling it to 8 bits reproduces the same text. */
static int put_glyph(int cell, int x, int y)
{
    if (cell < 0 || cell >= PSX_FUSION_FONT_CELLS) return 0;
    const uint8_t *g = s_small + (size_t)cell * FO_SMALL_W * FO_SMALL_H;
    const int lo = s_glyph_lo[cell], hi = s_glyph_hi[cell];
    if (hi < 0) return 0;
    for (int row = 0; row < FO_SMALL_H; row++)
        for (int col = lo; col <= hi; col++) {
            const uint8_t v = g[row * FO_SMALL_W + col];
            if (!v) continue;
            const uint32_t l = (uint32_t)v * 17u;
            put_px(x + col - lo, y + row,
                   0xFF000000u | (l << 16) | (l << 8) | l);
        }
    return hi - lo + 1;
}

static void put_sprite(const PsxSprite *s, int x, int y)
{
    if (!s || !s->px) return;
    for (int row = 0; row < s->h; row++)
        for (int col = 0; col < s->w; col++) {
            const uint32_t p = s->px[row * s->w + col];
            if (p >> 24) put_px(x + col, y + row, p);
        }
}

/* Attack and defence in the game's small card-stat digits, separated by a
 * drawn slash — the digit set is 0..9 and nothing else, and borrowing the
 * alphabet's '/' would put a 12-tall glyph in the middle of 8-tall numbers. */
static int put_number(int v, int x, int y)
{
    char buf[8];
    const int n = snprintf(buf, sizeof buf, "%d", v < 0 ? 0 : v);
    for (int i = 0; i < n; i++) {
        const int d = buf[i] - '0';
        if (d >= 0 && d <= 9) put_sprite(&psx_spr_digit[d], x, y);
        x += psx_spr_digit[d].w ? psx_spr_digit[d].w : 8;
    }
    return x;
}

static void put_slash(int x, int y)
{
    for (int i = 0; i < 6; i++) put_px(x + 2 - i / 3, y + 1 + i, 0xFFC8C8C8u);
}

static void redraw(void)
{
    memset(s_canvas, 0, sizeof s_canvas);
    s_have_content = 0;
    if (!s_metrics_done) measure_font();
    s_tint = k_track_tint[(s_track_drawn >= 0 && s_track_drawn <= 2)
                              ? s_track_drawn : 0];

    /* The line: "<name>  <atk>/<def>", name proportional, numbers small. */
    int x = s_text_x;
    const int ty = s_text_y;
    const char *p = s_drawn;
    for (; *p && *p != '\t'; p++) {
        if (*p == ' ') { x += 3; continue; }
        const int cell = psx_fusion_font_cell((unsigned char)*p);
        const int w = put_glyph(cell, x, ty);
        x += (w ? w : 4) + 1;
        s_have_content = 1;
    }
    /* Only the NAME takes the colour. The icons and digits are the game's own
     * art, and recolouring them made the whole line read as a warning rather
     * than as a card — the name alone carries the signal. */
    s_tint = 0xFFFFFFu;
    if (*p == '\t') {
        /* Stats in the small digits, each behind the icon the game itself
         * prints it with — the sword for attack, the shield for defence. That
         * is what makes this read as a card's stat line rather than as two
         * numbers with a slash between them. */
        int atk = 0, def = 0;
        if (sscanf(p + 1, "%d/%d", &atk, &def) == 2) {
            x += 6;
            put_sprite(&psx_fusion_icon_atk, x, ty);
            x = put_number(atk, x + 9, ty);
            put_sprite(&psx_fusion_icon_def, x + 4, ty);
            x = put_number(def, x + 13, ty);
            s_have_content = 1;
        }
    }

    for (int i = 0; i < PSX_FUSION_HAND_MAX; i++) {
        const int n = s_badges_drawn[i];
        if (n < 1 || n > 9) continue;
        put_sprite(&psx_spr_digit[n],
                   s_card_x + i * s_card_dx + s_badge_dx, s_badge_dy);
        s_have_content = 1;
    }
}

/* Build the line and the badge set this frame should show. */
/* Is the card-detail view up? Holding Triangle in a duel opens a full-screen
 * card inspector over the field, and the hand stays "pickable" underneath it --
 * every gate built from the selection table still reads true -- so the hint
 * kept drawing its pick-order badges and result name straight through the card
 * being examined.
 *
 * 0x8009B140 is 0 with the inspector closed and 0x0D while it is open.
 * Established by a three-way RAM diff (closed / Triangle held / released,
 * keeping only bytes that changed and reverted), then confirmed live in both
 * contexts it can be opened from: over a field monster, and from hand view at
 * turn start where the hint is actually on screen. 0x8009B254 (0 -> 0x82)
 * tracks it exactly and is the cross-check; 0x8009B248 also moves but takes
 * several values, so it is animation state rather than the flag. */
#define PSX_FUSION_CARD_DETAIL 0x8009B140u

static int card_detail_open(void)
{
    return psx_mod_read_byte(PSX_FUSION_CARD_DETAIL) != 0u;
}

static void compose(char *out, int cap, uint8_t *badges)
{
    out[0] = 0;
    memset(badges, 0, PSX_FUSION_HAND_MAX);
    if (s_mode == PSX_FUSION_HINT_OFF || !psx_fusion_db_ready()) return;
    /* Empty content, not an early return from the tick: the tick diffs against
     * what it last drew, so composing nothing is what makes it redraw and
     * clear the canvas. Returning from the tick instead would leave the badges
     * on screen until something else happened to change them. */
    if (card_detail_open()) return;

    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    if (psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX) < 2) return;

    PsxFusionCard steps[PSX_FUSION_HAND_MAX];
    uint16_t chain = 0;
    const int picked = psx_fusion_assist_chain(steps, PSX_FUSION_HAND_MAX,
                                               &chain);
    uint8_t pick[PSX_FUSION_HAND_MAX];
    uint16_t best = 0;
    const int nbest = psx_fusion_assist_best(&best, NULL, NULL, NULL, pick,
                                             PSX_FUSION_HAND_MAX);
    uint16_t show = 0;
    if (picked >= 2) {
        show = chain;                       /* the player's own running answer */
        s_track = (best && chain == best) ? FO_TRACK_ON : FO_TRACK_OFF;
        (void)nbest;
    } else {
        show = best;
        s_track = FO_TRACK_NEUTRAL;
        /* Badges only while nothing is picked: the game draws its own numbers
         * on the cards the moment the player starts. */
        if (!picked)
            for (int i = 0; i < nbest && i < PSX_FUSION_HAND_MAX; i++)
                if (pick[i] < PSX_FUSION_HAND_MAX) badges[pick[i]] = (uint8_t)(i + 1);
    }
    if (s_mode < PSX_FUSION_HINT_FULL) return;
    /* A hand that makes nothing says so. Silence would be ambiguous — the
     * player cannot tell "nothing here" from "the hint is off or broken". */
    if (!show) {
        snprintf(out, (size_t)cap, "No fusions in hand");
        return;
    }

    char name[40];
    card_name(show, name, sizeof name);
    if (!name[0]) return;

    int atk = 0, def = 0;
    psx_fusion_db_stats(show, &atk, &def, NULL);
    /* The tab is the seam between "set this in the alphabet" and "set this in
     * the small digits"; nothing downstream treats it as whitespace. */
    snprintf(out, (size_t)cap, "%s\t%d/%d", name, atk, def);
}

void psx_fusion_overlay_tick(void)
{
    uint8_t badges[PSX_FUSION_HAND_MAX];
    compose(s_text, sizeof s_text, badges);
    if (strcmp(s_text, s_drawn) == 0 && s_track == s_track_drawn &&
        memcmp(badges, s_badges_drawn, sizeof badges) == 0) {
        if (s_present_hold > 0) s_present_hold--;
        return;
    }
    memcpy(s_drawn, s_text, sizeof s_drawn);
    s_track_drawn = s_track;
    memcpy(s_badges_drawn, badges, sizeof badges);
    memcpy(s_badges, badges, sizeof badges);
    redraw();
    s_present_hold = 4;
}

int psx_fusion_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_have_content) return 0;
    if (pixels) *pixels = s_canvas;
    if (w) *w = FO_W;
    if (h) *h = FO_H;
    return 1;
}

void psx_fusion_overlay_origin(int *x, int *y)
{
    if (x) *x = s_x;
    if (y) *y = s_y;
}

int psx_fusion_overlay_needs_present(void)
{
    return s_have_content || s_present_hold > 0;
}

void psx_fusion_overlay_tune(int x, int y, int text_x, int mode)
{
    if (x      != PSX_FUSION_OVERLAY_KEEP) s_x      = x;
    if (y      != PSX_FUSION_OVERLAY_KEEP) s_y      = y;
    if (text_x != PSX_FUSION_OVERLAY_KEEP) s_text_x = text_x;
    if (mode   != PSX_FUSION_OVERLAY_KEEP &&
        mode >= PSX_FUSION_HINT_OFF && mode <= PSX_FUSION_HINT_FULL)
        s_mode = mode;
    s_drawn[0] = 0;
    memset(s_badges_drawn, 0xFF, sizeof s_badges_drawn);
    psx_fusion_overlay_tick();
}

void psx_fusion_overlay_tune_get(int *x, int *y, int *text_x, int *mode)
{
    if (x)      *x      = s_x;
    if (y)      *y      = s_y;
    if (text_x) *text_x = s_text_x;
    if (mode)   *mode   = s_mode;
}

void psx_fusion_overlay_tune_cards(int card_x, int card_dx, int badge_dy,
                                   int badge_dx, int text_y)
{
    if (card_x   != PSX_FUSION_OVERLAY_KEEP) s_card_x   = card_x;
    if (card_dx  != PSX_FUSION_OVERLAY_KEEP) s_card_dx  = card_dx;
    if (badge_dy != PSX_FUSION_OVERLAY_KEEP) s_badge_dy = badge_dy;
    if (badge_dx != PSX_FUSION_OVERLAY_KEEP) s_badge_dx = badge_dx;
    if (text_y   != PSX_FUSION_OVERLAY_KEEP) s_text_y   = text_y;
    memset(s_badges_drawn, 0xFF, sizeof s_badges_drawn);
    psx_fusion_overlay_tick();
}

void psx_fusion_overlay_tune_cards_get(int *card_x, int *card_dx, int *badge_dy,
                                       int *badge_dx, int *text_y)
{
    if (card_x)   *card_x   = s_card_x;
    if (card_dx)  *card_dx  = s_card_dx;
    if (badge_dy) *badge_dy = s_badge_dy;
    if (badge_dx) *badge_dx = s_badge_dx;
    if (text_y)   *text_y   = s_text_y;
}

int psx_fusion_overlay_badges(uint8_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < PSX_FUSION_HAND_MAX && i < cap; i++) {
        out[i] = s_badges[i];
        if (s_badges[i]) n++;
    }
    return n;
}

/* VIEW > FUSION HINT and VIEW > SUGGEST FUSION BY.
 *
 * Two rows rather than one combined list: the display choice and the ranking
 * choice are independent, and folding them together would make six states
 * where two suffice. SUGGEST FUSION BY is named to read as a continuation of
 * the row above -- on its own "SUGGEST BY" gives no clue which feature it
 * belongs to. */
static const char *const FUSION_HINT_LABELS[] = {
    "OFF", "NUMBERS", "NUMBERS + INFO"
};
static const char *const FUSION_HINT_HINTS[] = {
    "SHOW WHAT YOUR HAND CAN FUSE INTO",
    "PICK ORDER ON THE CARDS ONLY",
    "PICK ORDER PLUS THE CARD IT MAKES"
};
static const char *const FUSION_BY_LABELS[] = { "ATTACK", "DEFENSE" };
static const char *const FUSION_BY_HINTS[]  = {
    "SUGGEST THE BEST ATTACK",
    "SUGGEST THE BEST DEFENSE"
};

static void fusion_hint_changed(int value) {
    psx_fusion_overlay_set_mode(value);
}

static void fusion_by_changed(int value) {
    psx_fusion_assist_set_rank(value);
}

PSX_MOD_CONSTRUCTOR(psx_fusion_overlay_install) {
    psx_fusion_overlay_register_menu();
    (void)psx_game_add_frame_hook(psx_fusion_overlay_tick);
}

void psx_fusion_overlay_register_menu(void) {
    int h = psx_video_menu_add_option(
        PSX_VM_MENU_VIEW, "FUSION HINT", FUSION_HINT_HINTS[0],
        FUSION_HINT_LABELS, 3, "fusion_hint",
        PSX_VM_FUSION_HINT_OFF, fusion_hint_changed);
    psx_video_menu_set_row_hints(h, FUSION_HINT_HINTS);

    h = psx_video_menu_add_option(
        PSX_VM_MENU_VIEW, "SUGGEST FUSION BY", FUSION_BY_HINTS[0],
        FUSION_BY_LABELS, 2, "fusion_by_def", 0, fusion_by_changed);
    psx_video_menu_set_row_hints(h, FUSION_BY_HINTS);
}

void psx_fusion_overlay_set_mode(int mode)
{
    if (mode < PSX_FUSION_HINT_OFF || mode > PSX_FUSION_HINT_FULL) return;
    if (s_mode == mode) return;
    s_mode = mode;
    s_drawn[0] = 0;
    memset(s_badges_drawn, 0xFF, sizeof s_badges_drawn);
    psx_fusion_overlay_tick();
}

const char *psx_fusion_overlay_text(void) { return s_drawn; }
