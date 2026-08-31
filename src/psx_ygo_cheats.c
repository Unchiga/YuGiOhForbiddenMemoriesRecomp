/* psx_ygo_cheats.c — see psx_ygo_cheats.h.
 *
 * Lifted out of main.cpp and the shared overlay menu unchanged: the addresses,
 * the reasoning that found them and the ordering constraints are all as they
 * were when they were measured. What changed is only where they live and how
 * the rows reach them — each row now calls its own handler directly instead of
 * being relayed through a menu-state struct the framework had to carry fields
 * for.
 */

#include "psx_ygo_cheats.h"
#include "psx_duelist_icon_cache.h"
#include "psx_card_extend.h"
#include "psx_card_chest.h"
#include "psx_card_save.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

/* --- Is a save actually resident? ----------------------------------------
 * `psx_mod_game_started()` is NOT this question, and three rows below were
 * asking it as if it were. From fntrace.c it is a one-shot that fires the
 * first time the game EXE's entry_pc is dispatched, so it is already true
 * during the Konami cards, throughout the intro movie, on the title screen,
 * and on the name-entry screen of a brand new game. Those rows write SAVE
 * data, and before a save exists their addresses belong to the boot sequence.
 *
 * Measured 2026-08-21 on the debug build: toggling ALL CARDS during the intro
 * scrambles the movie into macroblock garbage and then the picture stops
 * advancing for good — the emulator keeps running at full speed and the CD
 * keeps streaming, but the game never reaches the title. Filling each of that
 * row's three destinations on its own attributes it exactly: the two save
 * struct copies change nothing, 0x80105D98 alone reproduces it every time.
 *
 * The save struct carries its own signature. Its first 0x50 bytes are the
 * 40-card deck, one u16 card id per slot, and ids run 1..722; the region is
 * all zeros on every screen that precedes a save. So "forty halfwords, every
 * one of them a real card id" separates the two cases outright — measured
 * false on the intro, the title and name entry, and true in all twelve
 * in-game states on hand. Same self-identifying trick psx_drop_missing.c uses
 * to name a duellist from its resident drop table instead of trusting an
 * index, and deliberately not a timer: "wait N seconds after boot" is a guess
 * that a slow load breaks, while this asks the actual question. */
#define PSX_SAVE_LIVE     0x801D0200u   /* live save struct, 0x680 bytes */
#define PSX_SAVE_MIRROR   0x801D3200u   /* its +0x3000 copy */
#define PSX_SAVE_DECK_N   40u           /* deck at +0x00, one u16 per card */
#define PSX_TRUNK_OFF     0x50u         /* per-card counts start here */
#define PSX_TRUNK_LEN     722u
#define PSX_CARD_ID_MAX   722u

static int deck_resident(uint32_t save_base) {
    for (uint32_t i = 0; i < PSX_SAVE_DECK_N; i++) {
        const uint32_t id = psx_mod_read_half(save_base + i * 2u);
        if (id < 1u || id > PSX_CARD_ID_MAX) return 0;
    }
    return 1;
}

int psx_ygo_save_is_live(void) {
    return psx_mod_game_started()
        && deck_resident(PSX_SAVE_LIVE)
        && deck_resident(PSX_SAVE_MIRROR);
}

/* A row that writes save data and finds no save has nothing to do, and saying
 * so beats doing nothing quietly — the row would otherwise sit there showing
 * a value the player never actually got. Putting it back re-enters the
 * callback once with 0, which every one of these returns on immediately. */
static int s_starchips_row = -1;
static int s_all_cards_row = -1;

static void refuse(int row, const char *what) {
    char msg[64];
    snprintf(msg, sizeof msg, "%s: load a save first", what);
    host_osd_push(msg, 2200);
    if (row >= 0) psx_video_menu_set_row(row, 0);
}

/* --- Free spending -------------------------------------------------------
 * The StarChip total is a read-modify-write: `$v0 = $v0 + $v1` at 0x80021EE0
 * then `sw $v0, 0x5E0($a0)`. A single patched instruction cannot express
 * "ignore negative deltas but keep positive ones" — freezing the add would
 * block earnings too. So this watches the live field once per frame and puts
 * back any DECREASE, while letting increases through. The purchase itself
 * still succeeds (the game has already granted the item); only the deduction
 * is undone. */
