/* psx_card_chest.c — stretch the chest / deck-builder structure past 722.
 *
 * THE STRUCTURE (reverse-engineered live; every claim here was verified by
 * disassembly plus watchpoints on the running game)
 * ---------------------------------------------------------------------------
 * A static pointer at 0x80010000 (copies: gp+0x3F4 = 0x8009B2FC, 0x8009B498)
 * holds the screen arena, 0x80100000. No code references the arena's interior
 * absolutely; everything is register+immediate off that pointer. The chest
 * screen builds TWO instances (player 1, trade partner), stride 0x6344:
 *
 *   inst+0x0000  u32   save-struct pointer for this instance (NULL = unused)
 *   inst+0x0004  chest LIST: 722 records x 16 bytes  (id at rec+4, ATK/DEF
 *                derived from the stats word in-loop, flag byte at rec+0xD),
 *                terminator id at +0x2D28
 *   inst+0x2D34  chest list metadata: 3 panel-object pointers, then u16
 *                scroll-current/-target, u16 count x2, state bytes
 *   inst+0x2D50  deck LIST: 40 records x 16 bytes, terminator at +0x2FD4
 *   inst+0x5A84  deck list metadata (same shape as the chest list's)
 *   inst+0x5AC5  u8[722] per-card "seen" flags        (cells 0x5AC4+id)
 *   inst+0x5D98  u8[722] working trunk copy           (cells 0x5D97+id)
 *   inst+0x606B  u8[722] per-card deck-slot lookup    (cells 0x606A+id)
 *   inst+0x633E  tail state machine: u16 state, u16 arg, list-index byte
 *
 * THE LIST ABSTRACTION IS THE WHOLE TRICK. The chest and deck panes are the
 * SAME code driving a generic list object: base = inst + 4 + idx*0x2D4C, with
 * that list's metadata at base+0x2D34. idx (0 = chest, 1 = deck) comes from
 * the tail byte at +0x6342 or a panel object's +0x67, and the *0x2D4C is not
 * an immediate anywhere -- it is three separate 9-instruction shift/add
 * chains. 0x2D50 + 0x2D34 = 0x5A84 is why the deck metadata sits where it
 * does. A first attempt patched only the chest side and the deck pane
 * dereferenced garbage: hard freeze in an address-error exception storm.
 *
 * THE STRETCH. With N = PSX_CARD_EXT_LAST, D = N-722, S = 16*D:
 *   chest records        grow in place              (end 4+16N, term +16N+8)
 *   chest metadata       +S      (immediates 0x2D24..0x2D5F)
 *   deck list            +S      (same group: its base addius are 0x2D5x)
 *   list stride          0x2D4C -> 0x2D4C+S         (rewrite the 3 chains)
 *   deck metadata        +2S     (immediates 0x5A84..0x5AAF)
 *   seen flags           +2S     (immediate 0x5AC4)
 *   working trunk        +2S+D   (immediates 0x5D97/0x5D98)
 *   deck-slot lookup     +2S+2D  (immediates 0x6066/0x606A)
 *   tail                 +2S+3D  (immediates 0x633E..0x6343)
 *   instance stride      0x6344 -> align16(0x6344 + 2S+3D)
 * Every group keeps its stock-relative gaps, so a site's immediate moves by
 * its group shift no matter which register bias feeds it (bias never crosses
 * a group boundary; measured). The one computed instance-2 access -- the
 * builder's `ori 0x8000; addu; sb 0x4686(v0)` clearing the partner's tail
 * byte -- is patched as a special case.
 *
 * WHAT IS DELIBERATELY NOT PATCHED
 *   0x80033B7C  the exit-path copy of the working trunk into the SAVE trunk
 *               stays bounded at 722: the stock save block has exactly 722
 *               trunk bytes with live fields directly after them, and the
 *               loop can only write ONE contiguous run. Extended ids stage in
 *               the working trunk and are carried to and from
 *               psx_card_ext_trunk_get/set by sync_ext_trunk below;
 *               psx_card_save.c is what makes those counts persist.
 *   library / free-duel constants (0x8002A0B8, 0x8002C2F4, ...): different
 *               screen, different blocker, out of scope here.
 *
 * The space the relocated groups grow into ([0x2FD8..0x5A84) and past
 * +0x6344) is other screens' scratch: watchpoints during chest use show zero
 * traffic, and every screen rebuilds its arena content on entry. */

