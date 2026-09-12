/* psx_card_drops.c — MODS > CARD DROPS for Yu-Gi-Oh! Forbidden Memories.
 *
 * Extracted verbatim from main.cpp (2026-08-18) so the guest-side knowledge
 * for this feature lives in one place instead of a thousand lines inside the
 * host's main file. See psx_card_drops.h for the interface; the rationale for
 * every address and mechanism is in the comments below, where it was written
 * as each piece was measured.
 *
 * The only edits made during extraction were mechanical: C++ `extern "C"` and
 * `std::` qualifiers dropped (this is a C translation unit now), the hook
 * registration that used to sit in main.cpp's startup moved in beside the
 * addresses it registers, and the frame tick given an exported name.
 */

#include "psx_card_drops.h"

#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_inventory.h"
#include "psx_cd_overlay.h"
#include "psx_drop_edits.h"
#include "psx_game_hooks.h"
#include "psx_story_rewards.h"
#include "psx_video_menu.h"
#include "psx_ygo_netplay.h"

/* Small present-only message used when an authored zero count or exhausted
 * Smart pool suppresses the in-flight award. Keeping this out of guest text
 * memory avoids inventing a card id or leaving string-table state behind
 * after RESULTS OF DUEL closes. */
#include "psx_ui_font8.inc"

/* ---- MODS > CARD DROPS ----------------------------------------------------
 *
 * Award N cards for a won duel instead of 1, by re-running the GAME'S OWN drop
 * roll — not by picking cards ourselves. That is the whole point: the pool a
 * drop comes from depends on the opponent and on your duel rank, and the game
 * already encodes both. Rolling here would mean reimplementing that and
 * silently drifting from it.
 *
 * At 2+ the cards are dealt with the COMMUNITY DROP MOD's exact RNG pattern
 * (decoded 2026-08-30 from the 15- and 5-card mod discs, whose overlay-space
 * patch is the version that actually runs): the stock roll consumes one
 * rand() call but its card is never awarded, then each of the N cards burns
 * six rand() calls and rolls on the seventh — N cards from exactly 1 + 7N
 * calls. Matching that stream bit for bit is what lets a duel played here be
 * verified against (or by) the community's modded-ISO tooling: same seed,
 * same actions, same cards. The burned calls are applied by advancing the
 * seed word directly — arithmetically identical to discarded rand() calls —
 * cards 1..N-1 roll through func_80021810 itself, and card N IS the game's
 * own in-flight roll and award, which the hook steers onto the pattern's
 * last roll position. No code is patched: the first version NOPed the stock
 * rand and award instead, and those per-frame writes dirtied the region
 * holding the results-screen state function, killing its entry hook and the
 * CARD DROPS page's navigation with it (see the on_roll hook's comment).
 *
 * The two guest routines, established by write-tracing the trunk during a real
 * duel reward and then reading the generated C for the enclosing functions
 * (2026-08-17):
 *
 *   func_80021810(a0 = tier) -> v0 = card id
 *       table = 0x8017878C + tier*1460; roll = rng() & 0x7FF + 1; walks 722
 *       cumulative n-in-2048 weights and returns the first index whose running
 *       sum reaches the roll, +1. Returns 0 if the table is empty. The table
 *       belongs to the CURRENT opponent and is reloaded per duel, so calling
 *       this again draws from exactly the pool the game would have used.
 *
 *   func_80021894(a0 = card id)
 *       trunk byte at 0x801D024F + card_id, +1, capped at 251.
 *
 * `tier` is recomputed the same way the game does at 0x80021C44..0x80021C58,
 * from two bytes in the duel-results block — rather than captured from a live
 * register, so this does not depend on having observed the game's own call.
 *
 * Hooked at func_80021894's ENTRY, filtered to the duel-drop call site by $ra.
 * Normal bonus cards are awarded from this prologue. A guaranteed campaign
 * reward is deliberately awarded first; the visible stock roll is steered to
 * that card so SPOILS remains truthful, then its eventual award argument is
 * changed to the previewed final normal card. This preserves both visible and
 * banked award order without changing the RNG stream. */
static int g_card_drops = PSX_VM_CARD_DROPS_DEFAULT;
static int s_card_drops_row = -1;

#define PSX_DROP_ROLL_FN    0x80021810u
#define PSX_DROP_AWARD_FN   0x80021894u
/* The game's own post-call addresses. psx_dispatch_call uses these purely as
 * the stop condition for the nested call; nothing executes at them here. */
#define PSX_DROP_ROLL_RET   0x80021C68u
#define PSX_DROP_AWARD_RET  0x80021F1Cu
/* Call site of the duel reward's award, i.e. the $ra we must see to know this
 * is the end-of-duel drop and not some other card grant. */
#define PSX_DROP_AWARD_SITE 0x80021F1Cu
/* $ra at the roll's single call site (0x80021C60 jal, link 0x80021C68). */
#define PSX_DROP_ROLL_SITE  0x80021C68u

/* The community drop pattern's one moving part: the rand seed word, advanced
 * arithmetically for the discarded call and the six burns per card. NO code
 * is patched for the pattern — the stock roll and award ARE the pattern's
 * final card; see the long comment in the on_roll hook for why (and for the
 * results-screen paging bug the patched first version caused). */
#define PSX_RNG_SEED_ADDR        0x800FE6F8u
#define PSX_DROP_TABLE_ADDR      0x8017878Cu
#define PSX_DROP_TIER_STRIDE     1460u
#define PSX_DROP_TIER_COUNT      3u

static uint32_t cd_lcg_advance(uint32_t s, int n) {
    while (n--) s = s * 0x41C64E6Du + 0x3039u;
    return s;
}

/* ---- extended "New!" list -------------------------------------------------
 *
 * The chest's New! label and its sort-to-top both read a per-card flag byte at
 * 0x8010606A + card_id (UI work buffer, NOT the save struct). The guest fills
 * it at the top of func_800323F8, the chest display-list builder: for each
 * card 1..722 it clears the byte (sb $zero at 0x800324B4), then re-marks the
 * cards found in the 16-slot ring at 0x801D07BC that the award routine pushes.
 * So no matter how many cards CARD DROPS grants, at most the ring's 16 ever
 * show as New!. Both readers (0x80031A7C, and the sort at 0x80033004) only
 * test the byte for nonzero.
 *
 * The extension therefore only has to REWRITE that array between the
 * builder's flag pass and the first read. That window is crossed by a guest
 * call: the builder jals its sort/insert body func_80032C48 (links 0x800325E4
 * and 0x80032710), and the flag consumption happens inside that body. So:
 * hook the BUILDER entry to arm, hook the SORT entry (filtered to the
 * builder's two call sites by $ra) to write the whole array, first call only
 * — 1 for each card the last duel awarded, 0 for every other card.
 *
 * Clearing the rest is the point, not housekeeping. The ring is a ROLLING
 * 16-slot list that the game never empties per duel, so stock "New!" means
 * "the last 16 cards you acquired", spanning as many duels as that took. An
 * add-only overlay therefore left up to 15 cards from previous duels labelled
 * New — and, because our set was then a subset of the ring, changed nothing
 * at all below 16 cards, making the whole feature a silent no-op at low
 * settings. Rewriting makes "New!" mean "this duel" at every setting.
 *
 * The byte is a recency RANK, not a boolean: the guest walks the ring writing
 * 16 (oldest) down to 1 (newest, at the ring head), and the chest sorts on it.
 * Writing 1 for all of this duel's cards says "all equally newest", which is
 * what groups them at the top.
 *
 * Deliberately NOT done by patching the guest's per-card clear to NOP and
 * replaying it host-side: psx_mod_write_code_word inside the 256-byte window
 * behind the builder's entry makes dirty_ram_text_native_ok() sticky-diverge
 * the function to the dirty-RAM interpreter, and interpreted dispatches never
 * run psx_mod_function_entry — the patch would disarm its own hook after one
 * build. This way modifies no guest code at all. */
#define PSX_DROP_CHEST_BUILD_FN     0x800323F8u
#define PSX_DROP_CHEST_SORT_FN      0x80032C48u
/* $ra at the builder's two sort/insert call sites (jal links). */
#define PSX_DROP_CHEST_SORT_SITE1   0x800325E4u
#define PSX_DROP_CHEST_SORT_SITE2   0x80032710u
#define PSX_DROP_NEWFLAG_BASE       0x8010606Au /* + card_id */
#define PSX_DROP_CARD_ID_MAX        722

/* Observability for the hook, queryable as `card_drops_state`. Without it the
 * only symptom of a mis-wire is "no extra cards", which is indistinguishable
 * between: hook never called, wrong $ra filter, setting not applied, or the
 * nested call bailing. */
static int      s_cd_calls;        /* times the callback ran */
static uint32_t s_cd_last_ra;      /* $ra it saw (should be the drop site) */
static int      s_cd_last_tier = -1;
static int      s_cd_granted;      /* extras actually awarded, cumulative */
static int      s_cd_bails;

/* Cards the LAST duel awarded (extras included), for the extended New! list.
 * Reset when the roll hook sees a fresh duel drop, so "New!" means "won in
 * the most recent duel" — the natural reading once a duel can grant 99. */
static uint8_t  s_cd_new_this_duel[PSX_DROP_CARD_ID_MAX + 1];
static int      s_cd_new_distinct; /* nonzero entries above */
/* COPIES of each id this duel awarded, and whether the player owned NONE of it
 * before the duel. The count array is what the results page lists as "xN"; the
 * bool is the "New!" mark.
 *
 * "Was new" has to be sampled at the AWARD ENTRY, before func_80021894 runs:
 * the award increments the trunk byte, so one instruction later a first copy
 * and a second copy are indistinguishable. Sampled once per id per duel (on
 * the transition to count 1) for the same reason — the mod's own extra copies
 * arrive through this hook too, and by the second one the byte is already 1. */
static uint8_t  s_cd_copies_this_duel[PSX_DROP_CARD_ID_MAX + 1];
static uint8_t  s_cd_was_new_this_duel[PSX_DROP_CARD_ID_MAX + 1];
static int      s_cd_awarded_total;  /* sum of s_cd_copies_this_duel */
static uint16_t s_cd_award_order[PSX_VM_CARD_DROPS_MAX];
static uint8_t  s_cd_order_committed[PSX_VM_CARD_DROPS_MAX];
static int      s_cd_order_n;
static uint32_t s_cd_pending_card;   /* previewed final stock-table award */
static int      s_cd_pending_order = -1;
static int      s_cd_rewrite_award;  /* visible roll is story; bank pending */
static int      s_cd_award_is_story;
static uint8_t  s_cd_order_story[PSX_VM_CARD_DROPS_MAX];
static int      s_cd_chest_builds; /* chest-builder hook invocations */
static int      s_cd_chest_armed;  /* builder seen, overlay not yet written */
/* A real duel drop has been observed this session, so s_cd_new_this_duel is
 * meaningful and the overlay may take over the flag array. Before the first
 * duel it stays clear and the save's own ring shows through untouched — the
 * right thing for a player who just loaded a save and walked to the chest.
 * Not restored by save states (host statics never are), so a state loaded
 * from before a duel keeps that duel's list; harmless, and it beats the
 * alternative of the list vanishing whenever a state is loaded. */
