#ifndef PSX_CARD_SAVE_H
#define PSX_CARD_SAVE_H

/* Persistence for the extended card ids, and the toggle that gates the whole
 * extension.
 *
 * The extension changes what a save MEANS (a deck slot may hold an id the
 * stock game cannot resolve), so it gets its own memory-card FILE and its own
 * versioned block inside it. Both are decided at boot: the row below is a
 * stored preference, and flipping it asks for a restart rather than swapping
 * save identities under a game that is already running. */

/* Registers the menu row, the boot latch and the frame hook. Call once from
 * the overlay wiring, BEFORE psx_card_extend_init(). */
void psx_card_save_init(void);

/* The latched answer to "is the card-list extension running this session".
 * Everything the extension patches must ask this: with it 0 the game is
 * stock, down to the memory-card file name. */
int psx_card_save_ext_enabled(void);

#endif /* PSX_CARD_SAVE_H */
