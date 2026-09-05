/* psx_drop_viewer.h — VIEW > DROP TABLE MANAGER.
 * (Named "viewer" in filenames and debug commands for history's sake; the
 * user-facing name is Drop Table Manager.)
 *
 * A real second OS window, not an overlay: it can be moved to another monitor,
 * resized, and left open beside the game while you play. That is the whole
 * reason it is a window — an overlay cannot leave the game's screen.
 *
 * Two views over the same data:
 *   BY CARD     every card (id, name, ATK, DEF), sortable on any column and
 *               searchable, with every duelist that drops the selected one,
 *               the rank band needed, and the chance.
 *   BY DUELIST  every duelist, with everything they drop, the band, and the
 *               weight both raw and as a percentage.
 *
 * The table shown is the one the player will actually roll against: with
 * MODS > DROP MISSING CARDS on, the same transform the mod applies to guest
 * memory is applied here, so the two can never disagree.
 */
#ifndef PSX_DROP_VIEWER_H
#define PSX_DROP_VIEWER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the VIEW row. An ACTION, not a toggle: the window is closed from its
 * own title bar or with Escape, which a toggle would fall out of step with.
 * Activating it while open raises that window instead of making a second. */
void psx_drop_viewer_register_menu(void);

/* Open, close, or flip. Opening creates the window; closing destroys it, so a
 * closed viewer costs nothing at all. */
void psx_drop_viewer_open(void);
void psx_drop_viewer_close(void);
void psx_drop_viewer_toggle(void);
int  psx_drop_viewer_is_open(void);

/* Debug-server read-back: what the window is showing right now, geometry
 * included (a "geom" object of [x,y,w,h] rects in canvas pixels). */
int  psx_drop_viewer_state_json(char *out, unsigned cap);

/* Dump the window's canvas as a binary PPM. The window is a host surface the
 * game's screenshot commands cannot reach; this is how a script sees it.
 * Returns 0 if the viewer is closed or the file cannot be written. */
int  psx_drop_viewer_shot(const char *path);

/* Debug-server drive: set what it is showing. -1 / NULL leaves a field alone.
 * view 0 = by card, 1 = by duelist. Returns 0 if the viewer is closed. */
int  psx_drop_viewer_set(int view, int sort, int desc, int card, int duelist,
                         const char *search);

/* Ask the main thread to open (1) or close (0) the window. Safe from any
 * thread — the debug server cannot create SDL windows itself. */
void psx_drop_viewer_request_open(int open);

/* Deliver mouse motion, clicks, a key press, or typed text — as real SDL
 * events carrying this window's id, so the debug server exercises the exact
 * event path a physical mouse and keyboard do. button: SDL numbering (1
 * left, 3 right; <=0 means left). click = press+release; a drag is composed
 * from press, motions, release. Return 0 if the viewer is closed. */
int  psx_drop_viewer_inject_motion(int x, int y);
int  psx_drop_viewer_click(int x, int y, int button);
int  psx_drop_viewer_press(int x, int y, int button);
int  psx_drop_viewer_release(int x, int y, int button);
int  psx_drop_viewer_inject_key(int keycode);
int  psx_drop_viewer_inject_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DROP_VIEWER_H */
