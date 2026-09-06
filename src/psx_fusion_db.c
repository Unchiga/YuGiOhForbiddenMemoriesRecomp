/* psx_fusion_db.c — the game's fusion data, read live. See psx_fusion_db.h.
 *
 * ---- how a fusion is decided ----------------------------------------------
 *
 * Forbidden Memories resolves a two-card summon in func_8001A280, which makes
 * exactly three attempts and takes the first that answers (established by
 * tracing the calls during a real in-duel fusion, 2026-08-18):
 *
 *     v0 = func_80019A60(first, second)          the fusion table
 *     if (v0) -> that card is summoned
 *     v0 = func_80019A08(second, first)          equip table, one order
 *     if (v0) -> v0 (== first) survives
 *     v0 = func_80019A08(first, second)          equip table, other order
 *     if (v0) -> v0 (== second) survives
 *     otherwise no fusion: `second` is summoned as itself
 *
 * The two ids it passes come from the 28-byte card records the duel keeps at
 * 0x801A7AE4 (`lh a0, 14016(v0)` with v0 = slot*28 + 0x801A4424); the id is at
 * record+0. psx_fusion_assist.c owns that array.
 *
 * ---- func_80019A60: the fusion table --------------------------------------
 *
 * Base 0x8017C2D8. The routine sorts its arguments first, so a pair is stored
 * exactly once, under min(a,b), with max(a,b) as the partner:
 *
 *     off = u16 at base + min*2          0 -> this card fuses with nothing
 *     rec = base + off
 *     count = rec[0]                     body at rec+1
 *             ...unless rec[0] == 0, in which case
 *             count = 511 - rec[1]       body at rec+2   (counts 256..511)
 *
 * The body is groups of FIVE bytes, each carrying TWO (partner, result) pairs.
 * Card ids are 10 bits, and the four high halves are packed into the group's
 * first byte, two bits each:
 *
 *     partner1 = ((g[0] << 8) & 0x300) | g[1]
 *     result1  = ((g[0] << 6) & 0x300) | g[2]
 *     partner2 = ((g[0] << 4) & 0x300) | g[3]
 *     result2  = ((g[0] << 2) & 0x300) | g[4]
 *
 * An ODD count writes only THREE bytes for the final group (g[0..2]) — the
 * one entry it holds — but the guest's loop consumes two entries per group and
 * stops on `count <= 0` AFTER the decrement, so it still tests a second half
 * that is not there: g[3] and g[4] are the FIRST TWO BYTES OF THE NEXT RECORD.
 *
 * That is not padding and it does not read as zero. Of the 293 odd records in
 * the shipping table, 263 decode to a nonzero result; 245 name a partner below
 * the record's own card id (the lookup sorts its arguments, so that record is
 * never the one consulted for the pair), 1 is out of range, and 2 name a
 * partner an earlier entry in the same record already answered. The remaining
 * FIFTEEN are pairs the game really does fuse and the table never meant to
 * hold — the "glitch fusions":
 *
 *     22+31=72   23+147=4   26+138=4    31+34=84    40+126=4
 *     45+170=136 61+144=136 64+168=72   66+98=132   74+150=72
 *     90+130=136 96+231=136 120+166=132 127+128=72  167+225=64
 *
 * Copying the guest's loop, as pair_lookup does below, reproduces all fifteen
 * for free. An ENUMERATION has to be more careful — see the walk near the end
 * of this file. (Established against the disc bytes and a transcription of the
 * lookup over all 261003 unordered pairs, 2026-09-05.)
 *
 * ---- func_80019A08: the equip table ---------------------------------------
 *
 * Base 0x8017A1D8. A flat list of groups, terminated by a zero key:
 *
 *     u16 key            an equip card
 *     u16 count
 *     u16 member[count]  the monsters it may attach to
 *
 * The routine returns its SECOND argument when that argument is in the first
 * argument's member list. That is the equip mechanic: the monster stays on the
 * field and the equip powers it up, so the monster is the "result". 34 groups
 * covering 4041 memberships in the shipping data — this is the whole of the
 * "magic cards that fuse" behaviour.
 *
 * ---- why read the tables instead of calling the guest ---------------------
 *
 * psx_card_drops.c re-runs the game's own drop roll through psx_dispatch_call
 * because that roll is stateful: it draws from a per-opponent pool and burns
 * RNG, and reimplementing it would silently drift. Nothing here is stateful.
 * These are two static tables in the game's data segment, and the lookups are
 * the pure functions transcribed above, so reading the tables IS the game's
 * answer — with none of the cost or risk of re-entering guest code from a
 * frame tick, which is what an assistant that re-evaluates on every hand
 * change would be doing dozens of times a second.
 *
 * Both tables are DUEL data: the game streams them in from disc when a duel
 * starts and they read back as solid zeros everywhere else, the same way the
 * rank coefficients psx_rank_logic.c uses do. That is why readiness is probed
 * rather than assumed, and why this module answers only inside a duel -- which
 * is the only place the question is asked anyway. The probe leans on the EQUIP
 * table, not this one: psx_fusion_table.c can legitimately empty the fusion
 * table, and a probe that treats "empty" as "absent" would report the tables
 * missing when they are merely bare.
 *
 * Checked against the game itself rather than against a FAQ: five pairs were
 * injected into a live duel's hand records and summoned, and the game produced
 * exactly what these tables predict, including two cases where the widely
 * mirrored GameFAQs fusion list is wrong (Air Marmot of Nefariousness +
 * Greenkappa is Tiger Axe, not Flower Wolf; Dissolverock + Skull Servant is
 * Stone Ghost, not Flame Ghost) and one where it invents a fusion that does
 * not exist (Bone Mouse + Skull Servant).
 */

