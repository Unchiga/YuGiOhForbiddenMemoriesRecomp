/* psx_freeze_report.c - catch the mid-duel freeze and leave evidence behind.
 *
 * The bug: the duel stops advancing while the music keeps playing and the
 * frame rate stays pinned at full speed. It is not a hang. The guest is inside
 * func_800137E4, a wait loop whose body is a RENDER call, spinning until
 *
 *     (word[0x8009B0F4] & 0x02000030) | word[0x8009B134]  ==  0
 *
 * Bit 4 of that mask is claimed when an effect is submitted and released by a
 * callback that, in the frozen case, never fires -- so the loop draws the same
 * frame forever. See FREEZE_INVESTIGATION.md for the full chain.
 *
 * Why a detector and not a fix: the maintainer cannot reproduce it. Two
 * reporters hit it readily. Everything known about the bug came from two
 * savestates they happened to capture by pressing F10 while stuck. This makes
 * that automatic, so the next occurrence produces a report and a loadable
 * state instead of "it froze again".
 *
 * Why the predicate and not a stasis timer: the freeze has an exact signature.
 * Testing the waiter's own exit condition cannot be wrong about whether the
 * game is stuck, where "nothing changed for N seconds" has to guess. The same
 * mask is used by ordinary loads, which is why the trigger is a long hold
 * rather than a single sample -- FM's longest measured load stalls ~759 ms,
 * and the in-duel threshold here is more than ten times that.
 *
 * Why an overlay and not host_osd_push: this title builds with
 * PSX_RECOMP_UI=OFF, which compiles the host OSD out entirely. host_osd_push
 * is a silent no-op here. A guest-space overlay draws either way.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_guest_overlay.h"
#include "savestate.h"
#include "debug_server.h"

#include "psx_ui_font8.inc"    /* FONT8[95][8], ASCII 32..126 */

/* ---- what the freeze looks like ---------------------------------------- */

#define FR_BUSY          0x8009B0F4u  /* engine-wide busy/pending bitmask    */
#define FR_BUSY_WAIT     0x02000030u  /* the bits the duel's waiter tests    */
#define FR_BUSY_BIT_WAIT 0x00000010u  /* claimed per effect; stuck when set  */
#define FR_BUSY_BIT_GATE 0x00000400u  /* blocks the release chain's tick     */
#define FR_BUSY_BIT_PUMP 0x00080000u  /* arms the retry; clear when frozen   */
#define FR_BUSY2         0x8009B134u  /* second operand of the same wait     */

#define FR_EFFECT_IDX    0x8009B100u  /* effect-module state index (0..6)    */
#define FR_EFFECT_FLAGS  0x8009B112u
#define FR_PHASE         0x800EADA0u  /* signed; -1 occurs when HEALTHY too  */
#define FR_MODE          0x8009B26Cu  /* &0x1F = mode index; 0xC3 = in duel  */
#define FR_MODE_IN_DUEL  0xC3u
#define FR_SUBSTATE      0x8009B26Eu
#define FR_TURN          0x8009B1D5u  /* 0 player, 1 opponent                */
#define FR_SELCOUNT      0x800E9F25u
#define FR_RECORDS       0x801A7AE4u  /* card records, 28-byte stride        */
#define FR_REC_STRIDE    28u
#define FR_REC_COUNT     24

/* Guest vblanks the predicate must hold before firing. These are GUEST time,
 * not wall time: the hook runs on the guest's cadence, so at game speed 3 the
 * wall-clock wait is a third of the nominal figure. 60 vblanks ~ 1 second. */
#define FR_HOLD_IN_DUEL  600   /* ~10 s guest,  ~3.3 s wall at speed 3 */
#define FR_HOLD_ELSE    1200   /* ~20 s guest; loads live out here     */

/* ---- rolling history --------------------------------------------------- */

typedef struct {
    uint64_t frame;
    uint32_t busy, busy2;
    uint16_t idx, flags;
    int16_t  phase;
    uint8_t  mode, sub, turn, sel;
} FrSample;

#define FR_RING 24
static FrSample s_ring[FR_RING];
static int      s_ring_head, s_ring_count;
static FrSample s_last;
static int      s_have_last;