#include "psx_card_chest.h"

#include <stdint.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_extend.h"

#define N_CARDS   PSX_CARD_EXT_LAST
#define STOCKN    722u
#define D_GROW    (N_CARDS - STOCKN)
#define S_GROW    (16u * D_GROW)

#define SH_A        (S_GROW)
#define SH_DECKMETA (2u * S_GROW)
#define SH_SEEN     (2u * S_GROW)
#define SH_TRUNK    (2u * S_GROW + D_GROW)
#define SH_ARR3     (2u * S_GROW + 2u * D_GROW)
#define SH_TAIL     (2u * S_GROW + 3u * D_GROW)
#define NEW_STRIDE  ((0x6344u + SH_TAIL + 15u) & ~15u)
#define NEW_LIST_STRIDE (0x2D4Cu + S_GROW)

/* u16 tail fields need an even shift; word fields get multiples of 16/32. */
typedef char chest_d_even[((D_GROW & 1u) == 0u) ? 1 : -1];
/* Two stretched instances must stay inside the scratch arena's measured
 * extent (other screens scribble to ~+0x10000; we stay well below). */
typedef char chest_arena_fit[(2u * NEW_STRIDE <= 0xE000u) ? 1 : -1];

typedef struct {
    uint32_t addr;
    uint16_t stock_imm;
    uint8_t  section;
} PsxChestImmSite;

enum {
    SEC_A, SEC_DECKMETA, SEC_SEEN, SEC_TRUNK, SEC_ARR3, SEC_TAIL, SEC_STRIDE
};

#include "psx_card_chest_sites.h"

static const uint16_t SECTION_SHIFT[] = {
    [SEC_A]        = (uint16_t)SH_A,
    [SEC_DECKMETA] = (uint16_t)SH_DECKMETA,
    [SEC_SEEN]     = (uint16_t)SH_SEEN,
    [SEC_TRUNK]    = (uint16_t)SH_TRUNK,
    [SEC_ARR3]     = (uint16_t)SH_ARR3,
    [SEC_TAIL]     = (uint16_t)SH_TAIL,
    [SEC_STRIDE]   = (uint16_t)(NEW_STRIDE - 0x6344u),
};

/* Count literals inside the chest module (each verified by disassembly; the
 * "all 722-immediates are counts" assumption corrupted the chest once and is
 * exactly what this table is NOT). */
typedef struct { uint32_t addr, stock, patched; } PsxChestWordSite;
static const PsxChestWordSite CHEST_COUNT_SITES[] = {
    { 0x80032604u, 0x240202D2u, 0x24020000u | N_CARDS },        /* count store */
    { 0x800326E8u, 0x290202D2u, 0x29020000u | N_CARDS },        /* record loop */
    { 0x800326F8u, 0x240202D2u, 0x24020000u | N_CARDS },        /* count store */
    { 0x800324E8u, 0x290202D3u, 0x29020000u | (N_CARDS + 1u) }, /* slot-lookup loop */
    { 0x8003207Cu, 0x28C202D3u, 0x28C20000u | (N_CARDS + 1u) }, /* seen-merge loop */
    /* Builder's computed clear of instance 2's tail byte:
     * ori v0,zero,0x8000 ; addu v0,inst,v0 ; sb zero, 0x4686(v0). */
    { 0x80032490u, 0xA0404686u,
      0xA0400000u | (NEW_STRIDE + 0x6342u + SH_TAIL - 0x8000u) },
};

