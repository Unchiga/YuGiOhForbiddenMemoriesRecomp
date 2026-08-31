/* psx_card_extend.c — add card ids past the game's hard 722 limit.
 *
 * The new cards are clones of Kuriboh (id 58): same stats word, same name
 * string, same aux byte. They exist to prove the plumbing, not to be
 * interesting.
 *
 * WHY THIS IS NOT A CONSTANTS PATCH
 * ---------------------------------
 * A previous revision of CARD_LIBRARY_PROMPT.md said the per-card tables had
 * room for another ~670 cards. They do not. Measured against the running game,
 * SIX tables are each EXACTLY 722 entries and each butts straight into live
 * data:
 *
 *     name text     0x801D0000 + offset          (variable length)
 *     trunk count   0x801D024F + id              save struct +0x50
 *     stats         0x801D4244 + (id-1)*4
 *     alpha rank    0x801D4D8E + (id-1)*2        1-based A-Z rank
 *     aux (unknown) 0x801D5332 + (id-1)
 *     name offsets  0x801D5800 + id*2            SHARED string index
 *
 * So stats, rank, aux AND (above 767 cards) the name-offset table all have to
 * MOVE before the extra cards can exist.
 *
 * WHY THE TABLES MOVE HERE AND NOT INTO THE MOD APERTURE
 * -----------------------------------------------------
 * psx_mod_alloc_guest_memory() hands back 0x9F000000, which would mean
 * rewriting the `lui` half of every reference. That is a trap: 9 of the 61
 * `lui` instructions feeding stats sites are SHARED with a different target
 * (0x801CFFFF, 0x801D0000, 0x801D39FC, 0x801D4686). Repointing those corrupts
 * the other consumer, silently, several screens later.
 *
 * Staying inside `lui 0x801D` reach (0x801C8000..0x801D7FFF) means only the
 * 16-bit `addiu` immediate changes -- one word per site, and the shared `lui`
 * question never arises. 0x801CD5A0..0x801D0000 is 10,848 bytes that read as
 * zero in every state sampled so far; the layout below packs stats, rank, aux
 * and the relocated name-offset table into it and checks the fit at compile
 * time.
 *
 * THE NAME-OFFSET TABLE (deliberately NOT relocated -- see below)
 * ---------------------------------------------------------------
 * 0x801D5800 is a u16 string index for the whole game. String ids >= 0x8000
 * resolve here; a card's name is string id 0x8000+id. Indices 723..767 are
 * free filler slots, so clone names for ids 723..767 are written straight
 * into the stock table's spare entries. That is the whole name story for this
 * milestone (the maintainer's spec: "new ids are Kuriboh clones for now, real
 * names come later").
 *
 * Ids 768..800 are left with NO name entry of their own: their name string id
 * (0x8300..0x8320) is the SAME id space the game uses for monster-type and
 * Guardian-Star strings, so those ids already resolve to real text. A clone
 * at 768..800 therefore shows a type/Star word as a placeholder name -- ugly
 * but harmless, and every existing card's type/Star display is untouched.
 *
 * Relocating the table to give 768..800 proper names is the eventual fix, but
 * it is a bigger job than the four reader repoints it first appears to be, and
 * it is NOT needed for this milestone:
 *   - the readers derive the text SEGMENT from the table base's high half
 *     (`and reg, reg, 0xFFFF0000`), so a base at 0x801CExxx makes every
 *     string resolve a segment low and the text renderer polls forever (hard
 *     hang, measured on the menu->chest transition);
 *   - freeing 0x8300+ for names means moving BOTH the monster-type AND the
 *     Guardian-Star id producers, and only the type producer (`ori 0x8300` at
 *     0x80037F14) has been located -- the Star producer has not, so a partial
 *     move corrupts Guardian-Star text for every card.
 * Do that work behind its own flag, verified in isolation, before raising the
 * clone range past 767.
 *
 * WHY EVERY PATCH IS RE-ASSERTED PER FRAME
 * ----------------------------------------
 * Nine stats sites and twelve rank sites live in the library overlay at
 * 0x8018xxxx, which is NOT resident at boot and is reloaded from disc every
 * time the library is entered -- restoring stock code bytes and undoing any
 * one-shot patch. Savestate loads restore stock bytes for everything. So the
 * idiom from psx_card_shop.c applies everywhere: compare against the stock
 * word, write only when it is stock, every frame. */

