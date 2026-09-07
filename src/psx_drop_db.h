/* psx_drop_db.h — every duelist's full drop table, as the Drop Table Viewer
 * needs it.
 *
 * The definitions live in psx_drop_db.c, which is GENERATED from the player's
 * own disc at build time by tools/gen_drop_db.py and is neither committed nor
 * shipped. Only this shape is source.
 *
 * Why a bake and not a live read: the game keeps exactly ONE duelist's drop
 * table resident (0x801781D8, the current opponent), so a viewer that lists
 * all forty cannot get them from RAM. Card names and ATK/DEF are the opposite
 * case — they sit in the EXE and are always resident — so those are read live
 * and are not baked. See psx_drop_viewer.c.
 */
#ifndef PSX_DROP_DB_H
#define PSX_DROP_DB_H

#include <stdint.h>

#define PSX_DROP_DB_DUELISTS  39
#define PSX_DROP_DB_TIERS      3
#define PSX_DROP_DB_CARDS    722
/* Every tier's weights sum to this, which is what makes one readable as a
 * percentage. gen_drop_db.py refuses to emit anything if it ever does not. */
#define PSX_DROP_DB_TOTAL   2048

/* Sparse: only the cards a duelist can actually drop in that tier. */
typedef struct { uint16_t card; uint16_t weight; } PsxDropWeight;

typedef struct {
    const char          *name;
    const PsxDropWeight *tier[PSX_DROP_DB_TIERS];  /* S/A POW, B/C/D, S/A TEC */
    uint16_t             count[PSX_DROP_DB_TIERS];
    /* The DECK pool: the same shape, one array earlier in the duelist's disc
     * record, and the pool the game draws the opponent's 40 cards from. Also
     * sums to 2048. See the CPU Manager (psx_cpu_manager.c). */
    const PsxDropWeight *deck;
    uint16_t             deck_n;
} PsxDropDbDuelist;

extern const PsxDropDbDuelist PSX_DROP_DB[PSX_DROP_DB_DUELISTS];

/* The three drop tiers, in table order, as the game's own rank bands. */
extern const char *const PSX_DROP_TIER_NAMES[PSX_DROP_DB_TIERS];

#endif /* PSX_DROP_DB_H */