static void sample_now(FrSample *s)
{
    s->frame = debug_server_frame_number();
    s->busy  = psx_mod_read_word(FR_BUSY);
    s->busy2 = psx_mod_read_word(FR_BUSY2);
    s->idx   = psx_mod_read_half(FR_EFFECT_IDX);
    s->flags = psx_mod_read_half(FR_EFFECT_FLAGS);
    s->phase = (int16_t)psx_mod_read_half(FR_PHASE);
    s->mode  = psx_mod_read_byte(FR_MODE);
    s->sub   = psx_mod_read_byte(FR_SUBSTATE);
    s->turn  = psx_mod_read_byte(FR_TURN);
    s->sel   = psx_mod_read_byte(FR_SELCOUNT);
}

/* Everything except the frame number: the ring records CHANGES, and a frame
 * counter that ticks every vblank would make every sample look like one. */
static int sample_differs(const FrSample *a, const FrSample *b)
{
    return a->busy != b->busy || a->busy2 != b->busy2 || a->idx != b->idx ||
           a->flags != b->flags || a->phase != b->phase || a->mode != b->mode ||
           a->sub != b->sub || a->turn != b->turn || a->sel != b->sel;
}

static void ring_push(const FrSample *s)
{
    s_ring[s_ring_head] = *s;
    s_ring_head = (s_ring_head + 1) % FR_RING;
    if (s_ring_count < FR_RING) s_ring_count++;
}

/* ---- banner ------------------------------------------------------------ */

#define FR_GLYPH   8
#define FR_LINES   3
#define FR_COLS   38            /* 38 * 8 = 304 px, inside a 320-wide guest */
#define FR_PAD     4
#define FR_BAN_W  (FR_COLS * FR_GLYPH + FR_PAD * 2)
#define FR_BAN_H  (FR_LINES * (FR_GLYPH + 2) + FR_PAD * 2)

#define FR_C_BG    0xE0101018u
#define FR_C_EDGE  0xFFFFC020u
#define FR_C_TEXT  0xFFFFFFFFu

static char     s_line[FR_LINES][FR_COLS + 1];
static uint32_t s_px[FR_BAN_W * FR_BAN_H];
static int      s_visible, s_dirty, s_px_valid;

static void put_text(int row, const char *t)
{
    const int y0 = FR_PAD + row * (FR_GLYPH + 2);
    for (int i = 0; t[i] && i < FR_COLS; i++) {
        const unsigned char c = (unsigned char)t[i];
        if (c < 32 || c > 126) continue;
        const unsigned char *g = FONT8[c - 32];
        const int x0 = FR_PAD + i * FR_GLYPH;
        /* FONT8 rows are LSB-first: bit 0 is the LEFTMOST pixel. Reading them
         * MSB-first draws every glyph mirrored, which is legible enough in a
         * hex dump to pass review and obvious on screen. Same order as the
         * drop viewer's blitter, the other consumer of this sheet. */
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx))
                    s_px[(y0 + gy) * FR_BAN_W + (x0 + gx)] = FR_C_TEXT;
    }
}

static void render_banner(void)
{
    for (int i = 0; i < FR_BAN_W * FR_BAN_H; i++) s_px[i] = FR_C_BG;
    for (int x = 0; x < FR_BAN_W; x++) {
        s_px[x] = FR_C_EDGE;
        s_px[(FR_BAN_H - 1) * FR_BAN_W + x] = FR_C_EDGE;
    }
    for (int y = 0; y < FR_BAN_H; y++) {
        s_px[y * FR_BAN_W] = FR_C_EDGE;
        s_px[y * FR_BAN_W + FR_BAN_W - 1] = FR_C_EDGE;
    }
    for (int r = 0; r < FR_LINES; r++) put_text(r, s_line[r]);
    s_px_valid = 1;
}

static int banner_image(const uint32_t **px, int *w, int *h)
{
    if (!s_visible) return 0;
    if (!s_px_valid) render_banner();
    *px = s_px; *w = FR_BAN_W; *h = FR_BAN_H;
    return 1;
}

static void banner_origin(int *x, int *y) { *x = 4; *y = 4; }

static int banner_needs_present(void)
{
    const int d = s_dirty;
    s_dirty = 0;
    return d;
}

static void banner_set(const char *l0, const char *l1, const char *l2)
{
    snprintf(s_line[0], sizeof s_line[0], "%s", l0 ? l0 : "");
    snprintf(s_line[1], sizeof s_line[1], "%s", l1 ? l1 : "");
    snprintf(s_line[2], sizeof s_line[2], "%s", l2 ? l2 : "");
    s_px_valid = 0;
    s_dirty    = 1;
}

/* ---- the report file --------------------------------------------------- */

static unsigned s_fire_count;

