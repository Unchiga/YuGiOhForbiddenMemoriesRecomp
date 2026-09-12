/* psx_card_inventory.h -- one title-owned view of deck, trunk and starchips.
 *
 * Forbidden Memories moves copies placed in the 40-card deck out of the
 * 722-byte trunk.  Any ownership or bulk-inventory feature must therefore
 * count both.  Mutation is deliberately narrower: callers may reduce trunk
 * counts and update starchips, but never rewrite the active deck. */
#ifndef PSX_CARD_INVENTORY_H
#define PSX_CARD_INVENTORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_CARD_INVENTORY_CARDS 722
#define PSX_CARD_INVENTORY_KEEP 3
#define PSX_CARD_STARCHIP_CAP 999999u

typedef struct {
    uint8_t deck[PSX_CARD_INVENTORY_CARDS];
    uint8_t trunk[PSX_CARD_INVENTORY_CARDS];
    uint32_t starchips;
} PsxCardInventorySnapshot;

/* Snapshot the live save. deck[id-1] is the number of that ID among the 40
 * slots; trunk[id-1] is the spare-copy byte. Returns 0 off-save. */
int psx_card_inventory_snapshot(PsxCardInventorySnapshot *out);

/* Ownership is deck + trunk, because deck construction moves rather than
 * copies cards. Returns -1 if no live save or the ID is invalid. */
int psx_card_inventory_owned(int card_id);

/* Atomically validate EXPECTED against the current live save, then replace
 * only its trunk bytes with TARGET_TRUNK and set starchips. Target counts may
 * never exceed their expected values and NEW_STARCHIPS may not exceed the
 * display/save cap. The mirror and a currently-live chest working copy are
 * kept coherent. Returns 1 on success, 0 on stale/invalid/netplay state. */
int psx_card_inventory_commit_trunk(
    const PsxCardInventorySnapshot *expected,
    const uint8_t target_trunk[PSX_CARD_INVENTORY_CARDS],
    uint32_t new_starchips);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_INVENTORY_H */
