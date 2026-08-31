/* psx_rng_view.c — see psx_rng_view.h.
 *
 * One line, top-left:
 *
 *     S 03171C9C  N 31791 +1  D 1469C51C
 *
 *   S  the rand() state word at 0x800FE6F8. Boot init (func_80012B50) calls
 *      srand(0x55555555); every rand() (0x8008E590) advances it as
 *      seed = seed * 0x41C64E6D + 0x3039 and returns (seed >> 16) & 0x7FFF.
 *   N  how many rand() calls it took to get from 0x55555555 to that seed —
 *      the run's absolute RNG index, and the "seed number" the community's
 *      predictors talk about. Not counted by watching: it is SOLVED from the
 *      seed itself, bit by bit (see lcg_index), so it is exact from the first
 *      frame the viewer is switched on and cannot drift. The name-entry
 *      screen advances it by exactly 1 per frame (its loop calls rand() and
 *      discards the result).
 *   +n calls consumed since the previous frame; 0 on screens that leave the
 *      RNG alone.
 *   D  the seed as it stood on the frame BEFORE the last duel-start burst.
 *      A duel start runs its whole setup in one frame — 320 calls shuffling
 *      each deck (160 swaps of two rand()%40 picks, the deck first re-sorted
 *      ascending by card id by the game's own qsort), campaign opponents
 *      adding pool rolls for their generated deck — so a frame that consumes
 *      600+ calls IS a duel start, and the value latched here is the duel
 *      seed the deck predictors search for. Survives in-duel churn until the
 *      next duel; absent until one has started.
 *
 * A cheat check follows from S alone: a legit seed is always LCG-reachable
 * from 0x55555555, and N must line up with frames spent on rand-calling
 * screens. */

#include "psx_rng_view.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#define PSX_RNG_SEED_ADDR  0x800FE6F8u

#define PSX_RNG_LCG_MUL  0x41C64E6Du
#define PSX_RNG_LCG_ADD  0x3039u
#define PSX_RNG_SRAND    0x55555555u

/* ---- solving N from the seed -----------------------------------------------
 *
 * x(n) = MUL^n * x0 + ADD * (MUL^n - 1) / (MUL - 1), all mod 2^32. MUL is
 * 1 mod 4 and ADD is odd, so the LCG has full period 2^32 and x(n) mod 2^k
 * cycles with period exactly 2^k — which means the low k bits of x determine
 * the low k bits of n, and n can be lifted one bit at a time: 32 probes, each
 * a 32-step square-and-multiply, instead of walking up to 2^32 states. */

static uint32_t lcg_at(uint32_t n)
{
    uint32_t mul = 1, add = 0;                       /* accumulated x -> mul*x+add */
    uint32_t am = PSX_RNG_LCG_MUL, aa = PSX_RNG_LCG_ADD;   /* one 2^i-step hop */
    while (n) {
        if (n & 1u) { mul *= am; add = add * am + aa; }
        aa = aa * am + aa;   /* compose the hop with itself; aa first, it */
        am *= am;            /* needs the pre-square am */
        n >>= 1;
    }
    return PSX_RNG_SRAND * mul + add;
}

static uint32_t lcg_index(uint32_t x)
{
    uint32_t n = 0;
    for (int k = 0; k < 32; k++) {
        const uint32_t mask = (k == 31) ? 0xFFFFFFFFu : ((1u << (k + 1)) - 1u);
        if ((lcg_at(n) ^ x) & mask) n |= 1u << k;
    }
    return n;
}

/* ---- panel ----------------------------------------------------------------- */

/* Hand-drawn 5x7 glyphs for exactly the characters this line uses. A first
 * cut max-pooled the shared 8x8 ASCII face down and the strokes merged into
 * mush — a face this small has to be AUTHORED at its size, and twenty glyphs
 * is cheap to author. Bit 0 is the LEFTMOST pixel of a row, matching the
 * project's other bit fonts. */
