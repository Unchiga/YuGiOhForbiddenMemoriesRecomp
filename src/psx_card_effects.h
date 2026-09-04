/* psx_card_effects.h -- the effects half of an edited card.
 *
 * Reads the effect fields of every edited card (psx_card_packs) and makes the
 * game honour them: Magic effects and their magnitudes, an Equip's bonus and
 * the monsters it fits, a field card's boosts, a trap's ATK ceiling and a
 * ritual's recipe. Nothing here owns a file; card.ini is the source.
 * See psx_card_effects.c for what each one is made of. */
#ifndef PSX_CARD_EFFECTS_H
#define PSX_CARD_EFFECTS_H

#include <stdint.h>
#include "psx_card_packs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill the stock effect facts of a card (effect, amount, equip_bonus,
 * trap_atk_max, boost[], ritual) into a PsxCardStock. Values are -1 (or
 * PSX_CARD_PACK_BOOST_UNSET) when the game's tables are not resident yet or
 * the card has no such fact. */
void psx_card_effects_stock(int id, PsxCardStock *out);

/* What the layer can and cannot do to this card, one line for the manager. */
const char *psx_card_effects_note(int id, int effective_type);

/* Debug-server state line. */
int psx_card_effects_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_EFFECTS_H */