/* The three shift/add chains computing list_base = inst + 4 + idx*0x2D4C.
 * Nine instruction slots each; rewritten to  li dst, stride ; mult ; mflo. */
typedef struct {
    uint32_t addr;      /* first instruction of the chain */
    uint32_t stock_w0;  /* its stock first word, the idempotence probe */
    uint8_t  src, dst;  /* register numbers */
} PsxChestChain;
static const PsxChestChain CHEST_CHAINS[] = {
    { 0x80031924u, 0x001610C0u, 22u, 2u },   /* row-draw ptr, idx from panel+0x67 */
    { 0x8003355Cu, 0x000218C0u,  2u, 3u },   /* tail state 3 handler */
    { 0x8003370Cu, 0x000218C0u,  2u, 3u },   /* tail state 2 handler */
};

#define MODE_BYTE   0x8009B26Cu
#define MODE_CHEST  0xC7u
#define ARENA_PTR   0x80010000u

/* The triangle card viewer's mailbox (psx_card_shop.c documents the pump
 * protocol). Its art loader resolves id -> WA_MRG offset through a table
 * that ends at 722; asking it for a clone id makes the viewer wedge mid-
 * spawn and the chest stays suspended behind the pump forever (measured:
 * required a savestate reload). Clones are Kuriboh clones, so the request
 * id is clamped to Kuriboh and the viewer shows the real card: right name,
 * right stats, right art, right description.
 *
 * The mailbox is written and pumped within the same guest frame, so a
 * between-frames clamp loses the race (measured). Instead the two chest-side
 * `sh s0, B246` writers are redirected through a stub that stores min(id,
 * Kuriboh) -- s0 is dead after the store at both sites, and the displaced
 * delay-slot `lui $at` is re-done by the stub, so nothing else changes. */
/* Top of the reclaimed 0x801CD5A0..0x801D0000 region; psx_card_extend.c's
 * layout assert keeps its tables below this. */
#define VIEWER_STUB_ADDR  0x801CFFC0u
#define VIEWER_STUB_WORDS 7u
static const uint32_t VIEWER_STUB[VIEWER_STUB_WORDS] = {
    0x2E0102D3u,    /* sltiu at, s0, 723                    */
    0x14200002u,    /* bnez  at, +2 (to the lui)            */
    0x0200D025u,    /* or    k0, s0, zero      (delay slot) */
    0x241A003Au,    /* addiu k0, zero, 58      (the clamp)  */
    0x3C01800Au,    /* lui   at, 0x800a                     */
    0x03E00008u,    /* jr    ra                             */
    0xA43AB246u,    /* sh    k0, -0x4dba(at)   (delay slot) */
};

#define COUNT_OF(a) (uint32_t)(sizeof(a) / sizeof((a)[0]))

static int      s_patched;         /* all patches verified asserted        */
static uint8_t  s_shadow[D_GROW];  /* last ext-trunk values we synced       */

static uint32_t arena_base(void)
{
    return psx_mod_read_word(ARENA_PTR);
}

uint32_t psx_card_chest_ui_trunk_cell(uint32_t id)
{
    if (!s_patched || id < 1u || id > N_CARDS) return 0;
    return arena_base() + 0x5D97u + SH_TRUNK + id;
}

/* ---- patch assertion ----------------------------------------------------- */

