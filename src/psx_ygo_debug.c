/* psx_ygo_debug.c - this title's TCP debug commands.
 *
 * Rule 3 makes every observable a debug command, so each feature here grew
 * one: the duel-rank meter, card drops and the fusion assistant are all
 * driven and read back over TCP rather than judged from pixels. The handlers
 * are unchanged from when they lived in the framework - what changed is only
 * that they now register themselves instead of being named by debug_server.c,
 * which had 34 direct calls into this game and so failed to LINK for any
 * other title.
 *
 * The fade ring is the one piece that is more than a read-back: it samples
 * per simulated vblank through psx_game_add_vblank_hook, which is the same
 * cadence and the same frame stamp the framework rings use.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "cpu_state.h"
#include "debug_server.h"
#include "mod_plugins.h"
#include "psx_debug_commands.h"
#include "psx_game_hooks.h"

#include "psx_fusion_assist.h"
#include "psx_fusion_db.h"
#include "psx_fusion_overlay.h"
#include "psx_ygo_overlays.h"
#include "psx_drop_edits.h"
#include "psx_duelist_icon_cache.h"
#include "psx_drop_missing.h"
#include "psx_drop_viewer.h"
#include "psx_card_manager.h"
#include "psx_card_packs.h"
#include "psx_card_effects.h"
#include "psx_card_share.h"
#include "psx_card_colors.h"
#include "psx_monster_effects.h"

/* rank_meter_tune — nudge the duel-rank meter's layout while the game runs.
 * {"cmd":"rank_meter_tune","letter_x":N,"letter_y":N,"gap":N,"dx":N,"dy":N}
 * Any omitted field is left alone. Values are ABSOLUTE, not relative nudges —
 * sending dy=-4 twice leaves it at -4, not -8. Exists because positioning pixel
 * art beside the game's own HUD is a by-eye job and a rebuild costs the player
 * their duel. With no arguments it just reports the current values. */
static void handle_rank_meter_tune(int id, const char *json)
{
    extern void psx_rank_meter_tune(int, int, int, int, int);
    extern void psx_rank_meter_tune_get(int *, int *, int *, int *, int *);
    int lx, ly, gap, dx, dy;
    psx_rank_meter_tune_get(&lx, &ly, &gap, &dx, &dy);
    lx  = json_get_int(json, "letter_x", lx);
    ly  = json_get_int(json, "letter_y", ly);
    gap = json_get_int(json, "gap", gap);
    dx  = json_get_int(json, "dx", dx);
    dy  = json_get_int(json, "dy", dy);
    {
        extern int  psx_rank_meter_subpixel_y(void);
        extern void psx_rank_meter_tune_sub(int);
        psx_rank_meter_tune_sub(json_get_int(json, "dy2",
                                             psx_rank_meter_subpixel_y()));
    }
    psx_rank_meter_tune(lx, ly, gap, dx, dy);
    psx_rank_meter_tune_get(&lx, &ly, &gap, &dx, &dy);
    {
        extern int psx_rank_meter_subpixel_y(void);
        send_fmt("{\"id\":%d,\"ok\":true,\"letter_x\":%d,\"letter_y\":%d,"
                 "\"gap\":%d,\"dx\":%d,\"dy\":%d,\"dy2\":%d}",
                 id, lx, ly, gap, dx, dy, psx_rank_meter_subpixel_y());
    }
}

/* rank_meter_state — why is (or is not) the duel-rank meter on screen?
 * "Nothing is drawn" has several distinct causes — mode off, no duel, the HUD
 * tweened off screen, something drawn over it — and they are indistinguishable
 * from the pixels. That is how an occlusion-latch deadlock survived a whole
 * play session looking like "it just stopped working". */
static void handle_rank_meter_state(int id, const char *json)
{
    (void)json;
    extern void psx_rank_meter_debug(int *, int *, int *, int *, int *, int *, int *);
    extern void psx_rank_meter_fade_debug(int *, int *);
    extern void psx_rank_meter_origin(int *, int *);
    extern int  psx_rank_meter_image(const uint32_t **, int *, int *);
    int mode = 0, active = 0, anchor = 0, occ = 0, ax = 0, ay = 0;
    int ox = 0, oy = 0, w = 0, h = 0;
    int fade = 0, fade_t = 0;
    const uint32_t *px = 0;
    int show_hold = 0;
    psx_rank_meter_debug(&mode, &active, &anchor, &occ, &ax, &ay, &show_hold);
    psx_rank_meter_fade_debug(&fade, &fade_t);
    psx_rank_meter_origin(&ox, &oy);
    int visible = psx_rank_meter_image(&px, &w, &h);
    extern void gpu_sprite_watch_occluder(int *out4);
    int pl[10] = {0};
    int oc[4] = {0};
    psx_ygo_rank_placement(pl);
    gpu_sprite_watch_occluder(oc);
    send_fmt("{\"id\":%d,\"ok\":true,\"mode\":%d,\"duel_active\":%d,"
             "\"fade\":%d,\"fade_t\":%d,"
             "\"anchor_found\":%d,\"anchor_x\":%d,\"anchor_y\":%d,"
             "\"occluded\":%d,\"show_hold\":%d,\"visible\":%d,\"origin_x\":%d,"
             "\"origin_y\":%d,\"w\":%d,\"h\":%d,"
             "\"box\":[%d,%d,%d,%d],\"native\":[%d,%d],"
             "\"dest\":[%d,%d,%d,%d],\"occluder\":[%d,%d,%d,%d]}",
             id, mode, active, fade, fade_t, anchor, ax, ay, occ, show_hold,
             visible,
             ox, oy, w, h,
             pl[0],pl[1],pl[2],pl[3], pl[4],pl[5], pl[6],pl[7],pl[8],pl[9],
             oc[0],oc[1],oc[2],oc[3]);
}

