/* psx_card_colors.c -- see psx_card_colors.h.
 *
 * THE THREE PLACES A FRAME IS CHOSEN (decomp + TEA-Online's cardcolor tool):
 *   init_container_entry_objs 0x800291E0 (the big card): frame sprite
 *     selector setup in s3, joined at the shared tail 0x8002940C;
 *   func_80016784 (the in-duel hand card): CLUT row 241 + slot in the
 *     scratchpad packet at 0x1F800320+0x12, joined at 0x80016BBC;
 *   func_8002BFCC (the library grid): a texture/CLUT selector per card, read through
 *     the lookup table installed by the loop at 0x8002C290.
 * All three take the slot from one nibble table (byte n = card 2n+1 high,
 * 2n+2 low) which stock does not have. The patch is TEA's, byte for byte:
 * the table over the PsyQ libgpu debug strings at 0x80012150 (never
 * printed by the retail build), two stubs at 0x800122C0, two hook words
 * each at 0x8002940C and 0x80016BBC, and the rewritten grid loop. One
 * word differs: the small-card stub accepts all ten slots. The four slots
 * after orange use authored palettes derived from recolored stock artwork,
 * drawn through tag CLUTs the extended slot arithmetic selects (see
 * sync_overrides).
 *
 * The code words go in with psx_mod_write_code_word (those three functions
 * then run in the interpreter) and are re-asserted every frame, since a
 * save state puts the stock bytes back; nothing is written until a card
 * actually wants a non-stock frame, so a stock install stays compiled.
 *
 * PALETTES: WA_MRG +0xB84000, seven 512-byte BGR555 blocks (the frame tones
 * are the first 72 entries), mirrored in ten places on the disc. Six are
 * populated: yellow, green, pink, blue, purple, orange. Red, white, dark
 * yellow and navy are authored replacements that retain the stock frame's
 * transparency, shading and highlight structure. */

#include "psx_card_colors.h"
#include "psx_card_color_palettes.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_db.h"
#include "psx_card_extend.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_ygo_netplay.h"
#include "gpu.h"
#include "gpu_render.h"

#define CARD_COUNT 722
#define SECTOR 2048

#define TABLE_ADDR   0x80012150u    /* 361 bytes */
#define STUBS_ADDR   0x800122C0u    /* 176 bytes = 44 words */
#define HOOK_BIG     0x8002940Cu    /* 2 words */
#define HOOK_SMALL   0x80016BBCu    /* 2 words */
#define HOOK_GRID    0x8002C290u    /* 28 words */
#define HOOK_BIG_ATTR 0x800290BCu   /* attribute CLUT x must stay stock */
#define STATS_STOCK  0x801D4244u
#define SERVICE_SLOT3 0x800E9DBCu
#define LIBRARY_UPDATE 0x80029EC4u

#define PAL_LBA      15998u         /* WA +0xB84000: palettes 0..3 here, 4..6 in the next sector */
#define PAL_BLOCK    512
#define PAL_ENTRIES  72

/* The preview renderer draws the frame through CLUT (256 * (n >> 8), 240 + (n & 0xFF))
 * with n = selector + 8: stock slots are n = 0x108.. at (256,248..253). Word 12
 * (addiu s3,s3,-0x106) gives the four authored slots n = 8..11, tag CLUTs
 * (0,248..251) that sync_overrides() resolves; nothing is stored there. */
#define PREVIEW_CLUT_Y 248

