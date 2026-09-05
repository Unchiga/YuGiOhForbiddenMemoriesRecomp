/* psx_free_duel_rows.c -- add rows to the FREE DUEL opponent grid.
 *
 * MILESTONE 1 (visual): one extra row of five PLACEHOLDER cells under the
 * stock 8x5 grid. Every placeholder shows Simon's portrait, name and record,
 * and picking one duels Simon. Nothing in the save is touched: the post-duel
 * win/loss bump is skipped for placeholder rows because the 39-pair record
 * table has no slot for them (the words after it are the chest's New! ring).
 *
 * HOW THE STOCK SCREEN WORKS (free_duel overlay, resident 0x80168000..0x801690A8;
 * decomp findings F79-F86)
 *   init 0x8016824C(a0 = portrait buffer = *(u32*)0x80010000 = 0x80100000):
 *     - flag 0x80 in 0x8009B365 -> bump duelistRecords[row*5+col] (post duel)
 *     - availability[0..39] = 1, then clear ids 1..38 the campaign has not met
 *     - upload 40 portraits (48x48 8bpp + 64-entry CLUT, 2432 bytes each)
 *       into VRAM page 18 (25 tiles) and page 20 (15 tiles, 3 rows of 5);
 *       CLUTs go to y=496.. at x=128/192/256
 *     - spawn one sprite per available cell, u/v from the cell index
 *   tick 0x80168C7C: DPAD moves the pending cell, rows clamped 0..7; confirm
 *     starts a duel with opponent id row*5+col; PlaceCursor 0x80168090 shows
 *     the cell's name (string 0x8328+id) and record.
 *
 * WHAT CHANGES
 *   - availability table: the stock 40 bytes at 0x80169030 butt into the
 *     thumb-widget pointer, so the six lui/addiu sites are repointed to a
 *     45-byte table in the reclaimed 0x801CFE00 region (same zero region
 *     psx_card_extend / psx_card_chest use; below the chest stub at 0x801CFFC0).
 *   - loop counts: availability init 39->44, upload 40->45, sprites 15->20.
 *   - upload source: tiles >= 40 do not exist in the buffer (what follows it
 *     is other loaded data), so the two upload calls go through stubs that
 *     substitute Simon's tile for those indices. Page 20 rows 3-4 and CLUT
 *     slots 40..47 are free in VRAM, so the stock placement math just works.
 *   - DPAD clamp 7->8; scrollbar divisor 364->416 (8 rows of 52px travel).
 *   - PlaceCursor / confirm: cell ids >= 40 map to Simon (id 1) through stubs.
 *   - init prologue: rows >= 8 drop the 0x80 (post-duel) flag before the
 *     record bump so no out-of-table write happens.
 *
 * WHY PER-FRAME RE-ASSERT: the overlay is re-streamed from disc every time
 * the screen is entered (stock bytes again) and savestates restore stock
 * bytes. Same idiom as psx_card_extend.c: write only when the word is stock,
 * and only when the whole module signature is present, so another overlay
 * sharing 0x80168000 is never touched. Patched code runs in the runtime's
 * dirty-RAM path (psx_mod_write_code_word), which is correct, just slower --
 * fine for a menu tick. */

#include <stdint.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"

#define FD_ROWS_STOCK   8u
#define FD_ROWS_EXTRA   1u
#define FD_COLS         5u
#define FD_CELLS_STOCK  (FD_ROWS_STOCK * FD_COLS)                   /* 40 */
#define FD_CELLS        ((FD_ROWS_STOCK + FD_ROWS_EXTRA) * FD_COLS) /* 45 */
#define FD_ROWS         (FD_ROWS_STOCK + FD_ROWS_EXTRA)

#define FD_PLACEHOLDER_ID 1u   /* Simon */

/* Reclaimed region slice (all zero in every sampled state; see
 * psx_card_extend.c). Stay below psx_card_chest.c's stub at 0x801CFFC0. */
#define FD_REGION_LO    0x801CFE00u
#define FD_REGION_HI    0x801CFF00u
#define FD_AVAIL_NEW    0x801CFE00u    /* 64 bytes */
#define FD_TRAMP_IMG    0x801CFE40u
#define FD_TRAMP_CLUT   0x801CFE60u
#define FD_TRAMP_CONFIRM 0x801CFE80u
#define FD_TRAMP_INIT   0x801CFEA0u
#define FD_TRAMP_PLACE  0x801CFED0u
typedef char fd_cells_fit[(FD_CELLS <= 64u) ? 1 : -1];
typedef char fd_region_fit[(FD_TRAMP_PLACE + 7u * 4u <= FD_REGION_HI) ? 1 : -1];
typedef char fd_region_below_chest[(FD_REGION_HI <= 0x801CFFC0u) ? 1 : -1];

