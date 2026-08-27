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
#include "psx_cd_overlay.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

/* ---- MODS > CARD DROPS ----------------------------------------------------
 *
 * Award N cards for a won duel instead of 1, by re-running the GAME'S OWN drop
 * roll — not by picking cards ourselves. That is the whole point: the pool a
 * drop comes from depends on the opponent and on your duel rank, and the game
 * already encodes both. Rolling here would mean reimplementing that and
 * silently drifting from it.
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
 * The extra cards are awarded BEFORE the game's own (order is irrelevant — the
 * total is what matters), which keeps this a pure prologue: we never have to
 * interfere with the call already in flight. */
static int g_card_drops = PSX_VM_CARD_DROPS_DEFAULT;

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
/* Live trunk counts, index = card_id - 1, so the byte for `id` is base + id.
 * The two mirrors (0x801D3250, 0x80105D98) only matter for a bulk EDIT; this
 * is a read of the authoritative copy. */
#define PSX_DROP_TRUNK_BASE         0x801D024Fu /* + card_id */
/* The 40-card DECK, at the head of the same save struct the trunk sits in
 * (trunk is that struct + 0x50, which is why the two bases differ by 0x50).
 * One u16 card id per slot. Owning a card in the deck does NOT show up in the
 * trunk count: building a deck MOVES copies out of the trunk, so a card you
 * play with and hold no spares of reads trunk 0. See psx_ygo_cheats.c, which
 * documents the same struct and uses this array's shape to tell a live save
 * from boot-time garbage. */
#define PSX_DROP_DECK_BASE          0x801D0200u
#define PSX_DROP_DECK_N             40u
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
    for (int pass = 0; pass < 2 && n + 64u < cap; pass++) {
        for (uint32_t id = 1; id <= PSX_DROP_CARD_ID_MAX && n + 64u < cap; id++) {
            if (!s_cd_copies_this_duel[id]) continue;
            const int is_new = s_cd_was_new_this_duel[id] ? 1 : 0;
            if (is_new != (pass == 0)) continue;   /* new cards first */
            n += (unsigned)snprintf(out + n, cap - n,
                                         "%s{\"id\":%u,\"n\":%u,\"new\":%d}",
                                         distinct ? "," : "", id,
                                         s_cd_copies_this_duel[id], is_new);
            distinct++;
        }
    }
    snprintf(out + n, cap - n, "]");
    return distinct;
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
 * The extras are granted BEFORE the game's own roll runs; order does not matter
 * because only the total lands in the trunk, and doing it as a prologue means
 * never disturbing the call already in flight. */
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
                                       int *out_bail) {
    if (!cpu) return 0;
    const int saved_setting = g_card_drops;
    if (drops >= 1) g_card_drops = drops;
    const int granted_before = s_cd_granted;

    CPUState saved = *cpu;
    int bail = 0;
    /* No busy guard: this is the point — the hook must fire. */
    cpu->pc = 0;
    cpu->gpr[4] = (uint32_t)tier;
    cpu->gpr[31] = PSX_DROP_ROLL_RET;
    psx_dispatch_call(cpu, PSX_DROP_ROLL_FN, PSX_DROP_ROLL_RET);
    bail = (g_psx_call_bail || cpu->pc != 0);
    const uint32_t card = cpu->gpr[2] & 0xFFFFu;
    if (!bail && card >= 1 && card <= 722)
        cd_award_one(cpu, card, &bail);
    *cpu = saved;

    if (out_card)    *out_card    = card;
    if (out_granted) *out_granted = s_cd_granted - granted_before;
    if (out_bail)    *out_bail    = bail;
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
    if (!cpu || address != PSX_DROP_ROLL_FN || s_cd_busy) return;
    s_cd_calls++;
    s_cd_last_ra = cpu->gpr[31];
    if (cpu->gpr[31] != PSX_DROP_ROLL_SITE) return;   /* not the duel drop */

    /* A fresh duel drop: from here on "New!" means THIS duel. Cleared before
     * the extra<=0 return so the set tracks the single stock card too — the
     * chest hook decides separately whether the extended list is in effect. */
    memset(s_cd_new_this_duel, 0, sizeof s_cd_new_this_duel);
    memset(s_cd_copies_this_duel, 0, sizeof s_cd_copies_this_duel);
    memset(s_cd_was_new_this_duel, 0, sizeof s_cd_was_new_this_duel);
    s_cd_new_distinct = 0;
    s_cd_awarded_total = 0;
    s_cd_have_duel = 1;
    s_cd_page_duel = 1;

    int extra = g_card_drops - 1;
    if (extra <= 0) return;
    if (extra > PSX_VM_CARD_DROPS_MAX - 1) extra = PSX_VM_CARD_DROPS_MAX - 1;

    const uint32_t tier = cpu->gpr[4] & 0xFFu;
    s_cd_last_tier = (int)tier;

    /* A nested guest call clobbers caller-saved registers and walks the stack
     * below $sp, so snapshot everything the in-flight call still needs. The
     * MEMORY effects (trunk counts, RNG advance) are the point and stay. */
    CPUState saved = *cpu;
    int granted = 0;
    for (int i = 0; i < extra; i++) {
        int bail = 0;
        const uint32_t card = cd_roll_one(cpu, tier, &bail);
        if (bail) { s_cd_bails++; break; }
        if (card == 0 || card > 722) continue;        /* empty table roll */
        cd_award_one(cpu, card, &bail);
        if (bail) { s_cd_bails++; break; }
        granted++;
    }
    *cpu = saved;
    s_cd_granted += granted;

    if (granted > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "+%d bonus card%s",
                      granted, granted == 1 ? "" : "s");
        host_osd_push(msg, 1500);
    }
}