/* TEA-Online cardcolor v2 patch (process-cardcolor/patch/v2_*.bin), little-endian words. */
static const uint32_t STUBS[44] = {
    0x00139842,0x3C028001,0x24422150,0x00531021,0x90420000,0x30930001,0x12600002,0x3053000F,
    0x00029902,0x2A620006,0x14400002,0x26730100,0x2673FEFA,0x3C03801D,0x0800A505,0x24635332,
    0x8FA20028,0x3C038001,0x9442000C,0x24632150,0x24440000,0x2442FFFF,0x00021042,0x00621821,
    0x90630000,0x30840001,0x10800002,0x3062000F,0x00031102,0x00021040,0x3C040000,0x24840000,
    0x00822021,0x94830018,0xA6230012,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x3C046666,0x2402000E,0x08005AF1,0x00000000
};
static const uint32_t HOOK_BIG_W[2]   = { 0x080048B0, 0x2493FFFF };
static const uint32_t HOOK_SMALL_W[2] = { 0x080048C0, 0x00000000 };
static const uint32_t HOOK_BIG_ATTR_W = 0x24030100u; /* addiu v1,zero,0x100 */
static const uint32_t HOOK_GRID_W[28] = {
    0x3C028001,0x24422150,0xA0800056,0xA6A00054,0x2671FFFF,0x00118842,0x00518821,0x92310000,
    0x32720001,0x12400002,0x3232000F,0x00119102,0x3C110000,0x26310000,0x00129040,0x02328821,
    0x96310000,0x00000000,0x00000000,0xA4910054,0x24840004,0x26730001,0x2A7102D3,0x1620FFEA,
    0x24A50004,0x00000000,0x00000000,0x00000000
};

static const uint16_t GRID_SELECTOR[PSX_CARD_COLOR_COUNT] = {
    0x0160, 0x0170, 0x0180, 0x0190, 0x01A0, 0x01B0,
    0x01C0, 0x01D0, 0x01E0, 0x01F0
};
static const uint16_t SMALL_CLUT_Y[PSX_CARD_COLOR_COUNT] = {
    0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6,
    0x01F8, 0x01F9, 0x01FA, 0x01FB
};
/* stock words, captured before the first write so a clean uninstall exists */
static uint32_t s_stock_stubs[44], s_stock_big[2], s_stock_small[2], s_stock_grid[28];
static uint32_t s_stock_big_attr;
static uint8_t  s_stock_table[361];
static int      s_stock_ok, s_patched;
static uint32_t s_map_addr;

static uint8_t  s_slot[CARD_COUNT + 1];      /* wanted slot per card */
static uint8_t  s_table[361];
static unsigned s_seen_gen = (unsigned)-1;
static int      s_non_stock, s_has_custom;

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
    s_has_custom = 0;
    for (int id = 1; id <= CARD_COUNT; id++) {
        PsxCardPack c;
        int slot = stock_slot(id);
        if (psx_card_packs_get(id, &c)) {
            if (c.color >= 0 && c.color < PSX_CARD_COLOR_COUNT) slot = c.color;
            else if (slot == 0 && psx_card_effects_monster_has_effect(id)) slot = PSX_CARD_COLOR_ORANGE;
        }
        s_slot[id] = (uint8_t)slot;
        if (slot != stock_slot(id)) s_non_stock = 1;
        if (slot >= PSX_CARD_COLOR_RED) s_has_custom = 1;
    }
    memset(s_table, 0, sizeof s_table);
    for (int id = 1; id <= CARD_COUNT; id++) {
        const int n = (id - 1) / 2;
        if (id & 1) s_table[n] |= (uint8_t)(s_slot[id] << 4);
        else        s_table[n] |= (uint8_t)(s_slot[id] & 0xF);
    }
}

/* ---- palettes -------------------------------------------------------------- */
static uint8_t s_pal_sector[2 * SECTOR];
static int s_pal_loaded;

static int load_palettes(void)
{
    if (!s_pal_loaded) {
        if (!psx_mod_cd_read_stock_sector(PAL_LBA, s_pal_sector) ||
            !psx_mod_cd_read_stock_sector(PAL_LBA + 1, s_pal_sector + SECTOR)) return 0;
        s_pal_loaded = 1;
    }
    return 1;
}

static void make_custom_palette(int slot, uint16_t out[256])
{
    memcpy(out, PSX_CARD_COLOR_CUSTOM[slot - PSX_CARD_COLOR_RED], 256u * sizeof *out);
}

/* The 52x60 duel card uses a different indexed texture from the 128x256
 * preview. Its six palettes share UI/attribute colours and vary only these
 * frame entries. Reusing the preview palette therefore recolours (and changes
 * the STP bit of) unrelated pixels such as the attribute badge. */