static int      s_cd_have_duel;
/* Like s_cd_have_duel, but scoped to ONE results screen: the drops page must
 * describe the duel whose results are on screen, never the last one that
 * awarded something. The roll (a win-only path - a loss never calls it) sets
 * it; the tick clears it once the results screen has been gone for a few
 * frames. Without this, losing a duel after a winning one showed the previous
 * duel's cards on the drops page. */
static int      s_cd_page_duel;
static int      s_cd_overlays;     /* overlay passes actually written */

/* Card Drops = 0 is an intentional no-normal-award mode. The game still
 * performs its one stock roll (one RNG call), because the result routine owns
 * that call and uses the returned card to construct its summary. The award
 * entry is skipped instead. An eligible guaranteed campaign reward remains a
 * separate award and uses that in-flight stock roll as its carrier.
 *
 * Smart exhaustion shares the presentation when there is no truthful story
 * reward in SPOILS. Without the present-only cover the stock summary would
 * announce the rolled card even though it was deliberately not banked. This
 * panel covers only the card-number/name strip below SPOILS and says what
 * actually happened. */
#define PSX_DROP_NO_CARD_X 12
#define PSX_DROP_NO_CARD_Y 200
#define PSX_DROP_NO_CARD_W 296
#define PSX_DROP_NO_CARD_H 34
static uint32_t s_cd_no_card_canvas[PSX_DROP_NO_CARD_W * PSX_DROP_NO_CARD_H];
static int      s_cd_no_card_canvas_reason;
static int      s_cd_no_card_reason;
static unsigned s_cd_zero_duels;
static unsigned s_cd_zero_story_duels;
static unsigned s_cd_suppressed_awards;
static unsigned s_cd_zero_suppressed;
static unsigned s_cd_zero_rng_calls_last;
static uint32_t s_cd_zero_seed_before;
static uint32_t s_cd_zero_seed_after;
static int      s_cd_zero_seed_pending;
static int      s_cd_zero_rng_exact;
static int      s_cd_no_card_present_hold;
static int      s_cd_no_card_placement[10];

void psx_card_drops_debug(int *setting, int *calls, uint32_t *last_ra,
                                     int *last_tier, int *granted, int *bails,
                                     int *new_count, int *chest_builds,
                                     int *overlays, int *page_duel,
                                     int *awarded_total) {
    if (setting)      *setting      = g_card_drops;
    if (calls)        *calls        = s_cd_calls;
    if (last_ra)      *last_ra      = s_cd_last_ra;
    if (last_tier)    *last_tier    = s_cd_last_tier;
    if (granted)      *granted      = s_cd_granted;
    if (bails)        *bails        = s_cd_bails;
    if (new_count)    *new_count    = s_cd_new_distinct;
    if (chest_builds) *chest_builds = s_cd_chest_builds;
    if (overlays)     *overlays     = s_cd_overlays;
    if (page_duel)    *page_duel    = s_cd_page_duel;
    if (awarded_total) *awarded_total = s_cd_awarded_total;
}

/* This duel's award list, sorted the way the results page shows it: cards the
 * player owned NONE of first, then by card id. Emitted as JSON for
 * `card_drops_list` so the tracker can be checked against a simulated drop
 * without winning a duel. Returns the number of DISTINCT cards written. */
int psx_card_drops_list_json(char *out, unsigned cap, int *out_total) {
    if (out_total) *out_total = s_cd_awarded_total;
    if (!out || cap < 32u) return 0;
    unsigned n = 0;
    int distinct = 0;
    n += (unsigned)snprintf(out + n, cap - n, "[");
    uint8_t seen[PSX_DROP_CARD_ID_MAX + 1]; memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_cd_order_n && n + 80u < cap; i++) {
        const uint32_t id = s_cd_award_order[i];
        if (!id || seen[id]) continue;
        seen[id] = 1;
        n += (unsigned)snprintf(out + n, cap - n,
                               "%s{\"id\":%u,\"n\":%u,\"new\":%d,\"first\":%d}",
                               distinct ? "," : "", id,
                               s_cd_copies_this_duel[id],
                               s_cd_was_new_this_duel[id] ? 1 : 0, i);
        distinct++;
    }
    snprintf(out + n, cap - n, "]");
    return distinct;
}

int psx_card_drops_order_json(char *out, unsigned cap)
{
    if (!out || cap < 8u) return 0;
    unsigned n = (unsigned)snprintf(out, cap, "[");
    for (int i = 0; i < s_cd_order_n && n + 48u < cap; i++)
        n += (unsigned)snprintf(out + n, cap - n,
                               "%s{\"id\":%u,\"kind\":\"%s\",\"committed\":%s}", i ? "," : "",
                               (unsigned)s_cd_award_order[i],
                               s_cd_order_story[i] ? "story" : "normal",
                               s_cd_order_committed[i] ? "true" : "false");
    snprintf(out + n, cap - n, "]");
    return s_cd_order_n;
}

/* ---- name-stream probe ----------------------------------------------------
 *
 * Pins the compressed card-name blob's base address, which no amount of
 * searching guest RAM could: the names are dictionary-compressed (0xFF nn
 * escape pairs, SPACE = 0x00), so every plain-string scan comes back empty.
 *
 * The measurement instead reads the game's own answer. func_80037DA4 is the
 * text engine; on entry, a0 is the widget and the widget holds the stream
 * cursor at obj[obj[88] * 4], still pointing at the FIRST byte of the string it
 * is about to decode. With a known card id on the results screen, that cursor
 * IS `blob_base + T[card_id]`, so the base falls out by subtraction.
 *
 * func_80036C14(obj, ch) then receives every DECODED character, which reads the
 * name out in the clear without implementing the decompressor — the check that
 * the base is right rather than merely self-consistent.
 *
 * Diagnostic scaffolding, compiled in only for the debug tools build. */
#ifndef PSX_NO_DEBUG_TOOLS
#define PSX_NP_TEXT_FN      0x80037DA4u
#define PSX_NP_GLYPH_FN     0x80036C14u
#define PSX_NP_MAX          24
typedef struct PsxNameProbe {
    uint32_t obj;        /* widget */
    int      slot;       /* obj[88], the cursor slot index */
    uint32_t cursor;     /* stream pointer at entry = blob_base + T[id] */
    int      nchars;
    uint8_t  chars[64];  /* decoded characters, in order */
} PsxNameProbe;
static PsxNameProbe s_np[PSX_NP_MAX];
static int          s_np_count;
static int          s_np_armed;

void psx_name_probe_arm(int on) {
    if (on) { memset(s_np, 0, sizeof s_np); s_np_count = 0; }
    s_np_armed = on ? 1 : 0;
}

void psx_mod_name_probe_on_text(CPUState *cpu, uint32_t address) {
    if (!cpu || address != PSX_NP_TEXT_FN || !s_np_armed) return;
    if (s_np_count >= PSX_NP_MAX) return;
    const uint32_t obj = cpu->gpr[4];
    const int slot = (int8_t)psx_mod_read_byte(obj + 88);
    PsxNameProbe *e = &s_np[s_np_count++];
    e->obj = obj;
    e->slot = slot;
    /* Negative slots are the label streams; only a real slot indexes a cursor. */
    e->cursor = (slot >= 0 && slot < 12) ? psx_mod_read_word(obj + (uint32_t)slot * 4u) : 0u;
    e->nchars = 0;
}

void psx_mod_name_probe_on_glyph(CPUState *cpu, uint32_t address) {
    if (!cpu || address != PSX_NP_GLYPH_FN || !s_np_armed || !s_np_count) return;
    /* Attribute the character to the most recent text-engine entry for this
     * widget; the engine decodes one string at a time, so that is the owner. */
    for (int i = s_np_count - 1; i >= 0; i--) {
        if (s_np[i].obj != cpu->gpr[4]) continue;
        if (s_np[i].nchars < (int)sizeof s_np[i].chars)
            s_np[i].chars[s_np[i].nchars++] = (uint8_t)(cpu->gpr[5] & 0xFFu);
        return;
    }
}

int psx_name_probe_json(char *out, unsigned cap) {
    if (!out || cap < 64u) return 0;
    unsigned n = 0;
    n += (unsigned)snprintf(out + n, cap - n, "[");
    for (int i = 0; i < s_np_count && n + 300u < cap; i++) {
        const PsxNameProbe *e = &s_np[i];
        n += (unsigned)snprintf(out + n, cap - n,
                                     "%s{\"obj\":\"0x%08X\",\"slot\":%d,"
                                     "\"cursor\":\"0x%08X\",\"chars\":[",
                                     i ? "," : "", e->obj, e->slot, e->cursor);
        for (int k = 0; k < e->nchars && n + 8u < cap; k++)
            n += (unsigned)snprintf(out + n, cap - n, "%s%u",
                                         k ? "," : "", e->chars[k]);
        n += (unsigned)snprintf(out + n, cap - n, "]}");
    }
    snprintf(out + n, cap - n, "]");
    return s_np_count;
}
#endif /* PSX_NO_DEBUG_TOOLS */

/* Guards the hook against the rolls and awards we drive ourselves. */
static int s_cd_busy;

/* ---- smart normal drops -------------------------------------------------
 *
 * The resident row is already the fully composed table for this opponent and
 * rank (stock + editor + missing-card transforms). Before EVERY normal award,
 * remove weighted cards already owned at three copies and scale the survivors
 * back to the guest's exact 2048 total. Each early bonus award increments the
 * trunk before the next filter is built, so later rolls include cards already
 * earned in this duel and can never push a card beyond three total copies.
 * The table is restored byte-for-byte after each roll. */
static uint16_t s_smart_saved[PSX_DROP_CARD_ID_MAX];
static uint32_t s_smart_addr;
static int s_smart_active;
static unsigned s_smart_attempts;
static unsigned s_smart_applied;
static unsigned s_smart_unchanged;
static unsigned s_smart_fallbacks;
static unsigned s_smart_restores;
static unsigned s_smart_skipped;
static unsigned s_smart_skipped_duel;
static int s_smart_last_tier = -1;
static int s_smart_last_eligible;
static int s_smart_last_excluded;
static int s_smart_last_capacity;
static uint32_t s_smart_last_eligible_weight;
static uint32_t s_smart_last_card;
enum {
    CD_SUPPRESS_NONE = 0,
    CD_SUPPRESS_SMART_EXHAUSTED,
    CD_SUPPRESS_ZERO_NORMAL,
};
static int s_cd_suppress_award;

static uint32_t cd_table_addr(unsigned tier)
{
    return PSX_DROP_TABLE_ADDR + tier * PSX_DROP_TIER_STRIDE;
}

/* Build the filtered row without touching the guest. Stable card-ID tie
 * breaking in the largest-remainder pass makes the integer redistribution
 * deterministic. Since the source total is 2048, scaling after removals is
 * upward and every surviving nonzero card keeps at least one unit. */
