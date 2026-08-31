/* Chest-screen prim-spray containment.
 *
 * THE CRASH THIS CONTAINS (measured 2026-08-30, bugs/crash-2026-08-30-
 * deckedit/): with the card extension on and every card granted, adding the
 * bottom chest row's card to the deck makes the row-text walk inside
 * func_80035E20 run away within one frame. The walk emits one textured
 * sprite per record through the libgs emitter at 0x800849F0, whose write
 * cursor is the global at 0x800FE240; a runaway march tramples the record
 * pool it is feeding from (self-sustaining), the five text-widget structs,
 * the cursor global itself (teleporting the march), and eventually the
 * kernel TCB at 0x80009110 -- at which point the next exception return
 * `jr $k0` jumps into a packet's UV/CLUT word (values like 0xB65827AC:
 * u=0xAC v=0x27 clut=0xB6xx) and the runtime fail-fasts on the unknown
 * dispatch. Reproduced 3/3 without instrumentation; any armed wtrace slows
 * stores enough to close the window (0/9), so the first domino is timing-
 * dependent (vsync landing inside the edit-frame rebuild) and is contained
 * rather than chased.
 *
 * THE CONTAINMENT. The emitter's first instruction is replaced with a jump
 * to a stub that bounds the cursor: if [0x800FE240] is outside the chest
 * screen's legitimate prim area [0x80090000, 0x800F8000) it is re-parked at
 * a sacrificial pit inside the extension's reclaimed name region before the
 * packet is written. The check runs on EVERY emitted prim, so a wild cursor
 * can never advance more than one packet past the pit: the kernel, the
 * record pool and the widget structs become unreachable and the worst case
 * is one visually glitched frame. The walk itself then terminates because
 * the pool it feeds from is no longer being overwritten.
 *
 * Applied only while the chest screen owns the frame (mode byte 0xC7) --
 * other screens may park their prim buffers elsewhere legitimately, and the
 * crash is chest-specific. Same per-frame assert idiom as the viewer clamp
 * in psx_card_chest.c: the entry word is patched when it reads stock and
 * restored when the screen is left.
 *
 * Register safety at the emitter entry: the entry `j` preserves $ra (the
 * stub must not clobber it either -- callers are identified by it in the
 * crash reports); the displaced `move $t3, $a0` runs at the stub tail; $at
 * and $v0 are scratch at that point ($v0's first use inside the emitter is
 * a write at 0x80084A08), and the entry jump's own delay slot is the stock
 * `move $t5, $a1` at 0x800849F4, untouched.
 *
 * The stub executes from a data page, which is exactly how the viewer clamp
 * stub already works: the region is dirty, so every call re-reads guest RAM
 * and the patch is honoured per call (the A2 stale-loop-immediate trap does
 * not apply to a function ENTRY -- calls re-dispatch; only loop back-edges
 * go stale). */

#include "psx_card_guard.h"
#include "psx_card_extend.h"
#include "psx_card_save.h"

#include <stdint.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"

#define EMITTER_ENTRY   0x800849F0u
#define EMITTER_STOCK   0x00805821u   /* move t3, a0 */
#define CURSOR_GLOBAL   0x800FE240u   /* what the emitter advances          */

/* Legitimate chest-screen prim area, measured. */
#define LEGIT_LO        0x800A0000u
#define LEGIT_SPAN      0x00038000u   /* LEGIT_HI = 0x800D8000. Healthy chest
 * cursors sampled 2026-08-30 late: 0x800A5768..0x800CB658 across browse,
 * scroll, pane switches, edits and a re-sort. The record pool (0x800E9xxx+),
 * widget structs (0x800EFE48+) and the kernel are all OUTSIDE, so a march
 * is parked the moment it leaves the true prim banks -- the old 0x800F8000
 * ceiling let it chew the pool first (measured, that was the second-freeze
 * flavour). If the triangle card viewer ever legitimately parks its cursor
 * past 0x800D8000 the pit OSD will say so -- widen then, with numbers. */

/* Stub + pit live in the free tail of the name region psx_card_extend.c
 * documents as zero in every sampled state (0x801D916F..0x801DA000). The
 * extension's own strings end just past 0x801D9600; generous margin. */
#define PIT_ADDR        0x801D9C00u   /* 0x200 bytes of sacrificial space   */
#define STUB_ADDR       0x801D9E00u

#define J_TO(addr)      (0x08000000u | (((addr) & 0x0FFFFFFFu) >> 2))

/* Per-frame emission counter and its cap, guest-visible cells our tick owns.
 * The 2026-08-30 evening incidents showed the runaway can march INSIDE the
 * address window for a long way (it starts in the legit banks and tramples
 * the record pool at 0x800E9xxx.. and the widget structs long before the
 * kernel), so the address check alone fires too late. The VOLUME check is
 * the real tripwire: a healthy chest frame emits a bounded number of prims,
 * the runaway emits thousands in its single never-ending frame -- and since
 * the frame hook that resets the counter only runs between frames, the
 * runaway cannot reset its own budget. */
#define CNT_ADDR        0x801D9BF8u
#define CAP_ADDR        0x801D9BFCu

