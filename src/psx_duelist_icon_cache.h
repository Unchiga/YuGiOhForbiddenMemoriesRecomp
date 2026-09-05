/* psx_duelist_icon_cache.h — FREE DUEL portraits, captured by the running
 * game from its own screen.
 *
 * The Drop Table Manager shows each duelist's FREE DUEL portrait. That art is
 * Konami's: like the sprites and the font it is never shipped. Since
 * 2026-09-04 the Manager reads the portraits straight off the player's disc
 * (psx_duelist_portraits.c: forty plain 48x48 tiles at WA_MRG 0xF55000, in
 * drop-table order — the "compressed atlas" this file used to describe was
 * a wrong guess), so this capture is now the FALLBACK for a disc whose
 * sectors fail that module's check.
 *
 * What the capture relies on is the drawn screen. On the FREE DUEL grid the game draws
 * the portraits at a fixed lattice, the highlighted duelist's index is
 * readable at 0x8009B32E (+41), and the cursor's border animates — so two
 * samples of the drawn frame differ exactly at the cursor cell. Anchoring on
 * that locates the visible page, drop-order gives every visible cell its
 * duelist, and a luminance-contrast gate tells a portrait from the stone wall
 * behind an empty cell. The same algorithm as tools/capture_duelist_icons.py,
 * which built the reference set — moved into the game so it runs for every
 * player, automatically, whenever they are on that screen.
 *
 * Captures accumulate in <player-data>/duelist_icons.bin and load back on
 * every launch, so one visit to FREE DUEL populates the Manager for good.
 * The compile-time bake (assets/duelist_icons/, dev machines only) still
 * wins when present; this cache fills the plates everywhere else.
 */
#ifndef PSX_DUELIST_ICON_CACHE_H
#define PSX_DUELIST_ICON_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_ICON_CACHE_N 39
#define PSX_ICON_CACHE_W 38

/* The cached portrait for a duelist as 38x38 0xAARRGGBB, or NULL when none
 * has been captured yet. Loads the disk cache on first use. */
const uint32_t *psx_duelist_icon_cache_get(int duelist);

/* Bumped whenever a new portrait lands (capture or disk load), so a viewer
 * holding plates knows to redraw. */
unsigned psx_duelist_icon_cache_generation(void);

/* How many portraits the cache still lacks. The REVEAL ALL PORTRAITS cheat
 * watches this to revert itself the moment the set is complete. */
int psx_duelist_icon_cache_missing(void);

/* Debug-server read-back. */
int psx_duelist_icon_cache_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DUELIST_ICON_CACHE_H */