static int assert_patches(void)
{
    int all = 1;
    for (uint32_t i = 0; i < COUNT_OF(CHEST_IMM_SITES); i++) {
        const PsxChestImmSite *s = &CHEST_IMM_SITES[i];
        const uint32_t w = psx_mod_read_word(s->addr);
        const uint16_t want = (uint16_t)(s->stock_imm + SECTION_SHIFT[s->section]);
        if ((uint16_t)(w & 0xFFFFu) == s->stock_imm)
            psx_mod_write_code_word(s->addr, (w & 0xFFFF0000u) | want);
        else if ((uint16_t)(w & 0xFFFFu) != want)
            all = 0;   /* neither stock nor ours: leave it alone, stay off */
    }
    for (uint32_t i = 0; i < COUNT_OF(CHEST_COUNT_SITES); i++) {
        const PsxChestWordSite *s = &CHEST_COUNT_SITES[i];
        const uint32_t w = psx_mod_read_word(s->addr);
        if (w == s->stock)
            psx_mod_write_code_word(s->addr, s->patched);
        else if (w != s->patched)
            all = 0;
    }
    for (uint32_t i = 0; i < COUNT_OF(CHEST_CHAINS); i++) {
        const PsxChestChain *c = &CHEST_CHAINS[i];
        const uint32_t w0 = psx_mod_read_word(c->addr);
        if (w0 == c->stock_w0) {
            /* li dst, stride ; mult src, dst ; (gap) ; mflo dst ; nops */
            psx_mod_write_code_word(c->addr + 0u,
                0x24000000u | ((uint32_t)c->dst << 16) | NEW_LIST_STRIDE);
            psx_mod_write_code_word(c->addr + 4u,
                0x00000018u | ((uint32_t)c->src << 21) | ((uint32_t)c->dst << 16));
            psx_mod_write_code_word(c->addr + 8u, 0u);
            psx_mod_write_code_word(c->addr + 12u,
                0x00000012u | ((uint32_t)c->dst << 11));
            for (uint32_t k = 16u; k < 36u; k += 4u)
                psx_mod_write_code_word(c->addr + k, 0u);
        } else if (w0 != (0x24000000u | ((uint32_t)c->dst << 16) | NEW_LIST_STRIDE)) {
            all = 0;
        }
    }
    return all;
}

/* ---- extended-trunk staging ---------------------------------------------- */

/* The chest builder's record loop (0x80032640..0x800326EC) seeds one working-
 * trunk cell per card from save+0x50+i, sets that record's owned flag from the
 * same byte and adds it to the owned total. Past i=721 the byte it reads is
 * whatever live save fields follow the real 722-byte trunk, so every extended
 * id is seeded with garbage. The mod-side array is the real store; this pushes
 * it over the garbage and pulls the player's staging edits back.
 *
 * WHICH WAY TO SYNC IS THE WHOLE PROBLEM. The builder does not run once -- it
 * re-runs, and a "push on entry, pull afterwards" rule loses to that: the
 * rebuild re-seeds the cells from the save and the next pull writes the
 * overread bytes into the mod store, permanently. Measured 2026-08-29 on
 * BUILD DECK: ids 723..740 came back 0 (the free bytes after the trunk) and
 * 741..743 came back 226/11/39 (live save fields), matching save+0x372
 * onwards byte for byte, and the rows showed exactly that.
 *
 * A function-entry hook on the builder (0x800323F8, already listed in
 * game.toml) would say "it just ran", but psx_card_chest.c PATCHES the
 * builder's text, which diverges it to the dirty-RAM interpreter -- and that
 * path skips entry hooks (psx_card_drops.c records the same trap).
 *
 * So the rebuild is detected from its own footprint instead: a rebuild leaves
 * the extended cells byte-for-byte equal to the save bytes it copied them
 * from. A player edit cannot fake that -- it moves ONE cell, and the other 77
 * still hold pushed values. The one state where the test is ambiguous is when
 * the mod-side counts already equal those save bytes, and there pushing is a
 * no-op, so the ambiguity costs nothing. */