#include "psx_card_extend.h"

#include <stdint.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_save.h"
#include "psx_card_new_cards.h"

/* ---- stock table bases ------------------------------------------------- */
#define STATS_STOCK   0x801D4244u
#define RANK_STOCK    0x801D4D8Eu
#define AUX_STOCK     0x801D5332u
#define NAMEOFF_STOCK 0x801D5800u
#define NAME_SEGMENT  0x801D0000u   /* name offsets are relative to here */

#define CLONE_SRC     58u          /* Kuriboh */
#define STOCK_COUNT   722u
#define EXT_COUNT     PSX_CARD_EXT_LAST

/* Kuriboh's stats word. Used as the "is the card DB actually loaded" probe:
 * the tables are streamed from disc, and copying them before that lands would
 * bake garbage into the relocated copies. */
#define KURIBOH_STATS 0x1DCC281Eu

/* ---- relocated layout, derived from EXT_COUNT --------------------------- */
/* Each base must satisfy  base == 0x801D0000 + (int16)imm  so only the addiu
 * immediate changes at a reference site. Packed back to back, 32-byte aligned
 * so a future count bump cannot introduce a silent overlap (the 740 build had
 * hand-placed bases that 800 entries would have overrun). */
#define ALIGN32(x)    (((x) + 31u) & ~31u)
#define STATS_NEW     0x801CD5A0u
#define RANK_NEW      (STATS_NEW + ALIGN32(EXT_COUNT * 4u))
#define AUX_NEW       (RANK_NEW + ALIGN32(EXT_COUNT * 2u))

/* Name-table indices usable for card names. 723..767 are the stock table's
 * free filler; 768..807 are freed by assert_type_gs_strings() below, which
 * moves the monster-type / Guardian-Star block off string ids 0x8300+.
 * 808..863 are menu text and duelist names -- that is the real wall. */
#define NAME_FREE_LAST  807u

/* A marker written past the tables so the rebuild happens once per load rather
 * than every frame. Value chosen to be nothing the game would leave there.
 * It also encodes the count, so a build with a different EXT_COUNT rebuilds
 * rather than trusting a stale layout from a savestate. */
#define MARK_ADDR     (AUX_NEW + ALIGN32(EXT_COUNT) + 0x20u)
#define MARK_VALUE    (0x58070000u | EXT_COUNT)

/* 0x801CFFC0.. is psx_card_chest.c's viewer-clamp stub; stay below it. */
typedef char ext_layout_fits[(MARK_ADDR + 4u <= 0x801CFFC0u) ? 1 : -1];
typedef char ext_count_ceiling[(EXT_COUNT <= 900u) ? 1 : -1];

/* ---- the new cards' NAME STRINGS ---------------------------------------- */
/* A name is one byte per character in the game's own frequency-ordered code
 * table, terminated by 0xFF, at NAME_SEGMENT + (the u16 in the offset table).
 * So a real name needs somewhere in reach of that u16 to put the bytes.
 *
 * The stock indexed blob ends at 0x801D8C66 (measured: the furthest offset in
 * the whole 823-entry table plus its string). 0x801D916F..0x801DA000 past it
 * reads zero in every state sampled -- library, chest, duel, a fresh game --
 * so the strings go there, well inside u16 reach of 0x801D0000. */
#define NAMES_BASE     0x801D9200u
#define NAMES_LIMIT    0x801DA000u
#define NAME_ENC_MAX   32u
/* Rewritten whenever this marker is missing: the name blob is streamed from
 * disc per screen, and a savestate restores whatever was there before. */
#define NAMES_MARK_VAL (0x4E414D45u)   /* 'NAME' */

typedef char ext_card_table_matches[
    (sizeof(PSX_NEW_CARDS) / sizeof(PSX_NEW_CARDS[0])
     == EXT_COUNT - STOCK_COUNT) ? 1 : -1];
/* Every new id needs a free slot in the stock name-offset table. */
typedef char ext_names_fit[(EXT_COUNT <= NAME_FREE_LAST) ? 1 : -1];

#define ADDIU_IMM(base) ((uint16_t)((uint32_t)(base) - 0x801D0000u))

/* ---- the measured reference sites --------------------------------------- */
/* Regenerated by scanning all of RAM for lui+addiu pairs resolving to each
 * base, in BOTH the library state and a duel state, then unioned -- an overlay
 * has to be resident for its sites to appear in a scan. All are addiu form;
 * none use the load/store-offset form. */

