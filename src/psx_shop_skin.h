/* The CARD SHOP panel skin — the password screen's box furniture, baked
 * from the player's disc by tools/disc_assets.py (the .c is build output,
 * never committed; see the manifest there for the disc coordinates).
 *
 * Pieces, as the password screen itself assembles a box:
 *   top / bot    128x8 border strips, corners included, tile horizontally
 *   left / right 8x64 side edges, tile vertically between the strips
 *   field        112x72 mottled navy interior, tiles both ways
 *   star         16x16 starchip, the STARCHIP readout's own sprite
 */
#ifndef PSX_SHOP_SKIN_H
#define PSX_SHOP_SKIN_H

#include "psx_rank_sprites.h"   /* PsxSprite */

#ifdef __cplusplus
extern "C" {
#endif

extern const PsxSprite psx_spr_shop_top;
extern const PsxSprite psx_spr_shop_bot;
extern const PsxSprite psx_spr_shop_left;
extern const PsxSprite psx_spr_shop_right;
extern const PsxSprite psx_spr_shop_field;
extern const PsxSprite psx_spr_shop_star;

#ifdef __cplusplus
}
#endif

#endif /* PSX_SHOP_SKIN_H */