static void sync_ext_trunk(void)
{
    /* A healthy, player-driven chest runs with substate 0x02, and the same
     * value shows during the menu->chest transition -- substate does NOT
     * distinguish them. The count field does: during the transition the menu
     * handler pre-builds and then scribbles the arena, so count reads as
     * garbage there and the sync below stays out of the way. */
    const uint8_t mode = psx_mod_read_byte(MODE_BYTE);
    if (mode != MODE_CHEST) return;

    const uint32_t inst = arena_base();
    const uint16_t count = psx_mod_read_half(inst + 0x2D44u + SH_A);
    if (count != N_CARDS) return;    /* builder has not (re)run yet */

    const uint32_t save = psx_mod_read_word(inst);
    int cheat_changed = 0;
    int rebuilt = 1;
    for (uint32_t i = 0; i < D_GROW; i++) {
        const uint32_t id = PSX_CARD_EXT_FIRST + i;
        if (psx_card_ext_trunk_get(id) != s_shadow[i]) cheat_changed = 1;
        if (psx_mod_read_byte(inst + 0x5D97u + SH_TRUNK + id)
            != psx_mod_read_byte(save + 0x50u + id - 1u))
            rebuilt = 0;
    }

    if (rebuilt || cheat_changed) {
        /* The header's owned-total was summed by the builder, extended ids
         * included -- i.e. it counted the save bytes PAST the real trunk.
         * Replace that contribution with the mod-side counts: after a rebuild
         * subtract the overread bytes the builder actually added, on a cheat
         * push subtract what the cells held (== the shadow). */
        int32_t delta = 0;
        /* The sort permutes the records in place, so a record's index is NOT
         * id-1 once the player has re-ordered the list: find each extended
         * id's record by its id field. */
        uint32_t rec_of[D_GROW];
        for (uint32_t i = 0; i < D_GROW; i++) rec_of[i] = 0;
        for (uint32_t r = 0; r < N_CARDS; r++) {
            const uint32_t rec = inst + 4u + 16u * r;
            const uint16_t rid = psx_mod_read_half(rec + 4u);
            if (rid >= PSX_CARD_EXT_FIRST && rid <= N_CARDS)
                rec_of[rid - PSX_CARD_EXT_FIRST] = rec;
        }
        for (uint32_t id = PSX_CARD_EXT_FIRST; id <= N_CARDS; id++) {
            const uint8_t n = psx_card_ext_trunk_get(id);
            const uint8_t old = rebuilt
                ? psx_mod_read_byte(save + 0x50u + id - 1u)
                : s_shadow[id - PSX_CARD_EXT_FIRST];
            delta += (int32_t)n - (int32_t)old;
            psx_mod_write_byte(inst + 0x5D97u + SH_TRUNK + id, n);
            /* record flag: 1 = owned row, 0 = never seen (blank row) */
            if (rec_of[id - PSX_CARD_EXT_FIRST])
                psx_mod_write_byte(rec_of[id - PSX_CARD_EXT_FIRST] + 0xDu,
                                   n ? 1u : 0u);
            s_shadow[id - PSX_CARD_EXT_FIRST] = n;
        }
        const uint32_t total_addr = inst + 0x5A9Cu + SH_DECKMETA;
        psx_mod_write_word(total_addr,
                           (uint32_t)((int32_t)psx_mod_read_word(total_addr)
                                      + delta));
        return;
    }

    for (uint32_t id = PSX_CARD_EXT_FIRST; id <= N_CARDS; id++) {
        const uint8_t n = psx_mod_read_byte(inst + 0x5D97u + SH_TRUNK + id);
        psx_card_ext_trunk_set(id, n);
        s_shadow[id - PSX_CARD_EXT_FIRST] = n;
    }
}


/* ---- deck row ordinal: keep the STALE COMPILED read in step -------------- */

