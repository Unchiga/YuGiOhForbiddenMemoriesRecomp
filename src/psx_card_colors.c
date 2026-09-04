/* psx_card_colors.c -- see psx_card_colors.h.
 *
 * THE THREE PLACES A FRAME IS CHOSEN (decomp + TEA-Online's cardcolor tool):
 *   init_container_entry_objs 0x800291E0 (the big card): frame sprite
 *     s3 = 0x100 + slot, joined at the shared tail 0x8002940C;
 *   func_80016784 (the in-duel small card): CLUT row 241 + slot in the
 *     scratchpad packet at 0x1F800320+0x12, joined at 0x80016BBC;
 *   func_8002BFCC (the library grid): u16 0x160 + 16*slot per card, the
 *     loop at 0x8002C290.
 * All three take the slot from one nibble table (byte n = card 2n+1 high,
 * 2n+2 low) which stock does not have. The patch is TEA's, byte for byte:
 * the table over the PsyQ libgpu debug strings at 0x80012150 (never
 * printed by the retail build), two stubs at 0x800122C0, two hook words
 * each at 0x8002940C and 0x80016BBC, and the rewritten grid loop. One
 * word differs: the small-card stub's "slot >= 6 falls back" test is
 * raised to 7, because the seventh palette is filled here (stock ships it
 * empty, which is why TEA needed the fallback).
 *
 * The code words go in with psx_mod_write_code_word (those three functions
 * then run in the interpreter) and are re-asserted every frame, since a
 * save state puts the stock bytes back; nothing is written until a card
 * actually wants a non-stock frame, so a stock install stays compiled.
 *
 * PALETTES: WA_MRG +0xB84000, seven 512-byte BGR555 blocks (the frame tones
 * are the first 72 entries), mirrored in ten places on the disc. Six are
 * populated: yellow, green, pink, blue, purple, orange. The seventh is
 * empty, and a slot for it draws nothing in the library grid and garbage
 * in the duel (no CLUT row), so only six slots are offered. */

#include "psx_card_colors.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_db.h"
#include "psx_card_extend.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"

#define CARD_COUNT 722
#define SECTOR 2048

#define TABLE_ADDR   0x80012150u    /* 361 bytes */
#define STUBS_ADDR   0x800122C0u    /* 176 bytes = 44 words */
#define HOOK_BIG     0x8002940Cu    /* 2 words */
#define HOOK_SMALL   0x80016BBCu    /* 2 words */
#define HOOK_GRID    0x8002C290u    /* 28 words */
#define STATS_STOCK  0x801D4244u

#define PAL_LBA      15998u         /* WA +0xB84000: palettes 0..3 here, 4..6 in the next sector */
#define PAL_BLOCK    512
#define PAL_ENTRIES  72

/* TEA-Online cardcolor v2 patch (process-cardcolor/patch/v2_*.bin), little-endian words. */
static const uint32_t STUBS[44] = {
    0x00139842,0x3C028001,0x24422150,0x00531021,0x80420000,0x30930001,0x304200FF,0x12600002,
    0x3053000F,0x00029902,0x26730100,0x3C03801D,0x0800A505,0x24635332,0x6761544F,0x28766E45,
    0x8FA20028,0x3C038001,0x9442000C,0x24632150,0x24440000,0x2442FFFF,0x00021042,0x00621821,
    0x90630000,0x30840001,0x10800002,0x3062000F,0x00031102,0x28440007,0x14800006,0x00000000,
    0x3C0200F1,0x34420180,0xAE220010,0x00000000,0x24020000,0x240400F1,0x00441821,0xA6230012,
    0x3C046666,0x2402000E,0x08005AF1,0x00000000
};
static const uint32_t HOOK_BIG_W[2]   = { 0x080048B0, 0x2493FFFF };
static const uint32_t HOOK_SMALL_W[2] = { 0x080048C0, 0x00000000 };
static const uint32_t HOOK_GRID_W[28] = {
    0x3C028001,0x24422150,0xA0800056,0xA6A00054,0x2671FFFF,0x00118842,0x00518821,0x92310000,
    0x32720001,0x12400002,0x3232000F,0x00119102,0x2A510006,0x16200002,0x00000000,0x00000000,
    0x00129100,0x24110160,0x02328821,0xA4910054,0x24840004,0x26730001,0x2A7102D3,0x1620FFEA,
    0x24A50004,0x00000000,0x00000000,0x00000000
};

/* stock words, captured before the first write so a clean uninstall exists */
static uint32_t s_stock_stubs[44], s_stock_big[2], s_stock_small[2], s_stock_grid[28];
static uint8_t  s_stock_table[361];
static int      s_stock_ok, s_patched;

static uint8_t  s_slot[CARD_COUNT + 1];      /* wanted slot per card */
static uint8_t  s_table[361];
static unsigned s_seen_gen = (unsigned)-1;
static int      s_non_stock;

/* ---- slots ---------------------------------------------------------------- */
static int stock_slot(int id)
{
    const uint32_t w = psx_mod_read_word(psx_card_extend_stats_base() + (uint32_t)(id - 1) * 4u);
    const int type = (int)((w >> 26) & 0x1Fu);
    return type < 20 ? 0 : type == 21 ? 2 : type == 22 ? 3 : 1;
}

int psx_card_colors_slot(int id)
{
    if (id < 1 || id > CARD_COUNT) return 0;
    return s_slot[id];
}

