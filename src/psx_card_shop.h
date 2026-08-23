#ifndef PSX_CARD_SHOP_H
#define PSX_CARD_SHOP_H

/* CARD SHOP — a fifth row on the campaign shopkeeper's menu that opens a
 * pack-buying panel. See psx_card_shop.c for the mechanism. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest-overlay contract (psx_ygo_overlays.c registers these). */
int  psx_card_shop_image(const uint32_t **px, int *w, int *h);
void psx_card_shop_origin(int *x, int *y);
int  psx_card_shop_needs_present(void);

/* Per-frame driver: screen detection, menu-count extension, input. */
void psx_card_shop_tick(void);

/* MODS-menu row registration (called from the cheats/menu install path). */
void psx_card_shop_register_menu(void);

/* Debug server read-back. */
int  psx_card_shop_state_json(char *out, unsigned cap);

/* Where one card actually landed: the pinned rarity mask the config resolved
 * for it, and which [pack][tier] pools ended up holding it. "It came out of a
 * pack it should not be in" is otherwise only observable by buying packs until
 * it shows up. Matching is by the game's own decoded name, so this also says
 * whether that name matched the config at all. */
int  psx_card_shop_card_json(char *out, unsigned cap, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_SHOP_H */
