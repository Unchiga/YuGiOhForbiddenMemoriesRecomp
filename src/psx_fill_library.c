/* psx_fill_library.c — MODS > LIBRARY PLACEHOLDERS. See psx_fill_library.h.
 *
 * WHERE "SEEN" LIVES
 * ------------------
 * The save carries a 2048-bit flag array at 0x801D0618 (live save struct
 * 0x801D0200 + 0x418), MSB first within each byte, tested by
 * Campaign_TestStoryFlag (0x8002CCA8) and written by
 * Library_UpdateCardUsedFlag (0x8002CCE4). Card N's "seen" bit is flag
 * 0x120 + N, so cards own 0x121..0x3F2 and nothing else does: 0x20..0x6F are
 * the story's own flags, 0x401..0x6D2 are "password already used", and
 * 0x6E1..0x706 are the Free Duel unlocks. Touching the card range therefore
 * cannot move a story flag, unlock a duelist or free a password.
 *
 * THE SCREEN'S OWN LOOP
 * ---------------------
 * func_8002BFCC builds the LIBRARY once, when it opens. Cards 1..722, and
 * for each one:
 *
 *     8002C318  jal  Campaign_TestStoryFlag   ; a0 = card + 0x120  (seen?)
 *     8002C320  beq  v0, zero, 8002C358       ; not seen -> next card
 *     8002C328  lw   v0, 0x5608(s1)           ; the completion figure
 *     8002C330  addiu v0, v0, 1               ;   ... counts SEEN cards
 *     8002C338  jal  func_8002C518            ; a0 = card: own it, or in deck?
 *     8002C33C  sb   s2, 0x56(s0)             ; 0x80 = draw this entry
 *     8002C340  bgez v0, 8002C358             ; not owned -> leave it dim
 *     8002C350  ori  v0, v0, 1                ; owned -> full brightness
 *
 * So one branch decides everything: skip it and the card is a blank entry,
 * take it and the card is drawn, dim or bright according to the trunk. This
 * row retargets that one branch from "next card" (+13) to the draw at
 * 0x8002C338 (+5), which leaves the counter increment as the only thing an
 * unseen card skips. Every card is drawn; the figure still counts only the
 * cards whose flag is set.
 *
 * Which is what makes the figure worth reading: the row then clears the seen
 * bit of every card the player does NOT own — psx_mod_read_byte of the trunk
 * plus the 40-card deck, exactly the pair func_8002C518 asks about — so the
 * count that comes out of that loop is the player's real collection, while
 * the grid shows all 722. The bits are put back on the way out.
 *
 * It grants nothing: the trunk is never written, so a filled-in card is drawn
 * the way the game draws a card you have seen but do not own.
 *
 * WHY IT ONLY RUNS ON THE LIBRARY SCREEN
 * --------------------------------------
 * SaveData_RequestWrite (0x8003F87C) copies the live struct into the memory
 * card transfer buffer at 0x801D3200 and queues the write. Anything sitting
 * in the live struct at that moment goes to the card. There is no save
 * command on the LIBRARY screen — SAVE lives on the loaded-save menu and in
 * the campaign's card shop — so confining the edits to that screen is what
 * makes them unable to reach the player's file at all, rather than a promise
 * to take them out again in time. Mode byte 0x8009B26C reads 0xC4 there
 * (0xC8 loaded-save menu, 0xC6 Free Duel grid, 0xC3 deck view and duel), and
 * the work lands early enough on the frame the byte flips: measured live
 * 2026-09-06, the screen came up filled from a save with 14 cards seen.
 *
 * The row remembers exactly which bits it cleared and puts those back, so a
 * save whose library was half full comes back half full — bit for bit, not
 * "recomputed". The patched instruction is restored the same way.
 */

#include "psx_fill_library.h"

#include <stdio.h>
#include <string.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"
#include "psx_ygo_cheats.h"      /* psx_ygo_save_is_live() */
#include "psx_ygo_netplay.h"