static void rebuild(void)
{
    s_non_stock = 0;
    for (int id = 1; id <= CARD_COUNT; id++) {
        PsxCardPack c;
        int slot = stock_slot(id);
        if (psx_card_packs_get(id, &c)) {
            if (c.color >= 0 && c.color < PSX_CARD_COLOR_COUNT) slot = c.color;
            else if (slot == 0 && psx_card_effects_monster_has_effect(id)) slot = PSX_CARD_COLOR_ORANGE;
        }
        s_slot[id] = (uint8_t)slot;
        if (slot != stock_slot(id)) s_non_stock = 1;
    }
    memset(s_table, 0, sizeof s_table);
    for (int id = 1; id <= CARD_COUNT; id++) {
        const int n = (id - 1) / 2;
        if (id & 1) s_table[n] |= (uint8_t)(s_slot[id] << 4);
        else        s_table[n] |= (uint8_t)(s_slot[id] & 0xF);
    }
}

/* ---- palettes -------------------------------------------------------------- */
int psx_card_colors_swatch(int slot, unsigned char rgb[9])
{
    static uint8_t sec[2 * SECTOR];
    static int loaded;
    if (slot < 0 || slot >= PSX_CARD_COLOR_COUNT) return 0;
    if (!loaded) {
        if (!psx_mod_cd_read_stock_sector(PAL_LBA, sec) || !psx_mod_cd_read_stock_sector(PAL_LBA + 1, sec + SECTOR)) return 0;
        loaded = 1;
    }
    const uint8_t *pal = sec + slot * PAL_BLOCK;
    static const int pick[3] = { 1, 20, 40 };
    for (int i = 0; i < 3; i++) {
        const uint16_t v = (uint16_t)(pal[pick[i] * 2] | (pal[pick[i] * 2 + 1] << 8));
        rgb[i * 3] = (unsigned char)((v & 31) * 255 / 31);
        rgb[i * 3 + 1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
        rgb[i * 3 + 2] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
    }
    return 1;
}

/* ---- the patch ------------------------------------------------------------- */
static void assert_code(uint32_t at, uint32_t v) { if (psx_mod_read_word(at) != v) psx_mod_write_code_word(at, v); }

static void take_stock(void)
{
    if (s_stock_ok || !psx_card_db_ready()) return;
    for (int i = 0; i < 44; i++) s_stock_stubs[i] = psx_mod_read_word(STUBS_ADDR + (uint32_t)i * 4u);
    for (int i = 0; i < 2; i++)  { s_stock_big[i] = psx_mod_read_word(HOOK_BIG + (uint32_t)i * 4u); s_stock_small[i] = psx_mod_read_word(HOOK_SMALL + (uint32_t)i * 4u); }
    for (int i = 0; i < 28; i++) s_stock_grid[i] = psx_mod_read_word(HOOK_GRID + (uint32_t)i * 4u);
    for (int i = 0; i < 361; i++) s_stock_table[i] = psx_mod_read_byte(TABLE_ADDR + (uint32_t)i);
    /* the hook sites must be the stock words the patch expects */
    if (s_stock_big[0] != 0x3C03801Du || s_stock_big[1] != 0x24635332u || s_stock_small[0] != 0x3C046666u || s_stock_small[1] != 0x2402000Eu) return;
    s_stock_ok = 1;
}

static void patch_assert(int on)
{
    if (on) {
        for (int i = 0; i < 361; i++) if (psx_mod_read_byte(TABLE_ADDR + (uint32_t)i) != s_table[i]) psx_mod_write_byte(TABLE_ADDR + (uint32_t)i, s_table[i]);
        for (int i = 0; i < 44; i++) assert_code(STUBS_ADDR + (uint32_t)i * 4u, STUBS[i]);
        for (int i = 0; i < 28; i++) assert_code(HOOK_GRID + (uint32_t)i * 4u, HOOK_GRID_W[i]);
        for (int i = 0; i < 2; i++)  { assert_code(HOOK_BIG + (uint32_t)i * 4u, HOOK_BIG_W[i]); assert_code(HOOK_SMALL + (uint32_t)i * 4u, HOOK_SMALL_W[i]); }
        s_patched = 1;
    } else if (s_patched) {
        for (int i = 0; i < 2; i++)  { assert_code(HOOK_BIG + (uint32_t)i * 4u, s_stock_big[i]); assert_code(HOOK_SMALL + (uint32_t)i * 4u, s_stock_small[i]); }
        for (int i = 0; i < 28; i++) assert_code(HOOK_GRID + (uint32_t)i * 4u, s_stock_grid[i]);
        for (int i = 0; i < 44; i++) assert_code(STUBS_ADDR + (uint32_t)i * 4u, s_stock_stubs[i]);
        for (int i = 0; i < 361; i++) if (psx_mod_read_byte(TABLE_ADDR + (uint32_t)i) != s_stock_table[i]) psx_mod_write_byte(TABLE_ADDR + (uint32_t)i, s_stock_table[i]);
        s_patched = 0;
    }
}

static void tick(void)
{
    if (!psx_mod_game_started()) return;
    take_stock();
    if (!s_stock_ok) return;
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        rebuild();
    }
    patch_assert(s_non_stock);
}

int psx_card_colors_state_json(char *out, unsigned cap)
{
    int counts[PSX_CARD_COLOR_COUNT] = { 0 };
    for (int id = 1; id <= CARD_COUNT; id++) counts[s_slot[id]]++;
    return (unsigned)snprintf(out, cap, "\"ready\":%d,\"patched\":%d,\"non_stock\":%d,\"counts\":[%d,%d,%d,%d,%d,%d]",
                              s_stock_ok, s_patched, s_non_stock,
                              counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]) < cap;
}

PSX_MOD_CONSTRUCTOR(psx_card_colors_install)
{
    (void)psx_game_add_frame_hook(tick);
}
