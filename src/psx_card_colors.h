/* psx_card_colors.h -- per-card frame colours.
 *
 * Stock derives a card's frame from its type (monster yellow, magic green,
 * trap pink, ritual blue). This gives every card its own slot out of seven
 * palettes: the disc's six (the unused purple and orange included) and a
 * generated red. A monster with any monster effect defaults to orange, the
 * way effect monsters look in the real card game, unless card.ini says
 * otherwise. See psx_card_colors.c. */
#ifndef PSX_CARD_COLORS_H
#define PSX_CARD_COLORS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The slot a card draws with right now (edit, effect default, or stock). */
int psx_card_colors_slot(int id);
/* A colour swatch for the manager: the palette's three sample RGB values. */
int psx_card_colors_swatch(int slot, unsigned char rgb[9]);
/* Debug-server state line. */
int psx_card_colors_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_COLORS_H */
