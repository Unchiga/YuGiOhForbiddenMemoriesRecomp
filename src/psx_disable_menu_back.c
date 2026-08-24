/* MODE SELECT CIRCLE GUARD — CIRCLE on the mode-select screen (CAMPAIGN /
 * FREE DUEL / BUILD DECK / LIBRARY / PASSWORD / SAVE) used to drop straight
 * back to the title menu with no confirmation, one accidental press losing
 * the player's place. MODS > BLOCK MENU CIRCLE EXIT blocks it outright:
 * Circle simply does nothing on this screen while the guard is on. Off
 * restores stock behavior exactly.
 *
 *
 * === How it works (measured live 2026-08-24 — see
 * docs/internal/MODE_SELECT_CIRCLE_FINDINGS.md for the full trace) ===
 *
 * The whole game's current screen/row is ONE byte — Data Crystal's RAM map
 * calls it "Menu ID" (0x80184594), shared by both the title menu and
 * mode-select as one list: 0x00..0x04 = title rows (NEW GAME.. OPTION),
 * 0x05..0x0A = mode-select rows (CAMPAIGN.. SAVE), 0x0B = the debug menu.
 * Verified live against the wiki's table (reads 0x01 on LOAD, 0x05 on
 * CAMPAIGN, exactly).
 *
 * Circle on mode-select writes 0x01 (Load) into this byte. The write site
 * sits inside `func_801807F8` — a 556-byte, almost entirely `nop`-padded
 * function that is a data-driven dispatch trampoline, not a clean, hookable
 * function entry. Mode-select's own pad read is no cleaner a target either:
 * it is inlined directly in the screen's coroutine driver (`func_80039794`),
 * which only calls the function-entry hook once, at the coroutine's first
 * frame — there is no race-free per-frame entry point on this screen the
 * way `psx_card_shop.c` has one for the campaign menu (`SHOP_MENU_NAV_FN`).
 *
 * So the gate works one level up, in a host frame hook, the same way this
 * project's frame hooks already win races elsewhere: compare the Menu ID
 * byte across two consecutive host frames. The instant mode-select's range
 * is replaced by 0x01 in a single frame, write the OLD value straight back —
 * before this frame presents, so the stock "straight to title" flash never
 * reaches the screen. Nothing else on this screen is touched; arrows,
 * Cross, and every other row still work exactly as normal. */

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#define MENU_ID_ADDR         0x80184594u
#define MENU_ID_LOAD         0x01u
#define MENU_ID_MODESEL_LO   0x05u
#define MENU_ID_MODESEL_HI   0x0Au

static int      g_enabled = 1;    /* MODS > BLOCK MENU CIRCLE EXIT, default on */
static int      s_have_prev;      /* s_prev_id holds a real prior sample */
static uint8_t  s_prev_id;        /* Menu ID observed last frame */

static int mode_select_row(uint8_t id) {
    return id >= MENU_ID_MODESEL_LO && id <= MENU_ID_MODESEL_HI;
}

static void guard_tick(void) {
    if (!psx_mod_game_started()) return;

    const uint8_t id = psx_mod_read_byte(MENU_ID_ADDR);

    if (!s_have_prev) {
        s_prev_id = id;
        s_have_prev = 1;
        return;
    }

    if (g_enabled && mode_select_row(s_prev_id) && id == MENU_ID_LOAD) {
        /* Circle just fired on mode-select this exact frame — undo it
         * before this frame presents. s_prev_id already holds the reverted
         * value, so this does not re-trigger next frame. */
        psx_mod_write_byte(MENU_ID_ADDR, s_prev_id);
        return;
    }

    s_prev_id = id;
}

static const char *const ONOFF[] = { "OFF", "ON" };

static void enabled_changed(int value) {
    g_enabled = value ? 1 : 0;
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(g_enabled ? "Menu Circle exit: blocked"
                            : "Menu Circle exit: normal", 1200);
}

PSX_MOD_CONSTRUCTOR(psx_mode_select_confirm_install) {
    (void)psx_game_add_frame_hook(guard_tick);
    (void)psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "BLOCK MENU CIRCLE EXIT",
        "STOPS CIRCLE FROM DROPPING CAMPAIGN/FREE DUEL/ETC STRAIGHT TO TITLE",
        ONOFF, 2, "block_menu2_circle", 1, enabled_changed);
}