/* lui/addiu reach: FD_AVAIL_NEW == 0x801D0000 + (int16)imm */
#define FD_AVAIL_HI     0x801Du
#define FD_AVAIL_IMM    ((uint32_t)(FD_AVAIL_NEW - 0x801D0000u) & 0xFFFFu)

#define PORTRAIT_BUF_PTR 0x80010000u   /* arena 0 base, 0x80100000 */
#define PORTRAIT_STRIDE  2432u
#define PORTRAIT_IMG     2304u

#define JAL(a) (0x0C000000u | (((a) & 0x0FFFFFFFu) >> 2))
#define J(a)   (0x08000000u | (((a) & 0x0FFFFFFFu) >> 2))

typedef struct { uint32_t addr, stock, patched; } FdSite;

/* Words that never change, used as the "module is resident" signature. */
static const FdSite FD_SIGNATURE[] = {
    { 0x80168010u, 0x84C2B148u, 0x84C2B148u },  /* lh v0,-20152(a2)   */
    { 0x801685B8u, 0x3C036666u, 0x3C036666u },  /* lui v1,0x6666      */
    { 0x801685BCu, 0x34636667u, 0x34636667u },  /* ori v1,v1,0x6667   */
    { 0x80168E78u, 0x80E5B367u, 0x80E5B367u },  /* lb a1,-19609(a3)   */
};

static const FdSite FD_SITES[] = {
    /* availability table init: li s0,39 -> 44 */
    { 0x801683A0u, 0x24100027u, 0x24100000u | (FD_CELLS - 1u) },
    /* availability table base, five lui/addiu pairs (the sixth, in
     * PlaceCursor, becomes a stub call below) */
    { 0x801683A4u, 0x3C028017u, 0x3C020000u | FD_AVAIL_HI },
    { 0x801683A8u, 0x24429030u, 0x24420000u | FD_AVAIL_IMM },
    { 0x801683C4u, 0x3C028017u, 0x3C020000u | FD_AVAIL_HI },
    { 0x801683C8u, 0x24519030u, 0x24510000u | FD_AVAIL_IMM },
    { 0x80168588u, 0x3C028017u, 0x3C020000u | FD_AVAIL_HI },
    { 0x8016858Cu, 0x24519030u, 0x24510000u | FD_AVAIL_IMM },
    { 0x801686A0u, 0x3C028017u, 0x3C020000u | FD_AVAIL_HI },
    { 0x801686A4u, 0x24549030u, 0x24540000u | FD_AVAIL_IMM },
    { 0x80168E68u, 0x3C048017u, 0x3C040000u | FD_AVAIL_HI },
    { 0x80168E70u, 0x24849030u, 0x24840000u | FD_AVAIL_IMM },
    /* PlaceCursor: lui/addiu -> jal stub (clamps id, returns table base) */
    { 0x80168118u, 0x3C028017u, JAL(FD_TRAMP_PLACE) },
    { 0x8016811Cu, 0x24429030u, 0x00000000u },
    /* portrait upload loop: slti v0,s2,40 -> 45; both uploads via stubs */
    { 0x80168518u, 0x2A420028u, 0x2A420000u | FD_CELLS },
    { 0x80168500u, 0x0C02077Au, JAL(FD_TRAMP_IMG) },
    { 0x8016850Cu, 0x0C02077Au, JAL(FD_TRAMP_CLUT) },
    /* second sprite loop: slti v0,s0,15 -> 20 */
    { 0x801687BCu, 0x2A02000Fu, 0x2A020000u | (FD_CELLS - 25u) },
    /* DPAD row clamp: slti v0,v0,8 -> 9 ; li v0,7 -> 8 */
    { 0x80168DDCu, 0x28420008u, 0x28420000u | FD_ROWS },
    { 0x80168DE8u, 0x24020007u, 0x24020000u | (FD_ROWS - 1u) },
    /* scrollbar: thumb = 7 + (y-40)*72/364 -> /416 (magic for /416) */
    { 0x80168038u, 0x3C04B40Bu, 0x3C049D89u },
    { 0x80168050u, 0x348440B5u, 0x3484D89Eu },
    /* confirm: jal init_duel(-1, id, ..) -> stub clamps id */
    { 0x80168F84u, 0x0C009372u, JAL(FD_TRAMP_CONFIRM) },
    /* init prologue -> stub (drops the post-duel flag on placeholder rows) */
    { 0x8016824Cu, 0x3C02800Au, J(FD_TRAMP_INIT) },
    { 0x80168250u, 0x9042B365u, 0x00000000u },
};
typedef char fd_rows_one[(FD_ROWS_EXTRA == 1u) ? 1 : -1]; /* stub divisor+clamps assume 9 rows */

#define COUNT_OF(a) (uint32_t)(sizeof(a) / sizeof((a)[0]))

/* Stubs. `at` is free at every call site; ra is the loop's own return
 * (the stubs tail-jump). */
static uint32_t s_simon_tile;   /* buffer + 1*2432, read at assert time */