/* Does the player already hold this card in the DECK?
 *
 * The trunk count alone is not "do I own one". A deck slot holds a copy that
 * has been moved out of the trunk, so a card sitting in the deck with no
 * spares reads trunk 0 and looked brand new -- reported from a results page
 * marking Twin-headed Thunder Dragon New! while a copy was in the deck being
 * played with.
 *
 * Validated the way psx_ygo_cheats.c validates it: every one of the 40 slots
 * must be a real card id. On any screen where the save is not resident the
 * region is zeros, and a zero fails the range test, so an unreadable deck
 * answers "not found" and the trunk test decides alone -- the old behaviour,
 * which is the safe direction to fail. Marking a genuinely new card as owned
 * would silently hide it; the reverse is merely the bug we already had. */
static int cd_owned_in_deck(uint32_t id)
{
    uint16_t slot[PSX_DROP_DECK_N];
    for (uint32_t i = 0; i < PSX_DROP_DECK_N; i++) {
        slot[i] = (uint16_t)psx_mod_read_half(PSX_DROP_DECK_BASE + i * 2u);
        if (slot[i] < 1u || slot[i] > PSX_DROP_CARD_ID_MAX) return 0;
    }
    for (uint32_t i = 0; i < PSX_DROP_DECK_N; i++)
        if (slot[i] == (uint16_t)id) return 1;
    return 0;
}

/* Every award on the duel-drop path, recorded for the extended New! list.
 * Deliberately NOT guarded by s_cd_busy: the extras this mod grants go through
 * the same award entry (cd_award_one dispatches with $ra = the duel-drop call
 * site), and they must count as New exactly like the game's own drop. The $ra
 * filter is what keeps password buys, starter decks, etc. out. */
