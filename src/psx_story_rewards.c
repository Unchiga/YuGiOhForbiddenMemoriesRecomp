/* psx_story_rewards.c — see psx_story_rewards.h.
 *
 * HOW THE REWARD IS DELIVERED
 * ---------------------------
 * The duel's drop is one call: Duel_SelectCardDrop (0x80021810, a0 = tier)
 * walks the resident opponent's 722 weights and returns a card id, which the
 * results screen announces and the award at 0x80021894 banks into the trunk.
 * The community's "unique drop" patch hooks that function's epilogue and
 * overwrites v0. A recompiled build has no epilogue to hook — the entry hooks
 * psx_card_drops.c owns cannot change a return value — so this does the same
 * job from the other side: it points the WEIGHT TABLE at one card (2048 out
 * of 2048) just before the game's own roll, and the game returns the card it
 * was always going to return, having rolled it fairly out of a table with one
 * entry. gDuel_wSelectedCardID, the duel object, the results text and the
 * trunk therefore all agree without this module knowing where any of them
 * live. The table is put back on the award that follows, and the RNG stream
 * is untouched either way: the same single roll happens.
 *
 * Only the weights BELOW the reward card need clearing — the walk returns the
 * first index whose running sum reaches the roll, and a full 2048 at the
 * card's own index stops every roll in 1..2048 there — but the whole tier row
 * is snapshotted and restored anyway, because "restore exactly what was
 * there" is worth more than 1444 bytes of memcpy at the end of a duel.
 *
 * WHEN IT FIRES
 * -------------
 * A campaign duel against a duelist the Drop Table Manager has given a card,
 * and — unless that pair says "every win" — only the first time.
 *
 * Campaign, because a story reward belongs to the story: gFreeDuel_bReturnFlags
 * (0x8009B365) carries 0x80 while the duel was started from the Free Duel
 * screen — the bit FreeDuel_Init reads to know it owes the record an update —
 * and this refuses to fire when it is set. Measured 0x80 in a Free Duel and
 * 0x00 in a campaign duel, 2026-09-06.
 *
 * First win, by gFreeDuel_dwUnlockedDuelists (0x801D06F4): the bitmask the
 * FREE DUEL grid is built from, MSB first, duelist id -> byte id>>3, bit
 * 0x80>>(id&7). The campaign sets a duelist's bit when you beat them, and it
 * does so AFTER the drop — which is why the community's unique-drop patch
 * reads the same mask from the roll's epilogue. So "bit still clear" IS "you
 * have never beaten them", it needs no state of its own, and it travels with
 * the save.
 *
 * gFreeDuel_aDuelistRecords (0x801D071C) looked like the better gate and is
 * not: measured 2026-09-06, a won CAMPAIGN duel against Heishin left his
 * record at 0 wins / 0 losses. FreeDuel_Init updates that table when the Free
 * Duel screen comes back, so it counts Free Duels only, and a gate built on
 * it would never close in the campaign — the reward would come again on every
 * rematch, and Heishin, the villagers, Teana and Jono can all be refought.
 *
 * The mask has one quirk worth knowing: beating Heishin sets his bit even
 * though the story wants you to lose to him, so his reward goes to whichever
 * win comes first. A pair set to "every win" sidesteps that, and is also what
 * a save that has already unlocked everyone needs — on those, "first win" has
 * nothing left to fire on.
 *
 * THERE IS NO MODS ROW
 * --------------------
 * A duelist with no card set is the off switch, and that is what a stock
 * install has: nothing scripted until the player sets a pair in the Drop
 * Table Manager. The pairs live in drop_table_edits.ini beside the weights,
 * so one Save keeps them and one Export shares them.
 *
 * Duelist numbering: PSX_DROP_DB's index order IS the opponent-id order —
 * Simon Muran 0/1 through Nitemare 37/38, Duel Master K 38/39 — so the file,
 * the Drop Table Manager and gDuel_bOpponentID all agree with a +1. Checked
 * name by name against the campaign order.
 */

#include "psx_story_rewards.h"

#include <stdio.h>

#include "cpu_state.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_packs.h"
#include "psx_drop_db.h"
#include "psx_drop_edits.h"

#define NDUEL         PSX_DROP_DB_DUELISTS      /* 39 */
#define NCARDS        PSX_DROP_DB_CARDS         /* 722 */