#define RV_FW    5
#define RV_FH    7

/* Wide enough for the full line with the duel seed latched; the strip's
 * background is only painted as far as the text actually reaches. */
#define RV_W     240
#define RV_PAD   2
#define RV_H     (RV_FH + 2 * RV_PAD)
#define RV_X     2
#define RV_Y     2

#define RV_BG     0xA010101Cu
#define RV_LABEL  0xFF9CB4C8u
#define RV_VALUE  0xFFFFFFFFu
#define RV_DELTA  0xFFFFD070u

/* A frame that consumed this many calls can only be a duel start: the two
 * deck shuffles alone are 640, and nothing else in this game comes close
 * to that in one frame (menus churn 1, duel actions tens). The upper bound
 * rejects the huge apparent jump of a re-seed. */
#define RV_DUEL_BURST_MIN 600

static int      s_on;
static uint32_t s_canvas[RV_W * RV_H];
static int      s_have_content;
static int      s_present_hold;

static uint32_t s_prev_index;
static uint32_t s_prev_seed;
static int      s_prev_valid;
static uint32_t s_duel_seed;
static int      s_duel_valid;

/* What the canvas currently shows, for change detection. */
static char s_line[64];
static char s_drawn[64];

static const struct { char ch; uint8_t rows[RV_FH]; } TINY[] = {
    { '0', { 0x0E, 0x11, 0x19, 0x15, 0x13, 0x11, 0x0E } },
    { '1', { 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x10, 0x0C, 0x02, 0x01, 0x1F } },
    { '3', { 0x0E, 0x11, 0x10, 0x0C, 0x10, 0x11, 0x0E } },
    { '4', { 0x08, 0x0C, 0x0A, 0x09, 0x1F, 0x08, 0x08 } },
    { '5', { 0x1F, 0x01, 0x01, 0x0F, 0x10, 0x11, 0x0E } },
    { '6', { 0x0C, 0x02, 0x01, 0x0F, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x10, 0x08, 0x04, 0x04, 0x04, 0x04 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x1E, 0x10, 0x08, 0x06 } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x0F, 0x11, 0x11, 0x0F, 0x11, 0x11, 0x0F } },
    { 'C', { 0x0E, 0x11, 0x01, 0x01, 0x01, 0x11, 0x0E } },
    { 'D', { 0x0F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0F } },
    { 'E', { 0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F } },
    { 'F', { 0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x01 } },
    { 'S', { 0x1E, 0x01, 0x01, 0x0E, 0x10, 0x10, 0x0F } },
    { 'N', { 0x11, 0x13, 0x13, 0x15, 0x19, 0x19, 0x11 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
};

static const uint8_t *tiny_glyph(char ch)
{
    for (unsigned i = 0; i < sizeof TINY / sizeof TINY[0]; i++)
        if (TINY[i].ch == ch) return TINY[i].rows;
    return NULL;
}

/* Draws (or, when `draw` is 0, just measures) one string. Returns the pen x
 * after the string. */
static int put_text(int x, uint32_t argb, const char *s, int draw)
{
    for (; *s; s++) {
        if (*s == ' ') { x += 3; continue; }
        const uint8_t *g = tiny_glyph(*s);
        if (!g) continue;
        if (draw) {
            for (int gy = 0; gy < RV_FH; gy++) {
                if (!g[gy]) continue;
                uint32_t *row = s_canvas + (size_t)(RV_PAD + gy) * RV_W;
                for (int gx = 0; gx < RV_FW; gx++)
                    if ((g[gy] & (1u << gx)) && x + gx < RV_W)
                        row[x + gx] = argb;
            }
        }
        x += RV_FW + 1;
    }
    return x;
}

static void redraw(void)
{
    memset(s_canvas, 0, sizeof s_canvas);
    s_have_content = 0;
    if (!s_drawn[0]) return;

    /* The line is authored as label/value/delta segments split by \t so each
     * can carry its own colour; measure first so the backing strip hugs the
     * text instead of spanning the screen. */
    static const uint32_t seg_color[] = { RV_LABEL, RV_VALUE, RV_DELTA };
    for (int pass = 0; pass < 2; pass++) {
        int x = RV_PAD, seg = 0;
        const char *p = s_drawn;
        char piece[64];
        while (*p) {
            int n = 0;
            while (*p && *p != '\t' && n < (int)sizeof piece - 1) piece[n++] = *p++;
            piece[n] = 0;
            x = put_text(x, seg_color[seg % 3], piece, pass);
            if (*p == '\t') { p++; seg++; }
        }
        if (pass == 0) {
            int w = x + RV_PAD;
            if (w > RV_W) w = RV_W;
            for (int y = 0; y < RV_H; y++)
                for (int i = 0; i < w; i++)
                    s_canvas[(size_t)y * RV_W + i] = RV_BG;
        }
    }
    s_have_content = 1;
}

static void compose(void)
{
    s_line[0] = 0;
    if (!s_on || !psx_mod_game_started()) { s_prev_valid = 0; return; }

    const uint32_t seed = psx_mod_read_word(PSX_RNG_SEED_ADDR);
    const uint32_t idx = lcg_index(seed);
    const uint32_t delta = s_prev_valid ? (idx - s_prev_index) : 0;
    if (s_prev_valid && delta >= RV_DUEL_BURST_MIN && delta <= 100000u) {
        s_duel_seed = s_prev_seed;
        s_duel_valid = 1;
    }
    s_prev_index = idx;
    s_prev_seed = seed;

    /* \t alternates label / value / delta colours; see redraw. */
    int n = snprintf(s_line, sizeof s_line, "S \t%08X\t\t N \t%u\t", seed, idx);
    if (s_prev_valid && delta && delta <= 99999u)
        n += snprintf(s_line + n, sizeof s_line - n, "+%u", delta);
    s_prev_valid = 1;
    if (s_duel_valid)
        snprintf(s_line + n, sizeof s_line - n, "\t D \t%08X", s_duel_seed);
}

static void rng_view_tick(void)
{
    compose();
    if (strcmp(s_line, s_drawn) == 0) {
        if (s_present_hold > 0) s_present_hold--;
        return;
    }
    memcpy(s_drawn, s_line, sizeof s_drawn);
    redraw();
    /* Held a few frames either way, so switching the row off actually clears
     * the panel from a static screen instead of leaving the last frame up. */
    s_present_hold = 4;
}

int psx_rng_view_image(const uint32_t **px, int *w, int *h)
{
    if (!s_have_content) return 0;
    if (px) *px = s_canvas;
    if (w) *w = RV_W;
    if (h) *h = RV_H;
    return 1;
}

void psx_rng_view_origin(int *x, int *y)
{
    if (x) *x = RV_X;
    if (y) *y = RV_Y;
}

int psx_rng_view_needs_present(void)
{
    return s_have_content || s_present_hold > 0;
}

/* ---- VIEW > RNG VIEWER ------------------------------------------------------ */

static const char *const RNG_VIEW_LABELS[] = { "Off", "On" };
static const char *const RNG_VIEW_HINTS[] = {
    "Show the RNG seed and call count",
    "Seed, calls since boot, last duel seed",
};

static void rng_view_changed(int value)
{
    s_on = (value != 0);
    s_prev_valid = 0;
}

PSX_MOD_CONSTRUCTOR(psx_rng_view_install) {
    const int h = psx_video_menu_add_option(
        PSX_VM_MENU_VIEW, "RNG viewer", RNG_VIEW_HINTS[0],
        RNG_VIEW_LABELS, 2, "rng_view", 0, rng_view_changed);
    psx_video_menu_set_row_hints(h, RNG_VIEW_HINTS);
    (void)psx_game_add_frame_hook(rng_view_tick);
}
