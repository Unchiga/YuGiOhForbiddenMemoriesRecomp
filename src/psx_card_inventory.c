/* Shared deck/trunk accounting for title gameplay-authoring features. */
#include "psx_card_inventory.h"

#include <string.h>

#include "mod_plugins.h"
#include "psx_card_chest.h"
#include "psx_ygo_cheats.h"
#include "psx_ygo_netplay.h"

#define SAVE_LIVE       0x801D0200u
#define SAVE_MIRROR     0x801D3200u
#define SAVE_DECK_N     40
#define SAVE_TRUNK_OFF  0x50u
#define SAVE_CHIPS_OFF  0x5E0u
#define UI_TRUNK_STOCK  0x80105D98u

static uint32_t ui_trunk_base(void)
{
    const uint32_t card1 = psx_card_chest_ui_trunk_cell(1u);
    return card1 ? card1 : UI_TRUNK_STOCK;
}

static int ui_is_live_trunk(uint32_t base)
{
    for (int i = 0; i < PSX_CARD_INVENTORY_CARDS; i++)
        if (psx_mod_read_byte(base + (uint32_t)i) !=
            psx_mod_read_byte(SAVE_LIVE + SAVE_TRUNK_OFF + (uint32_t)i))
            return 0;
    return 1;
}

int psx_card_inventory_snapshot(PsxCardInventorySnapshot *out)
{
    if (!out || !psx_ygo_save_is_live()) return 0;
    memset(out, 0, sizeof *out);
    for (int slot = 0; slot < SAVE_DECK_N; slot++) {
        const int id = (int)psx_mod_read_half(SAVE_LIVE + (uint32_t)slot * 2u);
        if (id < 1 || id > PSX_CARD_INVENTORY_CARDS) return 0;
        if (out->deck[id - 1] != 255u) out->deck[id - 1]++;
    }
    for (int i = 0; i < PSX_CARD_INVENTORY_CARDS; i++)
        out->trunk[i] = psx_mod_read_byte(
            SAVE_LIVE + SAVE_TRUNK_OFF + (uint32_t)i);
    out->starchips = psx_mod_read_word(SAVE_LIVE + SAVE_CHIPS_OFF);
    return 1;
}

int psx_card_inventory_owned(int card_id)
{
    if (card_id < 1 || card_id > PSX_CARD_INVENTORY_CARDS) return -1;
    PsxCardInventorySnapshot s;
    if (!psx_card_inventory_snapshot(&s)) return -1;
    return (int)s.deck[card_id - 1] + (int)s.trunk[card_id - 1];
}

int psx_card_inventory_commit_trunk(
    const PsxCardInventorySnapshot *expected,
    const uint8_t target_trunk[PSX_CARD_INVENTORY_CARDS],
    uint32_t new_starchips)
{
    if (!expected || !target_trunk || new_starchips > PSX_CARD_STARCHIP_CAP ||
        psx_ygo_netplay_session())
        return 0;
    PsxCardInventorySnapshot now;
    if (!psx_card_inventory_snapshot(&now) ||
        memcmp(now.deck, expected->deck, sizeof now.deck) ||
        memcmp(now.trunk, expected->trunk, sizeof now.trunk) ||
        now.starchips != expected->starchips)
        return 0;
    for (int i = 0; i < PSX_CARD_INVENTORY_CARDS; i++)
        if (target_trunk[i] > expected->trunk[i]) return 0;

    /* The chest arena is not always a trunk. Update it only when all 722
     * bytes match the live trunk before the first write, the same measured
     * discriminator used by the All Cards action. */
    const uint32_t ui = ui_trunk_base();
    const int write_ui = ui_is_live_trunk(ui);
    for (int i = 0; i < PSX_CARD_INVENTORY_CARDS; i++) {
        if (target_trunk[i] == expected->trunk[i]) continue;
        const uint32_t off = SAVE_TRUNK_OFF + (uint32_t)i;
        psx_mod_write_byte(SAVE_LIVE + off, target_trunk[i]);
        psx_mod_write_byte(SAVE_MIRROR + off, target_trunk[i]);
        if (write_ui) psx_mod_write_byte(ui + (uint32_t)i, target_trunk[i]);
    }
    psx_mod_write_word(SAVE_LIVE + SAVE_CHIPS_OFF, new_starchips);
    psx_mod_write_word(SAVE_MIRROR + SAVE_CHIPS_OFF, new_starchips);
    return 1;
}
