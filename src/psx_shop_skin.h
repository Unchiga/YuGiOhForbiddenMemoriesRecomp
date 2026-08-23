/* The CARD SHOP panel skin — the password screen's box furniture, baked
 * from the player's disc by tools/disc_assets.py (the .c is build output,
 * never committed; see the manifest there for the disc coordinates).
 *
 * Pieces, as the password screen itself assembles a box:
 *   top / bot    128x8 border strips, corners included, tile horizontally
 *   left / right 8x64 side edges, tile vertically between the strips
 *   field        112x72 mottled navy interior, stretched to fit
 *   star         16x16 starchip, the STARCHIP readout's own sprite
 *   xbtn / obtn  16x16 cross and circle buttons from the OK/END row
 *   arrow        16x16 right-pointing digit arrow (mirror it for left,
 *                the way the screen itself does)
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
extern const PsxSprite psx_spr_shop_xbtn;
extern const PsxSprite psx_spr_shop_tbtn;
extern const PsxSprite psx_spr_shop_obtn;
extern const PsxSprite psx_spr_shop_arrow;
extern const PsxSprite psx_spr_shop_arrow2;   /* second animation frame */

/* The card viewer's gold template atlas as RAW VRAM words (the same disc
 * blob the game uploads to (832,0) on the chest/deck screens). The shop
 * screen never loads it, so the mod uploads this before spawning the
 * viewer there. 64 words x 96 rows. */
extern const uint16_t psx_shop_tmpl_raw[64 * 96];

/* The viewer's composed card-body canvas, minus the face rect it streams
 * per card: the right strip (canvas u 128..256) and the bottom strip
 * (u 0..128, v 128..192), as raw VRAM words at their (768,256)-page
 * coordinates. Re-uploaded around the pump because the shop screen has
 * no RAM copy of this asset for the viewer's own compose to read. */
extern const uint16_t psx_shop_cardbody_r[64 * 192];
extern const uint16_t psx_shop_cardbody_b[64 * 64];

#ifdef __cplusplus
}
#endif

#endif /* PSX_SHOP_SKIN_H */