static const uint32_t PSX_STARCHIPS_ADDR = 0x801D07E0u;
static int      g_free_spending = 0;
static uint32_t s_sc_last = 0;
static int      s_sc_tracking = 0;

static void free_spending_tick(void) {
    if (!g_free_spending || !psx_ygo_save_is_live()) {
        s_sc_tracking = 0;
        return;
    }
    uint32_t cur = psx_mod_read_word(PSX_STARCHIPS_ADDR);
    /* Ignore obvious garbage: the field is small in practice, and a wild value
     * means we are mid-load or looking at an uninitialised buffer. */
    if (cur > 9999999u) { s_sc_tracking = 0; return; }
    if (!s_sc_tracking) { s_sc_last = cur; s_sc_tracking = 1; return; }
    if (cur < s_sc_last)
        psx_mod_write_word(PSX_STARCHIPS_ADDR, s_sc_last);   /* refund */
    else
        s_sc_last = cur;                                     /* keep earnings */
}

/* --- SHOW OPPONENT HAND --------------------------------------------------
 * The duel keeps one 0x20-byte struct per duellist — 0x800E9FF0 for the
 * player, 0x800EA010 for the opponent — and byte +0x1F of each is a SIGNED
 * "keep this hand face down" flag. Nothing here draws anything: the game
 * already knows how to draw their hand exactly like yours, and this only
 * clears the flag that tells it not to.
 *
 * Read three times, all in the card-display path, all testing the SIGN:
 *
 *   0x80017DF0 / 0x80017E20 (func_80017DB4)  hand card -> display byte +103
 *   0x80018058              (func_80018004)  same, single-card path
 *   0x800232C0              (func_80023144)  card tint byte at 0x8009B34E
 *
 * Zero skips the hide path outright; only a NEGATIVE value makes the first two
 * write 255 — the "card back" graphic index — instead of the real artwork
 * index, and makes the third leave the card on its dimmed tint. So clearing
 * the byte both turns the backs into real card sprites (art, name, ATK/DEF)
 * and un-greys them, through the game's own renderer.
 *
 * Duel init seeds both structs from one global (0x8001767C–0x80017690) and
 * then, for a CPU opponent, overwrites the opponent's copy with -1
 * (0x800176A8 `li $v0,-1`; 0x800176AC `sb $v0,-0x5FD1($v1)` => 0x800EA02F).
 * This is the flag the well-known `300EA02F 0000` GameShark code clears.
 *
 * Per frame, not once: a block copy at 0x8007431C rewrites +0x1A..+0x1F as a
 * unit — measured once per opponent turn across a full traced turn — so a
 * single write gets undone. Only a negative value is replaced, so a duel the
 * game itself chose to show face-up is never touched, and switching the row
 * off restores the -1 only if we were the ones who cleared it. */
static const uint32_t PSX_OPP_HAND_FLAG = 0x800EA02Fu;
static int g_show_opp_hand = 0;
static int s_opp_forced = 0;

static void show_opp_hand_tick(void) {
    if (!psx_mod_game_started()) { s_opp_forced = 0; return; }
    const int flag = (int)(int8_t)psx_mod_read_byte(PSX_OPP_HAND_FLAG);
    if (g_show_opp_hand) {
        if (flag < 0) {
            psx_mod_write_byte(PSX_OPP_HAND_FLAG, 0u);
            s_opp_forced = 1;
        }
    } else if (s_opp_forced) {
        if (flag >= 0) psx_mod_write_byte(PSX_OPP_HAND_FLAG, 0xFFu);
        s_opp_forced = 0;
    }
}

