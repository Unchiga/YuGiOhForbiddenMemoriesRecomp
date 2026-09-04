/* psx_lp_popup.c -- see psx_lp_popup.h.
 *
 * Drawn from psx_fusion_font, the 8x8 card-stat charset baked from the
 * player's disc, at twice its size so it reads like the game's own LP
 * popup, which sits at guest (160, 120). Holds for a second, then fades. */

#include "psx_lp_popup.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_guest_overlay.h"
#include "psx_fusion_font.h"

#define POP_W 160
#define POP_H 24
#define SCALE 2
#define HOLD_FRAMES 55
#define FADE_FRAMES 25

static uint32_t s_px[POP_W * POP_H];
static int      s_w, s_h;
static int      s_frames;           /* frames left, 0 = hidden */
static int      s_present_hold;

static int glyph_extent(int cell, int *lo, int *hi)
{
    const PsxFusionFont *f = &psx_fusion_font;
    const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
    int l = f->w, h = -1;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++)
            if (g[y * f->w + x]) { if (x < l) l = x; if (x > h) h = x; }
    *lo = l; *hi = h;
    return h >= 0;
}

static int put_glyph(int cell, int x0, uint32_t tint)
{
    const PsxFusionFont *f = &psx_fusion_font;
    int lo, hi;
    if (cell < 0 || cell >= PSX_FUSION_FONT_CELLS || !glyph_extent(cell, &lo, &hi)) return 0;
    const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
    const unsigned tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    for (int y = 0; y < f->h; y++)
        for (int x = lo; x <= hi; x++) {
            const uint8_t v = g[y * f->w + x];
            if (!v) continue;
            const unsigned l = (unsigned)v * 17u;         /* 1 = the dark outline, 15 = the core */
            const uint32_t c = 0xFF000000u | ((l * tr / 255u) << 16) | ((l * tg / 255u) << 8) | (l * tb / 255u);
            for (int dy = 0; dy < SCALE; dy++)
                for (int dx = 0; dx < SCALE; dx++) {
                    const int px = x0 + (x - lo) * SCALE + dx, py = (POP_H - f->h * SCALE) / 2 + y * SCALE + dy;
                    if (px >= 0 && px < POP_W && py >= 0 && py < POP_H) s_px[py * POP_W + px] = c;
                }
        }
    return (hi - lo + 1) * SCALE + SCALE;               /* one scaled column of air between glyphs */
}

void psx_lp_popup_show(int amount, int heal)
{
    char text[16];
    if (amount < 0) amount = -amount;
    if (amount > 99999) amount = 99999;
    snprintf(text, sizeof text, "%c%d", heal ? '+' : '-', amount);
    memset(s_px, 0, sizeof s_px);
    /* measure, then draw centred */
    int w = 0;
    for (const char *p = text; *p; p++) { int lo, hi; const int c = psx_fusion_font_cell((unsigned char)*p); if (c >= 0 && glyph_extent(c, &lo, &hi)) w += (hi - lo + 1) * SCALE + SCALE; }
    int x = (POP_W - w) / 2;
    const uint32_t tint = heal ? 0xFF9CF09Cu : 0xFFF4E070u;   /* the game's green-ish plus, yellow minus */
    for (const char *p = text; *p; p++) x += put_glyph(psx_fusion_font_cell((unsigned char)*p), x, tint);
    s_w = POP_W; s_h = POP_H;
    s_frames = HOLD_FRAMES + FADE_FRAMES;
    s_present_hold = 4;
}

static int image(const uint32_t **px, int *w, int *h)
{
    if (s_frames <= 0) return 0;
    *px = s_px; *w = s_w; *h = s_h;
    return 1;
}
static void origin(int *x, int *y) { *x = 160 - POP_W / 2; *y = 120 - POP_H / 2; }
static int needs_present(void) { return s_frames > 0 || s_present_hold > 0; }

static void tick(void)
{
    if (s_present_hold > 0) s_present_hold--;
    if (s_frames <= 0) return;
    s_frames--;
    if (s_frames < FADE_FRAMES) {
        /* fade: scale every pixel's alpha */
        const unsigned a = (unsigned)(255 * s_frames / FADE_FRAMES);
        for (int i = 0; i < POP_W * POP_H; i++) if (s_px[i]) s_px[i] = (s_px[i] & 0x00FFFFFFu) | (a << 24);
    }
    if (s_frames == 0) s_present_hold = 2;
}

PSX_MOD_CONSTRUCTOR(psx_lp_popup_install)
{
    PsxGuestOverlay ov = { image, origin, NULL, needs_present, -1, NULL };
    (void)psx_guest_overlay_register(&ov);
    (void)psx_game_add_frame_hook(tick);
}