static int cd_smart_build(unsigned tier,
                          uint16_t original[PSX_DROP_CARD_ID_MAX],
                          uint16_t filtered[PSX_DROP_CARD_ID_MAX],
                          int *eligible, int *excluded,
                          uint32_t *eligible_weight, int *capacity,
                          int *fallback)
{
    if (tier >= PSX_DROP_TIER_COUNT) return 0;
    PsxCardInventorySnapshot inv;
    if (!psx_card_inventory_snapshot(&inv)) return 0;

    const uint32_t base = cd_table_addr(tier);
    uint32_t sum = 0, keep_sum = 0;
    int keep_n = 0, cut_n = 0, slots = 0;
    for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++) {
        const uint16_t w = psx_mod_read_half(base + (uint32_t)i * 2u);
        original[i] = w;
        filtered[i] = 0;
        sum += w;
        if (!w) continue;
        const unsigned owned = (unsigned)inv.deck[i] + (unsigned)inv.trunk[i];
        if (owned < PSX_CARD_INVENTORY_KEEP) {
            keep_sum += w;
            keep_n++;
            slots += PSX_CARD_INVENTORY_KEEP - (int)owned;
        } else {
            cut_n++;
        }
    }
    if (eligible) *eligible = keep_n;
    if (excluded) *excluded = cut_n;
    if (eligible_weight) *eligible_weight = keep_sum;
    if (capacity) *capacity = slots;
    if (fallback) *fallback = keep_sum == 0;
    if (!sum) return 0;
    if (!keep_sum) return 1;

    uint32_t remainder[PSX_DROP_CARD_ID_MAX];
    uint8_t used[PSX_DROP_CARD_ID_MAX];
    memset(remainder, 0, sizeof remainder);
    memset(used, 0, sizeof used);
    uint32_t got = 0;
    for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++) {
        if (!original[i] ||
            (unsigned)inv.deck[i] + (unsigned)inv.trunk[i] >=
                PSX_CARD_INVENTORY_KEEP)
            continue;
        const uint32_t product = (uint32_t)original[i] * 2048u;
        filtered[i] = (uint16_t)(product / keep_sum);
        remainder[i] = product % keep_sum;
        got += filtered[i];
    }
    while (got < 2048u) {
        int best = -1;
        uint32_t best_rem = 0;
        for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++) {
            if (used[i] || !filtered[i]) continue;
            if (best < 0 || remainder[i] > best_rem) {
                best = i;
                best_rem = remainder[i];
            }
        }
        if (best < 0) return 0;
        used[best] = 1;
        filtered[best]++;
        got++;
    }
    return got == 2048u;
}

static void cd_smart_restore(void)
{
    if (!s_smart_active) return;
    s_smart_active = 0;
    for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++)
        psx_mod_write_half(s_smart_addr + (uint32_t)i * 2u,
                           s_smart_saved[i]);
    s_smart_restores++;
}

/* Returns 1 when the attempt has at least one eligible card, -1 when every
 * weighted card is already owned at three copies, and 0 for an invalid source
 * row. s_smart_active means the caller must restore after its roll (or let the
 * real award hook restore an in-flight final roll). */
static int cd_smart_prepare(unsigned tier)
{
    uint16_t filtered[PSX_DROP_CARD_ID_MAX];
    int eligible = 0, excluded = 0, capacity = 0, fallback = 0;
    uint32_t eligible_weight = 0;
    s_smart_attempts++;
    s_smart_last_tier = (int)tier;
    s_smart_last_card = 0;
    if (!cd_smart_build(tier, s_smart_saved, filtered,
                        &eligible, &excluded, &eligible_weight, &capacity,
                        &fallback)) {
        s_smart_last_eligible = 0;
        s_smart_last_excluded = 0;
        s_smart_last_capacity = 0;
        s_smart_last_eligible_weight = 0;
        s_smart_fallbacks++;
        return 0;
    }
    s_smart_last_eligible = eligible;
    s_smart_last_excluded = excluded;
    s_smart_last_capacity = capacity;
    s_smart_last_eligible_weight = eligible_weight;
    if (fallback) {
        s_smart_fallbacks++;
        return -1;
    }
    if (!excluded) {
        s_smart_unchanged++;
        return 1;
    }
    s_smart_addr = cd_table_addr(tier);
    for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++)
        psx_mod_write_half(s_smart_addr + (uint32_t)i * 2u, filtered[i]);
    s_smart_active = 1;
    s_smart_applied++;
    return 1;
}

static int cd_smart_configured(void)
{
    return psx_drop_edits_smart_drop();
}

static int cd_smart_effective(void)
{
    /* The preference remains saved at zero so raising Card Drops later needs
     * no second edit, but there is no normal award for it to filter now. */
    return g_card_drops > 0 && cd_smart_configured() &&
           !psx_ygo_netplay_session();
}

/* Hooked on the ROLL, not the award.
 *
 * The award looked like the natural place, but the tier is not recoverable
 * there. The game derives it from two bytes in the results block (+56/+57)
 * which are rewritten between the roll and the award, so an award-time
 * recompute described a different duel: it yielded tier 0, whose table is
 * empty, and every extra roll returned card 0. Capturing the tier at the roll
 * and using it later fails too — a save state taken between the two (the
 * results screen is exactly such a point) replays the award with no roll to
 * observe.
 *
 * At the roll the tier is simply $a0, correct by construction, with no state to
 * carry and nothing to reconstruct. func_80021810 has exactly ONE caller in the
 * whole game — the duel reward — so every invocation here is a real drop.
 *
 * Bonus awards are normally granted before the game's own roll. Guaranteed
 * story rewards are the ordering exception documented in the file header: the
 * reward is granted first and the in-flight award becomes the final normal
 * card after its visible SPOILS panel has shown the reward. */
/* The two nested-call primitives. The hook and the test command both go
 * through these, so validating the command validates the shipping path.
 *
 * $ra matters as much as the stop address: psx_dispatch_impl only runs the
 * continuation when the guest returns with $ra == stop_addr and the $sp it was
 * called with, and `jr $ra` uses the REGISTER. Passing stop_addr alone left the
 * chain unfinished and $v0 garbage — which is why the first working roll and
 * the first working award both needed this line. */
static uint32_t cd_roll_one(CPUState *cpu, uint32_t tier, int *bail) {
    cpu->pc = 0;
    cpu->gpr[4] = tier;
    cpu->gpr[31] = PSX_DROP_ROLL_RET;
    s_cd_busy = 1;
    psx_dispatch_call(cpu, PSX_DROP_ROLL_FN, PSX_DROP_ROLL_RET);
    s_cd_busy = 0;
    if (bail) *bail = (g_psx_call_bail || cpu->pc != 0);
    return cpu->gpr[2] & 0xFFFFu;
}

static void cd_award_one(CPUState *cpu, uint32_t card, int *bail) {
    cpu->pc = 0;
    cpu->gpr[4] = card;
    cpu->gpr[31] = PSX_DROP_AWARD_RET;
    s_cd_busy = 1;
    psx_dispatch_call(cpu, PSX_DROP_AWARD_FN, PSX_DROP_AWARD_RET);
    s_cd_busy = 0;
    if (bail) *bail = (g_psx_call_bail || cpu->pc != 0);
}

static uint32_t cd_preview_roll(CPUState *cpu, uint32_t tier, int *bail)
{
    const uint32_t seed = psx_mod_read_word(PSX_RNG_SEED_ADDR);
    CPUState saved = *cpu;
    const uint32_t card = cd_roll_one(cpu, tier, bail);
    psx_mod_write_word(PSX_RNG_SEED_ADDR, seed);
    *cpu = saved;
    return card;
}

static void cd_track(uint32_t id, int committed, int story)
{
    if (id < 1 || id > PSX_DROP_CARD_ID_MAX) return;
    if (!s_cd_new_this_duel[id]) {
        s_cd_new_this_duel[id] = 1;
        s_cd_new_distinct++;
        s_cd_was_new_this_duel[id] =
            psx_card_inventory_owned((int)id) == 0 ? 1u : 0u;
    }
    if (s_cd_copies_this_duel[id] < 255u) s_cd_copies_this_duel[id]++;
    s_cd_awarded_total++;
    if (s_cd_order_n < PSX_VM_CARD_DROPS_MAX) {
        const int at = s_cd_order_n++;
        s_cd_award_order[at] = (uint16_t)id;
        s_cd_order_committed[at] = committed ? 1u : 0u;
        s_cd_order_story[at] = story ? 1u : 0u;
        if (!committed) { s_cd_pending_card = id; s_cd_pending_order = at; }
    }
}

static void cd_discard_pending(void)
{
    if (s_cd_pending_order < 0 || s_cd_pending_order != s_cd_order_n - 1) return;
    const uint32_t id = s_cd_pending_card;
    s_cd_order_n--;
    if (id >= 1 && id <= PSX_DROP_CARD_ID_MAX) {
        if (s_cd_copies_this_duel[id]) s_cd_copies_this_duel[id]--;
        if (s_cd_awarded_total) s_cd_awarded_total--;
        if (!s_cd_copies_this_duel[id]) {
            s_cd_new_this_duel[id] = 0;
            s_cd_was_new_this_duel[id] = 0;
            if (s_cd_new_distinct) s_cd_new_distinct--;
        }
    }
    s_cd_pending_card = 0; s_cd_pending_order = -1;
}

/* Simulate one duel drop END TO END, exercising the REAL hook.
 *
 * The roll is dispatched WITHOUT the busy guard, so func_80021810's entry fires
 * psx_mod_card_drops_on_roll exactly as a duel does — same entry, same $a0,
 * same $ra — and the hook grants its extras. The card the roll itself returns
 * is then awarded here, standing in for the game's own award later in the
 * results sequence. Total landed should equal the configured CARD DROPS.
 *
 * This exists so the setting can be swept across its range without winning a
 * duel per value; the harness was calibrated against a real duel first (10 ->
 * exactly 10), so it is a shortcut for repetition, not a substitute for the
 * real path. `drops` temporarily overrides the setting for one simulation. */
int psx_card_drops_simulate(CPUState *cpu, int tier, int drops,
                                       uint32_t *out_card, int *out_granted,
                                       int *out_bail,
                                       uint32_t *out_seed_before,
                                       uint32_t *out_seed_after) {
    if (!cpu) return 0;
    const int saved_setting = g_card_drops;
    if (drops >= 0 && drops <= PSX_VM_CARD_DROPS_MAX) g_card_drops = drops;
    const int granted_before = s_cd_granted;
    const uint32_t seed_before = psx_mod_read_word(PSX_RNG_SEED_ADDR);

    CPUState saved = *cpu;
    int bail = 0;
    /* No busy guard: this is the point — the hook must fire. */
    cpu->pc = 0;
    cpu->gpr[4] = (uint32_t)tier;
    cpu->gpr[31] = PSX_DROP_ROLL_RET;
    psx_dispatch_call(cpu, PSX_DROP_ROLL_FN, PSX_DROP_ROLL_RET);
    bail = (g_psx_call_bail || cpu->pc != 0);
    const uint32_t card = cpu->gpr[2] & 0xFFFFu;
    /* The in-flight stock roll is a REAL award at every setting now: at 2+
     * it is the pattern's card N (the hook granted only N-1). */
    if (!bail && card >= 1 && card <= 722)
        cd_award_one(cpu, card, &bail);
    *cpu = saved;

    if (out_card)    *out_card    = card;
    if (out_granted) *out_granted = s_cd_granted - granted_before;
    if (out_bail)    *out_bail    = bail;
    if (out_seed_before) *out_seed_before = seed_before;
    if (out_seed_after)  *out_seed_after =
        psx_mod_read_word(PSX_RNG_SEED_ADDR);
    g_card_drops = saved_setting;
    return 1;
}

