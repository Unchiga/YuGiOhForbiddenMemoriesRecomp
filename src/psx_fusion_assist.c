/* psx_fusion_assist.c — the duel side of the fusion assistant.
 *
 * ---- where the hand lives -------------------------------------------------
 *
 * The duel keeps one 28-byte record per card in play, based at 0x801A7AE4.
 * That address is not a guess: func_8001A280, the routine that resolves a
 * two-card summon, forms it from immediates —
 *
 *     s2 = 0x8015C424, s0 = 0x00048000
 *     v0 = slot*28 + s2 + s0            = slot*28 + 0x801A4424
 *     lh a0, 14016(v0)                  = slot*28 + 0x801A7AE4
 *
 * — and hands the two halfwords it reads there straight to the fusion lookup.
 * So this array is, by construction, exactly what the game fuses. Confirmed by
 * writing ids into it and watching the summon produce what those ids fuse to.
 *
 * Fields used (all u16):
 *     +0  card id
 *     +2  attack
 *     +4  defence
 *     +10 flags; 0x8000 on a live card, cleared the moment it is consumed by a
 *         summon, and 0x8400 once it is a monster ON the field
 *     +12 the record's own slot index, 0-based
 *
 * ---- which slots are actually IN the hand ---------------------------------
 *
 * Not answerable from the records. The hand refills to five every turn, so at
 * rest slots 0..4 with the live flag are right — but during a summon the game
 * keeps spent materials in their records, flag and all, for the whole
 * animation. Measured: after picking slots 0 and 1 and summoning, the records
 * still read all five live for several seconds while the cards were visibly
 * gone. Worse, two slots that were being drawn and two that were not had
 * byte-identical record shapes, so no field in the record decides it.
 *
 * The selection table does decide it. Its per-slot entry holds the slot's card
 * OBJECT at +0, and the game tears those pointers down the instant the hand
 * stops being pickable: they read as five live pointers while the player is
 * choosing, and as five zeros from the first frame of the summon onward.
 * Checked against the game's own hand display pass (func_800170C8, called once
 * per card actually in the hand) across a turn: the two agree exactly while
 * the hand is pickable, which is the only time this feature has anything to
 * say. During the animation the gate reports an empty hand, which is the
 * honest answer — there is nothing to pick.
 *
 * Hooking the display pass would be the other way to do it, and was tried
 * first. It needs 0x800170C8 added to game.toml's `mod_function_entry_funcs`
 * and the whole game regenerated, because that hook is an opt-in allow-list
 * baked into the recompiled C. A pointer that is already in RAM, already
 * per-slot, and already means exactly "this hand position is pickable" is not
 * worth a regenerate.
 */

#include "psx_fusion_assist.h"

#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_fusion_db.h"

/* ---- the selection the player is building ---------------------------------
 *
 * Picking cards with Up does NOT touch the card records — nothing in the whole
 * 28-byte array moves — so the selection lives elsewhere: a parallel table of
 * five 12-byte entries at 0x800EA030, one per hand slot.
 *
 *     +0  the slot's card object, always present
 *     +4  the selection marker object; ZERO while the card is unpicked
 *     +9  the 1-based order in which this card was picked
 *
 * with the number picked mirrored in a byte at 0x800E9F25. Measured by picking
 * slots 4, 1 and 3 in that order and watching entries appear with ordinals
 * 1, 2, 3. Pressing Up on an already-picked card is a no-op, not a toggle.
 *
 * ---- and the order the chain folds in --------------------------------------
 *
 * SELECTION order, not left-to-right hand order. Picking slot 4, then 1, then
 * 3 (Maiden of the Moonlight, Ganigumo, Beaked Snake — no fusion among them)
 * summoned Beaked Snake, the last card PICKED. Folding by hand position would
 * have ended on Maiden of the Moonlight, so the two hypotheses are cleanly
 * separated and this is the one that happened. That also pins the fold's base
 * case: when a step does not fuse, the incoming card is what stands.
 */
