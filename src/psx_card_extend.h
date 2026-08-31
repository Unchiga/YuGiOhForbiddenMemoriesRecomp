#ifndef PSX_CARD_EXTEND_H
#define PSX_CARD_EXTEND_H

#include <stdint.h>

/* Extends the game's 722-card list with Kuriboh clones.
 *
 * Registers its own frame hook. Call once from the overlay/mod wiring. */
void psx_card_extend_init(void);

/* The card count the game is CURRENTLY running with: 722 when the extension is
 * inactive (tables not resident, patches not asserted), otherwise the extended
 * count. Anything that loops over "every card" must ask this instead of
 * hard-coding 722 -- notably ALL CARDS in psx_ygo_cheats.c, which is how the
 * new ids get into the trunk. */
uint32_t psx_card_extend_count(void);

/* First and last id this mod adds (inclusive). Ids below FIRST are stock.
 *
 * PSX_CARD_EXT_LAST is THE count parameter for the whole extension: the card
 * DB tables, the name-offset table and the chest structure stretch in
 * psx_card_chest.c all derive from it. Raising it is intended; before doing
 * so check the two ceilings:
 *   - 807: string ids 0x8328+ (menu text, duelist names) still resolve
 *          through the RELOCATED name-offset table at their stock indices.
 *          A card id above 807 would collide with them; moving those
 *          consumers has not been mapped yet.
 *   - ~1004: the chest per-card arrays are packed into the instance's spare
 *          region (see psx_card_chest.c layout math, checked at runtime). */
#define PSX_CARD_EXT_FIRST 723u
#define PSX_CARD_EXT_LAST  790u

/* Trunk counts for extended ids (>=PSX_CARD_EXT_FIRST). The stock save block
 * has exactly 722 trunk bytes with live fields directly after them, so counts
 * for extended ids live HERE, mod-side, never in the save struct. Writing the
 * save trunk past id 722 corrupts the save (it stomps the fields at +0x322).
 * id must be in [PSX_CARD_EXT_FIRST, PSX_CARD_EXT_LAST].
 *
 * psx_card_save.c carries this array to and from the memory card, in its own
 * versioned block appended to the save image; psx_card_chest.c carries it to
 * and from the chest's working trunk. */
uint8_t psx_card_ext_trunk_get(uint32_t id);
void    psx_card_ext_trunk_set(uint32_t id, uint8_t count);

#endif /* PSX_CARD_EXTEND_H */