#include "psx_fusion_db.h"

#include <string.h>

#include "mod_plugins.h"

#define PSX_FUSION_PAIR_BASE   0x8017C2D8u
#define PSX_FUSION_EQUIP_BASE  0x8017A1D8u
/* The index array is one u16 per card id plus entry 0, so the first record
 * cannot start before it ends. Doubles as the validation floor. */
#define PSX_FUSION_INDEX_BYTES ((PSX_FUSION_CARD_ID_MAX + 1) * 2)
/* Generous walk limits. The shipping tables are 34 equip groups and 578 cards
 * with fusion records; these only exist so a garbage table cannot spin. */
#define PSX_FUSION_MAX_EQUIP_GROUPS 256
#define PSX_FUSION_MAX_PAIRS        1024
/* the live part of the equip chunk: stage 2 copies 0x2800 bytes but the fusion
 * chunk starts 0x2100 later in RAM, so the tail of it is dead */
#define PSX_FUSION_EQUIP_BYTES      0x2100u

/* Cached verdict, plus the cheap sentinel it is valid for. Both tables are
 * DUEL data loaded from disc, not part of the EXE image: outside a duel every
 * byte of them reads back as zero. So readiness cannot be latched once and
 * kept -- a stale "ready" would turn "the tables are gone" into the much worse
 * answer "these cards do not fuse". The sentinel below is three halfwords, so
 * re-checking it on every query costs nothing, and the full walk only re-runs
 * when it changes. */
static int s_ready;
static int s_sentinel_ok = -1;