static void write_report(int slot, const char *slot_path, const FrSample *now)
{
    const char *dir = psx_mod_player_data_dir();
    char path[1024];
    FILE *f;
    uint32_t bios_cksum = 0, entry_pc = 0;
    const char *tok;

    if (!dir || !dir[0]) return;
    snprintf(path, sizeof path, "%s/freeze_report.txt", dir);
    f = fopen(path, "w");
    if (!f) return;

    savestate_get_integrity(&bios_cksum, &entry_pc);
    tok = savestate_bios_token();

    fprintf(f, "Yu-Gi-Oh Forbidden Memories Recompiled - freeze report\n");
    fprintf(f, "======================================================\n\n");
    fprintf(f, "Written automatically because the game stopped advancing while\n"
               "still running. Please send this file to the developer, together\n"
               "with the save state named at the bottom if there is one.\n\n");

    fprintf(f, "freeze #        : %u this session\n", s_fire_count);
    fprintf(f, "guest frame     : %llu\n", (unsigned long long)now->frame);
    fprintf(f, "held for        : %d guest vblanks\n\n",
            now->mode == FR_MODE_IN_DUEL ? FR_HOLD_IN_DUEL : FR_HOLD_ELSE);

    fprintf(f, "-- the blocker --------------------------------------------\n");
    fprintf(f, "word[0x8009B0F4] : %08X   wait=%d gate=%d pump=%d\n",
            now->busy,
            (now->busy & FR_BUSY_BIT_WAIT) ? 1 : 0,
            (now->busy & FR_BUSY_BIT_GATE) ? 1 : 0,
            (now->busy & FR_BUSY_BIT_PUMP) ? 1 : 0);
    fprintf(f, "word[0x8009B134] : %08X\n", now->busy2);
    fprintf(f, "half[0x8009B100] : %04X   (effect state index)\n", now->idx);
    fprintf(f, "half[0x8009B112] : %04X   (effect flags)\n\n", now->flags);

    fprintf(f, "-- duel ---------------------------------------------------\n");
    fprintf(f, "phase [0x800EADA0]: %d   (-1 is NORMAL, not a fault)\n", now->phase);
    fprintf(f, "mode  [0x8009B26C]: %02X  (C3 = in a duel)\n", now->mode);
    fprintf(f, "sub   [0x8009B26E]: %02X  (81 = driving the duel)\n", now->sub);
    fprintf(f, "turn  [0x8009B1D5]: %u   (0 player, 1 opponent)\n", now->turn);
    fprintf(f, "sel   [0x800E9F25]: %u   (cards selected)\n\n", now->sel);

    fprintf(f, "-- card records (0x801A7AE4, 28-byte stride) --------------\n");
    for (int i = 0; i < FR_REC_COUNT; i++) {
        const uint32_t a = FR_RECORDS + (uint32_t)i * FR_REC_STRIDE;
        uint32_t b;
        fprintf(f, "[%02d] id=%-4u atk=%-5u def=%-5u flags=%04X  ",
                i, psx_mod_read_half(a), psx_mod_read_half(a + 2),
                psx_mod_read_half(a + 4), psx_mod_read_half(a + 10));
        for (b = 0; b < FR_REC_STRIDE; b++)
            fprintf(f, "%02X", psx_mod_read_byte(a + b));
        fprintf(f, "\n");
    }

    fprintf(f, "\n-- last state changes before the freeze (oldest first) ----\n");
    fprintf(f, "%-10s %-8s %-8s %-4s %-4s %-5s %-4s %-3s %-4s %s\n",
            "frame", "busy", "busy2", "idx", "flg", "phase", "mode", "sub",
            "turn", "sel");
    for (int i = 0; i < s_ring_count; i++) {
        const FrSample *s =
            &s_ring[(s_ring_head + FR_RING - s_ring_count + i) % FR_RING];
        fprintf(f, "%-10llu %08X %08X %04X %04X %-5d %02X   %02X  %-4u %u\n",
                (unsigned long long)s->frame, s->busy, s->busy2, s->idx,
                s->flags, s->phase, s->mode, s->sub, s->turn, s->sel);
    }

    fprintf(f, "\n-- this build ---------------------------------------------\n");
    /* The save state is per-BIOS and is REFUSED by a build running a different
     * one, silently apart from one on-screen line. Whoever receives the state
     * needs this to know what can load it. */
    fprintf(f, "bios            : %s\n", tok ? tok : "(unknown)");
    fprintf(f, "bios checksum   : %08X\n", bios_cksum);
    fprintf(f, "entry pc        : %08X\n", entry_pc);
    if (slot >= 0)
        fprintf(f, "save state      : slot %d -> %s\n", slot,
                slot_path && slot_path[0] ? slot_path : "(path unavailable)");
    else
        fprintf(f, "save state      : NOT WRITTEN - all %d slots were in use.\n"
                   "                  Press F10 and save to a slot by hand;\n"
                   "                  the menu still works while frozen.\n",
                SAVESTATE_SLOTS);
    fprintf(f, "\nSettings are in settings.toml beside this file.\n");
    fclose(f);
}