/* One nested roll on demand, for `card_drops_test`. Exists because the only
 * other way to exercise the guest-call path is to win a duel, and a wrong
 * answer there costs a full playthrough to retry. Reports what the call
 * actually produced rather than only whether cards appeared. */
int psx_card_drops_test_roll(CPUState *cpu, int tier, int do_award,
                                        uint32_t *out_card, uint32_t *out_pc,
                                        int *out_bail) {
    if (!cpu) return 0;
    CPUState saved = *cpu;
    int bail = 0;
    const uint32_t card = cd_roll_one(cpu, (uint32_t)tier, &bail);
    if (!bail && do_award && card >= 1 && card <= 722)
        cd_award_one(cpu, card, &bail);
    if (out_card) *out_card = card;
    if (out_pc)   *out_pc   = cpu->pc;
    if (out_bail) *out_bail = bail;
    *cpu = saved;
    return 1;
}

void psx_mod_card_drops_on_roll(CPUState *cpu, uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_ROLL_FN || s_cd_busy) return;
    s_cd_calls++;
    s_cd_last_ra = cpu->gpr[31];
    if (cpu->gpr[31] != PSX_DROP_ROLL_SITE) return;   /* not the duel drop */

    /* A previous final roll should have restored at its award. Recover here
     * as well so a debug interruption cannot leak a filtered row into a new
     * duel. */
    cd_smart_restore();

    /* A fresh duel drop: from here on "New!" means THIS duel. Cleared before
     * the extra<=0 return so the set tracks the single stock card too — the
     * chest hook decides separately whether the extended list is in effect. */
    memset(s_cd_new_this_duel, 0, sizeof s_cd_new_this_duel);
    memset(s_cd_copies_this_duel, 0, sizeof s_cd_copies_this_duel);
    memset(s_cd_was_new_this_duel, 0, sizeof s_cd_was_new_this_duel);
    memset(s_cd_award_order, 0, sizeof s_cd_award_order);
    memset(s_cd_order_committed, 0, sizeof s_cd_order_committed);
    memset(s_cd_order_story, 0, sizeof s_cd_order_story);
    s_cd_new_distinct = 0;
    s_cd_awarded_total = 0;
    s_cd_order_n = 0;
    s_cd_pending_card = 0;
    s_cd_pending_order = -1;
    s_cd_rewrite_award = 0;
    s_cd_award_is_story = 0;
    s_cd_suppress_award = CD_SUPPRESS_NONE;
    s_smart_skipped_duel = 0;
    s_cd_no_card_reason = CD_SUPPRESS_NONE;
    s_cd_zero_rng_calls_last = 0;
    s_cd_zero_seed_before = 0;
    s_cd_zero_seed_after = 0;
    s_cd_zero_seed_pending = 0;
    s_cd_zero_rng_exact = 0;
    s_cd_have_duel = 1;
    s_cd_page_duel = 1;

    const uint32_t tier = cpu->gpr[4] & 0xFFu;
    s_cd_last_tier = (int)tier;

    /* At zero, a guaranteed campaign reward remains separate and is carried
     * by the game's in-flight roll. With no eligible story reward, the stock
     * roll still consumes its ordinary one RNG call, but its later award is
     * skipped and the summary's misleading card strip is covered. */
    if (g_card_drops == 0) {
        const int story = tier < 3 ? psx_story_rewards_select() : 0;
        s_cd_zero_duels++;
        s_cd_zero_seed_before = psx_mod_read_word(PSX_RNG_SEED_ADDR);
        s_cd_zero_seed_pending = 1;
        if (story) {
            s_cd_zero_story_duels++;
            cd_track((uint32_t)story, 0, 1);
            psx_story_rewards_steer_card(cpu, tier, story);
        } else {
            s_cd_no_card_reason = CD_SUPPRESS_ZERO_NORMAL;
            s_cd_no_card_present_hold = 3;
            s_cd_suppress_award = CD_SUPPRESS_ZERO_NORMAL;
        }
        return;
    }

    /* With one drop, the in-flight stock roll is the only award. Preview it
     * for exact-order evidence, or steer it directly to an eligible story
     * reward. psx_story_rewards.c restores the table at the award hook. */
    if (g_card_drops < 2) {
        const int story = tier < 3 ? psx_story_rewards_select() : 0;
        const int smart = !story && cd_smart_effective();
        int bail = 0;
        const int prepared = smart ? cd_smart_prepare(tier) : 1;
        if (smart && prepared < 0) {
            s_cd_suppress_award = CD_SUPPRESS_SMART_EXHAUSTED;
            s_cd_no_card_reason = CD_SUPPRESS_SMART_EXHAUSTED;
            return;
        }
        const uint32_t card = story ? (uint32_t)story : cd_preview_roll(cpu, tier, &bail);
        if (smart) s_smart_last_card = card;
        if (!bail && card >= 1 && card <= PSX_DROP_CARD_ID_MAX)
            cd_track(card, 0, story != 0);
        else {
            cd_smart_restore();
            if (bail) s_cd_bails++;
        }
        if (story) psx_story_rewards_steer_card(cpu, tier, story);
        return;
    }
    int count = g_card_drops;
    if (count > PSX_VM_CARD_DROPS_MAX) count = PSX_VM_CARD_DROPS_MAX;

    /* A nested guest call clobbers caller-saved registers and walks the stack
     * below $sp, so snapshot everything the in-flight call still needs. The
     * MEMORY effects (trunk counts, RNG advance) are the point and stay. */
    CPUState saved = *cpu;
    int granted = 0;
    const int story = tier < 3 ? psx_story_rewards_select() : 0;
    /* Call #1 of the community stream: the pattern's discarded stock roll,
     * applied as a pure seed advance. */
    psx_mod_write_word(PSX_RNG_SEED_ADDR,
                       cd_lcg_advance(psx_mod_read_word(PSX_RNG_SEED_ADDR),
                                      1));
    /* A scripted campaign reward is a real award at position one. The normal
     * table then supplies N-1 cards. Without one, all N cards are normal. */
    if (story) {
        int bail = 0;
        s_cd_award_is_story = 1;
        cd_award_one(cpu, (uint32_t)story, &bail);
        s_cd_award_is_story = 0;
        if (bail) s_cd_bails++; else granted++;
    }
    /* Normal cards before the in-flight final one are dispatched here:
     * six burned calls, then the roll is call seven. Card N is NOT ours: its
     * six burns are applied below and the game's own in-flight roll -- the
     * one this hook is running in front of -- then lands on precisely the
     * pattern's last roll call, computes the same card the community mod
     * would, and awards it through the stock path (counted by the on_award
     * hook like every other drop). That is what lets this run with ZERO code
     * patches. The first version instead NOPed the stock roll's rand and the
     * stock award site per frame; those writes dirtied the 64K region that
     * also holds the results-screen state function, whose ENTRY HOOK the
     * dirty dispatch path skips (the trap documented at the chest builder),
     * and the CARD DROPS page's Left/Right navigation -- which lives on that
     * hook's page-byte snapshot -- quietly broke: every Right re-entered
     * sub-page 0, so the results screen showed one card page no matter how
     * many cards dropped. Measured live 2026-08-31: RESULT[55] held 3 while
     * prev_page stayed 0. */
    const int early_normals = count - 1 - (story ? 1 : 0);
    for (int i = 0; i < early_normals; i++) {
        psx_mod_write_word(PSX_RNG_SEED_ADDR,
                           cd_lcg_advance(
                               psx_mod_read_word(PSX_RNG_SEED_ADDR), 6));
        int bail = 0;
        const int smart = cd_smart_effective();
        const int prepared = smart ? cd_smart_prepare(tier) : 1;
        /* The game's own in-flight roll is the final award and cannot simply
         * disappear from this caller. Preserve one remaining copy slot for
         * it; skipped bonus positions still consume their exact RNG call. */
        if (smart && (prepared < 0 || s_smart_last_capacity <= 1)) {
            cd_smart_restore();
            psx_mod_write_word(PSX_RNG_SEED_ADDR,
                               cd_lcg_advance(
                                   psx_mod_read_word(PSX_RNG_SEED_ADDR), 1));
            s_smart_skipped++;
            s_smart_skipped_duel++;
            continue;
        }
        const uint32_t card = cd_roll_one(cpu, tier, &bail);
        if (smart) s_smart_last_card = card;
        cd_smart_restore();
        if (bail) { s_cd_bails++; break; }
        if (card == 0 || card > 722) continue;        /* empty table roll */
        cd_award_one(cpu, card, &bail);
        if (bail) { s_cd_bails++; break; }
        granted++;
    }
    /* Card N's six burned calls; the stock roll provides call seven. */
    psx_mod_write_word(PSX_RNG_SEED_ADDR,
                       cd_lcg_advance(psx_mod_read_word(PSX_RNG_SEED_ADDR),
                                      6));
    {
        int bail = 0;
        const int smart = cd_smart_effective();
        const int prepared = smart ? cd_smart_prepare(tier) : 1;
        if (smart && prepared < 0) {
            s_cd_suppress_award = CD_SUPPRESS_SMART_EXHAUSTED;
            s_smart_last_card = 0;
            cd_smart_restore();
            if (!story)
                s_cd_no_card_reason = CD_SUPPRESS_SMART_EXHAUSTED;
        } else {
            const uint32_t final_normal = cd_preview_roll(cpu, tier, &bail);
            if (smart) s_smart_last_card = final_normal;
            /* A story roll must now own the row until its award. Its visible
             * card is the guaranteed reward; the pending normal card is
             * substituted at award entry. With no story, leave the smart row
             * up for the actual in-flight roll and restore it from that award
             * hook. */
            if (story || bail || final_normal < 1 ||
                final_normal > PSX_DROP_CARD_ID_MAX)
                cd_smart_restore();
            if (!bail && final_normal >= 1 &&
                final_normal <= PSX_DROP_CARD_ID_MAX) {
                cd_track(final_normal, 0, 0);
                s_cd_rewrite_award = story != 0;
            } else if (bail) {
                s_cd_bails++;
            }
        }
    }
    *cpu = saved;
    s_cd_granted += granted;
    /* Keep the stock SPOILS panel truthful about the special first card. Its
     * later award argument is rewritten to the previewed final normal card. */
    if (story && (s_cd_pending_card || s_cd_suppress_award))
        psx_story_rewards_steer_card(cpu, tier, story);

    if (granted > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "+%d bonus card%s",
                      granted, granted == 1 ? "" : "s");
        host_osd_push(msg, 1500);
    }
    if (s_smart_skipped_duel > 0) {
        char msg[72];
        snprintf(msg, sizeof msg,
                 "Smart drops: %u reward%s skipped (all eligible copies filled)",
                 s_smart_skipped_duel, s_smart_skipped_duel == 1 ? "" : "s");
        host_osd_push(msg, 2200);
    }
}

/* Every award on the duel-drop path, recorded for the extended New! list.
 * Deliberately NOT guarded by s_cd_busy: the extras this mod grants go through
 * the same award entry (cd_award_one dispatches with $ra = the duel-drop call
 * site), and they must count as New exactly like the game's own drop. The $ra
 * filter is what keeps password buys, starter decks, etc. out. */