/* --- FORCE FACE-UP -------------------------------------------------------
 * The duel keeps one 28-byte record per card in play from 0x801A7AE4:
 * 0-4 player hand, 5-14 player field, 15-19 opponent hand, 20-29 opponent
 * field. Halfword +10 is that card's flags, and **bit 0x1000 is "face down"**.
 *
 * Unlike SHOW OPPONENT HAND, this bit is real game state, not just display.
 * It is read by the display builders (0x80017EA0, 0x800180C0, 0x8001EA20 —
 * each turns it into a sprite attribute byte) AND by the battle routine
 * `func_8001D670` at 0x8001E700, which is what flips a set monster face-up
 * when it is attacked. Clearing it does not merely draw the card face-up; the
 * card genuinely IS face-up, and will not flip when attacked.
 *
 * The placement routine `func_8001BD8C` ORs 0x1000 in as the AI puts a card
 * down (0x8001C3A8, 0x8001CCE4) and the field record inherits it from the hand
 * record. Measured on a specimen where the AI was forced to set a monster: the
 * card lands as 0x9800 (face-down + defense) stock, and as 0x8800 (defense,
 * face-up) with the hand record pre-cleared. Clearing an ALREADY-placed field
 * record works too, but only redraws when something rebuilds the view, so both
 * ranges are swept every frame.
 *
 * This is the well-known `5000051C 0000` + `301A7C93 0080` GameShark pair —
 * a repeat modifier writing 0x80 to +11 (the flags high byte) of records 15-19.
 * We clear the BIT rather than stomping the byte: GameShark had no choice, but
 * the byte also carries 0x0800 (defense position), and stomping it flips a
 * defending monster into attack. Measured — that is a real state change, not a
 * cosmetic one, so it is not something a "show me their cards" row should do.
 *
 * One-way on purpose: switching the row off stops clearing, it does not put
 * 0x1000 back. Re-hiding a card the game has already resolved as face-up would
 * desync the display from what the battle code just used. */
#define PSX_CARD_RECORDS    0x801A7AE4u
#define PSX_CARD_STRIDE     28u
#define PSX_CARD_OFF_FLAGS  10u
#define PSX_CARD_FACEDOWN   0x1000u
#define PSX_OPP_REC_FIRST   15u   /* 15-19 hand, 20-29 field */
#define PSX_OPP_REC_LAST    29u

static int g_force_faceup = 0;

static void force_faceup_tick(void) {
    if (!g_force_faceup || !psx_mod_game_started()) return;
    for (uint32_t r = PSX_OPP_REC_FIRST; r <= PSX_OPP_REC_LAST; r++) {
        const uint32_t a = PSX_CARD_RECORDS + r * PSX_CARD_STRIDE;
        /* Only touch a slot holding a real card. Between duels these records
         * keep stale ids with zeroed flags, and this way an uninitialised
         * buffer is never written. */
        const uint32_t id = psx_mod_read_half(a);
        if (id < 1u || id > PSX_CARD_ID_MAX) continue;
        const uint32_t f = psx_mod_read_half(a + PSX_CARD_OFF_FLAGS);
        if (f & PSX_CARD_FACEDOWN)
            psx_mod_write_half(a + PSX_CARD_OFF_FLAGS,
                               (uint16_t)(f & ~PSX_CARD_FACEDOWN));
    }
}

static void reveal_tick(void);     /* defined with REVEAL ALL PORTRAITS */

void psx_ygo_cheats_tick(void) {
    show_opp_hand_tick();
    force_faceup_tick();
    free_spending_tick();
    reveal_tick();
}

/* --- LIFE POINTS ---------------------------------------------------------
 * The stock EXE loads the constant with `addiu $v0, $zero, 0x1F40` (8000)
 * at two sites: 0x800175D0 stores it as a halfword pair on the stack (both
 * duellists), 0x8002DC70 stores it to the global at 0x8009B236. Rewriting
 * the whole instruction as `addiu $v0, $zero, <lp>` covers both.
 *
 * psx_mod_write_code_word (not write_word) routes the address through the
 * executable-RAM path, so the text guard revokes the statically recompiled
 * block and the interpreter picks up the new immediate — and a restored save
 * state cannot leave a stale compiled instruction behind.
 *
 * addiu sign-extends its 16-bit immediate, so keep values under 32768.
 *
 * Unlike the three save rows this one is safe from the moment the game starts,
 * and was measured to be: both sites read 0x24021F40 from the frame the EXE
 * becomes resident — which is exactly when psx_mod_game_started() flips —
 * and hold it through the whole intro and the title. It patches the EXE's own
 * text, which is resident precisely when the EXE is running, so it needs no
 * save. The opcode check below therefore never refuses in practice; it is here
 * so that if anything ever does land on those addresses the row declines
 * instead of writing an immediate into the middle of it. */
