/* psx_duelist_portraits.h — FREE DUEL portraits, read from the player's disc.
 *
 * The FREE DUEL screen streams one 87-sector load from WA_MRG.MRG each time
 * it opens; 48 of those sectors are the forty 48x48 grid portraits, stored
 * plain — 2304 bytes of 8bpp indices then a 64-entry 15-bit CLUT, 2432 bytes
 * a tile — at WA_MRG offset 0xF55000, disc LBA 17952 (decomp F121/F122 and
 * the TEA-Online FREE DUEL image tool agree, and the tiles decode). Tile 0
 * is the Build Deck cell; tiles 1..39 are the duelists in exactly the drop
 * tables' order, so tile d+1 is PSX_DROP_DB[d].
 *
 * The art is Konami's and is never shipped: it is decoded out of the disc
 * image the player already supplied, at runtime, the first time something
 * asks for it — the same footing as the card art the Card Manager reads.
 * This replaces the need to capture portraits off the drawn screen
 * (psx_duelist_icon_cache.c), which stays as the fallback for a disc whose
 * sectors do not pass the sanity check below. */
#ifndef PSX_DUELIST_PORTRAITS_H
#define PSX_DUELIST_PORTRAITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_PORTRAIT_W 48
#define PSX_PORTRAIT_N 39          /* duelists, drop-table order */

/* The portrait of PSX_DROP_DB[duelist] as 48x48 opaque 0xFFRRGGBB, or NULL
 * when the disc has not been read yet (or failed the check). Reads and
 * decodes all forty tiles on the first call that finds the disc readable;
 * later calls are a table lookup. Call from the emulation thread, which owns
 * the disc reader. */
const uint32_t *psx_duelist_portraits_get(int duelist);

/* 1 once the tiles are decoded. */
int psx_duelist_portraits_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DUELIST_PORTRAITS_H */