static uint16_t pair_lookup(uint16_t a, uint16_t b)
{
    uint16_t lo = a < b ? a : b;
    uint16_t hi = a < b ? b : a;
    if (lo < 1 || hi > PSX_FUSION_CARD_ID_MAX) return 0;

    uint32_t off = psx_mod_read_half(PSX_FUSION_PAIR_BASE + (uint32_t)lo * 2u);
    if (off < PSX_FUSION_INDEX_BYTES) return 0;

    uint32_t p = PSX_FUSION_PAIR_BASE + off;
    int count = psx_mod_read_byte(p);
    if (count) {
        p += 1u;
    } else {
        count = 511 - psx_mod_read_byte(p + 1u);
        p += 2u;
    }
    if (count <= 0 || count > PSX_FUSION_MAX_PAIRS) return 0;

    for (; count > 0; count -= 2, p += 5u) {
        const uint8_t g0 = psx_mod_read_byte(p);
        if ((((g0 << 8) & 0x300) | psx_mod_read_byte(p + 1u)) == hi)
            return (uint16_t)(((g0 << 6) & 0x300) | psx_mod_read_byte(p + 2u));
        if ((((g0 << 4) & 0x300) | psx_mod_read_byte(p + 3u)) == hi)
            return (uint16_t)(((g0 << 2) & 0x300) | psx_mod_read_byte(p + 4u));
    }
    return 0;
}

/* func_80019A08: is `member` in `key`'s group? Returns `member` if so. */
static uint16_t equip_lookup(uint16_t key, uint16_t member)
{
    uint32_t p = PSX_FUSION_EQUIP_BASE;
    for (int g = 0; g < PSX_FUSION_MAX_EQUIP_GROUPS; g++) {
        const uint16_t k = psx_mod_read_half(p);
        if (k == 0) return 0;
        const uint16_t count = psx_mod_read_half(p + 2u);
        p += 4u;
        if (k != key) { p += (uint32_t)count * 2u; continue; }
        for (uint16_t i = 0; i < count; i++)
            if (psx_mod_read_half(p + (uint32_t)i * 2u) == member) return member;
        return 0;
    }
    return 0;
}

/* Both tables live in the game's data segment, so before the EXE is in place
 * this reads whatever RAM happens to hold. Rather than trust the addresses,
 * check the shape: a real index has hundreds of live entries and every one of
 * them points past the index array itself, and a real equip table opens with a
 * plausible card id and member count. */
static int validate(void)
{
    for (int cid = 1; cid <= PSX_FUSION_CARD_ID_MAX; cid++) {
        const uint16_t off = psx_mod_read_half(PSX_FUSION_PAIR_BASE + (uint32_t)cid * 2u);
        if (!off) continue;
        if (off < PSX_FUSION_INDEX_BYTES) return 0;
    }
    /* NOT "and at least 256 cards have a record". That was here to reject
     * uninitialised RAM, but the equip check below already does that job, and
     * the population test made a legitimate table look like garbage: the
     * Fusion Manager can DELETE recipes, and an emptied table has one live
     * index entry or none. Failing readiness there would have told the duel
     * assistant the tables were missing rather than empty -- the one wrong
     * answer this module exists to avoid. */

    /* The EQUIP table takes over the job the population test used to do, and
     * is walked end to end rather than sniffed at: a chain of length-prefixed
     * groups that terminates on a zero key inside its 0x2100 bytes, with every
     * key and count a plausible card id, is a shape random memory does not
     * hold. Uninitialised RAM reads as a zero first key, which is no groups,
     * which fails here.
     *
     * This is a garbage filter and nothing more. It is NOT what keeps the
     * duel assistant off the other screens -- psx_fusion_overlay.c does that
     * itself, refusing to compose unless the mode byte at 0x8009B26C reads
     * 0xC3 (066f2b5, "Fusion overlay: only compose inside an actual duel").
     * Do not talk yourself into thinking readiness is load-bearing for that;
     * it is not, and the overlay's own gate is the thing to check. */
    uint32_t p = PSX_FUSION_EQUIP_BASE;
    const uint32_t end = PSX_FUSION_EQUIP_BASE + PSX_FUSION_EQUIP_BYTES;
    int groups = 0;
    for (;;) {
        if (p + 4u > end) return 0;              /* ran off without a terminator */
        const uint16_t key = psx_mod_read_half(p);
        if (key == 0) break;
        if (key > PSX_FUSION_CARD_ID_MAX) return 0;
        const uint16_t cnt = psx_mod_read_half(p + 2u);
        if (cnt < 1 || cnt > PSX_FUSION_CARD_ID_MAX) return 0;
        p += 4u + (uint32_t)cnt * 2u;
        if (++groups > PSX_FUSION_MAX_EQUIP_GROUPS) return 0;
    }
    return groups > 0;
}