static void lp_patch_site(uint32_t pc, int value) {
    if ((psx_mod_read_word(pc) >> 16) != 0x2402u) return;   /* addiu $v0,$zero */
    psx_mod_write_code_word(pc, 0x24020000u | (uint32_t)value);
}

static void lp_changed(int value) {
    if (!psx_mod_game_started()) return;
    if (value < 1) value = 1;
    if (value > 32767) value = 32767;
    lp_patch_site(0x800175D0u, value);
    lp_patch_site(0x8002DC70u, value);
}

/* --- STARCHIPS -----------------------------------------------------------
 * Located by RAM scan + write trace: a 32-bit field at offset 0x5E0 in a
 * 0x680-byte game-state struct, live copy at 0x801D0200 => 0x801D07E0. The
 * award/spend routine at 0x80021EE0 does `$v0 = $v0 + $v1` then
 * `sw $v0, 0x5E0($a0)`, which is what confirmed the offset.
 *
 * Two mirrors exist (0x801D37E0, 0x801D3E60) but they are memcpy'd FROM the
 * live block, so writing the live copy is what propagates — writing a mirror
 * would display correctly and then be overwritten. */
static void starchips_changed(int value) {
    if (value <= 0) return;
    if (!psx_ygo_save_is_live()) { refuse(s_starchips_row, "StarChips"); return; }
    psx_mod_write_word(PSX_STARCHIPS_ADDR, (uint32_t)value);
    s_sc_tracking = 0;   /* re-baseline so the guard does not refund this */
    host_osd_push("StarChips set", 1200);
}

static void free_spending_changed(int value) {
    g_free_spending = value ? 1 : 0;
    if (!g_free_spending) s_sc_tracking = 0;
    host_osd_push(g_free_spending ? "Free spending: on" : "Free spending: off", 900);
}

/* The tick does the guest writes, so these only move a switch.
 *
 * The toast is for a player who just moved the row, not for the settings file.
 * `psx_video_menu_apply_restored()` replays on_change for every restored row as
 * the game starts, so an unguarded toast greets every launch with a setting
 * nobody touched — "Their cards: face up" on startup, even when the restored
 * value was OFF and nothing had changed. psx_video_menu_is_restoring() is set
 * only during that replay, which is the one thing a callback cannot otherwise
 * tell about its own caller. */
static void show_opp_hand_changed(int value) {
    g_show_opp_hand = value ? 1 : 0;
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(g_show_opp_hand ? "Opponent's hand: shown"
                                  : "Opponent's hand: hidden", 900);
}

static void force_faceup_changed(int value) {
    g_force_faceup = value ? 1 : 0;
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(g_force_faceup ? "Their cards: face up"
                                 : "Their cards: as dealt", 900);
}

/* --- ALL CARDS -----------------------------------------------------------
 * The trunk is a 722-byte array of per-card counts, card N at +(N-1), at
 * save-struct +0x50. Located 2026-08-16 by known-value search against three
 * counts read off the chest screen (Horn Imp #25 = 1, Griffore #46 = 1, Aqua
 * Snake #446 = 0). Exactly three regions in RAM match the signature and ALL
 * THREE must be written:
 *
 *   0x801D0250  live save struct (+0x50)
 *   0x801D3250  the known mirror (+0x3000)
 *   0x80105D98  third copy — the chest UI's working buffer
 *
 * Writing only the live copy does NOT stick: the chest screen rebuilds from
 * its own buffer and puts the old values straight back (measured — the first
 * attempt was reverted in full). Apply with the chest CLOSED, which is what
 * the row's hint tells the player.
 *
 * The third copy is only the trunk while the chest's arena owns that memory.
 * Measured across twelve in-game states: it is either a byte-for-byte copy of
 * the live trunk (seven of them) or something else entirely (five of them held
 * a foreign table of 8-byte records, 0xFE/0xFF bytes and all), with nothing in
 * between. So "does it equal the live trunk right now" is an exact test rather
 * than a plausibility one — and it has to be asked BEFORE the live trunk is
 * overwritten, or it answers about our own write. */
