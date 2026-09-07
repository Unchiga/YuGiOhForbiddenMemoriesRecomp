#ifndef PSX_RANK_METER_H
#define PSX_RANK_METER_H

/* Live duel-rank meter, drawn from the game's own sprites.
 *
 * Pure state + rasterisation, like psx_video_menu: it reads no guest memory and
 * touches no SDL/GL. The host computes the rank (main.cpp owns the addresses
 * and the formula), pushes it here, and the renderer composites the image.
 *
 * The canvas is authored in GUEST pixels — the same 320x240 space the game
 * draws in — so the meter can sit beside the FIELD box at any window size. The
 * renderer maps it into the letterboxed game rect.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rank letters, low to high, matching psx_spr_rank[]. */
enum { PSX_RANK_D = 0, PSX_RANK_C = 1, PSX_RANK_B = 2,
       PSX_RANK_A = 3, PSX_RANK_S = 4 };

/* What to show. score 0..99, pow != 0 for POWER (low scores are TECHNIQUE),
 * letter is a PSX_RANK_*. visible = 0 hides the meter entirely; show_score = 0
 * draws the badge and letter alone, and the canvas shrinks to match so the
 * occlusion rect stays honest. Cheap to call every frame: the canvas is only
 * re-rasterised when something actually changed. */
void psx_rank_meter_set(int visible, int score, int pow, int letter,
                        int show_score);

/* 1 + fills the ARGB canvas when the meter should be drawn. Dimensions are in
 * GUEST pixels. */
int  psx_rank_meter_image(const uint32_t **pixels, int *w, int *h);

/* Top-left corner in GUEST pixels. */
void psx_rank_meter_origin(int *x, int *y);

/* Move the meter. The host follows the game's own FIELD box, which slides off
 * the left edge whenever a card view / attack animation / 3D fight takes the
 * screen — so the meter tweens out with the HUD instead of hanging in mid-air
 * and popping. y is the LETTER's target row; the canvas is offset up from it
 * so the badge can overhang. */
void psx_rank_meter_set_origin(int letter_x, int letter_y);

/* Where the letter sits inside the canvas, so a caller anchoring to something
 * in the game can line the letter up with it rather than the canvas corner. */
void psx_rank_meter_letter_offset(int *x, int *y);

/* Live layout tuning, driven by the `rank_meter_tune` debug command. Placing
 * pixel art beside the game's own art is a by-eye job and a rebuild costs the
 * player their duel, so these can be nudged while the game runs.
 *
 * Pass PSX_RANK_TUNE_KEEP to leave a field alone. These are ABSOLUTE values,
 * not relative nudges — calling twice with the same argument does nothing the
 * second time. The compiled-in values are what ships. */
#define PSX_RANK_TUNE_KEEP (-100000)
void psx_rank_meter_tune(int letter_x, int letter_y, int gap,
                         int anchor_dx, int anchor_dy);
void psx_rank_meter_tune_get(int *letter_x, int *letter_y, int *gap,
                             int *anchor_dx, int *anchor_dy);

/* Fade, 0..255, applied to the whole widget's alpha. The game fades its HUD in
 * at duel start by modulating the primitive color it draws the box with, so
 * the host passes that brightness straight through and the meter fades in step
 * with the HUD rather than popping in at full strength. */
void psx_rank_meter_set_fade(int fade_0_255);

/* Vertical nudge in HALF guest pixels, resolved by the renderer against the
 * live magnification. -1 is half a guest pixel up. */
int  psx_rank_meter_subpixel_y(void);
void psx_rank_meter_tune_sub(int dy2);

/* Current canvas extent in GUEST pixels, valid once something has been shown.
 * The host needs it to tell the GPU which rect to watch for occlusion. */
void psx_rank_meter_extent(int *x, int *y, int *w, int *h);

/* 1 while the meter has something on screen, so a present can be forced. */
int  psx_rank_meter_needs_present(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RANK_METER_H */