/* Is the duel data plausibly resident? Three reads, no walk. */
static int sentinel(void)
{
    if (psx_mod_read_half(PSX_FUSION_EQUIP_BASE) - 1u >= PSX_FUSION_CARD_ID_MAX)
        return 0;
    if (psx_mod_read_half(PSX_FUSION_EQUIP_BASE + 2u) - 1u >= PSX_FUSION_CARD_ID_MAX)
        return 0;
    /* The fusion index used to be probed here too (card 2's record offset),
     * which an emptied table zeroes. The equip table is the same 235-sector
     * duel block, streamed in one stage earlier, so it answers "is the duel
     * data resident" on its own -- and unlike the fusion index, nothing can
     * legitimately empty it. */
    return 1;
}

int psx_fusion_db_ready(void)
{
    const int now = sentinel();
    if (now != s_sentinel_ok) {
        s_sentinel_ok = now;
        s_ready = now ? validate() : 0;
    }
    return s_ready;
}

uint16_t psx_fusion_db_result(uint16_t first, uint16_t second, int *out_kind)
{
    if (out_kind) *out_kind = PSX_FUSION_NONE;
    if (!psx_fusion_db_ready()) return 0;
    if (first < 1 || first > PSX_FUSION_CARD_ID_MAX) return 0;
    if (second < 1 || second > PSX_FUSION_CARD_ID_MAX) return 0;

    uint16_t r = pair_lookup(first, second);
    if (r) {
        if (r > PSX_FUSION_CARD_ID_MAX) return 0;   /* padding entry */
        if (out_kind) *out_kind = PSX_FUSION_PAIR;
        return r;
    }
    /* Equip, in the two orders the caller tries, in that order. */
    if (equip_lookup(second, first)) {
        if (out_kind) *out_kind = PSX_FUSION_EQUIP;
        return first;
    }
    if (equip_lookup(first, second)) {
        if (out_kind) *out_kind = PSX_FUSION_EQUIP;
        return second;
    }
    return 0;
}

/* ---- walking the whole table ---------------------------------------------
 *
 * The same decode as pair_lookup and equip_lookup, run over every record
 * instead of stopping at a match. Kept beside them so the two can never
 * drift: a change to the record format has to be made once, here.
 *
 * The pair walk emits BOTH halves of every group, including the trailing
 * half of an odd count, exactly as the guest's loop tests both -- and drops
 * whatever decodes to a zero or out-of-range id, which is what that padding
 * always decodes to and what the caller would read as "no fusion" anyway. */