#define PSX_UI_TRUNK  0x80105D98u

/* Where the chest's working copy of card id 1 lives right now: the stock
 * arena slot, or the chest stretch's relocated one when that is active. */
static uint32_t ui_trunk_base(void) {
    const uint32_t cell1 = psx_card_chest_ui_trunk_cell(1u);
    return cell1 ? cell1 : PSX_UI_TRUNK;
}

static int ui_copy_is_trunk(void) {
    const uint32_t base = ui_trunk_base();
    for (uint32_t i = 0; i < PSX_TRUNK_LEN; i++)
        if (psx_mod_read_byte(base + i) !=
            psx_mod_read_byte(PSX_SAVE_LIVE + PSX_TRUNK_OFF + i))
            return 0;
    return 1;
}

static void all_cards_changed(int value) {
    if (value <= 0) return;
    if (!psx_ygo_save_is_live()) { refuse(s_all_cards_row, "All cards"); return; }
    const int ui_is_trunk = ui_copy_is_trunk();
    const uint8_t n = (uint8_t)(value > 3 ? 3 : value);
    /* The stock range and the extended range are granted by two loops that
     * share nothing but n. The stock loop is bounded by PSX_TRUNK_LEN
     * unconditionally, so an id above 722 cannot reach the save, its mirror
     * or the UI copy under any extension state -- the stock save block has
     * exactly 722 trunk bytes with live fields directly after them, and a
     * count for id 723+ written there is save corruption (an earlier
     * revision did exactly that). */
    const uint32_t ui_base = ui_trunk_base();
    for (uint32_t i = 0; i < PSX_TRUNK_LEN; i++) {
        psx_mod_write_byte(PSX_SAVE_LIVE   + PSX_TRUNK_OFF + i, n);
        psx_mod_write_byte(PSX_SAVE_MIRROR + PSX_TRUNK_OFF + i, n);
        if (ui_is_trunk)
            psx_mod_write_byte(ui_base + i, n);
    }
    /* Extended ids go ONLY to the mod-side store; psx_card_chest.c's
     * sync_ext_trunk sees the store change and pushes counts, record
     * owned-flags and the owned-total into the relocated working buffer on
     * its next tick, so the grant lands whether the chest is open or not.
     *
     * Gate on the BOOT-LATCHED preference, not on psx_card_extend_count():
     * the count collapses to 722 whenever the card DB is not resident --
     * which includes exactly the chest-closed state this row's hint asks
     * for -- so gating on it silently skipped the extra cards (measured
     * 2026-08-30). The latch cannot change mid-run (flipping the MODS row
     * only asks for a restart), so there is no state where extras leak into
     * a stock session: with the row Off the store is never written, and the
     * store is never shown or saved through stock paths anyway. */
    if (psx_card_save_ext_enabled()) {
        for (uint32_t id = PSX_CARD_EXT_FIRST; id <= PSX_CARD_EXT_LAST; id++)
            psx_card_ext_trunk_set(id, n);
        host_osd_push("All cards granted (extra cards too)", 1500);
    } else {
        host_osd_push("All cards granted", 1500);
    }
}

/* --- REVEAL ALL PORTRAITS ------------------------------------------------
 * The FREE DUEL grid draws a portrait only for a duelist the campaign has
 * MET: bit 0x80>>(id&7) of save byte +0x418+(id>>3), flag id 0x6E0+duelist
 * — the flag func_8002CCA8 reads and the screen's roster-build loop (at
 * 0x801683CC, overlay) consults; the published "all free duel opponents"
 * GameShark patches that loop's lock branch instead. Setting all 39 makes
 * every cell render on the next visit to the screen, which is what lets the
 * Drop Table Manager's portrait capture complete a set the player's
 * campaign never could — a missed Seto 2nd has no cell to capture.
 *
 * DELIBERATELY TEMPORARY. The row remembers exactly which bits it set (only
 * ones that were clear) and clears them again — by itself the moment the
 * portrait cache is complete, or when the player turns the row off. The
 * portraits live in the Manager's own cache file, never in the save, so
 * after the revert the player's game is bit-for-bit what their campaign
 * earned. Selectability (who you can actually duel) is gated elsewhere and
 * is not touched either way.
 *
 * Like every save write here it goes to the live struct AND the +0x3000
 * mirror, and refuses until a save is loaded (ISSUES #5). */