/* Whose turn it is: 0 the player, 1 the opponent.
 *
 * The selection table alone is NOT enough of a gate. During the opponent's
 * turn the game keeps the hand's card objects allocated and the records
 * populated -- it just draws the cards face DOWN -- so every "is the hand
 * pickable" test built from them stays true and the hint kept giving advice
 * while the player could not act on it.
 *
 * Found by recording a full turn cycle: 49 whole-RAM samples, each paired with
 * a composited screenshot so every sample could be labelled by what was
 * actually on screen rather than by assumption, then keeping only the bytes
 * that held one value across all 44 player-turn samples and a different one
 * across all five opponent-turn samples -- including the summon animations,
 * which belong to whoever's turn they happen in. Four bytes survived; this is
 * the only one that also stayed put through the next turn's animations. */
#define PSX_FUSION_TURN       0x8009B1D5u

#define PSX_FUSION_SELECT     0x800EA030u
#define PSX_FUSION_SEL_STRIDE 12u
#define PSX_FUSION_SEL_MARK   4u    /* u32; nonzero => picked */
#define PSX_FUSION_SEL_ORDER  9u    /* byte; 1-based pick order */
#define PSX_FUSION_SEL_COUNT  0x800E9F25u

#define PSX_FUSION_RECORDS    0x801A7AE4u
#define PSX_FUSION_STRIDE     28u
#define PSX_FUSION_OFF_FLAGS  10u
#define PSX_FUSION_OFF_SLOT   12u
#define PSX_FUSION_FLAG_LIVE  0x8000u
#define PSX_FUSION_FLAG_FIELD 0x0400u
/* How many records `fusion_hand` reports. Wide enough to show the field slots
 * and any hand the game may have spread past slot 4. */
#define PSX_FUSION_WINDOW    12

static uint32_t rec_addr(int slot)
{
    return PSX_FUSION_RECORDS + (uint32_t)slot * PSX_FUSION_STRIDE;
}

/* Can the player act on their hand at all right now? */
static int players_turn(void)
{
    return psx_mod_read_byte(PSX_FUSION_TURN) == 0u;
}

/* Is this hand position currently a pickable card? */
static int slot_pickable(int slot)
{
    return psx_mod_read_word(PSX_FUSION_SELECT +
                             (uint32_t)slot * PSX_FUSION_SEL_STRIDE) != 0u;
}

/* Bitmask of pickable hand slots, for the read-back. */
static int hand_gate_mask(void)
{
    int m = 0;
    for (int slot = 0; slot < PSX_FUSION_HAND_MAX; slot++)
        if (slot_pickable(slot)) m |= 1 << slot;
    return m;
}

static void read_record(int slot, PsxFusionCard *c)
{
    const uint32_t a = rec_addr(slot);
    c->id    = psx_mod_read_half(a);
    c->atk   = psx_mod_read_half(a + 2u);
    c->def   = psx_mod_read_half(a + 4u);
    c->flags = psx_mod_read_half(a + PSX_FUSION_OFF_FLAGS);
    c->slot  = (uint8_t)slot;
}

int psx_fusion_assist_hand(PsxFusionCard *out, int cap)
{
    if (!out || cap <= 0) return 0;
    if (!players_turn()) return 0;
    int n = 0;
    for (int slot = 0; slot < PSX_FUSION_HAND_MAX && n < cap; slot++) {
        if (!slot_pickable(slot)) continue;
        PsxFusionCard c;
        read_record(slot, &c);
        if (c.id < 1 || c.id > PSX_FUSION_CARD_ID_MAX) continue;
        out[n++] = c;
    }
    return n;
}