static const uint32_t STATS_SITES[] = {
    0x80017F60u, 0x800187D8u, 0x80018870u, 0x8001B9F4u, 0x8001C1F8u, 0x8001C3E4u,
    0x8001C9E0u, 0x8001CD18u, 0x8001DECCu, 0x8001E638u, 0x8001EE70u, 0x8001EE90u,
    0x8001EEC8u, 0x8001EEE8u, 0x8001FE80u, 0x8001FEA0u, 0x800231A4u, 0x80024AA0u,
    0x80024C84u, 0x80026C7Cu, 0x80026D28u, 0x800275D0u, 0x80027E48u, 0x80027F34u,
    0x80028020u, 0x80028140u, 0x80028298u, 0x800282C4u, 0x80028534u, 0x800292BCu,
    0x80029390u, 0x800293CCu, 0x8002ADD4u, 0x8002B124u, 0x8002B85Cu, 0x8002C260u,
    0x8002CC08u, 0x8002CC28u, 0x8002CC54u, 0x800315F8u, 0x800320D0u, 0x8003246Cu,
    0x80032D68u, 0x80032E24u, 0x80032EC4u, 0x80032F68u, 0x80037E64u, 0x80037E90u,
    0x80037ED8u, 0x80055F78u, 0x80060C98u, 0x80060F14u, 0x80071058u, 0x800711C8u,
    0x80071280u, 0x80183568u, 0x8018360Cu, 0x80183748u, 0x801837C8u, 0x801838D8u,
    0x80183958u, 0x80183A68u, 0x80183AB4u, 0x8018434Cu,
};

static const uint32_t RANK_SITES[] = {
    0x80032B8Cu, 0x80032C00u, 0x80183554u, 0x801835F8u, 0x80183734u, 0x801837B4u,
    0x801838C4u, 0x80183944u, 0x80183A54u, 0x80183AA0u, 0x801841ACu, 0x801841E0u,
    0x80184294u, 0x801842CCu,
};

static const uint32_t AUX_SITES[] = { 0x80029410u };

#define COUNT_OF(a) (uint32_t)(sizeof(a) / sizeof((a)[0]))

static int s_tables_built;
static int s_active;

/* Trunk counts for ids 723..EXT_COUNT. Mod-side; the stock save block has no
 * room for them (see header). psx_card_save.c persists them. */
static uint8_t s_ext_trunk[EXT_COUNT - STOCK_COUNT];

uint8_t psx_card_ext_trunk_get(uint32_t id)
{
    if (id < PSX_CARD_EXT_FIRST || id > PSX_CARD_EXT_LAST) return 0;
    return s_ext_trunk[id - PSX_CARD_EXT_FIRST];
}

void psx_card_ext_trunk_set(uint32_t id, uint8_t count)
{
    if (id < PSX_CARD_EXT_FIRST || id > PSX_CARD_EXT_LAST) return;
    s_ext_trunk[id - PSX_CARD_EXT_FIRST] = count;
}

/* ---- patch helpers ------------------------------------------------------ */

/* Rewrite only the low 16 bits, and only when the word still reads stock. The
 * stock test is what makes this safe to call every frame and what makes it
 * correct across an overlay reload: a reloaded overlay reads stock again and
 * gets re-patched; an already-patched site is left alone. */
static void assert_imm(const uint32_t *sites, uint32_t n,
                       uint16_t stock_imm, uint16_t new_imm)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w = psx_mod_read_word(sites[i]);
        if ((uint16_t)(w & 0xFFFFu) == stock_imm)
            psx_mod_write_code_word(sites[i], (w & 0xFFFF0000u) | new_imm);
    }
}

/* ---- table construction ------------------------------------------------- */

static int card_db_resident(void)
{
    return psx_mod_read_word(STATS_STOCK + (CLONE_SRC - 1u) * 4u) == KURIBOH_STATS;
}

/* The stats word, reverse-engineered by correlating all 722 stock words
 * against the game's published card data (0 mismatches on ATK/DEF over 722,
 * and 621/621 on the Guardian Star pairs):
 *
 *   bits  0..8   ATK / 10
 *   bits  9..17  DEF / 10
 *   bits 18..21  Guardian Star 1   1 Mars    2 Jupiter 3 Saturn  4 Uranus
 *   bits 22..25  Guardian Star 2   5 Pluto   6 Neptune 7 Mercury 8 Sun
 *   bits 26..30  type              9 Moon   10 Venus
 *                                  0..19 monster types in the stock order,
 *                                  20 Spell, 21 Trap, 22 Ritual, 23 Equip
 *
 * That is the WHOLE of a monster in this game -- FM monsters have no effects
 * -- which is why the added cards are monsters and why they duel correctly. */