int psx_fusion_db_walk_pairs(PsxFusionPairFn fn, void *ud)
{
    if (!fn || !psx_fusion_db_ready()) return 0;
    int seen = 0;
    for (uint16_t lo = 1; lo <= PSX_FUSION_CARD_ID_MAX; lo++) {
        const uint32_t off =
            psx_mod_read_half(PSX_FUSION_PAIR_BASE + (uint32_t)lo * 2u);
        if (off < PSX_FUSION_INDEX_BYTES) continue;

        uint32_t p = PSX_FUSION_PAIR_BASE + off;
        int count = psx_mod_read_byte(p);
        if (count) {
            p += 1u;
        } else {
            count = 511 - psx_mod_read_byte(p + 1u);
            p += 2u;
        }
        if (count <= 0 || count > PSX_FUSION_MAX_PAIRS) continue;

        /* which partners this record has already answered (rule 2 below) */
        uint8_t seen_partner[(PSX_FUSION_CARD_ID_MAX + 8) / 8];
        memset(seen_partner, 0, sizeof seen_partner);

        for (; count > 0; count -= 2, p += 5u) {
            const uint8_t g0 = psx_mod_read_byte(p);
            const uint16_t half[2][2] = {
                { (uint16_t)(((g0 << 8) & 0x300) | psx_mod_read_byte(p + 1u)),
                  (uint16_t)(((g0 << 6) & 0x300) | psx_mod_read_byte(p + 2u)) },
                { (uint16_t)(((g0 << 4) & 0x300) | psx_mod_read_byte(p + 3u)),
                  (uint16_t)(((g0 << 2) & 0x300) | psx_mod_read_byte(p + 4u)) },
            };
            for (int h = 0; h < 2; h++) {
                const uint16_t partner = half[h][0], result = half[h][1];
                if (partner < 1 || partner > PSX_FUSION_CARD_ID_MAX) continue;
                if (result < 1 || result > PSX_FUSION_CARD_ID_MAX) continue;
                /* UNREACHABLE ENTRIES. Two rules, both from how the guest
                 * lookup walks a record — see the header of this file for
                 * where they come from and what the shipping table holds.
                 *
                 * 1. pair_lookup only ever searches the record of min(a, b)
                 *    for max(a, b), so an entry whose partner is BELOW its
                 *    own key can never be matched. (partner == key stays:
                 *    two copies of one card do fuse, and 50 such entries are
                 *    real.) This is what removes the bulk of the trailing
                 *    halves that spill into the next record.
                 * 2. The loop returns on its FIRST match, so a second entry
                 *    naming a partner already seen in this record is dead.
                 *
                 * What survives is exactly what the game answers, the
                 * fifteen glitch fusions included: 25146 pairs, against the
                 * 25131 the table intends. */
                if (partner < lo) continue;
                if (seen_partner[partner >> 3] & (uint8_t)(1u << (partner & 7))) continue;
                seen_partner[partner >> 3] |= (uint8_t)(1u << (partner & 7));
                seen++;
                if (!fn(ud, lo, partner, result)) return seen;
            }
        }
    }
    return seen;
}

int psx_fusion_db_walk_equips(PsxFusionEquipFn fn, void *ud)
{
    if (!fn || !psx_fusion_db_ready()) return 0;
    int seen = 0;
    uint32_t p = PSX_FUSION_EQUIP_BASE;
    for (int g = 0; g < PSX_FUSION_MAX_EQUIP_GROUPS; g++) {
        const uint16_t key = psx_mod_read_half(p);
        if (key == 0) break;
        const uint16_t count = psx_mod_read_half(p + 2u);
        p += 4u;
        for (uint16_t i = 0; i < count; i++) {
            const uint16_t member = psx_mod_read_half(p + (uint32_t)i * 2u);
            if (member < 1 || member > PSX_FUSION_CARD_ID_MAX) continue;
            seen++;
            if (!fn(ud, key, member)) return seen;
        }
        p += (uint32_t)count * 2u;
    }
    return seen;
}

/* ---- the card table -------------------------------------------------------
 *
 * 0x801D4244, one 32-bit word per card, indexed by id-1. Read off the routine
 * that fills a card record when a hand is dealt (0x80024A94..0x80024B24),
 * which is also where the record's atk/def come from:
 *
 *     w = u32 at 0x801D4244 + (id - 1) * 4
 *     attack  = (w        & 0x1FF) * 10        `andi 0x1FF` then *5 then *2
 *     defence = ((w >> 9) & 0x1FF) * 10        `sra 9`, same scaling
 *     type    = (w >> 26) & 0x1F               `sra 26`, `andi 0x1F`
 *
 * Attack and defence check out against the GameFAQs password guide on 614 of
 * its 620 listed cards, and every one of the six disagreements is the guide
 * being wrong, not this: Castle of Dark Illusions really is 920/1930, Dragon
 * Zombie really is 1600/0, and Monster Eye's 250/350 was independently
 * confirmed from the game's own hand record earlier. The type code lands 604 of
 * 614 cards in the right group, the strays again being guide errors (one of
 * them spells Insect "Incest").
 *
 * Bits 18..25 are unread here. They are almost certainly the two guardian
 * stars at four bits each, since that is exactly the gap between defence and
 * type — but "almost certainly" is not measured, so nothing depends on it. */