#define SAVE_LIVE     0x801D0200u
#define DECK_N        40u              /* deck at +0x00, one u16 per slot */
#define TRUNK_OFF     0x50u            /* per-card counts, card N at +(N-1) */
#define FLAGS_OFF     0x418u           /* the flag array */
#define CARD_FIRST    1
#define CARD_LAST     722
#define SEEN_FID(id)  (0x120u + (unsigned)(id))

#define MODE_BYTE     0x8009B26Cu
#define MODE_LIBRARY  0xC4u

/* The flag bytes cards live in: 0x121 >> 3 through 0x3F2 >> 3. The first and
 * last are shared with flags that are not ours, hence the masks. */
#define BYTE_FIRST    0x24u
#define BYTE_LAST     0x7Eu
#define BYTE_N        (BYTE_LAST - BYTE_FIRST + 1u)     /* 91 */
#define MASK_FIRST    0x7Fu            /* flags 0x121..0x127 = cards 1..7 */
#define MASK_LAST     0xE0u            /* flags 0x3F0..0x3F2 = cards 720..722 */

/* The branch the whole feature turns on, and the word that retargets it from
 * "next card" to "draw it anyway". Written with psx_mod_write_code_word so the
 * runtime routes the address through executable RAM rather than leaving the
 * compiled instruction stale, and only ever over the exact stock word. */
#define SEEN_BRANCH   0x8002C320u
#define BRANCH_STOCK  0x1040000Du     /* beq v0, zero, 0x8002C358 (next card) */
#define BRANCH_FILL   0x10400005u     /* beq v0, zero, 0x8002C338 (draw it)   */

static int     s_on;                   /* the MODS row */
static int     s_active;               /* the screen is ours right now */
static uint8_t s_cleared[BYTE_N];      /* seen bits WE cleared, to put back */
static int     s_hidden;               /* how many cards that was */
static int     s_patched;              /* the branch is retargeted */

static uint8_t byte_mask(unsigned b)
{
    if (b == BYTE_FIRST) return MASK_FIRST;
    if (b == BYTE_LAST)  return MASK_LAST;
    return 0xFFu;
}

static uint32_t flag_byte_addr(unsigned b)
{
    return SAVE_LIVE + FLAGS_OFF + b;
}

static int in_library(void)
{
    return psx_mod_read_byte(MODE_BYTE) == MODE_LIBRARY;
}

static int popcount8(uint8_t v)
{
    int n = 0;
    for (; v; v &= (uint8_t)(v - 1u)) n++;
    return n;
}

/* Cards whose seen bit is set right now. */
static int seen_count(void)
{
    int n = 0;
    for (unsigned b = BYTE_FIRST; b <= BYTE_LAST; b++)
        n += popcount8((uint8_t)(psx_mod_read_byte(flag_byte_addr(b)) & byte_mask(b)));
    return n;
}

/* The screen's own question, in the screen's own terms: func_8002C518 calls a
 * card yours when its trunk byte is not zero OR it sits in the 40-card deck.
 * The completion figure this row leaves behind counts exactly these. */
static int card_is_owned(int id)
{
    if (id < CARD_FIRST || id > CARD_LAST) return 0;
    if (psx_mod_read_byte(SAVE_LIVE + TRUNK_OFF + (unsigned)(id - 1)) != 0u) return 1;
    for (unsigned i = 0; i < DECK_N; i++)
        if ((int)psx_mod_read_half(SAVE_LIVE + i * 2u) == id) return 1;
    return 0;
}

static int owned_count(void)
{
    int n = 0;
    for (int id = CARD_FIRST; id <= CARD_LAST; id++) n += card_is_owned(id);
    return n;
}

/* The bits the count should NOT include: every card the player does not own.
 * Cleared before the screen builds, put back when it closes. */