static uint32_t new_card_stats(const PsxNewCard *c)
{
    return ((uint32_t)(c->atk / 10u) & 0x1FFu)
         | (((uint32_t)(c->def / 10u) & 0x1FFu) << 9)
         | (((uint32_t)c->gs1 & 0xFu) << 18)
         | (((uint32_t)c->gs2 & 0xFu) << 22)
         | (((uint32_t)c->type & 0x1Fu) << 26);
}

/* Where card `i` of PSX_NEW_CARDS puts its name bytes. Packed back to back
 * from NAMES_BASE; the offset written into the table is relative to
 * NAME_SEGMENT, which is what every reader adds. */
static uint32_t new_name_addr(uint32_t idx)
{
    uint32_t a = NAMES_BASE;
    for (uint32_t i = 0; i < idx; i++)
        a += PSX_NEW_CARDS[i].enc_len;
    return a;
}

static uint32_t new_names_end(void)
{
    return new_name_addr(COUNT_OF(PSX_NEW_CARDS));
}

/* The strings themselves. Separate from the offset table because the two go
 * stale for different reasons: the offsets live in the streamed name table,
 * the strings live in the space past it. */
static void write_new_name_strings(void)
{
    for (uint32_t i = 0; i < COUNT_OF(PSX_NEW_CARDS); i++) {
        const PsxNewCard *c = &PSX_NEW_CARDS[i];
        const uint32_t a = new_name_addr(i);
        for (uint32_t k = 0; k < c->enc_len; k++)
            psx_mod_write_byte(a + k, c->enc[k]);
    }
    psx_mod_write_word(new_names_end(), NAMES_MARK_VAL);
}

/* Copy the stock entries, then append the new cards. Reads come from the
 * STOCK bases, which stay intact -- nothing here mutates the game's own
 * tables, so backing the extension out is just a matter of not asserting the
 * patches. */
static void build_tables(void)
{
    const uint8_t k_aux = psx_mod_read_byte(AUX_STOCK + (CLONE_SRC - 1u));

    for (uint32_t i = 0; i < STOCK_COUNT; i++) {
        psx_mod_write_word(STATS_NEW + i * 4u,
                           psx_mod_read_word(STATS_STOCK + i * 4u));
        psx_mod_write_half(RANK_NEW + i * 2u,
                           psx_mod_read_half(RANK_STOCK + i * 2u));
        psx_mod_write_byte(AUX_NEW + i, psx_mod_read_byte(AUX_STOCK + i));
    }

    write_new_name_strings();

    for (uint32_t id = PSX_CARD_EXT_FIRST; id <= EXT_COUNT; id++) {
        const uint32_t i = id - 1u;
        const uint32_t n = id - PSX_CARD_EXT_FIRST;
        psx_mod_write_word(STATS_NEW + i * 4u, new_card_stats(&PSX_NEW_CARDS[n]));
        /* The aux byte's meaning is still unknown, so the new cards borrow a
         * stock one rather than inventing a value. */
        psx_mod_write_byte(AUX_NEW + i, k_aux);
        /* Rank is a dense permutation of 1..N. Appending keeps every existing
         * card's rank valid and sorts the new ones last; interleaving them
         * alphabetically would mean renumbering all 722. */
        psx_mod_write_half(RANK_NEW + i * 2u, (uint16_t)id);
        /* Indices 723..767 of the stock name-offset table are free filler, so
         * a new card's name entry goes straight in. The table is re-streamed
         * from disc per screen, so this write is re-done in the tick. */
        psx_mod_write_half(NAMEOFF_STOCK + id * 2u,
                           (uint16_t)(new_name_addr(n) - NAME_SEGMENT));
    }

    psx_mod_write_word(MARK_ADDR, MARK_VALUE);
    s_tables_built = 1;
}

/* ---- per-frame ----------------------------------------------------------- */


