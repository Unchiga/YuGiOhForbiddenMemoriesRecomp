/* psx_fusion_manager.h — VIEW > FUSION MANAGER.
 *
 * A second OS window, like the Drop Table Manager and the Dialogue Manager,
 * over the game's own fusion tables (psx_fusion_db.h). Two views on the same
 * index:
 *
 *   BY CARD   every card, sortable and searchable, with two panels for the
 *             selected one: FUSES WITH (every partner and what the pair
 *             makes, equips included) and MADE FROM (every pair that makes
 *             it). Clicking a card name in either panel walks to that card,
 *             and Backspace walks back.
 *   RECIPES   every recipe in the game as one flat sortable list.
 *
 * And it EDITS them. The edit line under FUSES WITH takes a partner id and
 * what the pair should make (0 removes the fusion); double-clicking a row
 * fills it in. psx_fusion_table.c owns the data and the change: it reads the
 * stock table off the disc — so this window needs no duel and works from
 * boot — packs the edits back into the game's own format and installs them
 * as a disc-sector override, which is what makes the AI use them too.
 * MODS > FUSION EDITS turns that override on and off; Import…/Export… move
 * a recipe list in and out as text.
 */
#ifndef PSX_FUSION_MANAGER_H
#define PSX_FUSION_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the VIEW row. An ACTION, not a toggle, for the same reason as the
 * Drop Table Manager's: the window is closed from its own title bar or with
 * Escape, which a toggle would fall out of step with. */
void psx_fusion_manager_register_menu(void);

void psx_fusion_manager_open(void);
void psx_fusion_manager_close(void);
int  psx_fusion_manager_is_open(void);

/* Write every recipe and equip group as a tab-separated text file. Returns 1
 * with a summary in `err`, 0 with the reason. Works with no window open, and
 * from any index the manager holds (live or captured). */
int  psx_fusion_manager_export(const char *path, char *err, unsigned errcap);

/* Read a recipe list back in. Same format Export writes, and a plain
 * `card1,card2,result` CSV also works. Returns 1 with a summary in `err`. */
int  psx_fusion_manager_import(const char *path, char *err, unsigned errcap);

/* Change one pair from a script, exactly as the edit line does. `a` below 1
 * means "the card the window has selected". `result` 0 removes the fusion.
 * Returns 0 with the reason in `err` and changes nothing. */
int  psx_fusion_manager_edit(int a, int b, int result, char *err, unsigned errcap);
/* An equip pairing: `on` makes the equip fit the monster, 0 takes it away.
 * Goes through the equip card's card.ini list (psx_card_packs), not the
 * fusion table, so it travels with the cards. */
int  psx_fusion_manager_equip_set(int equip, int mon, int on, char *err, unsigned errcap);

/* Drop every edit and put the game's own table back (the edits are copied to
 * fusion_edits_backup.txt first). This is what the confirm dialog does; it
 * asks nothing itself, so a script does not have to click. */
void psx_fusion_manager_undo_all(void);

/* Drive that dialog instead, so the modal itself is testable: 1 opens it,
 * 2 confirms, 0 cancels. */
void psx_fusion_manager_confirm_restore(int ask);

/* Empty the table completely, or just one card's share of it. Both ask
 * nothing -- the window puts a confirm in front of them, a script does not
 * need one. Return 0 with the reason in `err`. */
int  psx_fusion_manager_clear_all(char *err, unsigned errcap);
int  psx_fusion_manager_clear_card(int id, char *err, unsigned errcap);

/* ---- debug-server surface ---------------------------------------------- */

/* Open/close on the main thread next frame — the debug server cannot create
 * SDL windows itself. */
void psx_fusion_manager_request_open(int open);

/* Point the window at something. -1 / NULL leaves a field alone.
 * view 0 = by card, 1 = recipes. Returns 0 when the window is closed. */
int  psx_fusion_manager_set(int view, int card, int sort, int desc,
                            const char *search);

/* What the window is showing, geometry included (a "geom" object of
 * [x,y,w,h] rects in canvas pixels). */
int  psx_fusion_manager_state_json(char *out, unsigned cap);

/* The selected card's two panels as JSON, so a script can check the content
 * without reading pixels. */
int  psx_fusion_manager_card_json(char *out, unsigned cap, int card);

/* Synthetic input, as real SDL events carrying this window's id, so the
 * debug server exercises the exact path a physical mouse and keyboard do.
 * button: SDL numbering (<=0 means left). Return 0 when closed. */
int  psx_fusion_manager_click(int x, int y, int button);
int  psx_fusion_manager_double_click(int x, int y);
/* The halves of a drag: press, move waypoints, release. */
int  psx_fusion_manager_press(int x, int y, int button);
int  psx_fusion_manager_release(int x, int y, int button);
int  psx_fusion_manager_move(int x, int y);
int  psx_fusion_manager_inject_key(int keycode);
int  psx_fusion_manager_inject_text(const char *text);

/* The window's canvas as a binary PPM — it is a host surface the game's
 * screenshot commands cannot reach. */
int  psx_fusion_manager_shot(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FUSION_MANAGER_H */