int psx_fusion_assist_hand_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    const int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"base\":\"0x%08X\",\"stride\":%u,\"in_hand\":%d,"
                            "\"hand\":[", PSX_FUSION_RECORDS,
                            PSX_FUSION_STRIDE, n);
    for (int i = 0; i < n && p + 64u < cap; i++)
        p += (unsigned)snprintf(out + p, cap - p, "%s%u", i ? "," : "",
                                hand[i].id);
    p += (unsigned)snprintf(out + p, cap - p, "],\"records\":[");
    /* The whole 28-byte record, not just the fields this module names: the
     * "which slots are the hand" rule is still an observation, and the next
     * person to question it should be able to answer from this output rather
     * than by re-deriving the record layout. */
    for (int slot = 0; slot < PSX_FUSION_WINDOW && p + 260u < cap; slot++) {
        PsxFusionCard c;
        read_record(slot, &c);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"slot\":%d,\"addr\":\"0x%08X\",\"id\":%u,"
                                "\"atk\":%u,\"def\":%u,\"flags\":\"0x%04X\","
                                "\"index\":%u,\"words\":[",
                                slot ? "," : "", slot, rec_addr(slot),
                                c.id, c.atk, c.def, c.flags,
                                psx_mod_read_half(rec_addr(slot) +
                                                  PSX_FUSION_OFF_SLOT));
        for (unsigned w = 0; w < PSX_FUSION_STRIDE / 2u; w++)
            p += (unsigned)snprintf(out + p, cap - p, "%s%u", w ? "," : "",
                                    psx_mod_read_half(rec_addr(slot) + w * 2u));
        p += (unsigned)snprintf(out + p, cap - p, "]}");
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

/* What a pair actually lands on. An equip pair keeps the monster and adds the
 * equip's bonus to attack and defence alike, so reporting the monster's
 * printed line alone understates it by 500 -- 1000 for Megamorph. */
static void pair_stats(uint16_t a, uint16_t b, uint16_t result, int kind,
                       int *atk, int *def, int *type)
{
    int bonus = 0;
    if (kind == PSX_FUSION_EQUIP)
        bonus = psx_fusion_db_equip_bonus((result == a) ? b : a);
    int at = 0, df = 0;
    psx_fusion_db_stats(result, &at, &df, type);
    if (atk) *atk = at ? at + bonus : at;
    if (def) *def = df ? df + bonus : df;
}

int psx_fusion_assist_list_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    const int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);

    /* Every unordered pair. The fusion table is order-free and an equip gives
     * the same monster either way, so one direction per pair is the whole
     * answer -- but the ORDER the player selects in still decides which card
     * the game keeps, so report the pair as (first, second) with `first` being
     * the lower hand position, which is what selecting left-to-right does. */
    typedef struct { int i, j; uint16_t result; int kind; } Hit;
    Hit hits[PSX_FUSION_HAND_MAX * (PSX_FUSION_HAND_MAX - 1) / 2];
    int nhits = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int kind = PSX_FUSION_NONE;
            const uint16_t r =
                psx_fusion_db_result(hand[i].id, hand[j].id, &kind);
            if (!r) continue;
            hits[nhits].i = i;
            hits[nhits].j = j;
            hits[nhits].result = r;
            hits[nhits].kind = kind;
            nhits++;
        }
    }

    /* Strongest first, by the RESULT's printed stats -- not the materials',
     * which is why this needs the card table rather than the hand records.
     * Attack decides, defence breaks ties: a hand offering both Kaminari
     * Attack (1900/1500) and Nekogal #2 (1900/2000) should put Nekogal on top,
     * which is the call a player makes too. Insertion sort over at most ten. */
    for (int a = 1; a < nhits; a++) {
        Hit v = hits[a];
        int va = 0, vd = 0;
        pair_stats(hand[v.i].id, hand[v.j].id, v.result, v.kind, &va, &vd, NULL);
        int b = a - 1;
        for (; b >= 0; b--) {
            int ba = 0, bd = 0;
            pair_stats(hand[hits[b].i].id, hand[hits[b].j].id,
                       hits[b].result, hits[b].kind, &ba, &bd, NULL);
            if (ba > va || (ba == va && bd >= vd)) break;
            hits[b + 1] = hits[b];
        }
        hits[b + 1] = v;
    }

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"in_hand\":%d,\"count\":%d,"
                            "\"fusions\":[",
                            psx_fusion_db_ready(), n, nhits);
    for (int k = 0; k < nhits && p + 240u < cap; k++) {
        int atk = 0, def = 0, type = -1;
        pair_stats(hand[hits[k].i].id, hand[hits[k].j].id,
                   hits[k].result, hits[k].kind, &atk, &def, &type);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"a\":%u,\"b\":%u,\"slot_a\":%d,"
                                "\"slot_b\":%d,\"result\":%u,\"kind\":%d,"
                                "\"atk\":%d,\"def\":%d,\"type\":%d}",
                                k ? "," : "",
                                hand[hits[k].i].id, hand[hits[k].j].id,
                                hand[hits[k].i].slot, hand[hits[k].j].slot,
                                hits[k].result, hits[k].kind, atk, def, type);
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