void psx_mod_card_drops_on_award(CPUState *cpu, uint32_t address) {
    if (!cpu || address != PSX_DROP_AWARD_FN) return;
    if (cpu->gpr[31] != PSX_DROP_AWARD_SITE) return;  /* not the duel drop */
    const uint32_t id = cpu->gpr[4] & 0xFFFFu;
    if (id < 1 || id > PSX_DROP_CARD_ID_MAX) return;
    if (!s_cd_new_this_duel[id]) {
        s_cd_new_this_duel[id] = 1;
        s_cd_new_distinct++;
        /* Pre-award trunk byte: 0 means the player holds no SPARE of this
         * card. Read HERE, at the entry hook, because the very next thing the
         * guest does is increment it. A deck copy is not counted there, so ask
         * the deck too -- New! means "you owned none of this", and a card you
         * are actively playing with does not qualify. */
        s_cd_was_new_this_duel[id] =
            (psx_mod_read_byte(PSX_DROP_TRUNK_BASE + id) == 0 &&
             !cd_owned_in_deck(id)) ? 1u : 0u;
    }
    if (s_cd_copies_this_duel[id] < 255u) s_cd_copies_this_duel[id]++;
    s_cd_awarded_total++;
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

static int cd_p3_gate(void) {
    return s_cd_page_duel && s_cd_awarded_total > 1;
}

/* The page's rows in display order: cards the player owned none of first,
 * then ascending id — the same order card_drops_list reports. */
static int cd_p3_row_ids(int *ids, int cap) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int id = 1; id <= PSX_DROP_CARD_ID_MAX && n < cap; id++) {
            if (!s_cd_copies_this_duel[id]) continue;
            if ((s_cd_was_new_this_duel[id] ? 1 : 0) != (pass == 0)) continue;
            ids[n++] = id;
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

/* Copy the current sub-page's stream into guest scratch and point the dead
 * name-table entry T[0] at it. Card id 0 does not exist, so entry 0 is never
 * resolved by the game itself; with it repointed, string id 0x8000 (card 0's
 * name) IS the CARD DROPS page. A savestate load restores the entry, and the
 * next publish re-poke makes that self-healing. A staged debug stream takes
 * precedence over the composer (escape experiments). */
static uint32_t cd_p3_publish_stream(int sub) {
    uint8_t buf[PSX_DROP_P3_SCRATCH_MAX];
    const uint8_t *src;
    int len;
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

void psx_mod_card_drops_on_results_state(CPUState *cpu,
                                                    uint32_t address) {
    if (!cpu || address != PSX_DROP_RESULTS_STATE_FN) return;
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    if (result) s_cd_p3_prev_page = psx_mod_read_byte(result + PSX_DROP_PAGE_OFF);
    s_cd_p3_state_ticks++;
}

/* Host-frame update for the New! tag overlay: visible only while the CARD
 * DROPS page is the one on screen AND the results state function is still
 * running (staleness catches the Cross exit, which fires no page apply). */
void psx_card_drops_tick(void) {
    static uint32_t last_ticks;
    static int stale;
    if (s_cd_p3_state_ticks != last_ticks) {
        last_ticks = s_cd_p3_state_ticks;
        stale = 0;
    } else if (stale < 1000) {
        stale++;
        /* The results screen is gone: this duel's card record must not
         * survive into the next duel's results (see s_cd_page_duel). */
        /* Threshold, not equality: `stale` saturates at its cap, so a record
         * created while it already sat there (or any time the counter had
         * run past the mark) would never have been cleared. */
        if (stale >= 64 && s_cd_page_duel) {
            s_cd_page_duel = 0;
            s_cd_awarded_total = 0;
            memset(s_cd_copies_this_duel, 0, sizeof s_cd_copies_this_duel);
            memset(s_cd_was_new_this_duel, 0, sizeof s_cd_was_new_this_duel);
        }
    }
    uint8_t rows[PSX_CD_OVERLAY_ROWS] = { 0 };
    const int on = s_cd_p3_active && stale < 8;
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
    if (!cpu || address != PSX_DROP_PAGE_APPLY_FN) return;
    /* Gate shut: also forget that the page was ever showing. Returning early
     * without clearing this left `active` true from a PREVIOUS duel's visit,
     * and the overlay tick reads it — so a duel that awarded a single card
     * could paint that card's "New!" sprite onto the stock summary page. */
    if (!cd_p3_gate()) { s_cd_p3_active = 0; return; }
    const uint32_t result = psx_mod_read_word(PSX_DROP_RESULT_PTR);
    if (!result) return;
    const uint16_t pad = psx_mod_read_half(PSX_DROP_PAD_NEW_ADDR);
    const int left  = (pad & PSX_DROP_PAD_LEFT)  != 0;
    const int right = (pad & PSX_DROP_PAD_RIGHT) != 0;
    /* The screen init also applies page 0; no press means it is not a page
     * TURN, so it is never ours to redirect. Entering the screen fresh also
     * resets the sub-page. */
    if (!left && !right) { s_cd_p3_active = 0; s_cd_p3_sub = 0; return; }
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
        return;
    }
    if (!enter) { s_cd_p3_active = 0; return; }         /* stock landing */
    cd_p3_publish_stream(s_cd_p3_sub);
    cpu->gpr[4] = 3;
    psx_mod_write_byte(result + PSX_DROP_PAGE_OFF, 3u);
    s_cd_p3_pending = 1;
    s_cd_p3_active = 1;
    s_cd_p3_applies++;
}

void psx_mod_card_drops_on_widget_draw(CPUState *cpu,
                                                  uint32_t address) {
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
    if (drops < 1 || drops > PSX_VM_CARD_DROPS_MAX) return 0;
    g_card_drops = drops;
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
    (void)psx_video_menu_add_number(
        PSX_VM_MENU_MODS, "CARD DROPS",
        "1 IS STOCK. CARDS PER DUEL WON",
        1, PSX_VM_CARD_DROPS_MAX, /*slider*/1,
        "card_drops", PSX_VM_CARD_DROPS_DEFAULT, cd_row_changed);
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