/* card_drops_test tier=N — drive ONE nested drop roll and report what the
 * guest call produced. Lets the guest-call path be validated without winning a
 * duel first. */
/* WARNING - these two run GUEST code re-entrantly and can wedge the emulator.
 *
 * Sweeping card_drops_test back to back stops the emulator dead after roughly
 * 9-13 calls: everything except the io-thread ping times out. The same sweep
 * with ~20 ms between calls ran 120 clean, so PACE IT. Not fixed, and the two
 * obvious fixes are already ruled out, so do not spend the time again:
 *
 *   - It is not frame starvation. A per-frame call budget never fired at all;
 *     a whole frame completes between commands anyway.
 *   - It is not "we interrupted the BIOS". Refusing unless
 *     g_debug_current_func_addr is in game text never fired either.
 *
 * What IS known: the freeze dump puts the guest at last_store_pc 0xBFC21B04,
 * inside the BIOS, with cpu->pc 0. The nested call keeps its device effects on
 * purpose (trunk writes, RNG advance) and that includes interrupt and event
 * state, so the likely mechanism is a BIOS wait stranded on an event the
 * nested call consumed. Proving that needs the event/IRQ state captured across
 * a nested call, which is the next step whenever this is picked up. */
static void handle_card_drops_test(int id, const char *json)
{
    extern int psx_card_drops_test_roll(CPUState *, int, int, uint32_t *,
                                        uint32_t *, int *);
    if (!debug_cpu_ptr) { send_err(id, "no cpu"); return; }
    int tier = json_get_int(json, "tier", 0);
    int award = json_get_int(json, "award", 0);
    uint32_t card = 0, pc = 0;
    int bail = 0;
    if (!psx_card_drops_test_roll(debug_cpu_ptr, tier, award, &card, &pc, &bail)) {
        send_err(id, "roll failed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"tier\":%d,\"card\":%u,"
             "\"pc_after\":\"0x%08X\",\"bail\":%d}",
             id, tier, card, pc, bail);
}

/* card_drops_sim tier=N drops=N — simulate one duel drop through the real hook. */
static void handle_card_drops_sim(int id, const char *json)
{
    extern int psx_card_drops_simulate(CPUState *, int, int, uint32_t *, int *, int *);
    if (!debug_cpu_ptr) { send_err(id, "no cpu"); return; }
    int tier = json_get_int(json, "tier", 0);
    int drops = json_get_int(json, "drops", 0);
    uint32_t card = 0;
    int granted = 0, bail = 0;
    if (!psx_card_drops_simulate(debug_cpu_ptr, tier, drops, &card, &granted, &bail)) {
        send_err(id, "simulate failed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"tier\":%d,\"drops\":%d,"
             "\"card\":%u,\"granted\":%d,\"bail\":%d}",
             id, tier, drops, card, granted, bail);
}

/* fusion_db / fusion_hand / fusion_list / fusion_try — the in-duel fusion
 * assistant's read-backs. The whole feature is checkable from here before it
 * draws anything: `fusion_db` says the game's tables were found and how big
 * they are, `fusion_hand` shows the raw card records the hand is read from,
 * `fusion_list` says what that hand can make, and `fusion_try` answers for any
 * two card ids at all. */
static void handle_fusion_db(int id, const char *json)
{
    (void)json;
    int ready = 0, cards = 0, pairs = 0, groups = 0, members = 0;
    uint32_t pair_base = 0, equip_base = 0;
    psx_fusion_db_debug(&ready, &pair_base, &equip_base, &cards, &pairs,
                        &groups, &members);
    send_fmt("{\"id\":%d,\"ok\":true,\"ready\":%d,"
             "\"pair_base\":\"0x%08X\",\"equip_base\":\"0x%08X\","
             "\"cards_with_fusions\":%d,\"pairs\":%d,"
             "\"equip_groups\":%d,\"equip_members\":%d}",
             id, ready, pair_base, equip_base, cards, pairs, groups, members);
}

static void handle_fusion_hand(int id, const char *json)
{
    (void)json;
    char body[2048];
    body[0] = 0;
    psx_fusion_assist_hand_json(body, sizeof body);
    int mask = 0, sel = 0, turn = 0;
    psx_fusion_assist_hand_source(&mask, &sel, &turn);
    send_fmt("{\"id\":%d,\"ok\":true,\"gate_mask\":\"0x%02X\",\"sel_count\":%d,\"turn\":%d,%s}",
             id, (unsigned)mask, sel, turn, body);
}

static void handle_fusion_list(int id, const char *json)
{
    (void)json;
    char body[2048];
    body[0] = 0;
    psx_fusion_assist_list_json(body, sizeof body);
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

static void handle_fusion_overlay(int id, const char *json)
{
    int x = json_get_int(json, "x", PSX_FUSION_OVERLAY_KEEP);
    int y = json_get_int(json, "y", PSX_FUSION_OVERLAY_KEEP);
    int tx = json_get_int(json, "text_x", PSX_FUSION_OVERLAY_KEEP);
    int en = json_get_int(json, "mode", PSX_FUSION_OVERLAY_KEEP);
    if (x != PSX_FUSION_OVERLAY_KEEP || y != PSX_FUSION_OVERLAY_KEEP ||
        tx != PSX_FUSION_OVERLAY_KEEP || en != PSX_FUSION_OVERLAY_KEEP)
        psx_fusion_overlay_tune(x, y, tx, en);
    int cx = json_get_int(json, "card_x", PSX_FUSION_OVERLAY_KEEP);
    int cdx = json_get_int(json, "card_dx", PSX_FUSION_OVERLAY_KEEP);
    int bdy = json_get_int(json, "badge_dy", PSX_FUSION_OVERLAY_KEEP);
    int bdx = json_get_int(json, "badge_dx", PSX_FUSION_OVERLAY_KEEP);
    int ty = json_get_int(json, "text_y", PSX_FUSION_OVERLAY_KEEP);
    if (cx != PSX_FUSION_OVERLAY_KEEP || cdx != PSX_FUSION_OVERLAY_KEEP ||
        bdy != PSX_FUSION_OVERLAY_KEEP || bdx != PSX_FUSION_OVERLAY_KEEP ||
        ty != PSX_FUSION_OVERLAY_KEEP)
        psx_fusion_overlay_tune_cards(cx, cdx, bdy, bdx, ty);
    psx_fusion_overlay_tune_get(&x, &y, &tx, &en);
    psx_fusion_overlay_tune_cards_get(&cx, &cdx, &bdy, &bdx, &ty);
    uint8_t badge[8] = {0};
    psx_fusion_overlay_badges(badge, 5);
    /* The line carries a TAB as its internal seam between the alphabet-set
     * name and the digit-set stats. A raw control character inside a JSON
     * string is invalid JSON and made every reader throw, so show it as a
     * space here — the seam is an implementation detail, not something a
     * read-back needs to reproduce. */
    char shown[80];
    const char *src = psx_fusion_overlay_text();
    size_t si = 0;
    for (; si + 1 < sizeof shown && src[si]; si++)
        shown[si] = (src[si] == '	') ? ' ' : src[si];
    shown[si] = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"text_x\":%d,"
             "\"card_x\":%d,\"card_dx\":%d,\"badge_dy\":%d,\"badge_dx\":%d,\"text_y\":%d,"
             "\"mode\":%d,\"on_screen\":%d,"
             "\"badges\":[%u,%u,%u,%u,%u],\"text\":\"%s\"}",
             id, x, y, tx, cx, cdx, bdy, bdx, ty, en,
             psx_fusion_overlay_needs_present(),
             badge[0], badge[1], badge[2], badge[3], badge[4],
             shown);
}

static void handle_fusion_best(int id, const char *json)
{
    (void)json;
    char body[512];
    body[0] = 0;
    psx_fusion_assist_best_json(body, sizeof body);
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

static void handle_fusion_chain(int id, const char *json)
{
    (void)json;
    char body[1024];
    body[0] = 0;
    psx_fusion_assist_chain_json(body, sizeof body);
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

static void handle_fusion_try(int id, const char *json)

{
    const int a = json_get_int(json, "a", 0);
    const int b = json_get_int(json, "b", 0);
    char body[256];
    body[0] = 0;
    psx_fusion_assist_try_json(body, sizeof body, a, b);
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

/* What DROP MISSING CARDS thinks is going on: whether it loaded the ini, how
 * many duels it has rewritten, and which duelist it last recognised. Without
 * this the mod is invisible -- an unrecognised drop table makes it do nothing
 * on purpose, which looks exactly like the row being off. */
static void handle_drop_missing_state(int id, const char *json)
{
    (void)json;
    char buf[512];
    if (!psx_drop_missing_state_json(buf, sizeof(buf))) {
        send_err(id, "state unavailable"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_packs — which cards are replaced and how. */
static void handle_card_packs(int id, const char *json)
{
    (void)json;
    char buf[8192];
    if (!psx_card_packs_state_json(buf, sizeof buf)) { send_err(id, "state too long"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_effects — the effects layer: overrides, holds, hook events. */
static void handle_card_effects(int id, const char *json)
{
    (void)json;
    static char buf[8192];
    if (!psx_card_effects_state_json(buf, sizeof buf)) { send_err(id, "state too long"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_share — {"op":"export"|"inspect"|"import","path":...}: the one-file
 * container, without the dialogs. */
static void handle_card_share(int id, const char *json)
{
    char op[16], path[1024];
    if (!json_get_str(json, "op", op, sizeof op) || !json_get_str(json, "path", path, sizeof path)) { send_err(id, "need op and path"); return; }
    char msg[256];
    if (!strcmp(op, "export")) {
        const int ok = psx_card_share_export(path, msg, sizeof msg);
        send_fmt("{\"id\":%d,\"ok\":%s,\"msg\":\"%s\"}", id, ok ? "true" : "false", msg);
    } else if (!strcmp(op, "import")) {
        const int ok = psx_card_share_import(path, msg, sizeof msg);
        send_fmt("{\"id\":%d,\"ok\":%s,\"msg\":\"%s\"}", id, ok ? "true" : "false", msg);
    } else if (!strcmp(op, "inspect")) {
        static PsxCardShareInfo info;
        const int ok = psx_card_share_inspect(path, &info);
        static char ids[4096]; unsigned n = 0; ids[0] = 0;
        for (int i = 0; i < info.card_n && n + 8 < sizeof ids; i++) n += (unsigned)snprintf(ids + n, sizeof ids - n, "%s%d", i ? "," : "", info.card_ids[i]);
        send_fmt("{\"id\":%d,\"ok\":%s,\"error\":\"%s\",\"version\":%d,\"cards\":[%s],\"replace\":%d,\"drops\":%d,\"bytes\":%ld}",
                 id, ok ? "true" : "false", info.error, info.version, ids, info.replace_n, info.has_drops, info.bytes);
    } else send_err(id, "op is export, inspect or import");
}

/* card_colors — frame colour slots and patch state. */
static void handle_card_colors(int id, const char *json)
{
    (void)json;
    char buf[512];
    if (!psx_card_colors_state_json(buf, sizeof buf)) { send_err(id, "state too long"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* monster_effects — queue, battle decision, hook events. */
static void handle_monster_effects(int id, const char *json)
{
    (void)json;
    static char buf[8192];
    if (!psx_monster_effects_state_json(buf, sizeof buf)) { send_err(id, "state too long"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_packs_reload — re-read one pack (card) or all (no card). */
static void handle_card_packs_reload(int id, const char *json)
{
    psx_card_packs_reload(json_get_int(json, "card", 0));
    handle_card_packs(id, json);
}

/* card_manager — the Card Manager window: state, open/select/search,
 * synthetic clicks and typing, and a canvas dump (binary PPM). */
static void handle_card_manager(int id, const char *json)
{
    (void)json;
    char buf[4096];
    psx_card_manager_state_json(buf, sizeof buf);
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}
static void handle_card_manager_set(int id, const char *json)
{
    const int open = json_get_int(json, "open", -1);
    if (open >= 0) psx_card_manager_request_open(open);
    const int card = json_get_int(json, "card", -1);
    if (card > 0) psx_card_manager_select(card);
    char search[32];
    if (json_get_str(json, "search", search, sizeof search)) psx_card_manager_search(search);
    char ipath[1024];
    if (json_get_str(json, "import", ipath, sizeof ipath)) psx_card_manager_import_preview(ipath);
    handle_card_manager(id, json);
}
static void handle_card_manager_click(int id, const char *json)
{
    if (!psx_card_manager_click(json_get_int(json, "x", 0), json_get_int(json, "y", 0), json_get_int(json, "button", 1))) {
        send_err(id, "manager is closed"); return;
    }
    send_ok(id);
}
static void handle_card_manager_move(int id, const char *json)
{
    const int down = json_get_int(json, "down", -1);
    int ok;
    if (down >= 0) ok = psx_card_manager_button(json_get_int(json, "x", 0), json_get_int(json, "y", 0), json_get_int(json, "button", 1), down);
    else ok = psx_card_manager_move(json_get_int(json, "x", 0), json_get_int(json, "y", 0));
    if (!ok) { send_err(id, "manager is closed"); return; }
    send_ok(id);
}
static void handle_card_manager_type(int id, const char *json)
{
    char text[64];
    if (!json_get_str(json, "text", text, sizeof text)) { send_err(id, "missing text"); return; }
    if (!psx_card_manager_type(text)) { send_err(id, "manager is closed"); return; }
    send_ok(id);
}
static void handle_card_manager_key(int id, const char *json)
{
    /* key: return, escape, backspace, tab, up, down, pageup, pagedown */
    char name[16];
    if (!json_get_str(json, "key", name, sizeof name)) { send_err(id, "missing key"); return; }
    int k = 0;
    if (!strcmp(name, "return")) k = 13;
    else if (!strcmp(name, "escape")) k = 27;
    else if (!strcmp(name, "backspace")) k = 8;
    else if (!strcmp(name, "tab")) k = 9;
    else if (!strcmp(name, "up")) k = 0x40000052;
    else if (!strcmp(name, "down")) k = 0x40000051;
    else if (!strcmp(name, "pageup")) k = 0x4000004B;
    else if (!strcmp(name, "pagedown")) k = 0x4000004E;
    else if (!strcmp(name, "left")) k = 0x40000050;
    else if (!strcmp(name, "right")) k = 0x4000004F;
    else if (!strcmp(name, "home")) k = 0x4000004A;
    else if (!strcmp(name, "end")) k = 0x4000004D;
    else if (!strcmp(name, "delete")) k = 0x7F;
    else if (name[0] && !name[1]) k = (unsigned char)name[0];     /* a letter, for ctrl+a/c/x/v */
    if (!k) { send_err(id, "unknown key"); return; }
    int mods = 0;
    if (json_get_int(json, "shift", 0)) mods |= 1;
    if (json_get_int(json, "ctrl", 0)) mods |= 2;
    if (!psx_card_manager_key_mod(k, mods)) { send_err(id, "manager is closed"); return; }
    send_ok(id);
}
static void handle_card_manager_shot(int id, const char *json)
{
    char path[1024];
    if (!json_get_str(json, "path", path, sizeof path)) { send_err(id, "missing path"); return; }
    if (!psx_card_manager_shot(path)) { send_err(id, "manager is closed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path);
}

/* drop_viewer — what the second window is showing. The window is a host
 * surface with no framebuffer the screenshot commands can reach, so without
 * this the only way to check it is to photograph the desktop. */
static void handle_drop_viewer(int id, const char *json)
{
    (void)json;
    char buf[2048];
    if (!psx_drop_viewer_state_json(buf, sizeof(buf))) {
        send_err(id, "state unavailable"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* drop_viewer_set — point the window at something, so the two views and the
 * search can be checked without a mouse. */
static void handle_drop_viewer_set(int id, const char *json)
{
    /* open lands on the main thread next frame — window creation is not this
     * thread's to do — so an "open":1 with other fields will report "viewer
     * is closed" for those; send the open alone, then the rest. */
    const int open = json_get_int(json, "open", -1);
    if (open >= 0) psx_drop_viewer_request_open(open);
    char search[32];
    /* json_get_str returns the string, or NULL when the key is absent — which
     * is exactly the "leave it alone" the setter wants. */
    const char *have_search =
        json_get_str(json, "search", search, sizeof(search));
    if (!psx_drop_viewer_set(json_get_int(json, "view", -1),
                             json_get_int(json, "sort", -1),
                             json_get_int(json, "desc", -1),
                             json_get_int(json, "card", -1),
                             json_get_int(json, "duelist", -1),
                             have_search ? search : NULL)
        && open < 0) {
        send_err(id, "viewer is closed"); return;
    }
    char buf[2048];
    psx_drop_viewer_state_json(buf, sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* drop_viewer_shot — the window's canvas as a PPM, like card_manager_shot. */
static void handle_drop_viewer_shot(int id, const char *json)
{
    char path[1024];
    if (!json_get_str(json, "path", path, sizeof path)) { send_err(id, "missing path"); return; }
    if (!psx_drop_viewer_shot(path)) { send_err(id, "viewer is closed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path);
}

/* drop_viewer_click / drop_viewer_move — a mouse press / motion at window
 * coordinates, pushed through SDL's queue with the VIEWER's window id, so
 * they run the shipping event path end to end (including the hook that keeps
 * viewer events out of the game's menu). The state echoed back is from before
 * the event is pumped — read drop_viewer a frame later for the result. */
static void handle_drop_viewer_click(int id, const char *json)
{
    const int x = json_get_int(json, "x", -1);
    const int y = json_get_int(json, "y", -1);
    const int button = json_get_int(json, "button", 1);
    if (x < 0 || y < 0) { send_err(id, "need x and y"); return; }
    if (!psx_drop_viewer_click(x, y, button)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"button\":%d}",
             id, x, y, button);
}

/* drop_viewer_press / drop_viewer_release — the halves of a drag: press,
 * then drop_viewer_move waypoints, then release at the drop point. */
static void handle_drop_viewer_press(int id, const char *json)
{
    const int x = json_get_int(json, "x", -1);
    const int y = json_get_int(json, "y", -1);
    const int button = json_get_int(json, "button", 1);
    if (x < 0 || y < 0) { send_err(id, "need x and y"); return; }
    if (!psx_drop_viewer_press(x, y, button)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void handle_drop_viewer_release(int id, const char *json)
{
    const int x = json_get_int(json, "x", -1);
    const int y = json_get_int(json, "y", -1);
    const int button = json_get_int(json, "button", 1);
    if (x < 0 || y < 0) { send_err(id, "need x and y"); return; }
    if (!psx_drop_viewer_release(x, y, button)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void handle_drop_viewer_move(int id, const char *json)
{
    const int x = json_get_int(json, "x", -1);
    const int y = json_get_int(json, "y", -1);
    if (x < 0 || y < 0) { send_err(id, "need x and y"); return; }
    if (!psx_drop_viewer_inject_motion(x, y)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d}", id, x, y);
}

/* drop_viewer_key / drop_viewer_text — keyboard into the viewer window, same
 * injected-SDL-event story. key is an SDL_Keycode (Return is 13, Escape 27). */
static void handle_drop_viewer_key(int id, const char *json)
{
    const int key = json_get_int(json, "key", -1);
    if (key < 0) { send_err(id, "need key"); return; }
    if (!psx_drop_viewer_inject_key(key)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"key\":%d}", id, key);
}

static void handle_drop_viewer_text(int id, const char *json)
{
    char text[32];
    if (!json_get_str(json, "text", text, sizeof(text))) {
        send_err(id, "need text"); return;
    }
    if (!psx_drop_viewer_inject_text(text)) {
        send_err(id, "viewer is closed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

/* drop_edits — the drop-table edit layer: entry counts, dirtiness, ini
 * status. The per-duelist detail is visible through the viewer itself. */
static void handle_drop_edits(int id, const char *json)
{
    (void)json;
    char buf[256];
    if (!psx_drop_edits_state_json(buf, sizeof(buf))) {
        send_err(id, "state unavailable"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* duelist_icons — the runtime portrait capture: how many the cache holds,
 * captures and contrast-rejections this session, and whether a disk write
 * is pending. */
static void handle_duelist_icons(int id, const char *json)
{
    (void)json;
    char buf[384];
    if (!psx_duelist_icon_cache_state_json(buf, sizeof(buf))) {
        send_err(id, "state unavailable"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_shop — the shopkeeper-menu CARD SHOP row/panel: signature match,
 * open state, purchase counters. "The row is not appearing" divides into
 * mod-off, signature-miss and count-byte-not-taken, and those are
 * indistinguishable from pixels. */
static void handle_card_shop(int id, const char *json)
{
    (void)json;
    extern int  psx_card_shop_state_json(char *, unsigned);
    char buf[768];
    if (psx_card_shop_state_json(buf, sizeof(buf)) <= 0) {
        send_err(id, "state unavailable"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

/* card_shop_card name=<card> — the rarity the config resolved for one card and
 * the pools it actually sits in. "That card came out of the wrong pack" is
 * otherwise only observable by buying packs until it turns up again, and a
 * pinned card silently demoted because its name did not match the config looks
 * exactly like a pinning that was never applied. */
static void handle_card_shop_card(int id, const char *json)
{
    extern int psx_card_shop_card_json(char *, unsigned, const char *);
    char name[48];
    if (!json_get_str(json, "name", name, sizeof(name))) {
        send_err(id, "missing name"); return;
    }
    char buf[512];
    if (psx_card_shop_card_json(buf, sizeof(buf), name) <= 0) {
        send_err(id, "card db not ready (load a save first)"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, buf);
}

static void handle_card_drops_state(int id, const char *json)
{
    (void)json;
    extern void psx_card_drops_debug(int *, int *, uint32_t *, int *, int *,
                                     int *, int *, int *, int *, int *, int *);
    int setting = 0, calls = 0, tier = -1, granted = 0, bails = 0;
    int new_count = 0, chest_builds = 0, overlays = 0;
    int page_duel = 0, awarded_total = 0;
    uint32_t last_ra = 0;
    psx_card_drops_debug(&setting, &calls, &last_ra, &tier, &granted, &bails,
                         &new_count, &chest_builds, &overlays,
                         &page_duel, &awarded_total);
    send_fmt("{\"id\":%d,\"ok\":true,\"setting\":%d,\"calls\":%d,"
             "\"last_ra\":\"0x%08X\",\"last_tier\":%d,\"granted\":%d,"
             "\"bails\":%d,\"new_count\":%d,\"chest_builds\":%d,"
             "\"overlays\":%d,\"page_duel\":%d,\"awarded_total\":%d}",
             id, setting, calls, last_ra, tier, granted, bails,
             new_count, chest_builds, overlays, page_duel, awarded_total);
}

#ifndef PSX_NO_DEBUG_TOOLS
static void handle_name_probe(int id, const char *json)
{
    extern void psx_name_probe_arm(int);
    extern int psx_name_probe_json(char *, unsigned);
    int arm = json_get_int(json, "arm", -1);
    if (arm >= 0) { psx_name_probe_arm(arm); send_ok(id); return; }
    static char body[32 * 1024];
    int n = psx_name_probe_json(body, (unsigned)sizeof(body));
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"entries\":%s}", id, n, body);
}
#endif /* PSX_NO_DEBUG_TOOLS */

/* card_drops_list — the cards THIS duel awarded: distinct id, copies, and
 * whether the player owned none before. Ordered new-first then by id, i.e. the
 * order the CARD DROPS results page lists them, so the page can be checked
 * against the tracker without reading pixels. */
static void handle_card_drops_list(int id, const char *json)
{
    (void)json;
    extern int psx_card_drops_list_json(char *, unsigned, int *);
    static char body[48 * 1024];
    int total = 0;
    int distinct = psx_card_drops_list_json(body, (unsigned)sizeof(body), &total);
    send_fmt("{\"id\":%d,\"ok\":true,\"distinct\":%d,\"total\":%d,\"cards\":%s}",
             id, distinct, total, body);
}

/* card_drops_p3 — the CARD DROPS results page (page 3).
 * stream=<hex> [subs=N] stages a raw text stream rendered verbatim on the
 * page (round-1 escape experiments); no args reads the page state back. */
static void handle_card_drops_p3(int id, const char *json)
{
    extern int psx_card_drops_p3_stage(const uint8_t *, int, int);
    extern void psx_card_drops_p3_state(int *, int *, int *, int *, int *,
                                        int *, int *, int *);
    char hexbuf[2048];
    if (json_get_str(json, "stream", hexbuf, sizeof(hexbuf))) {
        uint8_t bytes[1024];
        int n = 0;
        const char *p = hexbuf;
        while (p[0] && p[1] && n < (int)sizeof(bytes)) {
            char b[3] = { p[0], p[1], 0 };
            bytes[n++] = (uint8_t)strtoul(b, NULL, 16);
            p += 2;
        }
        int subs = json_get_int(json, "subs", -1);
        if (!psx_card_drops_p3_stage(bytes, n, subs)) {
            send_err(id, "stage failed"); return;
        }
        send_fmt("{\"id\":%d,\"ok\":true,\"staged\":%d}", id, n);
        return;
    }
    extern int psx_cd_overlay_needs_present(void);
    int active = 0, sub = 0, subs = 0, pending = 0, applies = 0;
    int overrides = 0, prev = 0, test_len = 0;
    psx_card_drops_p3_state(&active, &sub, &subs, &pending, &applies,
                            &overrides, &prev, &test_len);
    send_fmt("{\"id\":%d,\"ok\":true,\"active\":%d,\"sub\":%d,\"subs\":%d,"
             "\"pending\":%d,\"applies\":%d,\"overrides\":%d,"
             "\"prev_page\":%d,\"test_len\":%d,\"overlay\":%d}",
             id, active, sub, subs, pending, applies, overrides, prev,
             test_len, psx_cd_overlay_needs_present());
}

/* card_drops_layout — nudge the results page's typography and its New!
 * sprite while the page is on screen. Fields: text_y (name line), split
 * (number/count line below it), spr_x / spr_y / spr_dy (the sprite). Any
 * omitted field keeps its value; no args reads the current layout. */
static void handle_card_drops_layout(int id, const char *json)
{
    extern void psx_card_drops_layout(int, int, int, int, int, int, int);
    extern void psx_card_drops_layout_get(int *, int *, int *, int *, int *,
                                          int *, int *);
    const int keep = -100000;
    int text_y = json_get_int(json, "text_y", keep);
    int split  = json_get_int(json, "split",  keep);
    int name_x = json_get_int(json, "name_x", keep);
    int num_x  = json_get_int(json, "num_x",  keep);
    int spr_x  = json_get_int(json, "spr_x",  keep);
    int spr_y  = json_get_int(json, "spr_y",  keep);
    int spr_dy = json_get_int(json, "spr_dy", keep);
    psx_card_drops_layout(text_y, split, name_x, num_x, spr_x, spr_y, spr_dy);
    psx_card_drops_layout_get(&text_y, &split, &name_x, &num_x, &spr_x,
                              &spr_y, &spr_dy);
    send_fmt("{\"id\":%d,\"ok\":true,\"text_y\":%d,\"split\":%d,"
             "\"name_x\":%d,\"num_x\":%d,"
             "\"spr_x\":%d,\"spr_y\":%d,\"spr_dy\":%d}",
             id, text_y, split, name_x, num_x, spr_x, spr_y, spr_dy);
}

/* card_drops_set drops=N — set the CARD DROPS slider live (test loop). */
static void handle_card_drops_set(int id, const char *json)
{
    extern int psx_card_drops_set(int);
    int drops = json_get_int(json, "drops", -1);
    if (!psx_card_drops_set(drops)) { send_err(id, "bad drops"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"drops\":%d}", id, drops);
}

/* ---- duel-start fade ring -------------------------------------------------
 * The rank meter's fade ramp, sampled once per frame and stamped with the SAME
 * frame counter the display ring uses, so the host overlay's alpha and the
 * guest's own screen brightness can be compared entry-for-entry.
 *
 * A ring rather than a poll because `step` and `run_to_frame` are gone: an
 * external sampler necessarily reads a free-running guest, so its fade samples
 * and its display-ring frames land on different frames. An earlier attempt to
 * match this ramp to the game's arena fade did exactly that, inferred the
 * offset across two runs whose arm frames differed, and shipped a delay that
 * was several frames wrong. */
#define RANK_FADE_RING_CAP 256
typedef struct {
    uint32_t frame;
    int16_t  fade;      /* 0..255 alpha the meter is drawn at */
    int16_t  fade_t;    /* ramp's own frame counter */
    int16_t  anchor_x;  /* FIELD box position: shows the HUD tween in/out */
    int8_t   active;
    int8_t   anchor;    /* 0 while the HUD is off screen -> meter not drawn */
    int8_t   occluded;
} RankFadeRec;
static RankFadeRec s_rank_fade_ring[RANK_FADE_RING_CAP];
static uint64_t    s_rank_fade_ring_n;
static void rank_fade_ring_record(void)
{
    extern void psx_rank_meter_debug(int *, int *, int *, int *, int *, int *, int *);
    extern void psx_rank_meter_fade_debug(int *, int *);
    int mode = 0, active = 0, anchor = 0, occ = 0, ax = 0, ay = 0;
    int fade = 0, fade_t = 0;
    psx_rank_meter_debug(&mode, &active, &anchor, &occ, &ax, &ay, NULL);
    psx_rank_meter_fade_debug(&fade, &fade_t);
    RankFadeRec *e = &s_rank_fade_ring[s_rank_fade_ring_n % RANK_FADE_RING_CAP];
    e->frame    = (uint32_t)debug_server_frame_number();
    e->fade     = (int16_t)fade;
    e->fade_t   = (int16_t)fade_t;
    e->anchor_x = (int16_t)ax;
    e->active   = (int8_t)active;
    e->anchor   = (int8_t)anchor;
    e->occluded = (int8_t)occ;
    s_rank_fade_ring_n++;
}

/* rank_fade_ring [count=N] — newest-last window of the ramp. */
static void handle_rank_fade_ring(int id, const char *json)
{
    int count = json_get_int(json, "count", 128);
    if (count < 1) count = 1;
    if (count > RANK_FADE_RING_CAP) count = RANK_FADE_RING_CAP;
    uint64_t have = s_rank_fade_ring_n < (uint64_t)RANK_FADE_RING_CAP
                        ? s_rank_fade_ring_n : (uint64_t)RANK_FADE_RING_CAP;
    if ((uint64_t)count > have) count = (int)have;
    size_t cap = 128 + (size_t)count * 112u;
    char *buf = (char *)malloc(cap);
    if (!buf) { send_err(id, "alloc failed"); return; }
    size_t pos = (size_t)snprintf(buf, cap,
        "{\"id\":%d,\"ok\":true,\"count\":%d,\"entries\":[", id, count);
    for (int i = count; i > 0; i--) {
        RankFadeRec *e =
            &s_rank_fade_ring[(s_rank_fade_ring_n - (uint64_t)i) % RANK_FADE_RING_CAP];
        pos += (size_t)snprintf(buf + pos, cap - pos,
            "%s{\"frame\":%u,\"fade\":%d,\"fade_t\":%d,\"anchor_x\":%d,"
            "\"active\":%d,\"anchor\":%d,\"occluded\":%d}",
            (i == count) ? "" : ",", e->frame, e->fade, e->fade_t,
            e->anchor_x, e->active, e->anchor, e->occluded);
    }
    snprintf(buf + pos, cap - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

/* One place that names every command this title answers. Registration runs
 * during static initialisation, so they are live before the server's first
 * connection; the framework table is searched first, so none of these can
 * shadow a built-in.
 *
 * name_probe is debug-tools-only because the probe hooks it reads are
 * compiled out of release - registering it there would link against nothing. */
PSX_MOD_CONSTRUCTOR(psx_ygo_debug_install) {
    (void)psx_debug_add_command("rank_meter_tune",   handle_rank_meter_tune);
    (void)psx_debug_add_command("rank_meter_state",  handle_rank_meter_state);
    (void)psx_debug_add_command("rank_fade_ring",    handle_rank_fade_ring);
    (void)psx_debug_add_command("card_drops_state",  handle_card_drops_state);
    (void)psx_debug_add_command("drop_missing_state", handle_drop_missing_state);
    (void)psx_debug_add_command("drop_viewer",       handle_drop_viewer);
    (void)psx_debug_add_command("drop_viewer_set",   handle_drop_viewer_set);
    (void)psx_debug_add_command("drop_viewer_click", handle_drop_viewer_click);
    (void)psx_debug_add_command("card_packs",         handle_card_packs);
    (void)psx_debug_add_command("card_packs_reload",  handle_card_packs_reload);
    (void)psx_debug_add_command("card_effects",       handle_card_effects);
    (void)psx_debug_add_command("card_share",         handle_card_share);
    (void)psx_debug_add_command("card_colors",        handle_card_colors);
    (void)psx_debug_add_command("monster_effects",    handle_monster_effects);
    (void)psx_debug_add_command("card_manager",       handle_card_manager);
    (void)psx_debug_add_command("card_manager_set",   handle_card_manager_set);
    (void)psx_debug_add_command("card_manager_click", handle_card_manager_click);
    (void)psx_debug_add_command("card_manager_type",  handle_card_manager_type);
    (void)psx_debug_add_command("card_manager_move",  handle_card_manager_move);
    (void)psx_debug_add_command("card_manager_key",   handle_card_manager_key);
    (void)psx_debug_add_command("card_manager_shot",  handle_card_manager_shot);
    (void)psx_debug_add_command("drop_viewer_press", handle_drop_viewer_press);
    (void)psx_debug_add_command("drop_viewer_release",
                                handle_drop_viewer_release);
    (void)psx_debug_add_command("drop_viewer_move",  handle_drop_viewer_move);
    (void)psx_debug_add_command("drop_viewer_key",   handle_drop_viewer_key);
    (void)psx_debug_add_command("drop_viewer_text",  handle_drop_viewer_text);
    (void)psx_debug_add_command("drop_viewer_shot",  handle_drop_viewer_shot);
    (void)psx_debug_add_command("drop_edits",        handle_drop_edits);
    (void)psx_debug_add_command("duelist_icons",     handle_duelist_icons);
    (void)psx_debug_add_command("card_shop",         handle_card_shop);
    (void)psx_debug_add_command("card_shop_card",    handle_card_shop_card);
    (void)psx_debug_add_command("card_drops_list",   handle_card_drops_list);
    (void)psx_debug_add_command("card_drops_p3",     handle_card_drops_p3);
    (void)psx_debug_add_command("card_drops_set",    handle_card_drops_set);
    (void)psx_debug_add_command("card_drops_layout", handle_card_drops_layout);
    (void)psx_debug_add_command("card_drops_test",   handle_card_drops_test);
    (void)psx_debug_add_command("card_drops_sim",    handle_card_drops_sim);
    (void)psx_debug_add_command("fusion_db",         handle_fusion_db);
    (void)psx_debug_add_command("fusion_hand",       handle_fusion_hand);
    (void)psx_debug_add_command("fusion_list",       handle_fusion_list);
    (void)psx_debug_add_command("fusion_try",        handle_fusion_try);
    (void)psx_debug_add_command("fusion_chain",      handle_fusion_chain);
    (void)psx_debug_add_command("fusion_best",       handle_fusion_best);
    (void)psx_debug_add_command("fusion_overlay",    handle_fusion_overlay);
#ifndef PSX_NO_DEBUG_TOOLS
    (void)psx_debug_add_command("name_probe",        handle_name_probe);
#endif
    (void)psx_game_add_vblank_hook(rank_fade_ring_record);
}
