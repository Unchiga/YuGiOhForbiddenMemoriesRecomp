#ifndef PSX_RANK_SPRITES_H
#define PSX_RANK_SPRITES_H

/* Sprites lifted out of the game's own VRAM by tools/sprite_extract.py.
 *
 * They are BAKED rather than read from VRAM at draw time on purpose: the
 * POW/TEC badge and the rank letters are results-screen assets and are not
 * resident while a duel is running, which is exactly when the rank meter
 * draws. Baking also means the meter never has to track a CLUT or care what
 * the guest happens to have loaded.
 *
 * ARGB8888, row-major, alpha 0 where the PS1 palette entry was 0x0000 (the
 * transparent color), so they composite straight over the frame. The game
 * draws all of them with primitive color 0x808080 — neutral modulation — so
 * the stored pixels are exactly what appears on screen.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxSprite {
    const uint32_t *px;
    int w, h;
} PsxSprite;

/* Card-stat digit font: texpage (896,256) 8bpp, CLUT (256,241), glyph N at
 * u = N*8, v = 88. This is the font the game prints card ATK/DEF with. */
extern const PsxSprite psx_spr_digit[10];

/* DUEL SKILL badge and rank letter, from the RESULTS OF DUEL screen. */
extern const PsxSprite psx_spr_pow;
extern const PsxSprite psx_spr_tec;
extern const PsxSprite psx_spr_rank[5];   /* index 0..4 = D, C, B, A, S */

/* The grey Millennium-Eye stone plate the results screen draws BEHIND the rank
 * letter. The badge and letter carry anti-aliasing authored against it, so on a
 * bare background their soft edges read as a grey haze; putting the plate back
 * is what makes that art look right rather than thresholding it away. */
extern const PsxSprite psx_spr_plate;

#ifdef __cplusplus
}
#endif

#endif /* PSX_RANK_SPRITES_H */