/* ---- detection --------------------------------------------------------- */

static int s_hold;          /* consecutive vblanks the predicate has held */
static int s_fired;         /* one report per episode                     */
static int s_save_slot = -1;
static int s_save_watch;    /* waiting to see whether the save landed     */

/* Highest free slot, never an occupied one. A player's own saves are theirs;
 * a diagnostic that eats one to report a bug has done more harm than the bug.
 * Highest rather than lowest because quick-save habits fill from 0 upward, so
 * the top of the range is the least likely to be wanted next. All twelve in
 * use is a real case -- the maintainer's own install is like that -- and it is
 * handled by asking for a manual F10 save instead, which still works while
 * the guest is stuck because the menu is host-side. */
static int highest_free_slot(void)
{
    int i;
    for (i = SAVESTATE_SLOTS - 1; i >= 0; i--)
        if (!savestate_slot_exists(i)) return i;
    return -1;
}

static void fire(const FrSample *now)
{
    char slot_path[1024];
    slot_path[0] = '\0';

    s_fire_count++;
    s_save_slot = highest_free_slot();
    if (s_save_slot >= 0) {
        if (!savestate_slot_path(s_save_slot, slot_path, sizeof slot_path))
            slot_path[0] = '\0';
        if (savestate_request_save(s_save_slot))
            s_save_watch = 1;
        else
            s_save_slot = -1;
    }

    write_report(s_save_slot, slot_path, now);

    if (s_save_slot >= 0) {
        char l1[FR_COLS + 1];
        snprintf(l1, sizeof l1, "SAVED: freeze_report.txt + STATE %d",
                 s_save_slot);
        banner_set("GAME FROZEN - THIS IS A KNOWN BUG", l1,
                   "PLEASE SEND BOTH TO THE DEVELOPER");
    } else {
        banner_set("GAME FROZEN - THIS IS A KNOWN BUG",
                   "SAVED: freeze_report.txt (NO SLOT)",
                   "PRESS F10 > SAVE STATE, SEND BOTH");
    }
    s_visible = 1;
    s_dirty   = 1;
}

static void freeze_tick(void)
{
    FrSample now;
    int stuck, limit;

    if (!psx_mod_game_started()) return;

    sample_now(&now);

    if (!s_have_last || sample_differs(&now, &s_last)) {
        ring_push(&now);
        s_last = now;
        s_have_last = 1;
    }

    stuck = ((now.busy & FR_BUSY_WAIT) | now.busy2) != 0u;
    if (!stuck) {
        /* Out of the wait: re-arm for a later episode and drop the banner. */
        s_hold = 0;
        s_fired = 0;
        s_save_watch = 0;
        if (s_visible) { s_visible = 0; s_dirty = 1; }
        return;
    }

    /* A save requested from inside the freeze runs at the next safe point.
     * Correct the banner rather than leave it claiming a file that is not
     * there -- a report that lies is worse than no report. */
    if (s_save_watch && !savestate_pending()) {
        s_save_watch = 0;
        if (savestate_take_save_failed())
            banner_set("GAME FROZEN - THIS IS A KNOWN BUG",
                       "SAVED: freeze_report.txt (FAILED)",
                       "PRESS F10 > SAVE STATE, SEND BOTH");
    }

    if (s_fired) return;

    limit = (now.mode == FR_MODE_IN_DUEL) ? FR_HOLD_IN_DUEL : FR_HOLD_ELSE;
    if (++s_hold >= limit) {
        s_fired = 1;
        fire(&now);
    }
}

/* ---- install ----------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_freeze_report_install)
{
    static const PsxGuestOverlay ov = {
        banner_image,
        banner_origin,
        NULL,
        banner_needs_present,
        -1,
        NULL,
    };
    (void)psx_guest_overlay_register(&ov);
    (void)psx_game_add_vblank_hook(freeze_tick);
}