/* Guest map. The drop table is the resident opponent's, reloaded per duel. */
#define OPPONENT_ID     0x8009B361u             /* u8, 1..39 */
#define UNLOCKS         0x801D06F4u             /* Free Duel unlock bitmask */
#define FREE_DUEL_FLAGS 0x8009B365u             /* 0x80 = started from Free Duel */
#define FREE_DUEL_BIT   0x80u
#define DROP_TABLE      0x8017878Cu             /* tier 0 weights */
#define TIER_STRIDE     1460u
#define TIER_N          3

/* What the last drop decided, for the debug surface. */
static int      g_last_opponent = -1;
static int      g_last_free_duel = -1;
static int      g_last_beaten = -1;
static int      g_last_card;
static int      g_fired;

/* The tier row we replaced, and where. */
static uint16_t g_saved[NCARDS];
static uint32_t g_saved_addr;
static int      g_steered;

/* Has the campaign recorded a win over this duelist already? -1 when the id
 * is not a duelist at all. */
static int already_beaten(int opponent_id)
{
    if (opponent_id < 1 || opponent_id > NDUEL) return -1;
    const uint8_t b = psx_mod_read_byte(UNLOCKS + ((uint32_t)opponent_id >> 3));
    return (b & (uint8_t)(0x80u >> (opponent_id & 7))) ? 1 : 0;
}

static int in_free_duel(void)
{
    return (psx_mod_read_byte(FREE_DUEL_FLAGS) & FREE_DUEL_BIT) ? 1 : 0;
}

int psx_story_rewards_count(void) { return psx_drop_edits_reward_count(); }

int psx_story_rewards_get(int duelist, int *out_every)
{
    return psx_drop_edits_reward(duelist, out_every);
}

int psx_story_rewards_set(int duelist, int card, int every)
{
    return psx_drop_edits_reward_set(duelist, card, every);
}

void psx_story_rewards_steer_roll(CPUState *cpu, unsigned tier)
{
    (void)cpu;
    g_last_card = 0;
    if (tier >= TIER_N) return;

    const int id = (int)psx_mod_read_byte(OPPONENT_ID);
    g_last_opponent = id;
    g_last_free_duel = in_free_duel();
    g_last_beaten = already_beaten(id);
    if (g_last_free_duel) return;          /* Free Duel: the story is not here */
    int every = 0;
    const int card = psx_drop_edits_reward(id - 1, &every);
    if (!card) return;
    if (!every && g_last_beaten != 0) return;   /* beaten before: roll as stock */

    /* Snapshot the whole tier row, then leave one card holding all 2048. */
    const uint32_t base = DROP_TABLE + tier * TIER_STRIDE;
    for (int i = 0; i < NCARDS; i++)
        g_saved[i] = psx_mod_read_half(base + (uint32_t)i * 2u);
    for (int i = 0; i < card - 1; i++)
        psx_mod_write_half(base + (uint32_t)i * 2u, 0);
    psx_mod_write_half(base + (uint32_t)(card - 1) * 2u, PSX_DROP_DB_TOTAL);
    g_saved_addr = base;
    g_steered = 1;
    g_last_card = card;
    g_fired++;
}

void psx_story_rewards_restore_table(void)
{
    if (!g_steered) return;
    g_steered = 0;
    for (int i = 0; i < NCARDS; i++)
        psx_mod_write_half(g_saved_addr + (uint32_t)i * 2u, g_saved[i]);
    if (g_last_card) {
        char msg[80];
        snprintf(msg, sizeof msg, "Story reward: %.40s",
                 psx_card_packs_display_name(g_last_card));
        host_osd_push(msg, 2000);
    }
}

int psx_story_rewards_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    const int now = (int)psx_mod_read_byte(OPPONENT_ID);
    unsigned n = (unsigned)snprintf(out, cap,
        "\"pairs\":%d,\"fired\":%d,\"last_opponent\":%d,\"last_free_duel\":%d,"
        "\"last_beaten\":%d,\"last_card\":%d,\"steered\":%d,"
        "\"opponent_now\":%d,\"free_duel_now\":%d,\"beaten_now\":%d,\"rewards\":[",
        psx_story_rewards_count(), g_fired, g_last_opponent, g_last_free_duel,
        g_last_beaten, g_last_card, g_steered,
        now, in_free_duel(), already_beaten(now));
    int first = 1;
    for (int d = 0; d < NDUEL && n + 128u < cap; d++) {
        int every = 0;
        const int card = psx_drop_edits_reward(d, &every);
        if (!card) continue;
        n += (unsigned)snprintf(out + n, cap - n,
            "%s{\"duelist\":%d,\"id\":%d,\"name\":\"%s\",\"card\":%d,\"every\":%d}",
            first ? "" : ",", d, d + 1, PSX_DROP_DB[d].name, card, every);
        first = 0;
    }
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}