static void hide_unowned(void)
{
    for (unsigned b = BYTE_FIRST; b <= BYTE_LAST; b++) {
        const uint8_t cur = psx_mod_read_byte(flag_byte_addr(b));
        uint8_t drop = 0;
        for (int bit = 0; bit < 8; bit++) {
            const uint8_t m = (uint8_t)(0x80u >> bit);
            if (!(cur & m) || !(byte_mask(b) & m)) continue;
            const int id = (int)((b << 3) + (unsigned)bit) - 0x120;
            if (!card_is_owned(id)) drop |= m;
        }
        if (!drop) continue;
        psx_mod_write_byte(flag_byte_addr(b), (uint8_t)(cur & ~drop));
        s_cleared[b - BYTE_FIRST] |= drop;
        s_hidden += popcount8(drop);
    }
}

static void patch_branch(int on)
{
    const uint32_t want = on ? BRANCH_FILL : BRANCH_STOCK;
    const uint32_t other = on ? BRANCH_STOCK : BRANCH_FILL;
    if (psx_mod_read_word(SEEN_BRANCH) != other) return;   /* only ever over the word we know */
    psx_mod_write_code_word(SEEN_BRANCH, want);
    s_patched = on ? 1 : 0;
}

static void apply(void)
{
    if (!s_patched) patch_branch(1);
    hide_unowned();
    s_active = 1;
}

/* Put the player's own library back. `write` is 0 when the save those bits
 * belonged to is no longer resident, in which case there is nothing to write
 * to and only the record is dropped. */
static void revert(int write)
{
    if (s_patched) patch_branch(0);
    if (write) {
        for (unsigned b = BYTE_FIRST; b <= BYTE_LAST; b++) {
            const uint8_t ours = s_cleared[b - BYTE_FIRST];
            if (!ours) continue;
            const uint8_t cur = psx_mod_read_byte(flag_byte_addr(b));
            psx_mod_write_byte(flag_byte_addr(b), (uint8_t)(cur | ours));
        }
    }
    memset(s_cleared, 0, sizeof s_cleared);
    s_hidden = 0;
    s_active = 0;
}

/* Per frame: one byte read while the row is off or the player is elsewhere.
 *
 * The screen is ours from the frame the LIBRARY opens to the frame it closes,
 * which is exactly as long as anything reads these bits. */
static void tick(void)
{
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!s_active && !s_on) return;
    if (!psx_ygo_save_is_live()) { if (s_active) revert(0); return; }
    if (s_on && in_library()) {
        if (!s_active) apply();
        return;
    }
    if (s_active) revert(1);
}

/* ---- menu ---------------------------------------------------------------- */

static const char *const ONOFF[] = { "Off", "On" };
static const char *const HINTS[] = {
    "The LIBRARY shows only the cards you have seen",
    "The LIBRARY shows every card and counts the ones you own. It gives you nothing, and your own list comes back when this is off",
};

static void row_changed(int value)
{
    s_on = value ? 1 : 0;
    if (!s_on && s_active) revert(psx_ygo_save_is_live());
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(s_on ? "Library placeholders: on, open the LIBRARY to see them"
                       : "Library placeholders: off, your own list is back", 1600);
}

void psx_fill_library_register_menu(void)
{
    const int row = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "Library placeholders", HINTS[1],
        ONOFF, 2, "fill_library", 0, row_changed);
    psx_video_menu_set_row_hints(row, HINTS);
}

/* ---- debug surface ------------------------------------------------------- */

void psx_fill_library_set(int on)
{
    row_changed(on ? 1 : 0);
}

int psx_fill_library_state_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    const int live = psx_ygo_save_is_live();
    return snprintf(out, cap,
        "\"on\":%d,\"active\":%d,\"patched\":%d,\"hidden\":%d,\"save_live\":%d,"
        "\"in_library\":%d,\"seen\":%d,\"owned\":%d,\"cards\":%d",
        s_on, s_active, s_patched, s_hidden, live, live && in_library(),
        live ? seen_count() : 0, live ? owned_count() : 0, CARD_LAST);
}

PSX_MOD_CONSTRUCTOR(psx_fill_library_install)
{
    psx_fill_library_register_menu();
    (void)psx_game_add_frame_hook(tick);
}
