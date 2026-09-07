/* psx_cpu_manager.h — VIEW > CPU MANAGER.
 *
 * A window over the CPU duelists themselves: their portrait, their record,
 * the pool their deck is drawn from, and the nine bytes the duel AI reads
 * about them. psx_cpu_data.c owns all four; this is the window.
 *
 * Its own OS window, like the other managers, so it can sit on a second
 * screen while the game runs.
 */
#ifndef PSX_CPU_MANAGER_H
#define PSX_CPU_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

void psx_cpu_manager_register_menu(void);
void psx_cpu_manager_open(void);
void psx_cpu_manager_close(void);
int  psx_cpu_manager_is_open(void);
/* Ask the main thread to open (1) or close (0): the debug server cannot make
 * SDL windows itself. */
void psx_cpu_manager_request_open(int open);

/* Debug-server drive and read-back, the same shape the other managers have.
 * view 0 decks, 1 AI; -1 / NULL leaves a field alone. */
int  psx_cpu_manager_set(int view, int duelist, const char *search);
int  psx_cpu_manager_state_json(char *out, unsigned cap);
int  psx_cpu_manager_shot(const char *path);
int  psx_cpu_manager_click(int x, int y, int button);
int  psx_cpu_manager_press(int x, int y, int button);
int  psx_cpu_manager_release(int x, int y, int button);
int  psx_cpu_manager_move(int x, int y);
int  psx_cpu_manager_key(int keycode);
int  psx_cpu_manager_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CPU_MANAGER_H */