/* One step of the fold: what stands after putting `next` onto `carry`. A step
 * that does not fuse leaves the incoming card standing, which is what the game
 * does — so this never returns 0 for a real card. */
static uint16_t chain_step(uint16_t carry, uint16_t next, int *out_kind)
{
    const uint16_t r = psx_fusion_db_result(carry, next, out_kind);
    return r ? r : next;
}

/* One fold step, tracking the equip bonus alongside the surviving card.
 *
 * An equip leaves the monster standing and ADDS to a bonus that applies to
 * attack and defence alike, so a line's real stats are the surviving card's
 * printed stats plus everything equipped onto it. Without this the search
 * scored every equip at zero and line_better's "shorter wins" tie-break threw
 * the longer, stronger line away.
 *
 * A step that produces a DIFFERENT card summons a new one, so the bonus does
 * not follow: the equips were spent on the card that just got consumed. That
 * reset is the conservative reading and is NOT yet verified in game -- the
 * verified case is equips accumulating onto one monster. */
static uint16_t fold_step(uint16_t carry, uint16_t next, int *bonus,
                          int *out_kind)
{
    int kind = PSX_FUSION_NONE;
    const uint16_t r = psx_fusion_db_result(carry, next, &kind);
    if (out_kind) *out_kind = kind;
    if (kind == PSX_FUSION_EQUIP) {
        /* The equip is whichever of the two did NOT survive. */
        const uint16_t worn = (r == carry) ? next : carry;
        if (bonus) *bonus += psx_fusion_db_equip_bonus(worn);
        return r;
    }
    if (bonus) *bonus = 0;
    return r ? r : next;
}

/* Printed stats of `id`, plus everything equipped onto it. */
static void line_stats(uint16_t id, int bonus, int *atk, int *def)
{
    int a = 0, d = 0;
    psx_fusion_db_stats(id, &a, &d, NULL);
    if (atk) *atk = a ? a + bonus : a;
    if (def) *def = d ? d + bonus : d;
}

int psx_fusion_assist_chain(PsxFusionCard *steps, int cap, uint16_t *out_result)
{
    if (out_result) *out_result = 0;
    if (!steps || cap <= 0) return 0;

    /* Gather the picked slots, ordered by the pick ordinal rather than by slot,
     * because that is the order the fold runs in. */
    PsxFusionCard picked[PSX_FUSION_HAND_MAX];
    uint8_t order[PSX_FUSION_HAND_MAX];
    int n = 0;
    for (int slot = 0; slot < PSX_FUSION_HAND_MAX; slot++) {
        const uint32_t e = PSX_FUSION_SELECT + (uint32_t)slot * PSX_FUSION_SEL_STRIDE;
        if (!psx_mod_read_word(e + PSX_FUSION_SEL_MARK)) continue;
        PsxFusionCard c;
        read_record(slot, &c);
        if (c.id < 1 || c.id > PSX_FUSION_CARD_ID_MAX) continue;
        picked[n] = c;
        order[n] = psx_mod_read_byte(e + PSX_FUSION_SEL_ORDER);
        n++;
    }
    for (int a = 1; a < n; a++) {
        PsxFusionCard c = picked[a];
        const uint8_t o = order[a];
        int b = a - 1;
        for (; b >= 0 && order[b] > o; b--) {
            picked[b + 1] = picked[b];
            order[b + 1] = order[b];
        }
        picked[b + 1] = c;
        order[b + 1] = o;
    }

    uint16_t carry = n ? picked[0].id : 0;
    for (int i = 0; i < n && i < cap; i++) steps[i] = picked[i];
    for (int i = 1; i < n; i++) carry = chain_step(carry, picked[i].id, NULL);
    if (out_result) *out_result = carry;
    return n < cap ? n : cap;
}