void psx_mod_card_drops_on_award(CPUState *cpu, uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_AWARD_FN) return;
    if (cpu->gpr[31] != PSX_DROP_AWARD_SITE) return;  /* not the duel drop */
    if (s_cd_zero_seed_pending) {
        s_cd_zero_seed_after = psx_mod_read_word(PSX_RNG_SEED_ADDR);
        s_cd_zero_rng_exact =
            s_cd_zero_seed_after == cd_lcg_advance(s_cd_zero_seed_before, 1);
        s_cd_zero_rng_calls_last = s_cd_zero_rng_exact ? 1u : 0u;
        s_cd_zero_seed_pending = 0;
    }
    if (s_cd_suppress_award) {
        const int reason = s_cd_suppress_award;
        s_cd_suppress_award = CD_SUPPRESS_NONE;
        s_cd_rewrite_award = 0;
        cd_discard_pending();
        psx_story_rewards_restore_table();
        cd_smart_restore();
        s_cd_suppressed_awards++;
        if (reason == CD_SUPPRESS_SMART_EXHAUSTED) {
            s_smart_skipped++;
            s_smart_skipped_duel++;
            host_osd_push("Smart drops: no eligible card; nothing awarded", 2500);
        } else {
            s_cd_zero_suppressed++;
            host_osd_push("Card drops: no normal card awarded", 1800);
        }
        psx_mod_skip_current_function(cpu);
        return;
    }
    if (s_cd_rewrite_award && s_cd_pending_card)
        cpu->gpr[4] = s_cd_pending_card;
    const uint32_t id = cpu->gpr[4] & 0xFFFFu;
    if (id < 1 || id > PSX_DROP_CARD_ID_MAX) {
        /* A damaged/empty resident row must not leave either temporary table
         * installed, even though a valid 2048-weight row cannot get here. */
        psx_story_rewards_restore_table();
        cd_smart_restore();
        return;
    }
    if (s_cd_pending_card) {
        if (id == s_cd_pending_card && s_cd_pending_order >= 0) {
            s_cd_order_committed[s_cd_pending_order] = 1;
            s_cd_pending_card = 0; s_cd_pending_order = -1;
        } else {
            cd_discard_pending();
            cd_track(id, 1, s_cd_award_is_story);
        }
    } else {
        cd_track(id, 1, s_cd_award_is_story);
    }
    s_cd_rewrite_award = 0;
    /* The game has read its card out of the steered table; put the table back
     * before anything else looks at it. */
    psx_story_rewards_restore_table();
    cd_smart_restore();
}

/* Chest display-list builder entry: arm the overlay for this build.
 *
 * Armed on "a duel has happened", NOT on the slider. Gating this on
 * g_card_drops > 1 made "New!" mean two different things either side of a
 * slider value — this duel's cards above 1, the rolling last-16 at 1 — with
 * the added trap that the ring only LOOKS like "this duel" at high settings,
 * where the duel's own awards happen to fill all 16 slots. One meaning at
 * every setting is worth the one place this is no longer bit-for-bit stock;
 * the array is a UI scratch buffer rebuilt on every chest open, never save
 * state, so nothing here reaches a save file. */
void psx_mod_card_drops_on_chest_build(CPUState *cpu,
                                                  uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_CHEST_BUILD_FN) return;
    s_cd_chest_builds++;
    s_cd_chest_armed = s_cd_have_duel;
}

/* Builder's sort/insert body entry — the moment the New! flags are complete
 * (the builder's per-card clear and 16-slot ring pass have both run) and none
 * has been consumed yet (all reads happen inside this body). First call per
 * build: replace the array with this duel's cards, clearing the ring's older
 * duels in the same pass. The $ra filter
 * pins the injection to the builder's own call sites, so a stray armed flag
 * (a build that bailed before sorting) can never leak an overlay into some
 * other caller of this body while the shared buffer belongs to another
 * screen. */
void psx_mod_card_drops_on_chest_sort(CPUState *cpu,
                                                 uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_CHEST_SORT_FN || !s_cd_chest_armed) return;
    if (cpu->gpr[31] != PSX_DROP_CHEST_SORT_SITE1 &&
        cpu->gpr[31] != PSX_DROP_CHEST_SORT_SITE2) return;
    s_cd_chest_armed = 0;
    for (uint32_t id = 1; id <= PSX_DROP_CARD_ID_MAX; id++)
        psx_mod_write_byte(PSX_DROP_NEWFLAG_BASE + id,
                           s_cd_new_this_duel[id] ? 1u : 0u);
    s_cd_overlays++;
}

/* ---- CARD DROPS results page (page 4 of the results screen) --------------
 *
 * The results screen already cycles 3 pages on D-pad Left/Right; this adds a
 * fourth listing every card the duel just awarded, one row per distinct card.
 * The two wrap constants in the nav (func_800218F0, 0x80021FB0..50) cannot be
 * code-patched (dirty-RAM divergence would disarm every hook), so the cycle is
 * extended from OUTSIDE the nav:
 *
 *   - func_800218F0 entry (once per frame): snapshot the PRE-press page byte
 *     RESULT[55]. The nav runs later in the same invocation, so the snapshot
 *     is what the player was LOOKING AT when they pressed.
 *   - func_80021480 entry (the page apply, $a0 = post-nav page): in a 4-page
 *     cycle only the two transitions INTO page 3 land wrong (2+Right -> stock
 *     0, 0+Left -> stock 2); leaving page 3 lands right on its own (byte 3:
 *     Right 3+1=4 -> 0, Left 3+1-2=2). When a transition should enter page 3,
 *     rewrite $a0 and the page byte to 3 and arm the widget override. The
 *     guest body then renders "page 3": it reads its content id from
 *     byte[RESULT+52+page], and for page 3 that byte IS the page byte (52+3 =
 *     55), i.e. content id 3 — garbage, which is exactly why the next hook
 *     exists.
 *   - func_800393B0 entry (widget draw, $a0 = widget): the apply set up UI
 *     widget 0 with content id 3; before the body's init resolves that id
 *     into a stream cursor, replace the state so it decodes OUR stream
 *     instead: cursor word at obj+0, decode-pending obj[82]=1, cursor slot
 *     obj[88]=0, and bit14 of the flags half at obj+52 so the body's own init
 *     (which would re-resolve id 3) is skipped.
 *
 * Gate: what THIS duel actually awarded (total > 1), recorded at roll time by
 * the tracker above — never the live slider, which the player can change
 * mid-duel. At total <= 1 every hook returns before touching anything and the
 * screen stays bit-for-bit stock.
 *
 * The stream is composed host-side into the chest's New!-flag work buffer
 * (0x8010606A, 723 bytes): UI scratch, rebuilt by the chest builder on every
 * chest open, and the chest and results screens are mutually exclusive, so
 * the results page owning it while on screen collides with nothing. */
#define PSX_DROP_RESULTS_STATE_FN 0x800218F0u
#define PSX_DROP_PAGE_APPLY_FN    0x80021480u
#define PSX_DROP_WIDGET_DRAW_FN   0x800393B0u
#define PSX_DROP_GP               0x8009AF08u
#define PSX_DROP_RESULT_PTR       (PSX_DROP_GP + 736u)   /* -> results block */
#define PSX_DROP_PAGE_OFF         55u                    /* RESULT+55: page */
#define PSX_DROP_PAD_NEW_ADDR     0x8009B394u  /* new-press mask, swapped */
#define PSX_DROP_PAD_LEFT         0x8000u
#define PSX_DROP_PAD_RIGHT        0x2000u
#define PSX_DROP_WIDGET0          0x800EB0F8u
/* The stream must live inside the 0x801D segment: it is reached through the
 * card-name bank (string id 0x8000+id resolves to 0x801D0000 + u16 offset),
 * and the offsets are 16-bit. 0x801D9400..0x801D9FFF sits zero and unclaimed
 * between the engine's decode table (ends 0x801D9400) and the next live data
 * (0x801DA000); the middle of that window keeps margin from both. */
#define PSX_DROP_P3_SCRATCH       0x801D9800u
#define PSX_DROP_P3_SCRATCH_MAX   704u
#define PSX_DROP_P3_SCRATCH_OFF   0x9800u      /* scratch - 0x801D0000 */
#define PSX_DROP_NAME_TBL         0x801D5800u  /* u16 name offsets, by id */
#define PSX_DROP_HIDE_FN          0x80040410u  /* show/hide summary sprites */
#define PSX_DROP_HIDE_RET         0x800214A0u  /* the apply's own jal link */

static uint8_t  s_cd_p3_prev_page;    /* RESULT[55] before this frame's nav */
static int      s_cd_p3_pending;      /* widget draw must take the stream */
static int      s_cd_p3_active;       /* the screen is showing our page */
static int      s_cd_p3_sub;          /* current sub-page (auto-pagination) */
static int      s_cd_p3_subs = 1;     /* sub-page count for this duel */
/* Round-1 test channel: a raw stream poked over the debug server, rendered
 * verbatim. The composer replaces this as the default source in round 2. */
static uint8_t  s_cd_p3_test[PSX_DROP_P3_SCRATCH_MAX];
static int      s_cd_p3_test_len;
static int      s_cd_p3_applies;      /* corrections taken (observability) */
static int      s_cd_p3_overrides;    /* widget overrides taken */
static int      s_cd_p3_stale;        /* host frames since results hook ran */

static int cd_p3_gate(void) {
    return s_cd_page_duel && s_cd_awarded_total > 1;
}

/* One row per distinct card, preserving first-award order. This keeps a
 * scripted reward first even when a later normal card is newly owned. */
static int cd_p3_row_ids(int *ids, int cap) {
    int n = 0;
    uint8_t seen[PSX_DROP_CARD_ID_MAX + 1]; memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_cd_order_n && n < cap; i++) {
        const int id = s_cd_award_order[i];
        if (id < 1 || id > PSX_DROP_CARD_ID_MAX || seen[id]) continue;
        seen[id] = 1; ids[n++] = id;
    }
    return n;
}

#define PSX_DROP_P3_ROWS 7   /* one per plate of the borrowed furniture */

/* Row typography, tunable live over the debug server (`card_drops_layout`)
 * alongside the New! sprite's own offsets — fitting text to the game's plates
 * is by-eye work and a rebuild per nudge costs the player their screen. These
 * defaults are the user's measured fitting. */
static int s_cd_p3_text_y = 18;  /* first line's y operand (name line) */
static int s_cd_p3_split  = 4;   /* number/count line, px below the name */
static int s_cd_p3_name_x = 28;  /* name column */
static int s_cd_p3_num_x  = 0;   /* card-number column (0 = screen edge) */

static int cd_p3_sub_count(void) {
    int ids[PSX_DROP_CARD_ID_MAX];
    const int n = cd_p3_row_ids(ids, PSX_DROP_CARD_ID_MAX);
    return n > 0 ? (n + PSX_DROP_P3_ROWS - 1) / PSX_DROP_P3_ROWS : 1;
}