static void write_stub(uint32_t base, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (psx_mod_read_word(base + i * 4u) != w[i])
            psx_mod_write_code_word(base + i * 4u, w[i]);
}

static void assert_stubs(void)
{
    const uint32_t img  = s_simon_tile;
    const uint32_t clut = s_simon_tile + PORTRAIT_IMG;
    const uint32_t t_img[7] = {
        0x2A410028u,                        /* slti at,s2,40          */
        0x14200003u,                        /* bnez at,+3 (to the j)  */
        0x00000000u,
        0x3C050000u | (img >> 16),          /* lui a1,hi              */
        0x34A50000u | (img & 0xFFFFu),      /* ori a1,a1,lo           */
        J(0x80081DE8u),                     /* j upload               */
        0x00000000u,
    };
    const uint32_t t_clut[7] = {
        0x2A410028u, 0x14200003u, 0x00000000u,
        0x3C050000u | (clut >> 16),
        0x34A50000u | (clut & 0xFFFFu),
        J(0x80081DE8u), 0x00000000u,
    };
    const uint32_t t_confirm[6] = {
        0x28A10028u,                        /* slti at,a1,40          */
        0x14200002u,                        /* bnez at,+2             */
        0x00000000u,
        0x24050000u | FD_PLACEHOLDER_ID,    /* li a1,Simon            */
        J(0x80024DC8u),                     /* j init_duel            */
        0x00000000u,
    };
    const uint32_t t_init[10] = {
        0x3C02800Au,                        /* lui v0,0x800a  (displaced) */
        0x9042B365u,                        /* lbu v0,flags   (displaced) */
        0x3C01800Au,                        /* lui at,0x800a          */
        0x8021B367u,                        /* lb at,cursor row       */
        0x28210008u,                        /* slti at,at,8           */
        0x14200002u,                        /* bnez at,+2             */
        0x00000000u,
        0x3042007Fu,                        /* andi v0,v0,0x7f        */
        J(0x80168254u),                     /* j init body            */
        0x00000000u,
    };
    const uint32_t t_place[7] = {
        0x28810028u,                        /* slti at,a0,40          */
        0x14200002u,                        /* bnez at,+2             */
        0x00000000u,
        0x24040000u | FD_PLACEHOLDER_ID,    /* li a0,Simon            */
        0x3C020000u | FD_AVAIL_HI,          /* lui v0                 */
        0x03E00008u,                        /* jr ra                  */
        0x24420000u | FD_AVAIL_IMM,         /* addiu v0,v0,imm        */
    };
    write_stub(FD_TRAMP_IMG,     t_img,     7u);
    write_stub(FD_TRAMP_CLUT,    t_clut,    7u);
    write_stub(FD_TRAMP_CONFIRM, t_confirm, 6u);
    write_stub(FD_TRAMP_INIT,    t_init,    10u);
    write_stub(FD_TRAMP_PLACE,   t_place,   7u);
}

/* 1 when every word is stock or already ours (module resident). */
static int module_resident(void)
{
    for (uint32_t i = 0; i < COUNT_OF(FD_SIGNATURE); i++)
        if (psx_mod_read_word(FD_SIGNATURE[i].addr) != FD_SIGNATURE[i].stock)
            return 0;
    for (uint32_t i = 0; i < COUNT_OF(FD_SITES); i++) {
        const uint32_t w = psx_mod_read_word(FD_SITES[i].addr);
        if (w != FD_SITES[i].stock && w != FD_SITES[i].patched) return 0;
    }
    return 1;
}

static int s_region_ok = -1;   /* -1 unchecked, 0 someone else owns it, 1 ours */

static void fd_rows_tick(void)
{
    if (!psx_mod_game_started()) return;
    if (s_region_ok < 0) {
        s_region_ok = 1;
        for (uint32_t a = FD_REGION_LO; a < FD_REGION_HI; a += 4u)
            if (psx_mod_read_word(a) != 0u) { s_region_ok = 0; break; }
    }
    if (!s_region_ok) return;
    if (!module_resident()) return;

    const uint32_t buf = psx_mod_read_word(PORTRAIT_BUF_PTR);
    if ((buf & 0xFF000000u) != 0x80000000u) return;
    s_simon_tile = buf + FD_PLACEHOLDER_ID * PORTRAIT_STRIDE;

    assert_stubs();
    for (uint32_t i = 0; i < COUNT_OF(FD_SITES); i++)
        if (psx_mod_read_word(FD_SITES[i].addr) == FD_SITES[i].stock)
            psx_mod_write_code_word(FD_SITES[i].addr, FD_SITES[i].patched);
}

PSX_MOD_CONSTRUCTOR(psx_free_duel_rows_install) {
    (void)psx_game_add_vblank_hook(fd_rows_tick);
    (void)psx_game_add_frame_hook(fd_rows_tick);
}