int psx_fusion_assist_chain_json(char *out, unsigned cap)
{
    if (!out || cap < 192u) return 0;
    PsxFusionCard steps[PSX_FUSION_HAND_MAX];
    uint16_t result = 0;
    const int n = psx_fusion_assist_chain(steps, PSX_FUSION_HAND_MAX, &result);

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"picked\":%d,\"guest_count\":%u,"
                            "\"result\":%u,\"steps\":[",
                            psx_fusion_db_ready(), n,
                            psx_mod_read_byte(PSX_FUSION_SEL_COUNT), result);
    /* Replay the fold so each step reports the card standing after it — that
     * running value IS what the preview shows as the player picks. */
    uint16_t carry = n ? steps[0].id : 0;
    int bonus = 0;
    for (int i = 0; i < n && p + 160u < cap; i++) {
        int kind = PSX_FUSION_NONE;
        if (i) carry = fold_step(carry, steps[i].id, &bonus, &kind);
        int atk = 0, def = 0;
        line_stats(carry, bonus, &atk, &def);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"slot\":%u,\"id\":%u,\"carry\":%u,"
                                "\"kind\":%d,\"atk\":%d,\"def\":%d,"
                                "\"bonus\":%d}",
                                i ? "," : "", steps[i].slot, steps[i].id,
                                carry, kind, atk, def, bonus);
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

/* ---- the best line in the hand --------------------------------------------
 *
 * "The highest-attack monster you can make, using as few cards as possible."
 * Attack decides; among lines that reach the same attack the shortest wins,
 * and after that the strongest defence, so a two-card line is never passed
 * over for a four-card one that lands on the same card.
 *
 * Searched by brute force over every ORDERED subset of the hand, because the
 * fold is order-sensitive: with five cards that is 320 sequences, each a
 * handful of table reads. The recursion carries the running card, so a prefix
 * is evaluated once and extended rather than re-folded per permutation. */
typedef struct {
    int      atk, def, len;
    int      bonus;           /* equip bonus folded into atk/def above */
    uint16_t result;
    uint8_t  slot[PSX_FUSION_HAND_MAX];
} BestLine;

/* Which stat the search maximises. Attack by default -- the usual question is
 * "what is the biggest thing I can put down" -- but a defensive turn wants the
 * other one, and the two disagree often enough to be worth a switch. The
 * loser stat stays as the last tie-break either way. */
static int s_rank_by_def;

void psx_fusion_assist_set_rank(int by_defence)
{
    s_rank_by_def = by_defence ? 1 : 0;
}

int psx_fusion_assist_get_rank(void) { return s_rank_by_def; }

static int line_better(const BestLine *a, const BestLine *b)
{
    if (!b->len) return 1;
    const int ap = s_rank_by_def ? a->def : a->atk;
    const int bp = s_rank_by_def ? b->def : b->atk;
    if (ap != bp) return ap > bp;
    if (a->len != b->len) return a->len < b->len;
    return (s_rank_by_def ? a->atk : a->def) > (s_rank_by_def ? b->atk : b->def);
}

/* `last_fused` says the step that produced `carry` was a REAL fusion. Without
 * it every pair of cards looks like a line, because a step that does not fuse
 * still leaves the incoming card standing -- so the search would happily
 * "recommend" two unrelated cards whose only effect is discarding the first.
 * A hand with no fusions in it must come back empty, not come back with the
 * biggest card in it dressed up as a suggestion.
 *
 * It tracks the LAST step, not whether the line fused ANYWHERE. A sticky flag
 * looks equivalent and is not: once any step fused it stayed set, so a line
 * could fuse early, then take a step that does NOT fuse -- leaving the
 * incoming card standing as the result -- and still be recorded, scored at
 * that card's printed attack. Measured in a live duel: Amazon of the Seas +
 * Tentacle Plant fuse to 638, then 638 + Gate Guardian does not fuse, and the
 * search returned Gate Guardian at 3750 as a three-card "fusion". Its real
 * effect is discarding the first two cards to play a card that was already
 * playable on its own, which is why a line whose last step does not fuse is
 * never worth recommending. */