/* Text-stream composer. The row/advance shape is copied from the SPECIAL
 * ARTS layout string (id 69, the page whose plate furniture this page
 * borrows); the style escapes were mapped by measurement:
 *   f8 01 nn  start/advance the row line (14 = header seat that centers the
 *             text on the plates, 18 = row pitch)
 *   f8 0a nn  color: 00 white, 01 gold, 02 blue, 03 green, 05 red-salmon
 *   f8 04 nn  face: 01 thin, 02 the SPOILS bar's bold face
 *   f8 02 nn  pen x (unused here), f8 00 nn insert-variable (unused)
 * Characters are the engine's frequency codes; card names are byte-copied
 * from the game's own name blob (already in that encoding, including any
 * embedded f8-escape pairs, which must be copied atomically so a 0xF8 0xFF
 * pair is not mistaken for the terminator).
 * Row: `NNN Name xN New` — digits thin white, name bold (user-matched to the
 * SPOILS reward text), count thin only when this duel awarded >1 copy, and a
 * bold red New tag on cards the player owned none of. */
static const uint8_t k_cd_digit[10] = { 0x38u, 0x3Du, 0x3Au, 0x41u, 0x4Au,
                                        0x42u, 0x4Eu, 0x45u, 0x57u, 0x59u };
#define PSX_DROP_RAW_SPACE 0x00u
#define PSX_DROP_RAW_X     0x36u

static int cd_p3_compose(uint8_t *buf, int cap, int sub) {
    int ids[PSX_DROP_CARD_ID_MAX];
    const int n = cd_p3_row_ids(ids, PSX_DROP_CARD_ID_MAX);
    int len = 0;
    uint8_t head[6]   = { 0xF8u, 0x01u, 0x00u, 0xF8u, 0x0Au, 0x00u };
    head[2] = (uint8_t)s_cd_p3_text_y;
    static const uint8_t bold[3]   = { 0xF8u, 0x04u, 0x02u };
    static const uint8_t thin[3]   = { 0xF8u, 0x04u, 0x01u };
    /* Geometry, all measured from the glyph records:
     *  - `f8 02 nn` advances the pen x RELATIVE to where it stands; a line
     *    start (any `f8 01 nn`) resets x to 0. `f8 01 00` is therefore a
     *    same-y line reset — how the count reaches the right edge without
     *    knowing the name's width.
     *  - every glyph advances exactly 8px, bold face included;
     *  - the engine WRAPS any glyph whose pen x exceeds 284, so the usable
     *    right edge is 292.
     * Row layout (user's design): the upper strip of each plate carries the
     * host-drawn New! sprite (left edge); the text line sits 9px lower:
     * number at x=4, name at x=36 (28+8), count right-aligned to 292. A name
     * that would collide with its count is cut with `...` (raw 0x0b); the
     * longest real name is 30 chars, which only collides when a count is
     * present. */
    /* The name sits on the row's base line; the number and the count sit
     * `split` px lower on a second line (the user's fitting: name up 3,
     * number/count up 1 relative to the old shared line). Next row's base is
     * pitch minus the split. */
    const uint8_t x_num[3]  = { 0xF8u, 0x02u, (uint8_t)s_cd_p3_num_x };
    const uint8_t x_name[3] = { 0xF8u, 0x02u, (uint8_t)s_cd_p3_name_x };
    const uint8_t dn_split[3] = { 0xF8u, 0x01u, (uint8_t)s_cd_p3_split };
    const uint8_t dn_row[3]   = { 0xF8u, 0x01u,
                                  (uint8_t)(24 - s_cd_p3_split) };
    memcpy(buf + len, head, sizeof head); len += (int)sizeof head;
    const int lo = sub * PSX_DROP_P3_ROWS;
    for (int r = lo; r < n && r < lo + PSX_DROP_P3_ROWS; r++) {
        if (len + 96 > cap) break;
        const int id = ids[r];
        if (r > lo) { memcpy(buf + len, dn_row, 3); len += 3; }
        memcpy(buf + len, x_name, 3); len += 3;
        memcpy(buf + len, bold, 3); len += 3;
        const int copies = s_cd_copies_this_duel[id];
        /* Count field: x + 1-2 digits, right edge at 292. */
        const int cnt_w   = (copies >= 10) ? 24 : 16;
        const int cnt_x   = 292 - cnt_w;
        /* Name chars that fit before the count (8px gap), or to the edge. */
        const int name_cap =
            ((copies > 1 ? cnt_x - 8 : 292) - s_cd_p3_name_x) / 8;
        uint32_t p = 0x801D0000u +
                     psx_mod_read_half(PSX_DROP_NAME_TBL + (uint32_t)id * 2u);
        int nlen = 0;
        uint8_t name[32];
        for (int k = 0; k < 30; k++, p++) {
            const uint8_t b = psx_mod_read_byte(p);
            if (b == 0xFFu) break;
            if (nlen < (int)sizeof name) name[nlen++] = b;
        }
        if (nlen > name_cap) {
            nlen = name_cap - 3;
            if (nlen < 0) nlen = 0;
            memcpy(buf + len, name, (size_t)nlen); len += nlen;
            buf[len++] = 0x0Bu; buf[len++] = 0x0Bu; buf[len++] = 0x0Bu;
        } else {
            memcpy(buf + len, name, (size_t)nlen); len += nlen;
        }
        memcpy(buf + len, thin, 3); len += 3;
        /* Second line: number at the left, count at the right edge. */
        memcpy(buf + len, dn_split, 3); len += 3;
        memcpy(buf + len, x_num, 3); len += 3;
        buf[len++] = k_cd_digit[(id / 100) % 10];
        buf[len++] = k_cd_digit[(id / 10) % 10];
        buf[len++] = k_cd_digit[id % 10];
        if (copies > 1) {
            /* Pen stands just past the three number glyphs; walk it to the
             * count column. Derived from the number column rather than a
             * fixed pair of advances, so moving the number cannot drag the
             * count with it. Split in two because one operand caps at 255. */
            int adv = cnt_x - (s_cd_p3_num_x + 24);
            if (adv < 0) adv = 0;
            while (adv > 0) {
                const int step = (adv > 255) ? 255 : adv;
                buf[len++] = 0xF8u; buf[len++] = 0x02u;
                buf[len++] = (uint8_t)step;
                adv -= step;
            }
            buf[len++] = PSX_DROP_RAW_X;
            if (copies >= 10) buf[len++] = k_cd_digit[(copies / 10) % 10];
            buf[len++] = k_cd_digit[copies % 10];
        }
    }
    buf[len++] = 0xFFu;
    return len;
}

/* Copy the current sub-page's stream into guest scratch and point name-table
 * entry T[0] at it: with it repointed, string id 0x8000 (card 0's name) IS
 * the CARD DROPS page. A staged debug stream takes precedence over the
 * composer (escape experiments).
 *
 * Entry 0 is NOT dead. Card id 0 does not exist, but the LIBRARY resolves
 * string 0x8000 (stock: the empty string, a lone 0xFF at 0x801D6000) when it
 * opens, and its text box is built by a loop that spins until the stream
 * ends. Left pointing at the page, the library walked the drops text
 * instead, hit a wait-for-choice code, and the screen stayed black until the
 * player killed the game (issue #16: the reporter had just farmed several
 * copies in one duel). The entry lives in the savestate too, so a state
 * taken after such a duel carried the hang across restarts. The stock value
 * is remembered at the first publish and put back by cd_p3_unpublish() from
 * the leave paths and the frame tick whenever the page is not on screen. */
#define PSX_DROP_NAME0_STOCK_OFF  0x6000u   /* EXE value of T[0] */
static uint16_t s_cd_p3_name0_stock = PSX_DROP_NAME0_STOCK_OFF;
static int      s_cd_p3_name0_saved;

static void cd_p3_unpublish(void) {
    if (psx_mod_read_half(PSX_DROP_NAME_TBL) == PSX_DROP_P3_SCRATCH_OFF)
        psx_mod_write_half(PSX_DROP_NAME_TBL, s_cd_p3_name0_stock);
}

static uint32_t cd_p3_publish_stream(int sub) {
    uint8_t buf[PSX_DROP_P3_SCRATCH_MAX];
    const uint8_t *src;
    int len;
    if (!s_cd_p3_name0_saved) {
        const uint16_t cur = psx_mod_read_half(PSX_DROP_NAME_TBL);
        /* A loaded state may already carry the scratch offset; keep the EXE
         * value then rather than remembering the poison as stock. */
        if (cur != PSX_DROP_P3_SCRATCH_OFF) s_cd_p3_name0_stock = cur;
        s_cd_p3_name0_saved = 1;
    }
    if (s_cd_p3_test_len > 0) {
        src = s_cd_p3_test;
        len = s_cd_p3_test_len;
    } else {
        len = cd_p3_compose(buf, (int)sizeof buf, sub);
        src = buf;
    }
    for (int i = 0; i < len && i < (int)PSX_DROP_P3_SCRATCH_MAX; i++)
        psx_mod_write_byte(PSX_DROP_P3_SCRATCH + (uint32_t)i, src[i]);
    psx_mod_write_half(PSX_DROP_NAME_TBL, PSX_DROP_P3_SCRATCH_OFF);
    return PSX_DROP_P3_SCRATCH;
}

/* Bumped once per guest frame while the results screen is live; the overlay
 * tick uses it to notice the screen is gone (Cross exits without any apply
 * call our hooks would see). */
static uint32_t s_cd_p3_state_ticks;

static void cd_no_card_draw_text(const char *text, int x, int y)
{
    for (; text && *text; text++, x += 8) {
        unsigned c = (unsigned char)*text;
        if (c < 32u || c > 126u) c = '?';
        const unsigned char *glyph = FONT8[c - 32u];
        for (int gy = 0; gy < 8; gy++) {
            const unsigned bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(bits & (1u << gx))) continue;
                const int px = x + gx, py = y + gy;
                if (px < 0 || px >= PSX_DROP_NO_CARD_W ||
                    py < 0 || py >= PSX_DROP_NO_CARD_H) continue;
                /* One-pixel black offset keeps the white bitmap readable over
                 * the textured results art at every scaling mode. */
                if (px + 1 < PSX_DROP_NO_CARD_W && py + 1 < PSX_DROP_NO_CARD_H)
                    s_cd_no_card_canvas[(py + 1) * PSX_DROP_NO_CARD_W + px + 1] =
                        0xFF000000u;
                s_cd_no_card_canvas[py * PSX_DROP_NO_CARD_W + px] = 0xFFFFFFFFu;
            }
        }
    }
}

static void cd_no_card_prepare_canvas(int reason)
{
    if (s_cd_no_card_canvas_reason == reason) return;
    s_cd_no_card_canvas_reason = reason;
    for (int y = 0; y < PSX_DROP_NO_CARD_H; y++) {
        for (int x = 0; x < PSX_DROP_NO_CARD_W; x++) {
            const int edge = y < 2 || y >= PSX_DROP_NO_CARD_H - 2 ||
                             x < 2 || x >= PSX_DROP_NO_CARD_W - 2;
            s_cd_no_card_canvas[y * PSX_DROP_NO_CARD_W + x] =
                edge ? 0xFF808080u : 0xFF181818u;
        }
    }
    const char *label = reason == CD_SUPPRESS_SMART_EXHAUSTED
                            ? "NO ELIGIBLE CARD DROP"
                            : "NO NORMAL CARD DROP";
    cd_no_card_draw_text(label,
        (PSX_DROP_NO_CARD_W - (int)strlen(label) * 8) / 2,
        (PSX_DROP_NO_CARD_H - 8) / 2);
}