/* The deck pane numbers its eight visible rows `scroll + row + 1` (1..40).
 * The scroll comes from one patched site -- `lh a0, 0x2D3C(list)` at
 * 0x80031A34, an A-group immediate this stretch moves to 0x2D3C+SH_A.
 *
 * ONLY THE FIRST LOOP ITERATION HONOURS THAT PATCH. psx_mod_write_code_word
 * marks the word's page executable-dirty, so entering func_80031874 routes
 * through the recovering dirty-RAM interpreter -- but the interpreter hands
 * back to the statically compiled block, and the row loop's back edge
 * (0x80031C9C -> 0x800319E0) stays native from then on. Rows 1..7 therefore
 * execute the ORIGINAL instruction and read list_base+0x2D3C.
 *
 * Measured: rows drew 01 then garbage, and the garbage decodes exactly. The
 * stale cell held -2953, and int_to_digits(-2953+row+1, 2) produces the byte
 * pairs the write trace captured, row for row (D9/FF, D9/00, DA/F7 ...).
 * Forcing the value to a constant 42 rendered on row 0 ONLY, which is what
 * pins this on the stale text rather than on the value or the digit code.
 *
 * Patching harder is the wrong lever -- the site is already correct, the
 * recompiler simply does not re-enter it. Instead make both forms read the
 * same number: for the DECK list, list_base+0x2D3C lands at inst+0x5D4C,
 * inside the dead record tail the 40-slot deck never uses (the space a
 * 766-record chest list would occupy). Mirror the real scroll there every
 * frame. Watchpointed across a full scroll: the game never writes that cell.
 *
 * The chest pane needs no mirror -- 0x80031A20's `beqz s6` skips the whole
 * ordinal block for it, so the site only ever executes for the deck. */
static void mirror_deck_scroll(void)
{
    if (psx_mod_read_byte(MODE_BYTE) != MODE_CHEST) return;

    const uint32_t inst = arena_base();
    /* Same "builder has actually run" gate sync_ext_trunk uses: during the
     * MENU->chest transition the count reads as garbage and the arena is not
     * ours to write. */
    if (psx_mod_read_half(inst + 0x2D44u + SH_A) != N_CARDS) return;

    const uint32_t deck = inst + 4u + NEW_LIST_STRIDE;
    psx_mod_write_half(deck + 0x2D3Cu,
                       psx_mod_read_half(deck + 0x2D3Cu + SH_A));
}

/* ---- per-frame ------------------------------------------------------------ */

static void card_chest_tick(void)
{
    if (!psx_mod_game_started()) {
        s_patched = 0;
        return;
    }
    /* The card DB extension must be live first: the stretched list is built
     * from the relocated stats table, and both derive from the same count. */
    if (psx_card_extend_count() != N_CARDS) {
        s_patched = 0;
        return;
    }

    s_patched = assert_patches();
    if (!s_patched) return;

    /* Viewer-id clamp stub + the two redirected writers (see above). */
    for (uint32_t i = 0; i < VIEWER_STUB_WORDS; i++)
        if (psx_mod_read_word(VIEWER_STUB_ADDR + i * 4u) != VIEWER_STUB[i])
            psx_mod_write_code_word(VIEWER_STUB_ADDR + i * 4u, VIEWER_STUB[i]);
    {
        const uint32_t jal = 0x0C000000u | ((VIEWER_STUB_ADDR & 0x0FFFFFFFu) >> 2);
        if (psx_mod_read_word(0x800335D8u) == 0xA430B246u)
            psx_mod_write_code_word(0x800335D8u, jal);
        if (psx_mod_read_word(0x80033788u) == 0xA430B246u)
            psx_mod_write_code_word(0x80033788u, jal);
    }

    /* NO self-heal, deliberately. An earlier revision cleared the mode's init
     * bit whenever the chest was up with a non-matching count, to recover
     * from stock-built arenas after a savestate load. It fired during the
     * MENU -> chest transition instead: the menu handler pre-builds the chest
     * synchronously, then scribbles the arena during its own teardown, so
     * "mode says chest, count says garbage" is a NORMAL transient there --
     * and clearing the init bit mid-transition deadlocks the menu handler's
     * modal loop (which does not pump the pad). A stale-arena chest after a
     * cross-version savestate load just looks wrong until re-entered; a
     * deadlock needs the process killed. Wrong display wins. */

    sync_ext_trunk();
    mirror_deck_scroll();
}

void psx_card_chest_init(void)
{
    (void)psx_game_add_frame_hook(card_chest_tick);
}
