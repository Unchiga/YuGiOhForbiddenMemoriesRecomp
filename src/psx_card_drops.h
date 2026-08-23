#ifndef PSX_CARD_DROPS_H
#define PSX_CARD_DROPS_H

/* MODS > CARD DROPS — Yu-Gi-Oh! Forbidden Memories.
 *
 * Awards N cards for a won duel instead of 1 by re-running the GAME'S OWN
 * drop roll, tracks what the duel granted, republishes the chest's "New!"
 * flags so they mean "this duel", and renders the awarded cards as an extra
 * page on the RESULTS OF DUEL screen using the game's own text engine.
 *
 * All guest-side knowledge — the hooked function addresses, the RAM map, the
 * text-stream encoding — lives in the .c file. This header is the whole
 * interface: the host registers the hooks, ticks the module once a frame,
 * pushes the menu setting in, and reads state out for the debug server.
 *
 * The module talks to the guest only through mod_plugins.h and cpu_state.h,
 * and to the screen only through psx_cd_overlay.h. It owns no SDL or GL.
 */

#include <stdint.h>

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register every guest hook the mod needs. Call once at startup, after the
 * game image is identified. Cheap when the feature is off: each callback
 * returns immediately while its gate is closed. */
void psx_card_drops_register_hooks(void);

/* Stock award count, and the ceiling the menu row accepts. Owned here rather
 * than by the shared menu: how many cards a duel awards is this game's idea. */
#define PSX_VM_CARD_DROPS_DEFAULT 1
#define PSX_VM_CARD_DROPS_MAX 99

/* Adds MODS > CARD DROPS to the overlay menu. Called from this file's mod
 * constructor, so the row exists wherever this translation unit is linked. */
void psx_card_drops_register_menu(void);

/* Host-frame update: drives the results page's "New!" sprite overlay. */
void psx_card_drops_tick(void);

/* MODS > CARD DROPS menu row, 1..PSX_VM_CARD_DROPS_MAX (1 = stock). A
 * preference, not a live write — it only changes what the NEXT won duel
 * awards. Returns 0 and changes nothing if the value is out of range. */
int  psx_card_drops_set(int drops);

/* ---- debug-server surface ---------------------------------------------- */

/* Why the mod did or did not add cards (`card_drops_state`). Any pointer
 * may be NULL. */
/* page_duel / awarded_total describe the DROPS RESULTS PAGE's record: it is
 * scoped to one results screen, so a duel that awarded nothing (a loss - the
 * roll is win-only) cannot show the previous duel's cards. */
void psx_card_drops_debug(int *setting, int *calls, uint32_t *last_ra,
                          int *last_tier, int *granted, int *bails,
                          int *new_count, int *chest_builds, int *overlays,
                          int *page_duel, int *awarded_total);

/* This duel's awards as JSON, ordered the way the results page lists them:
 * cards the player owned none of first, then by card id. Returns the DISTINCT
 * card count and writes the total copies through out_total. */
int  psx_card_drops_list_json(char *out, unsigned cap, int *out_total);

/* Simulate one duel drop end to end through the REAL hook, so the setting can
 * be swept without winning a duel per value. */
int  psx_card_drops_simulate(CPUState *cpu, int tier, int drops,
                             uint32_t *out_card, int *out_granted,
                             int *out_bail);

/* One nested roll on demand, reporting what the guest call produced. */
int  psx_card_drops_test_roll(CPUState *cpu, int tier, int do_award,
                              uint32_t *out_card, uint32_t *out_pc,
                              int *out_bail);

/* Stage a raw text stream rendered verbatim on the page (escape experiments);
 * len 0 hands the page back to the composer. */
int  psx_card_drops_p3_stage(const uint8_t *bytes, int len, int subs);

/* Results-page state. Any pointer may be NULL. */
void psx_card_drops_p3_state(int *active, int *sub, int *subs, int *pending,
                             int *applies, int *overrides, int *prev_page,
                             int *test_len);

/* Live layout tuning; ABSOLUTE values, PSX_CD_OVERLAY_KEEP leaves a field
 * alone. Sprite fields apply instantly; text fields land on the next page
 * turn, because the engine skips re-decoding while the string id is
 * unchanged. */
void psx_card_drops_layout(int text_y, int split, int name_x, int num_x,
                           int spr_x, int spr_y, int spr_dy);
void psx_card_drops_layout_get(int *text_y, int *split, int *name_x,
                               int *num_x, int *spr_x, int *spr_y,
                               int *spr_dy);

#ifndef PSX_NO_DEBUG_TOOLS
/* Card-name stream probe: arm to capture, then read what the text engine
 * decoded. Diagnostic scaffolding, debug-tools builds only. */
void psx_name_probe_arm(int on);
int  psx_name_probe_json(char *out, unsigned cap);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_DROPS_H */