static const uint8_t SMALL_FRAME_INDEX[] = {
    65,66,67,68,69,70,71, 88,89,90,91,92,93,94,95, 115,
    120,121,122,123,124,125
};
static const uint8_t SMALL_SOURCE[4] = {
    PSX_CARD_COLOR_PINK, PSX_CARD_COLOR_PURPLE,
    PSX_CARD_COLOR_YELLOW, PSX_CARD_COLOR_BLUE
};
static uint16_t s_small_custom[4][256];
static uint8_t s_small_ready[4];

static unsigned color_distance(uint16_t a, uint16_t b)
{
    const int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    const int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    const int dr = ar - br, dg = ag - bg, db = ab - bb;
    return (unsigned)(dr * dr + dg * dg + db * db);
}

static uint16_t recolor_from_large(int custom, int source, uint16_t color)
{
    const uint8_t *src = s_pal_sector + source * PAL_BLOCK;
    unsigned best = 1, best_distance = ~0u;
    for (unsigned i = 1; i < PAL_ENTRIES; i++) {
        const uint16_t candidate =
            (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
        const unsigned distance = color_distance(color & 0x7FFFu,
                                                  candidate & 0x7FFFu);
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return (uint16_t)((color & 0x8000u) |
                      (PSX_CARD_COLOR_CUSTOM[custom][best] & 0x7FFFu));
}

static int make_custom_small_palette(int custom)
{
    uint16_t *out = s_small_custom[custom];
    const int source = SMALL_SOURCE[custom];

    if (!load_palettes()) return 0;
    gr_vram_transfer_out(256, 241 + source, 256, 1, out);
    /* These rows contain 0x8001 placeholders until the duel resources arrive. */
    if ((out[65] & 0x7FFFu) <= 1u || (out[71] & 0x7FFFu) <= 1u) return 0;

    for (unsigned n = 0; n < sizeof SMALL_FRAME_INDEX; n++) {
        const unsigned dst = SMALL_FRAME_INDEX[n];
        /* Transparency/semi-transparency belongs to the hand texture, not to
         * the large preview palette. Preserve it exactly. */
        out[dst] = recolor_from_large(custom, source, out[dst]);
    }
    s_small_ready[custom] = 1;
    return 1;
}

/* The Library grid is a third texture again: 4-bpp, with six adjacent
 * 16-entry CLUTs at (352,247); the grid selector is the CLUT x. Wait until
 * that screen has loaded all six, then recolour only the entries that differ
 * between its stock palettes. */
#define GRID_CLUT_Y 247
static uint16_t s_grid_custom[4][16];
static uint8_t s_grid_ready;

static int make_custom_grid_palettes(void)
{
    uint16_t stock[6][16];
    if (!load_palettes()) return 0;
    gr_vram_transfer_out(0x160, GRID_CLUT_Y, 6 * 16, 1, stock[0]);
    for (int slot = 0; slot < 6; slot++) {
        unsigned real = 0;
        for (int i = 1; i < 16; i++)
            if ((stock[slot][i] & 0x7FFFu) > 1u) real++;
        if (real < 4) return 0;
    }
    for (int custom = 0; custom < 4; custom++) {
        const int source = SMALL_SOURCE[custom];
        memcpy(s_grid_custom[custom], stock[source], sizeof stock[source]);
        for (int i = 1; i < 16; i++) {
            int varies = 0;
            for (int slot = 1; slot < 6; slot++)
                if (stock[slot][i] != stock[0][i]) varies = 1;
            if (varies)
                s_grid_custom[custom][i] =
                    recolor_from_large(custom, source, stock[source][i]);
        }
    }
    s_grid_ready = 1;
    return 1;
}

/* The authored palettes never live in guest VRAM: rows that look empty on one
 * screen hold another screen's palettes (the title wall reads (0,244), the
 * duel results (0..255,248), the Free Duel portraits (128,496..506)).
 * Each custom slot's primitives instead carry a tag CLUT, and the GPU draws
 * the primitives that use it on the card texture pages through the authored
 * palette (gpu_clut_override_set), borrowing the empty seventh card palette
 * row only for those primitives. Tags: the preview frame and its name glyphs
 * (0,248..251) on pages (768,256)/(896,256), the hand card (256,504..507) on
 * (896,256), the Library grid icons (448..496,247) at 4 bpp on (704,256).
 * The four 16-entry grid palettes sit side by side, so a page of mixed icons
 * needs no rewrites. */
#define CARD_PAGE(x, y) (1u << ((x) / 64 + 16 * ((y) / 256)))
#define SCRATCH_X 256
#define SCRATCH_Y 254               /* seventh card palette: empty on the disc */
enum { OV_PREVIEW = 0, OV_HAND = 4, OV_GRID = 8, OV_COUNT = 12 };
static unsigned s_ov_set;           /* override slots handed to the GPU */

static void set_override(int slot, uint16_t tag_x, uint16_t tag_y, int depth,
                         uint32_t pages, uint16_t scratch_x, const uint16_t *pal, int n)
{
    if ((s_ov_set >> slot) & 1u) return;
    if (gpu_clut_override_set(slot, tag_x, tag_y, depth, pages,
                              scratch_x, SCRATCH_Y, pal, n) == 0)
        s_ov_set |= 1u << slot;
}

static void clear_overrides(void)
{
    for (int slot = 0; slot < OV_COUNT; slot++)
        if ((s_ov_set >> slot) & 1u) gpu_clut_override_clear(slot);
    s_ov_set = 0;
}

static void sync_overrides(void)
{
    uint16_t pal[256];
    if (!s_has_custom || !s_patched) {
        clear_overrides();
        return;
    }
    if (psx_mod_read_word(SERVICE_SLOT3) == LIBRARY_UPDATE && !s_grid_ready)
        make_custom_grid_palettes();
    for (int custom = 0; custom < 4; custom++) {
        make_custom_palette(PSX_CARD_COLOR_RED + custom, pal);
        set_override(OV_PREVIEW + custom, 0, PREVIEW_CLUT_Y + custom, 8,
                     CARD_PAGE(768, 256) | CARD_PAGE(896, 256), SCRATCH_X, pal, 256);
        /* The hand palette is derived from the duel's own, so it waits for
         * the duel resources. */
        if (s_small_ready[custom] || make_custom_small_palette(custom))
            set_override(OV_HAND + custom, 256, SMALL_CLUT_Y[PSX_CARD_COLOR_RED + custom], 8,
                         CARD_PAGE(896, 256), SCRATCH_X, s_small_custom[custom], 256);
        if (s_grid_ready)
            set_override(OV_GRID + custom, GRID_SELECTOR[PSX_CARD_COLOR_RED + custom],
                         GRID_CLUT_Y, 4, CARD_PAGE(704, 256), SCRATCH_X + 16 * custom,
                         s_grid_custom[custom], 16);
    }
}

int psx_card_colors_swatch(int slot, unsigned char rgb[9])
{
    if (slot < 0 || slot >= PSX_CARD_COLOR_COUNT) return 0;
    if (!load_palettes()) return 0;
    uint16_t custom[256];
    const uint8_t *pal = s_pal_sector + slot * PAL_BLOCK;
    if (slot >= PSX_CARD_COLOR_RED) {
        make_custom_palette(slot, custom);
        pal = (const uint8_t *)custom;
    }
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
    s_stock_big_attr = psx_mod_read_word(HOOK_BIG_ATTR);
    for (int i = 0; i < 28; i++) s_stock_grid[i] = psx_mod_read_word(HOOK_GRID + (uint32_t)i * 4u);
    for (int i = 0; i < 361; i++) s_stock_table[i] = psx_mod_read_byte(TABLE_ADDR + (uint32_t)i);
    /* the hook sites must be the stock words the patch expects */
    if (s_stock_big[0] != 0x3C03801Du || s_stock_big[1] != 0x24635332u ||
        s_stock_small[0] != 0x3C046666u || s_stock_small[1] != 0x2402000Eu ||
        s_stock_big_attr != 0x96430040u) return;
    s_stock_ok = 1;
}

static void patch_assert(int on)
{
    if (on) {
        const uint32_t hi = ((s_map_addr + 0x8000u) >> 16) & 0xFFFFu;
        const uint32_t lo = s_map_addr & 0xFFFFu;
        for (int i = 0; i < PSX_CARD_COLOR_COUNT; i++) {
            psx_mod_write_half(s_map_addr + (uint32_t)i * 2u, GRID_SELECTOR[i]);
            psx_mod_write_half(s_map_addr + 24u + (uint32_t)i * 2u, SMALL_CLUT_Y[i]);
        }
        for (int i = 0; i < 361; i++) if (psx_mod_read_byte(TABLE_ADDR + (uint32_t)i) != s_table[i]) psx_mod_write_byte(TABLE_ADDR + (uint32_t)i, s_table[i]);
        for (int i = 0; i < 44; i++) {
            uint32_t word = STUBS[i];
            if (i == 30) word |= hi;
            if (i == 31) word |= lo;
            assert_code(STUBS_ADDR + (uint32_t)i * 4u, word);
        }
        for (int i = 0; i < 28; i++) {
            uint32_t word = HOOK_GRID_W[i];
            if (i == 12) word |= hi;
            if (i == 13) word |= lo;
            assert_code(HOOK_GRID + (uint32_t)i * 4u, word);
        }
        for (int i = 0; i < 2; i++)  { assert_code(HOOK_BIG + (uint32_t)i * 4u, HOOK_BIG_W[i]); assert_code(HOOK_SMALL + (uint32_t)i * 4u, HOOK_SMALL_W[i]); }
        assert_code(HOOK_BIG_ATTR, HOOK_BIG_ATTR_W);
        s_patched = 1;
    } else if (s_patched) {
        assert_code(HOOK_BIG_ATTR, s_stock_big_attr);
        for (int i = 0; i < 2; i++)  { assert_code(HOOK_BIG + (uint32_t)i * 4u, s_stock_big[i]); assert_code(HOOK_SMALL + (uint32_t)i * 4u, s_stock_small[i]); }
        for (int i = 0; i < 28; i++) assert_code(HOOK_GRID + (uint32_t)i * 4u, s_stock_grid[i]);
        for (int i = 0; i < 44; i++) assert_code(STUBS_ADDR + (uint32_t)i * 4u, s_stock_stubs[i]);
        for (int i = 0; i < 361; i++) if (psx_mod_read_byte(TABLE_ADDR + (uint32_t)i) != s_stock_table[i]) psx_mod_write_byte(TABLE_ADDR + (uint32_t)i, s_stock_table[i]);
        s_patched = 0;
    }
}

static void tick(void)
{
    if (psx_ygo_netplay_session()) {         /* netplay: per-machine layer, peers must stay bit-identical */
        clear_overrides();
        return;
    }
    if (!psx_mod_game_started()) return;
    if (!s_map_addr) return;
    take_stock();
    if (!s_stock_ok) return;
    const unsigned gen = psx_card_packs_generation();
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        rebuild();
    }
    patch_assert(s_non_stock);
    sync_overrides();
}

int psx_card_colors_state_json(char *out, unsigned cap)
{
    int counts[PSX_CARD_COLOR_COUNT] = { 0 };
    uint64_t hits, installs, restores;
    for (int id = 1; id <= CARD_COUNT; id++) counts[s_slot[id]]++;
    gpu_clut_override_stats(&hits, &installs, &restores);
    unsigned n = (unsigned)snprintf(out, cap,
                                    "\"ready\":%d,\"patched\":%d,\"non_stock\":%d,\"custom\":%d,"
                                    "\"overrides\":\"0x%03X\",\"ov_hits\":%llu,\"ov_installs\":%llu,"
                                    "\"ov_restores\":%llu,\"counts\":[",
                                    s_stock_ok, s_patched, s_non_stock, s_has_custom, s_ov_set,
                                    (unsigned long long)hits, (unsigned long long)installs,
                                    (unsigned long long)restores);
    for (int i = 0; i < PSX_CARD_COLOR_COUNT && n < cap; i++)
        n += (unsigned)snprintf(out + n, cap - n, "%s%d", i ? "," : "", counts[i]);
    if (n < cap) n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

PSX_MOD_CONSTRUCTOR(psx_card_colors_install)
{
    s_map_addr = psx_mod_alloc_guest_memory(48u, 4u);
    (void)psx_game_add_frame_hook(tick);
}