#define PSX_MEET_FLAGS_OFF 0x418u
#define PSX_MEET_FIRST_ID  0x6E0u

static int     s_reveal_row = -1;
static uint8_t s_reveal_set[39];   /* bits WE set (they were clear before) */
static int     s_reveal_active;

static void reveal_write(int d, int set) {
    const uint32_t id  = PSX_MEET_FIRST_ID + (uint32_t)d;
    const uint32_t off = PSX_MEET_FLAGS_OFF + (id >> 3);
    const uint8_t  bit = (uint8_t)(0x80u >> (id & 7u));
    uint8_t v = psx_mod_read_byte(PSX_SAVE_LIVE + off);
    psx_mod_write_byte(PSX_SAVE_LIVE + off,
                       (uint8_t)(set ? (v | bit) : (v & (uint8_t)~bit)));
    v = psx_mod_read_byte(PSX_SAVE_MIRROR + off);
    psx_mod_write_byte(PSX_SAVE_MIRROR + off,
                       (uint8_t)(set ? (v | bit) : (v & (uint8_t)~bit)));
}

/* The reveal ROW is a maintainer tool, not a player feature (user call,
 * 2026-08-21): a player browsing FREE DUEL captures every portrait their
 * campaign can show, and the full 39 matter only to us when building the
 * shared portrait set. Player (Release / PSX_NO_DEBUG_TOOLS) builds get no
 * CHEATS row; dev builds keep it. The tick/revert machinery below stays in
 * every build so a savestate carrying an armed reveal still reverts. */
#ifndef PSX_NO_DEBUG_TOOLS
static int reveal_flag_is_set(int d) {
    const uint32_t id  = PSX_MEET_FIRST_ID + (uint32_t)d;
    const uint32_t off = PSX_MEET_FLAGS_OFF + (id >> 3);
    const uint8_t  bit = (uint8_t)(0x80u >> (id & 7u));
    return (psx_mod_read_byte(PSX_SAVE_LIVE + off) & bit) != 0;
}
#endif

static void reveal_revert(const char *why) {
    if (!s_reveal_active) return;
    s_reveal_active = 0;               /* before set_row: it re-enters at 0 */
    for (int d = 0; d < 39; d++) {
        if (s_reveal_set[d]) reveal_write(d, 0);
        s_reveal_set[d] = 0;
    }
    if (s_reveal_row >= 0) psx_video_menu_set_row(s_reveal_row, 0);
    host_osd_push(why, 2000);
}

#ifndef PSX_NO_DEBUG_TOOLS
static void reveal_changed(int value) {
    if (value <= 0) {
        reveal_revert("Portrait reveal reverted");
        return;
    }
    if (!psx_ygo_save_is_live()) { refuse(s_reveal_row, "Reveal portraits"); return; }
    if (!psx_duelist_icon_cache_missing()) {
        host_osd_push("Every portrait is already captured", 2000);
        if (s_reveal_row >= 0) psx_video_menu_set_row(s_reveal_row, 0);
        return;
    }
    for (int d = 0; d < 39; d++) {
        if (reveal_flag_is_set(d)) continue;
        reveal_write(d, 1);
        s_reveal_set[d] = 1;
    }
    s_reveal_active = 1;
    host_osd_push("Portraits revealed - visit FREE DUEL", 2200);
}
#endif /* !PSX_NO_DEBUG_TOOLS */

/* Called from this module's frame hook: the reveal takes itself back the
 * moment the Manager's portrait cache is complete. */
static void reveal_tick(void) {
    if (s_reveal_active && !psx_duelist_icon_cache_missing())
        reveal_revert("All portraits captured - reveal reverted");
}

/* --- the rows ------------------------------------------------------------ */