int psx_card_drops_no_card_image(const uint32_t **pixels, int *w, int *h)
{
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    if (s_cd_no_card_reason == CD_SUPPRESS_NONE || !s_cd_page_duel ||
        s_cd_p3_stale >= 8 ||
        !result || psx_mod_read_byte(result + PSX_DROP_PAGE_OFF) != 0u ||
        psx_mod_read_byte(result + 52u) != 68u ||
        psx_mod_read_byte(result + 54u) != 69u ||
        psx_ygo_netplay_session()) return 0;
    cd_no_card_prepare_canvas(s_cd_no_card_reason);
    if (pixels) *pixels = s_cd_no_card_canvas;
    if (w) *w = PSX_DROP_NO_CARD_W;
    if (h) *h = PSX_DROP_NO_CARD_H;
    return 1;
}

void psx_card_drops_no_card_origin(int *x, int *y)
{
    if (x) *x = PSX_DROP_NO_CARD_X;
    if (y) *y = PSX_DROP_NO_CARD_Y;
}

int psx_card_drops_no_card_needs_present(void)
{
    return s_cd_no_card_present_hold > 0;
}

void psx_card_drops_no_card_placed(const int *placement)
{
    if (placement)
        memcpy(s_cd_no_card_placement, placement, sizeof s_cd_no_card_placement);
}

void psx_mod_card_drops_on_results_state(CPUState *cpu,
                                                    uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_RESULTS_STATE_FN) return;
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    if (result) s_cd_p3_prev_page = psx_mod_read_byte(result + PSX_DROP_PAGE_OFF);
    s_cd_p3_state_ticks++;
    if (s_cd_no_card_reason != CD_SUPPRESS_NONE)
        s_cd_no_card_present_hold = 3;
}

/* Host-frame update for the New! tag overlay: visible only while the CARD
 * DROPS page is the one on screen AND the results state function is still
 * running (staleness catches the Cross exit, which fires no page apply). */
void psx_card_drops_tick(void) {
    const int smart_allowed = !psx_ygo_netplay_session();
    if (!smart_allowed) {
        cd_smart_restore();
        return;   /* netplay: per-machine layer, peers must stay bit-identical */
    }
    static uint32_t last_ticks;
    if (s_cd_p3_state_ticks != last_ticks) {
        last_ticks = s_cd_p3_state_ticks;
        s_cd_p3_stale = 0;
    } else if (s_cd_p3_stale < 1000) {
        s_cd_p3_stale++;
        /* The results screen is gone: this duel's card record must not
         * survive into the next duel's results (see s_cd_page_duel). */
        /* Threshold, not equality: `stale` saturates at its cap, so a record
         * created while it already sat there (or any time the counter had
         * run past the mark) would never have been cleared. */
        if (s_cd_p3_stale >= 64 && s_cd_page_duel) {
            s_cd_page_duel = 0;
            /* Keep the completed record queryable for regression evidence.
             * The page gate is now closed, and the next real roll replaces
             * every array before another results screen can use it. */
        }
    }
    if (s_cd_no_card_present_hold > 0) s_cd_no_card_present_hold--;
    uint8_t rows[PSX_CD_OVERLAY_ROWS] = { 0 };
    const int on = s_cd_p3_active && s_cd_p3_stale < 8;
    /* Off the page (another page, a loaded state) or the results screen gone
     * for a second (Cross exit fires no apply): give string 0x8000 back to
     * the game. The 64-frame mark, not the overlay's 8, so a hitch on a slow
     * machine between the page turn and its draw cannot blank the page. */
    if (!s_cd_p3_active || s_cd_p3_stale >= 64) cd_p3_unpublish();
    if (on) {
        int ids[PSX_DROP_CARD_ID_MAX];
        const int n = cd_p3_row_ids(ids, PSX_DROP_CARD_ID_MAX);
        const int lo = s_cd_p3_sub * PSX_DROP_P3_ROWS;
        for (int r = 0; r < PSX_CD_OVERLAY_ROWS; r++)
            if (lo + r < n && s_cd_was_new_this_duel[ids[lo + r]])
                rows[r] = 1;
    }
    psx_cd_overlay_set(on, rows);
}

void psx_mod_card_drops_on_page_apply(CPUState *cpu,
                                                 uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_PAGE_APPLY_FN) return;
    /* Gate shut: also forget that the page was ever showing. Returning early
     * without clearing this left `active` true from a PREVIOUS duel's visit,
     * and the overlay tick reads it — so a duel that awarded a single card
     * could paint that card's "New!" sprite onto the stock summary page. */
    if (!cd_p3_gate()) { s_cd_p3_active = 0; cd_p3_unpublish(); return; }
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    if (!result) return;
    const uint16_t pad = psx_mod_read_half(PSX_DROP_PAD_NEW_ADDR);
    const int left  = (pad & PSX_DROP_PAD_LEFT)  != 0;
    const int right = (pad & PSX_DROP_PAD_RIGHT) != 0;
    /* The screen init also applies page 0; no press means it is not a page
     * TURN, so it is never ours to redirect. Entering the screen fresh also
     * resets the sub-page. */
    if (!left && !right) {
        s_cd_p3_active = 0; s_cd_p3_sub = 0; cd_p3_unpublish(); return;
    }
    const int page = (int)(int8_t)(cpu->gpr[4] & 0xFFu);
    const int prev = (int)s_cd_p3_prev_page;
    /* Row data can change between visits (a re-rolled rebuild); recount every
     * turn. A staged debug stream keeps its debug-set sub count. */
    if (s_cd_p3_test_len <= 0) s_cd_p3_subs = cd_p3_sub_count();
    /* The CARD DROPS page sits BETWEEN Summary and Statistics (user's order:
     * Right from the summary shows the cards first). Logical cycle
     * 0 -> 3(sub 0..last) -> 1 -> 2 -> 0; the byte still holds 3 while on the
     * page, so the stock nav computes Right: 3+1 -> wrap 0 and Left: 3+1-2 ->
     * 2, and every transition touching the page needs a redirect:
     *   0 + Right (stock 1)  -> enter at sub 0
     *   3 + Right, last sub  (stock 0)  -> Statistics (1)
     *   1 + Left  (stock 0)  -> enter at last sub
     *   3 + Left,  sub 0     (stock 2)  -> Summary (0)
     * The 1<->2<->0 arcs are stock and pass through untouched. */
    int enter = 0, leave_to = -1;
    if (prev == 0 && right && page == 1) {
        enter = 1; s_cd_p3_sub = 0;                     /* 0 -> cards */
    } else if (prev == 1 && left && page == 0) {
        enter = 1; s_cd_p3_sub = s_cd_p3_subs - 1;      /* 1 -> cards (back) */
    } else if (prev == 3 && right && page == 0) {
        if (s_cd_p3_sub + 1 < s_cd_p3_subs) { enter = 1; s_cd_p3_sub++; }
        else leave_to = 1;                              /* cards -> stats */
    } else if (prev == 3 && left && page == 2) {
        if (s_cd_p3_sub > 0) { enter = 1; s_cd_p3_sub--; }
        else leave_to = 0;                              /* cards -> summary */
    }
    if (leave_to >= 0) {
        cpu->gpr[4] = (uint32_t)leave_to;
        psx_mod_write_byte(result + PSX_DROP_PAGE_OFF, (uint8_t)leave_to);
        s_cd_p3_active = 0;
        cd_p3_unpublish();
        return;
    }
    if (!enter) { s_cd_p3_active = 0; cd_p3_unpublish(); return; } /* stock landing */
    /* Active before the publish, so no tick between the two can undo it. */
    s_cd_p3_active = 1;
    cd_p3_publish_stream(s_cd_p3_sub);
    cpu->gpr[4] = 3;
    psx_mod_write_byte(result + PSX_DROP_PAGE_OFF, 3u);
    s_cd_p3_pending = 1;
    s_cd_p3_applies++;
}

void psx_mod_card_drops_on_widget_draw(CPUState *cpu,
                                                  uint32_t address) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (!cpu || address != PSX_DROP_WIDGET_DRAW_FN) return;
    if (!s_cd_p3_pending) return;
    if ((cpu->gpr[4] & 0xFFFFFFFFu) != PSX_DROP_WIDGET0) return;
    s_cd_p3_pending = 0;
    s_cd_p3_overrides++;
    const uint32_t w = PSX_DROP_WIDGET0;
    /* Redirect, do not replicate: the body's own init runs in full (it frees
     * the previous page's glyph run, relinks the record pointers and resolves
     * the string id into the cursor) — it just resolves OUR id. 0x8000 is
     * card 0's name, whose table entry the publish step points at the
     * composed stream. The content-id-3 setup leaves the text-render bit
     * (bit8 of the flags half) clear, unlike every real page id — set it or
     * the decoded glyphs never draw. Cell 8x12 is the big text face; the
     * setup's value survives init only because bit8 skips the 8x8 default. */
    psx_mod_write_half(w + 54u, 0x8000u);
    psx_mod_write_half(w + 52u,
                       (uint16_t)(psx_mod_read_half(w + 52u) | 0x0100u));
    psx_mod_write_byte(w + 90u, 8u);                    /* cell width */
    psx_mod_write_byte(w + 91u, 12u);                   /* cell height */
    /* The apply already ran func_80040410(master, 3), which lands on its SHOW
     * path and leaves the summary sprites (DUEL SKILL, SPOILS, POW, rank) on
     * screen. Poking its state bytes does nothing — the hide is behavior
     * inside the call, not a flag — so run the game's own hide exactly as
     * page 2 does: nested guest call, the same pattern as the drop roll.
     * Stop/$ra is the apply's own post-call address for this jal. */
    {
        const uint32_t master = psx_mod_read_word(
            psx_mod_read_word(PSX_DROP_RESULT_PTR));
        if (master) {
            CPUState saved = *cpu;
            cpu->pc = 0;
            cpu->gpr[4] = master;
            cpu->gpr[5] = 2;
            cpu->gpr[31] = PSX_DROP_HIDE_RET;
            s_cd_busy = 1;
            psx_dispatch_call(cpu, PSX_DROP_HIDE_FN, PSX_DROP_HIDE_RET);
            s_cd_busy = 0;
            *cpu = saved;
        }
    }
}

/* Debug surface: stage a raw stream / read back the page state. */
int psx_card_drops_p3_stage(const uint8_t *bytes, int len,
                                       int subs) {
    if (len < 0 || len > (int)PSX_DROP_P3_SCRATCH_MAX) return 0;
    if (bytes && len > 0) memcpy(s_cd_p3_test, bytes, (size_t)len);
    s_cd_p3_test_len = len;
    if (subs >= 1) s_cd_p3_subs = subs;
    return 1;
}

void psx_card_drops_p3_state(int *active, int *sub, int *subs,
                                        int *pending, int *applies,
                                        int *overrides, int *prev_page,
                                        int *test_len) {
    if (active)    *active    = s_cd_p3_active;
    if (sub)       *sub       = s_cd_p3_sub;
    if (subs)      *subs      = s_cd_p3_subs;
    if (pending)   *pending   = s_cd_p3_pending;
    if (applies)   *applies   = s_cd_p3_applies;
    if (overrides) *overrides = s_cd_p3_overrides;
    if (prev_page) *prev_page = (int)s_cd_p3_prev_page;
    if (test_len)  *test_len  = s_cd_p3_test_len;
}