/* ---- monster-type / Guardian-Star strings: off 0x8300, onto 0x8400 ------ */
/* A card's name is string id 0x8000+id and the offset table is indexed by
 * (string id - 0x8000). Indices 723..767 are free filler, but 768 is NOT:
 * string id 0x8300 begins the monster-type and Guardian-Star words. That is
 * why an id at 768+ used to show "Fiend" or "Jupiter" as its name, and it is
 * the whole reason this extension stopped at 766.
 *
 * THERE IS EXACTLY ONE PRODUCER, not the two an earlier pass assumed. All
 * three consumers reach the same instruction pair:
 *     type            (stats >> 26) & 0x1F
 *     Guardian Star 2 (stats >> 22) & 0x0F, + 0x17
 *     Guardian Star 1 (stats >> 18) & 0x0F, + 0x17
 * each falls through to `ori v0,zero,0x8300 ; addu a1,a1,v0` at 0x80037F14 /
 * 0x80037F18. A whole-binary scan finds no other 0x8300 immediate. So the
 * block moves by rewriting ONE word and republishing its 34 table entries.
 *
 * WHERE IT MOVES -- AND THE TRAP THAT COST A ROUND TRIP. The table is
 * EXACTLY 1024 entries: 0x801D5800 + 1024*2 = 0x801D6000, and the string blob
 * starts at 0x801D6001 with card id 1's name. Reading 0x801D5800 as if it were
 * table all the way up therefore keeps returning plausible-looking "entries"
 * past the end -- they are string bytes. A first attempt put the block at
 * index 1024 (0x8400) on exactly that misreading and overwrote the names of
 * ids 1..5, which rendered as garbage in the chest while the type and Star
 * words still looked right. The lesson: verify the table's END, not just that
 * a range looks like filler.
 *
 * The real free run is indices 864..1023 -- real entries end at 863 and the
 * rest hold one filler offset (0x8ACE). This uses 896..929 (string ids
 * 0x8380..0x83A1), which also stays clear of 0x83BF / 0x83C0, the two high ids
 * func_80035E20 references. Verified: card viewer reads type "Fiend" and
 * Guardian Stars "Mercury" / "Saturn", and ids 1..8 read their real names.
 *
 * WHY THE OFFSETS ARE A CONSTANT rather than copied from 0x8300 at runtime:
 * once card names occupy 768.. the stock block IS the card names, so there is
 * nothing left to copy from. These are stock ROM values (dumped from the live
 * table), as fixed as the character-code table above.
 *
 * This frees indices 768..807 for names. 808..863 are menu text and duelist
 * names and stay put -- that is what sets NAME_FREE_LAST at 807. */
#define TYPEGS_STOCK_BASE  0x8300u
#define TYPEGS_NEW_BASE    0x8380u
#define TYPEGS_COUNT       34u        /* 24 card types + 10 Guardian Stars */
#define TYPEGS_ORI_SITE    0x80037F14u
#define TYPEGS_ORI_STOCK   (0x34020000u | TYPEGS_STOCK_BASE)
#define TYPEGS_ORI_MOVED   (0x34020000u | TYPEGS_NEW_BASE)

/* The table is 1024 entries; writing past index 1023 lands in the string blob
 * (card 1's name starts one byte later). Also keep clear of 0x83BF/0x83C0. */
#define TYPEGS_NEW_FIRST_IDX (TYPEGS_NEW_BASE - 0x8000u)
#define TYPEGS_NEW_LAST_IDX  (TYPEGS_NEW_FIRST_IDX + TYPEGS_COUNT - 1u)
typedef char typegs_inside_table[(TYPEGS_NEW_LAST_IDX <= 1023u) ? 1 : -1];
typedef char typegs_above_stock[(TYPEGS_NEW_FIRST_IDX >= 864u) ? 1 : -1];
typedef char typegs_clear_83bf[(TYPEGS_NEW_LAST_IDX < 959u
                                || TYPEGS_NEW_FIRST_IDX > 960u) ? 1 : -1];

static const uint16_t TYPEGS_OFFSETS[TYPEGS_COUNT] = {
    0x8ACEu, 0x8AD5u, 0x8AE1u, 0x8AE8u, 0x8AF0u, 0x8AFDu,
    0x8B03u, 0x8B0Fu, 0x8B15u, 0x8B1Bu, 0x8B22u, 0x8B2Bu,
    0x8B33u, 0x8B38u, 0x8B43u, 0x8B4Bu, 0x8B53u, 0x8B58u,
    0x8B5Du, 0x8B62u, 0x8B68u, 0x8B6Eu, 0x8B73u, 0x8B7Au,
    0x8B80u, 0x8B88u, 0x8B90u, 0x8B98u, 0x8BA0u, 0x8BA8u,
    0x8BB0u, 0x8BB8u, 0x8BC0u, 0x8BC8u,
};