static const char *const ONOFF_LABELS[] = { "Off", "On" };
static const char *const ONOFF_HINTS[]  = {
    "Refund any starchip spend",
    "Purchases refunded \xe2\x80\x94 earnings still count"
};
static const char *const ALLCARDS_LABELS[] = {
    "Off", "1 of each", "2 of each", "3 of each"
};
static const char *const ALLCARDS_HINTS[] = {
    "Fill the trunk with every card",
    "Apply with the chest closed",
    "Apply with the chest closed",
    "Apply with the chest closed"
};
static const char *const OPPHAND_HINTS[] = {
    "Their hand stays hidden",
    "Their hand is drawn like yours"
};
static const char *const FACEUP_HINTS[] = {
    "They may set cards face down",
    "Their set cards play face up \xe2\x80\x94 no flip on attack"
};
#ifndef PSX_NO_DEBUG_TOOLS
static const char *const REVEAL_LABELS[] = { "Off", "On" };
static const char *const REVEAL_HINTS[]  = {
    "Show every free duel portrait, temporarily",
    "Reverts itself once all portraits are captured"
};
#endif

PSX_MOD_CONSTRUCTOR(psx_ygo_cheats_install) {
    psx_ygo_cheats_register_menu();
    (void)psx_game_add_frame_hook(psx_ygo_cheats_tick);
}

void psx_ygo_cheats_register_menu(void) {
    int h;

    /* A preference: it patches a code constant, not save data, so it carries a
     * settings key and is restored at startup like any other setting. The
     * slider spans 1..9999 because the game's LP display is four digits, even
     * though the patched addiu would allow up to 32767. */
    h = psx_video_menu_add_number(
        PSX_VM_MENU_CHEATS, "Life points", "8000 is stock. Enter to type",
        1, 9999, /*slider*/1, "life_points",
        PSX_VM_LIFE_POINTS_DEFAULT, lp_changed);
    psx_video_menu_set_row_mark(h, PSX_VM_LIFE_POINTS_DEFAULT);

    /* Also a preference: the per-frame guard writes one duel-display flag and
     * touches no save data, so it carries a settings key too. */
    h = psx_video_menu_add_option(
        PSX_VM_MENU_CHEATS, "Show opponent hand", OPPHAND_HINTS[0],
        ONOFF_LABELS, 2, "show_opponent_hand", 0, show_opp_hand_changed);
    psx_video_menu_set_row_hints(h, OPPHAND_HINTS);

    h = psx_video_menu_add_option(
        PSX_VM_MENU_CHEATS, "Force face up", FACEUP_HINTS[0],
        ONOFF_LABELS, 2, "force_face_up", 0, force_faceup_changed);
    psx_video_menu_set_row_hints(h, FACEUP_HINTS);

    /* The remaining three are live save writes: NULL settings key, so they are
     * never written to the file and never re-applied at startup. */
    s_starchips_row = psx_video_menu_add_number(
        PSX_VM_MENU_CHEATS, "Starchips", "Enter to type a value",
        0, 99999, /*slider*/0, NULL, 0, starchips_changed);

    h = psx_video_menu_add_option(
        PSX_VM_MENU_CHEATS, "Free spending", ONOFF_HINTS[0],
        ONOFF_LABELS, 2, NULL, 0, free_spending_changed);
    psx_video_menu_set_row_hints(h, ONOFF_HINTS);

    s_all_cards_row = psx_video_menu_add_option(
        PSX_VM_MENU_CHEATS, "All cards", ALLCARDS_HINTS[0],
        ALLCARDS_LABELS, 4, NULL, 0, all_cards_changed);
    psx_video_menu_set_row_hints(s_all_cards_row, ALLCARDS_HINTS);

#ifndef PSX_NO_DEBUG_TOOLS
    /* Maintainer-only (see the reveal block above): player builds ship the
     * CHEATS menu without this row. */
    s_reveal_row = psx_video_menu_add_option(
        PSX_VM_MENU_CHEATS, "Reveal all portraits", REVEAL_HINTS[0],
        REVEAL_LABELS, 2, NULL, 0, reveal_changed);
    psx_video_menu_set_row_hints(s_reveal_row, REVEAL_HINTS);
#endif
}