static void best_search(const PsxFusionCard *hand, int n, uint8_t used,
                        uint16_t carry, int bonus, int depth, uint8_t *path,
                        int last_fused, BestLine *best)
{
    /* Belt and braces on the recursion depth. `n` cannot exceed the hand size,
     * but that is only provable at the call site, and this writes into a
     * fixed-size array on every level -- so bound it where the writes are. */
    if (depth > PSX_FUSION_HAND_MAX) return;
    if (depth >= 2 && last_fused) {
        BestLine cand;
        cand.result = carry;
        cand.len = depth;
        cand.bonus = bonus;
        cand.atk = cand.def = 0;
        line_stats(carry, bonus, &cand.atk, &cand.def);
        for (int i = 0; i < depth; i++) cand.slot[i] = path[i];
        if (line_better(&cand, best)) *best = cand;
    }
    if (depth >= n || depth >= PSX_FUSION_HAND_MAX) return;
    for (int i = 0; i < n; i++) {
        if (used & (1u << i)) continue;
        int kind = PSX_FUSION_NONE;
        int nbonus = bonus;
        const uint16_t next = depth ? fold_step(carry, hand[i].id, &nbonus, &kind)
                                    : hand[i].id;
        if (!depth) nbonus = 0;
        path[depth] = hand[i].slot;
        best_search(hand, n, (uint8_t)(used | (1u << i)), next, nbonus,
                    depth + 1, path, kind != PSX_FUSION_NONE, best);
    }
}

int psx_fusion_assist_best(uint16_t *result, int *atk, int *def, int *cards,
                           uint8_t *pick, int pick_cap)
{
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);
    if (n > PSX_FUSION_HAND_MAX) n = PSX_FUSION_HAND_MAX;

    BestLine best;
    memset(&best, 0, sizeof best);
    uint8_t path[PSX_FUSION_HAND_MAX];
    if (psx_fusion_db_ready())
        best_search(hand, n, 0, 0, 0, 0, path, 0, &best);

    if (result) *result = best.result;
    if (atk)    *atk    = best.atk;
    if (def)    *def    = best.def;
    if (cards)  *cards  = best.len;
    if (pick)
        for (int i = 0; i < best.len && i < pick_cap; i++) pick[i] = best.slot[i];
    return best.len;
}

int psx_fusion_assist_best_json(char *out, unsigned cap)
{
    if (!out || cap < 192u) return 0;
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);
    if (n > PSX_FUSION_HAND_MAX) n = PSX_FUSION_HAND_MAX;

    BestLine best;
    memset(&best, 0, sizeof best);
    psx_fusion_assist_best(&best.result, &best.atk, &best.def, &best.len,
                           best.slot, PSX_FUSION_HAND_MAX);

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"in_hand\":%d,\"result\":%u,"
                            "\"atk\":%d,\"def\":%d,\"cards\":%d,\"pick\":[",
                            psx_fusion_db_ready(), n, best.result,
                            best.atk, best.def, best.len);
    for (int i = 0; i < best.len && p + 24u < cap; i++)
        p += (unsigned)snprintf(out + p, cap - p, "%s%u", i ? "," : "",
                                best.slot[i]);
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

/* Hand gate state for `fusion_hand`: which slots the game currently treats as
 * pickable, and the selection count it keeps alongside them. */
void psx_fusion_assist_hand_source(int *mask, int *sel_count, int *turn)
{
    if (mask)      *mask      = hand_gate_mask();
    if (sel_count) *sel_count = psx_mod_read_byte(PSX_FUSION_SEL_COUNT);
    if (turn)      *turn      = (int)psx_mod_read_byte(PSX_FUSION_TURN);
}

int psx_fusion_assist_try_json(char *out, unsigned cap, int a, int b)
{
    if (!out || cap < 128u) return 0;
    int kind = PSX_FUSION_NONE;
    const uint16_t r = psx_fusion_db_result((uint16_t)a, (uint16_t)b, &kind);
    return snprintf(out, cap,
                    "\"ready\":%d,\"a\":%d,\"b\":%d,\"result\":%u,\"kind\":%d",
                    psx_fusion_db_ready(), a, b, r, kind);
}
