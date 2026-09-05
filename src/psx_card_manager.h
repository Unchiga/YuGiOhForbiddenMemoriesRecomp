/* psx_card_manager.h — VIEW > CARD MANAGER.
 *
 * A second OS window, like the Drop Table Manager, for editing card packs
 * (psx_card_packs.h) while the game runs: pick a card, change its name,
 * ATK/DEF, guardian stars, type, level, attribute, price and password, drop
 * in art, and see the result in the next screen that draws the card. Saves
 * write <player-data>/cards/<id>/card.ini; the packs mod applies them live.
 */
#ifndef PSX_CARD_MANAGER_H
#define PSX_CARD_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

void psx_card_manager_open(void);
void psx_card_manager_close(void);
int  psx_card_manager_is_open(void);
/* Select a card in the open window (no-op when closed). */
void psx_card_manager_select(int id);

/* Debug-server plumbing (psx_ygo_debug.c): open/close on the main thread
 * next frame, a state line, synthetic input through SDL's own queue, and a
 * dump of the canvas (binary PPM) since the window has no framebuffer the
 * screenshot commands can reach. */
void psx_card_manager_request_open(int open);
int  psx_card_manager_state_json(char *out, unsigned cap);
int  psx_card_manager_click(int x, int y, int button);
/* Press / move / release separately, for a scripted drag. */
int  psx_card_manager_button(int x, int y, int button, int down);
int  psx_card_manager_move(int x, int y);
int  psx_card_manager_type(const char *text);
int  psx_card_manager_key(int sdl_key);
void psx_card_manager_search(const char *text);
/* Show the import preview for a file, as the Import button would. */
void psx_card_manager_import_preview(const char *path);
/* Open the manager asking whether to activate the Card Effects mod; the
 * answer goes to psx_card_packs_set_dev. Used by the MODS menu row. */
void psx_card_manager_ask_activate(void);
int  psx_card_manager_shot(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_MANAGER_H */
