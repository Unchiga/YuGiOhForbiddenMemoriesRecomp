/* psx_drop_missing.h — MODS > DROP MISSING CARDS.
 *
 * Yu-Gi-Oh! Forbidden Memories only. 82 of the game's 722 cards are dropped by
 * nobody — Exodia's two legs among them, which is why the set cannot be
 * completed by duelling in stock FM. This gives every one of them a home.
 *
 * Nothing is patched on disc. The duel loads the current opponent's drop
 * weights into RAM; this rewrites that copy, so the game rolls its own tables
 * and the change lasts exactly as long as the duel does.
 *
 * The placement is editable: drop_missing_cards.ini in the player-data folder
 * is written on first run from the built-in defaults, and read back after.
 */
#ifndef PSX_DROP_MISSING_H
#define PSX_DROP_MISSING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the MODS row. Call before the settings file is read so a stored value
 * has a row to land in. */
void psx_drop_missing_register_menu(void);

/* Per-frame guard. Cheap: it samples a few words and only does real work when
 * the resident drop table changes, which is once per duel. */
void psx_drop_missing_tick(void);

/* Debug-server read-back: what the mod thinks is going on. */
int psx_drop_missing_state_json(char *out, unsigned cap);

/* Is the row on? The Drop Table Viewer shows a different table depending, so
 * it has to be able to ask rather than assume. */
int psx_drop_missing_enabled(void);

/* Load the ini placements if that has not happened yet. The tick does this
 * itself; the viewer needs it because it can be opened before the first duel. */
void psx_drop_missing_ensure_loaded(void);
/* Re-read drop_missing_cards.ini after it was replaced (a MOD package import). */
void psx_drop_missing_reload(void);

/* Apply this mod's placements to one duelist's tier, in place, over a plain
 * 722-entry weight array. Exposed so the viewer can show the table a player
 * will actually roll against without reimplementing the rescale — the mod
 * itself calls this with weights read from guest RAM.
 *
 * Returns 1 when w was transformed. Negative means it was left alone: -1
 * nothing placed in this band, -2 the tier was empty, -3 the result did not
 * total 2048 and so was rejected. Card ids are 1-based; w is indexed by id-1.
 */
int psx_drop_missing_transform(int duelist, int tier, uint16_t *w);

/* The renormalizer both that transform and the drop-table edit layer go
 * through: pin n cards to exact weights inside one 722-entry tier and rescale
 * everyone else so the tier still totals 2048. A pinned weight of 0 removes
 * the card from the band. Returns 1 on success; on any negative code w is
 * untouched: -1 no pins, -2 nothing left to scale, -3 the result missed 2048,
 * -4 the pins claim more than 2048-64 without reaching 2048. Pins that total
 * exactly 2048 are taken as the whole band: everything else becomes 0.
 * There is deliberately exactly ONE implementation of this arithmetic. */
int psx_drop_pins_rescale(uint16_t *w, const uint16_t *cards,
                          const uint16_t *weights, int n);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DROP_MISSING_H */
