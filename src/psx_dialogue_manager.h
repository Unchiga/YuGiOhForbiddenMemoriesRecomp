/* psx_dialogue_manager.h -- MODS > Dialogue manager: a window listing every
 * text the game types (story dialogue, duelist lines, menus, duel messages)
 * with its original and current wording, a search box, and Export... /
 * Import... for the translation file psx_dialogue.h describes. Drawn like
 * the Drop Table Manager (own SDL window, the F10 menu's toolkit). */
#ifndef PSX_DIALOGUE_MANAGER_H
#define PSX_DIALOGUE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

void psx_dialogue_manager_open(void);
void psx_dialogue_manager_close(void);
int  psx_dialogue_manager_is_open(void);
void psx_dialogue_manager_register_menu(void);

/* Debug-server side: open/close on the emulation thread, synthetic input,
 * a canvas dump (binary PPM) and the state line. */
void psx_dialogue_manager_request_open(int open);
int  psx_dialogue_manager_click(int x, int y, int button);
int  psx_dialogue_manager_inject_key(int keycode);
int  psx_dialogue_manager_inject_text(const char *text);
int  psx_dialogue_manager_shot(const char *path);
int  psx_dialogue_manager_state_json(char *out, unsigned cap);
/* Select a row by its bank offset, or filter (NULL = leave alone). */
int  psx_dialogue_manager_set(int key, const char *search);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DIALOGUE_MANAGER_H */