static const uint32_t GUARD_STUB[] = {
    0x3C018010u,             /* w0  lui   at, 0x8010                        */
    0x8C22E240u,             /* w1  lw    v0, -0x1DC0(at)  ; cursor         */
    0x3C0B800Au,             /* w2  lui   t3, hi(LEGIT_LO)                  */
    0x004B1023u,             /* w3  subu  v0, v0, t3                        */
    0x3C0B0003u,             /* w4  lui   t3, 0x0003                        */
    0x356B8000u,             /* w5  ori   t3, t3, 0x8000   ; t3 = SPAN      */
    0x004B102Bu,             /* w6  sltu  v0, v0, t3       ; in window?     */
    0x1040000Bu,             /* w7  beqz  v0, PARK (w19)                    */
    0x00000000u,             /* w8   nop                                    */
    0x3C0B801Eu,             /* w9  lui   t3, 0x801E                        */
    0x8D629BF8u,             /* w10 lw    v0, -0x6408(t3)  ; counter        */
    0x00000000u,             /* w11  nop (load delay)                       */
    0x24420001u,             /* w12 addiu v0, v0, 1                         */
    0xAD629BF8u,             /* w13 sw    v0, -0x6408(t3)                   */
    0x8D6B9BFCu,             /* w14 lw    t3, -0x6404(t3)  ; cap            */
    0x00000000u,             /* w15  nop (load delay)                       */
    0x004B102Bu,             /* w16 sltu  v0, v0, t3       ; count < cap?   */
    0x14400004u,             /* w17 bnez  v0, OK (w22)                      */
    0x00000000u,             /* w18  nop                                    */
    0x3C02801Eu,             /* w19 PARK: lui v0, 0x801E                    */
    0x24429C00u,             /* w20 addiu v0, v0, -0x6400  ; v0 = PIT       */
    0xAC22E240u,             /* w21 sw    v0, -0x1DC0(at)  ; park cursor    */
    0x00805821u,             /* w22 OK: move t3, a0        ; displaced insn */
    J_TO(EMITTER_ENTRY + 8u),/* w23 j     0x800849F8                        */
    0x00000000u,             /* w24  nop                                    */
};
#define STUB_WORDS (sizeof(GUARD_STUB) / sizeof(GUARD_STUB[0]))

/* Stay inside the documented-free region and clear of the extension's own
 * ceiling at NAMES_LIMIT (0x801DA000). */
typedef char guard_stub_fits[
    (STUB_ADDR + STUB_WORDS * 4u <= 0x801DA000u) ? 1 : -1];
typedef char guard_pit_fits[(PIT_ADDR + 0x200u <= STUB_ADDR) ? 1 : -1];

#define MODE_BYTE   0x8009B26Cu
#define MODE_CHEST  0xC7u

static int      s_armed;
static int      s_reported;
static uint32_t s_frame_max;     /* rolling max prims/frame while armed     */
static uint32_t s_calib_frames;  /* frames observed before the cap is set   */

/* The cap self-calibrates: watch real chest frames for a while, then allow
 * a wide multiple of the observed peak. The runaway emits thousands in one
 * frame, so even x8 headroom trips it almost immediately -- and because the
 * counter only resets HERE, between frames, the runaway's own frame can
 * never refresh its budget. */
#define CALIB_FRAMES  120u
#define CAP_FLOOR     4096u
#define CAP_OPEN      0x7FFFFFFFu

static void card_guard_tick(void)
{
    if (!psx_card_save_ext_enabled() || !psx_mod_game_started()) return;

    const int in_chest = (psx_mod_read_byte(MODE_BYTE) == MODE_CHEST);

    if (in_chest) {
        for (uint32_t i = 0; i < STUB_WORDS; i++)
            if (psx_mod_read_word(STUB_ADDR + i * 4u) != GUARD_STUB[i])
                psx_mod_write_code_word(STUB_ADDR + i * 4u, GUARD_STUB[i]);
        if (!s_armed) {
            psx_mod_write_word(CNT_ADDR, 0u);
            psx_mod_write_word(CAP_ADDR, CAP_OPEN);
            s_frame_max = 0;
            s_calib_frames = 0;
        }
        if (psx_mod_read_word(EMITTER_ENTRY) == EMITTER_STOCK)
            psx_mod_write_code_word(EMITTER_ENTRY, J_TO(STUB_ADDR));
        s_armed = 1;

        /* Per-frame budget: read what last frame emitted, track the peak,
         * and once enough healthy frames are seen pin the cap. */
        const uint32_t n = psx_mod_read_word(CNT_ADDR);
        psx_mod_write_word(CNT_ADDR, 0u);
        if (n > s_frame_max) s_frame_max = n;
        if (s_calib_frames < CALIB_FRAMES) {
            s_calib_frames++;
            if (s_calib_frames == CALIB_FRAMES) {
                uint32_t cap = s_frame_max * 8u;
                if (cap < CAP_FLOOR) cap = CAP_FLOOR;
                psx_mod_write_word(CAP_ADDR, cap);
            }
        }
    } else if (s_armed) {
        if (psx_mod_read_word(EMITTER_ENTRY) == J_TO(STUB_ADDR))
            psx_mod_write_code_word(EMITTER_ENTRY, EMITTER_STOCK);
        s_armed = 0;
    }

    /* Telemetry: a packet landing in the pit means a runaway was parked.
     * Say so once per incident so the reports name the guard, then re-arm. */
    if (psx_mod_read_word(PIT_ADDR) != 0u) {
        if (!s_reported) {
            host_osd_push("Prim guard: runaway draw parked", 2500);
            s_reported = 1;
        }
        psx_mod_write_word(PIT_ADDR, 0u);
    } else {
        s_reported = 0;
    }
}

void psx_card_guard_init(void)
{
    (void)psx_game_add_frame_hook(card_guard_tick);
}