/* The relocated entries live in the same per-screen re-streamed table as the
 * card names, so they are re-asserted the same way. The producer is only
 * pointed at them once all 34 are in place, and pointed BACK the moment they
 * are not: a frame drawn through a half-restored table would render the type
 * and Guardian-Star words as filler, which is worse than stock behaviour. */
static void assert_type_gs_strings(void)
{
    const uint32_t dst = NAMEOFF_STOCK + (TYPEGS_NEW_BASE - 0x8000u) * 2u;

    int ok = 1;
    for (uint32_t i = 0; i < TYPEGS_COUNT; i++) {
        if (psx_mod_read_half(dst + i * 2u) != TYPEGS_OFFSETS[i])
            psx_mod_write_half(dst + i * 2u, TYPEGS_OFFSETS[i]);
        if (psx_mod_read_half(dst + i * 2u) != TYPEGS_OFFSETS[i]) ok = 0;
    }

    const uint32_t w = psx_mod_read_word(TYPEGS_ORI_SITE);
    if (ok) {
        if (w == TYPEGS_ORI_STOCK)
            psx_mod_write_code_word(TYPEGS_ORI_SITE, TYPEGS_ORI_MOVED);
    } else if (w == TYPEGS_ORI_MOVED) {
        psx_mod_write_code_word(TYPEGS_ORI_SITE, TYPEGS_ORI_STOCK);
    }
}

static void card_extend_tick(void)
{
    /* The whole extension is one stored preference, latched at boot by
     * psx_card_save.c: the memory-card FILE the session writes is chosen with
     * it, so it cannot be flipped mid-run. Off means stock, patches and all --
     * psx_card_extend_count() then answers 722 and psx_card_chest.c's stretch
     * never asserts either. */
    if (!psx_card_save_ext_enabled()) {
        s_tables_built = 0;
        s_active = 0;
        return;
    }

    if (!psx_mod_game_started()) {
        s_tables_built = 0;
        s_active = 0;
        return;
    }

    if (!card_db_resident()) {
        /* Between screens the DB can be absent; do not assert patches that
         * would point live code at a table we have not built yet. */
        s_active = 0;
        return;
    }
    if (!s_tables_built || psx_mod_read_word(MARK_ADDR) != MARK_VALUE)
        build_tables();

    assert_imm(STATS_SITES, COUNT_OF(STATS_SITES),
               (uint16_t)(STATS_STOCK & 0xFFFFu), ADDIU_IMM(STATS_NEW));
    assert_imm(RANK_SITES, COUNT_OF(RANK_SITES),
               (uint16_t)(RANK_STOCK & 0xFFFFu), ADDIU_IMM(RANK_NEW));
    assert_imm(AUX_SITES, COUNT_OF(AUX_SITES),
               (uint16_t)(AUX_STOCK & 0xFFFFu), ADDIU_IMM(AUX_NEW));

    /* Re-assert the new cards' names. Two independent things go stale: the
     * offset table is re-streamed from disc per screen, and the strings sit
     * in space a savestate restores. The strings are checked through one
     * marker word rather than byte by byte -- they are only ever written
     * together, so one is as good as all of them. */
    if (psx_mod_read_word(new_names_end()) != NAMES_MARK_VAL)
        write_new_name_strings();
    for (uint32_t n = 0; n < COUNT_OF(PSX_NEW_CARDS); n++) {
        const uint32_t id = PSX_CARD_EXT_FIRST + n;
        const uint16_t off = (uint16_t)(new_name_addr(n) - NAME_SEGMENT);
        if (psx_mod_read_half(NAMEOFF_STOCK + id * 2u) != off)
            psx_mod_write_half(NAMEOFF_STOCK + id * 2u, off);
    }

    assert_type_gs_strings();

    s_active = 1;
}

uint32_t psx_card_extend_count(void)
{
    return s_active ? (uint32_t)EXT_COUNT : STOCK_COUNT;
}

void psx_card_extend_init(void)
{
    (void)psx_game_add_frame_hook(card_extend_tick);
}