#define PSX_FUSION_STATS_BASE 0x801D4244u

int psx_fusion_db_stats(uint16_t id, int *atk, int *def, int *type)
{
    if (id < 1 || id > PSX_FUSION_CARD_ID_MAX) return 0;
    const uint32_t w =
        psx_mod_read_word(PSX_FUSION_STATS_BASE + ((uint32_t)id - 1u) * 4u);
    if (!w) return 0;               /* table not resident, or no such card */
    if (atk)  *atk  = (int)(w & 0x1FFu) * 10;
    if (def)  *def  = (int)((w >> 9) & 0x1FFu) * 10;
    if (type) *type = (int)((w >> 26) & 0x1Fu);
    return 1;
}

/* ---- what an equip is worth ----------------------------------------------
 *
 * Traced at 0x8001A7F8, not taken from a FAQ. The summon path stores a flat
 * 500 into the target's bonus field, then overrides it to 1000 for Megamorph
 * (id 657) alone:
 *
 *     8001A7F8  li   v0, 500
 *     8001A800  sh   v0, 42(s1)
 *     8001A814  lh   v1, 766(gp)      the equip's card id
 *     8001A818  li   v0, 657          Megamorph
 *     8001A81C  bne  v1, v0 -> skip   anything else keeps 500
 *     8001A828  sh   v1, 42(s1)       ...Megamorph stores 1000
 *
 * There is no per-equip bonus in the card table to read instead: every equip's
 * stats word is byte-identical (0x5C000000 -- atk 0, def 0, type 23), so this
 * two-case rule is the whole of it.
 *
 * The bonus lands on ATTACK AND DEFENCE ALIKE, and successive equips ADD into
 * one accumulator at card-record +6. Verified in a live duel: Blue-Eyes White
 * Dragon (3000/2500) fused with Dragon Treasure and Megamorph came out
 * 4500/4000, with that field reading 1500 = 500 + 1000. */
#define PSX_FUSION_MEGAMORPH_ID 657u

int psx_fusion_db_equip_bonus(uint16_t equip_id)
{
    return equip_id == PSX_FUSION_MEGAMORPH_ID ? 1000 : 500;
}

void psx_fusion_db_debug(int *ready, uint32_t *pair_base, uint32_t *equip_base,
                         int *cards_with_fusions, int *pairs,
                         int *equip_groups, int *equip_members)
{
    if (ready)      *ready      = psx_fusion_db_ready();
    if (pair_base)  *pair_base  = PSX_FUSION_PAIR_BASE;
    if (equip_base) *equip_base = PSX_FUSION_EQUIP_BASE;

    if (cards_with_fusions || pairs) {
        int cards = 0, total = 0;
        for (int cid = 1; cid <= PSX_FUSION_CARD_ID_MAX; cid++) {
            const uint16_t off =
                psx_mod_read_half(PSX_FUSION_PAIR_BASE + (uint32_t)cid * 2u);
            if (off < PSX_FUSION_INDEX_BYTES) continue;
            cards++;
            const uint32_t p = PSX_FUSION_PAIR_BASE + off;
            const uint8_t b0 = psx_mod_read_byte(p);
            total += b0 ? b0 : (511 - psx_mod_read_byte(p + 1u));
        }
        if (cards_with_fusions) *cards_with_fusions = cards;
        if (pairs)              *pairs              = total;
    }

    if (equip_groups || equip_members) {
        int groups = 0, members = 0;
        uint32_t p = PSX_FUSION_EQUIP_BASE;
        for (int g = 0; g < PSX_FUSION_MAX_EQUIP_GROUPS; g++) {
            if (psx_mod_read_half(p) == 0) break;
            const uint16_t count = psx_mod_read_half(p + 2u);
            groups++;
            members += count;
            p += 4u + (uint32_t)count * 2u;
        }
        if (equip_groups)  *equip_groups  = groups;
        if (equip_members) *equip_members = members;
    }
}