/* Live layout tuning for the page: text line y, the number/count split, and
 * the New! sprite's x / first-row y / in-row dy. Absolute values; -100000
 * keeps a field. Re-publishes the stream so a text change lands without a
 * page turn. */
void psx_card_drops_layout(int text_y, int split, int name_x,
                                      int num_x, int spr_x, int spr_y,
                                      int spr_dy) {
    if (psx_ygo_netplay_session()) return;   /* netplay: per-machine layer, peers must stay bit-identical */
    if (text_y != PSX_CD_OVERLAY_KEEP) s_cd_p3_text_y = text_y;
    if (split  != PSX_CD_OVERLAY_KEEP) s_cd_p3_split  = split;
    if (name_x != PSX_CD_OVERLAY_KEEP && name_x >= 0 && name_x <= 255)
        s_cd_p3_name_x = name_x;
    if (num_x  != PSX_CD_OVERLAY_KEEP && num_x  >= 0 && num_x  <= 255)
        s_cd_p3_num_x = num_x;
    psx_cd_overlay_tune(spr_x, spr_y, spr_dy);
    if (s_cd_p3_active) {
        cd_p3_publish_stream(s_cd_p3_sub);
        /* Re-decode in place. Bit 0x4000 of the widget's flags is the body's
         * "already set up" latch (func_800393B0 tests it at 0x800393CC and
         * sets it); clearing it makes the next draw re-run the init, which
         * re-resolves the string id — and our draw hook redirects that to the
         * freshly composed stream. Deliberately NOT the results-init rebuild
         * poke: that re-runs the whole screen and drops the player back to
         * page 0, which is useless for judging a nudge. */
        psx_mod_write_half(PSX_DROP_WIDGET0 + 52u,
                           (uint16_t)(psx_mod_read_half(PSX_DROP_WIDGET0 + 52u) &
                                      (uint16_t)~0x4000u));
        s_cd_p3_pending = 1;
    }
}

void psx_card_drops_layout_get(int *text_y, int *split, int *name_x,
                                          int *num_x, int *spr_x, int *spr_y,
                                          int *spr_dy) {
    if (text_y) *text_y = s_cd_p3_text_y;
    if (split)  *split  = s_cd_p3_split;
    if (name_x) *name_x = s_cd_p3_name_x;
    if (num_x)  *num_x  = s_cd_p3_num_x;
    psx_cd_overlay_tune_get(spr_x, spr_y, spr_dy);
}

/* Live setting override for the test loop: winning a real duel per slider
 * value is the only other way to exercise the gate. */
int psx_card_drops_set(int drops) {
    if (drops < 0 || drops > PSX_VM_CARD_DROPS_MAX) return 0;
    if (s_card_drops_row >= 0 &&
        psx_video_menu_get_row(s_card_drops_row) != drops)
        psx_video_menu_set_row(s_card_drops_row, drops);
    g_card_drops = drops;
    return 1;
}

int psx_card_drops_smart_set(int enabled)
{
    const int value = enabled ? 1 : 0;
    (void)psx_drop_edits_smart_drop_set(value);
    if (!value) cd_smart_restore();
    return 1;
}

int psx_card_drops_smart_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    const int n = snprintf(out, cap,
        "\"configured\":%d,\"effective\":%d,\"normal_awards\":%d,"
        "\"row_enabled\":%d,"
        "\"toggle_enabled\":%d,"
        "\"attempts\":%u,\"applied\":%u,\"unchanged\":%u,"
        "\"fallbacks\":%u,\"restores\":%u,\"skipped\":%u,"
        "\"skipped_duel\":%u,\"table_active\":%d,"
        "\"last_tier\":%d,\"last_eligible\":%d,\"last_excluded\":%d,\"last_capacity\":%d,"
        "\"last_eligible_weight\":%u,\"last_card\":%u",
        cd_smart_configured(), cd_smart_effective(), g_card_drops,
        !psx_ygo_netplay_session(), !psx_ygo_netplay_session(),
        s_smart_attempts, s_smart_applied, s_smart_unchanged,
        s_smart_fallbacks, s_smart_restores, s_smart_skipped,
        s_smart_skipped_duel, s_smart_active,
        s_smart_last_tier, s_smart_last_eligible, s_smart_last_excluded,
        s_smart_last_capacity, s_smart_last_eligible_weight, s_smart_last_card);
    return n > 0 && (unsigned)n < cap ? n : 0;
}

int psx_card_drops_suppression_state_json(char *out, unsigned cap)
{
    if (!out || cap < 320u) return 0;
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    const int page = result ?
        (int)psx_mod_read_byte(result + PSX_DROP_PAGE_OFF) : -1;
    const int visible = s_cd_no_card_reason != CD_SUPPRESS_NONE &&
                        s_cd_page_duel &&
                        s_cd_p3_stale < 8 && page == 0 &&
                        !psx_ygo_netplay_session();
    const char *reason = s_cd_no_card_reason == CD_SUPPRESS_ZERO_NORMAL
                             ? "zero_normal"
                             : s_cd_no_card_reason == CD_SUPPRESS_SMART_EXHAUSTED
                                   ? "smart_exhausted" : "none";
    const char *pending = s_cd_suppress_award == CD_SUPPRESS_ZERO_NORMAL
                              ? "zero_normal"
                              : s_cd_suppress_award == CD_SUPPRESS_SMART_EXHAUSTED
                                    ? "smart_exhausted" : "none";
    const int n = snprintf(out, cap,
        "\"zero_configured\":%d,\"zero_effective\":%d,"
        "\"reason\":\"%s\",\"pending_reason\":\"%s\","
        "\"zero_duels\":%u,\"zero_story_duels\":%u,"
        "\"suppressed_awards\":%u,\"zero_suppressed_awards\":%u,"
        "\"rng_calls_last\":%u,\"rng_exact_one\":%d,"
        "\"rng_seed_before\":%u,\"rng_seed_after\":%u,"
        "\"rng_policy\":\"one in-flight stock roll; no retries\","
        "\"page\":%d,\"visible\":%d,\"stale\":%d,"
        "\"placement\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
        g_card_drops == 0, g_card_drops == 0 && !psx_ygo_netplay_session(),
        reason, pending,
        s_cd_zero_duels, s_cd_zero_story_duels, s_cd_suppressed_awards,
        s_cd_zero_suppressed,
        s_cd_zero_rng_calls_last, s_cd_zero_rng_exact,
        s_cd_zero_seed_before, s_cd_zero_seed_after,
        page, visible, s_cd_p3_stale,
        s_cd_no_card_placement[0], s_cd_no_card_placement[1],
        s_cd_no_card_placement[2], s_cd_no_card_placement[3],
        s_cd_no_card_placement[4], s_cd_no_card_placement[5],
        s_cd_no_card_placement[6], s_cd_no_card_placement[7],
        s_cd_no_card_placement[8], s_cd_no_card_placement[9]);
    return n > 0 && (unsigned)n < cap ? n : 0;
}

int psx_card_drops_smart_distribution(
    int tier, uint32_t seed, int rolls,
    uint32_t counts[PSX_DROP_CARD_ID_MAX],
    uint16_t weights[PSX_DROP_CARD_ID_MAX],
    int *eligible, int *excluded, int *fallback, uint32_t *final_seed)
{
    if (!counts || !weights || tier < 0 ||
        tier >= (int)PSX_DROP_TIER_COUNT || rolls < 1 || rolls > 1000000)
        return 0;
    uint16_t original[PSX_DROP_CARD_ID_MAX];
    uint32_t eligible_weight = 0;
    int fb = 0;
    if (!cd_smart_build((unsigned)tier, original, weights,
                        eligible, excluded, &eligible_weight, NULL, &fb))
        return 0;
    (void)eligible_weight;
    if (fallback) *fallback = fb;
    memset(counts, 0, sizeof(uint32_t) * PSX_DROP_CARD_ID_MAX);
    uint32_t s = seed;
    for (int r = 0; r < rolls; r++) {
        s = cd_lcg_advance(s, 1);
        const uint32_t roll = ((((s >> 16) & 0x7FFFu) & 0x7FFu) + 1u);
        uint32_t cumulative = 0;
        for (int i = 0; i < PSX_DROP_CARD_ID_MAX; i++) {
            cumulative += weights[i];
            if (roll <= cumulative) {
                counts[i]++;
                break;
            }
        }
    }
    if (final_seed) *final_seed = s;
    return 1;
}

/* Every guest hook the mod needs, registered beside the addresses they name
 * rather than from the host's startup path — moving an address then only
 * means touching this file. Registration is unconditional: a callback costs
 * nothing while its gate is closed (they return immediately), and doing it
 * once here avoids a second registration path for a player who raises the
 * setting mid-session. */
/* MODS > CARD DROPS. Registered rather than written into the shared menu:
 * how many cards a duel awards is this game's idea, and no other title should
 * compile a row about it.
 *
 * A preference, not a live save write, so it carries a settings key and is
 * restored at startup like any other setting -- the extra cards are rolled by
 * the game's own drop routine, from the same per-opponent, per-rank pool as
 * the first. */
static void cd_row_changed(int value) {
    (void)psx_card_drops_set(value);
}

PSX_MOD_CONSTRUCTOR(psx_card_drops_install) {
    psx_card_drops_register_menu();
    (void)psx_game_add_start_hook(psx_card_drops_register_hooks);
    (void)psx_game_add_frame_hook(psx_card_drops_tick);
}

void psx_card_drops_register_menu(void) {
    s_card_drops_row = psx_video_menu_add_number(
        PSX_VM_MENU_MODS, "Card drops",
        "0 skips normal cards; guaranteed story rewards stay separate",
        0, PSX_VM_CARD_DROPS_MAX, /*slider*/1,
        "card_drops", PSX_VM_CARD_DROPS_DEFAULT, cd_row_changed);
    psx_video_menu_set_row_mark(s_card_drops_row, PSX_VM_CARD_DROPS_DEFAULT);
}

void psx_card_drops_register_hooks(void) {
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops", PSX_DROP_ROLL_FN, psx_mod_card_drops_on_roll);
    /* The extended New! list rides the same setting: an award tracker on the
     * drop path and the chest-builder hook that publishes the set. */
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.new", PSX_DROP_AWARD_FN,
        psx_mod_card_drops_on_award);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.chest", PSX_DROP_CHEST_BUILD_FN,
        psx_mod_card_drops_on_chest_build);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.chest_sort", PSX_DROP_CHEST_SORT_FN,
        psx_mod_card_drops_on_chest_sort);
    /* The CARD DROPS results page: pre-press page snapshot, page-cycle
     * correction, and the widget stream override. All three return
     * immediately while the duel gate (total awarded > 1) is closed. */
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.p3_state", PSX_DROP_RESULTS_STATE_FN,
        psx_mod_card_drops_on_results_state);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.p3_apply", PSX_DROP_PAGE_APPLY_FN,
        psx_mod_card_drops_on_page_apply);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_drops.p3_draw", PSX_DROP_WIDGET_DRAW_FN,
        psx_mod_card_drops_on_widget_draw);
#ifndef PSX_NO_DEBUG_TOOLS
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.name_probe.text", PSX_NP_TEXT_FN,
        psx_mod_name_probe_on_text);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.name_probe.glyph", PSX_NP_GLYPH_FN,
        psx_mod_name_probe_on_glyph);
#endif
}
