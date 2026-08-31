#ifndef PSX_CARD_CHEST_H
#define PSX_CARD_CHEST_H

#include <stdint.h>

/* Stretches the chest / deck-builder structure so its one continuous list
 * holds PSX_CARD_EXT_LAST cards instead of 722. Registers its own frame
 * hook; call once from the overlay/mod wiring, after psx_card_extend_init().
 *
 * See psx_card_chest.c for the reverse-engineered structure and the layout
 * this imposes on it. */
void psx_card_chest_init(void);

/* Guest address of card `id`'s cell in the chest screen's (relocated) working
 * trunk copy, or 0 while the stretch is not active. The chest stages trunk
 * edits in this buffer and commits it to the save on exit; the ALL CARDS
 * cheat needs the real cell, not the stock 0x80105D98 one. */
uint32_t psx_card_chest_ui_trunk_cell(uint32_t id);

#endif /* PSX_CARD_CHEST_H */
