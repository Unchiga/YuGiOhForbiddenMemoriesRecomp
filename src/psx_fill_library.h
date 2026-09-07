/* psx_fill_library.h — MODS > FILL LIBRARY.
 *
 * The LIBRARY lists all 722 cards in three states: never met (a blank entry),
 * SEEN but not owned (the card, drawn semi-transparent — a fusion result or a
 * ritual you watched resolve), and owned. This row makes the first state read
 * as the second while it is on: every card shows its picture, name, stats and
 * password, and the completion figure reads 722/722.
 *
 * It grants NOTHING. The trunk — what you own, what you can put in a deck —
 * is not touched, so a card filled in here still draws as seen-not-owned and
 * the chest and deck builder do not know about it.
 *
 * Turning the row off puts your own library straight back. So does leaving
 * the screen: the marks exist only while the LIBRARY is on screen, which is
 * also why they can never reach your memory card (see the .c file).
 */
#ifndef PSX_FILL_LIBRARY_H
#define PSX_FILL_LIBRARY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the MODS row and the frame hook. Registers itself; nothing to call. */
void psx_fill_library_register_menu(void);

/* Debug-server surface: `fill_library` reads this, and {"on":1/0} drives the
 * row from a script. Writes what the row is, whether marks are live right
 * now, and how many cards each of the three states holds. */
int  psx_fill_library_state_json(char *out, unsigned cap);
void psx_fill_library_set(int on);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FILL_LIBRARY_H */
