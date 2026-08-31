/* psx_card_save.c — make the extended card ids survive a power cycle, in a
 * save file the stock game will never be handed.
 *
 * THE MEMORY-CARD IMAGE (reverse-engineered from the running game; every
 * address below was read out of a live RAM dump and cross-checked against the
 * .mcd on disk)
 * ---------------------------------------------------------------------------
 * The game keeps one save STRUCT at 0x801D0200 (0x680 bytes: 40-slot deck at
 * +0x00, 722 trunk counts at +0x50, live fields straight after) and a mirror
 * of it at 0x801D3200 that doubles as the memory-card transfer buffer:
 *
 *   0x801D3000  0x200  card header (title/icon), memcpy'd from the template
 *                      at 0x801D4000 by func_8003D03C
 *   0x801D3200  0x680  save struct, copy 1        <- file offset 0x200
 *   0x801D3880  0x680  save struct, copy 2        <- file offset 0x880
 *                      (func_8003D03C's own memcpy at 0x8003D0D0; the game's
 *                       redundant backup, byte-identical to copy 1)
 *   0x801D3F00  0x100  UNUSED, reads zero in every state sampled
 *
 * SAVE  func_8003F87C: memcpy(mirror, live, 0x680); func_8003D03C rebuilds the
 *       header and copy 2; then write(buf 0x801D3200, len 0xD00, offset 0x200).
 * LOAD  func_8003F7D4: read(buf 0x801D3200, len 0x680, offset 0x200), i.e. only
 *       copy 1; func_8003F810 then memcpy(live, mirror, 0x680).
 *
 * 0xD00 == 2 * 0x680 exactly, which is what identifies the tail as the second
 * copy rather than slack. The file itself is one 8192-byte block (checked in
 * the card directory: state 0x51, size 8192), so bytes 0xF00..0x2000 of the
 * block are erased 0xFF and there is room to write more without changing the
 * block count func_8003EBD8 asks the card for.
 *
 * WHY THE TRUNK IS NOT GROWN IN PLACE
 * -----------------------------------
 * The obvious move -- 722 trunk bytes at struct+0x50 become 800 -- shifts
 * every field after +0x322 by 78 bytes: starchips, the met-duellist bitmap,
 * the New! ring, progress, all of it. Those fields are reached both absolutely
 * (0x801D0618, 0x801D07BC, 0x801D07DC, ...) and as register+offset off a
 * pointer to the struct base, scattered across the duel, chest, shop and
 * library code. Repointing all of them is a whole-binary dataflow job whose
 * failure mode is silent save corruption, and it would buy nothing the append
 * below does not: the chest's exit-path copy at 0x80033B7C can only write ONE
 * contiguous run, so even a grown trunk would still need the extended ids
 * staged separately (psx_card_chest.c's sync_ext_trunk already does exactly
 * that, every frame the chest is up).
 *
 * So the extended counts are APPENDED instead, in their own versioned block at
 * 0x801D3F00 -- inside the transfer buffer, past both struct copies, in the
 * 0x100 bytes that read zero everywhere. Two immediates carry it into the
 * file and back:
 *
 *   0x8003F8B0  write length  0xD00 -> 0xE00
 *   0x8003F7E4  read  length  0x680 -> 0xE00
 *
 * The read grows past copy 1 on purpose: the block is at the END of the image,
 * so getting it back means reading the whole thing. That refills the copy-2
 * buffer from the file as a side effect, which is the same content it already
 * held, and leaves the game's own memcpy(live, mirror, 0x680) untouched. Every
 * stock byte of the save keeps its stock offset, so this file is a strict
 * superset of a stock one.
 *
 * WHY IT STILL GETS ITS OWN FILE
 * ------------------------------
 * Because a deck slot can now hold an id above 722, and the stock game would
 * resolve that through 722-entry tables. The saves are not interchangeable
 * even though the layout is compatible, so the mod writes
 * BASLUS-01411-YUGIOX and leaves BASLUS-01411-YUGIOH alone. Both files live on
 * the same card -- nothing has to be moved or renamed on disk, and a player
 * who turns the row off finds their stock save exactly where they left it.
 *
 * The rename is one byte at 0x80010384 (the filename string all five memcard
 * call sites pass), not a longer name: the string is followed immediately by
 * "Make File" with a single NUL between them, so it cannot grow in place, and
 * a 20th character would also test whether the strcpy destination at
 * 0x800FFE18 has room for it. One character is enough to make the two files
 * distinct, and keeping the BASLUS-01411 prefix keeps card managers
 * associating the file with the game.
 *
 * WHY THE TOGGLE LATCHES AT BOOT
 * ------------------------------
 * Which file the game reads and writes is chosen once, at the first save or
 * load. Flipping the row mid-session would leave the running game married to
 * one file and the patches to the other. The row is a stored preference; the
 * session reads it in a start hook (psx_video_menu_apply_restored() runs
 * immediately before psx_game_run_start_hooks(), so the stored value is
 * already in by then) and never looks again. */

#include "psx_card_save.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_video_menu.h"

#include "psx_card_extend.h"

/* ---- the guest side ------------------------------------------------------ */

#define SAVE_MIRROR      0x801D3200u   /* memcard transfer buffer            */
#define FILE_LEN_STOCK   0xD00u        /* two 0x680 struct copies            */
#define FILE_LEN_EXT     0xE00u        /* ... plus our block                 */
#define EXT_BLOCK        (SAVE_MIRROR + FILE_LEN_STOCK)   /* 0x801D3F00      */
#define EXT_BLOCK_LEN    0x100u

/* Card-I/O request state (all $gp-relative in the guest; $gp = 0x8009AF08,
 * from the one `lui gp / addiu gp` pair at 0x80012A54). func_8003F740 sets
 * the busy word to 0x8000 when a request is posted and func_8003F70C treats
 * "not zero" as still running, so this is the read that says a transfer is in
 * flight -- and therefore the read that keeps the block from being rewritten
 * underneath a load that is still filling it. */
#define MC_BUSY          0x8009B3FAu   /* $gp + 0x4F2, u16 */
/* The transfer's op ($gp + 0x4D6: 0/1 read, 2 write, 4 header) and the state
 * machine's state ($gp + 0x4E3, low nibble). State 0x0E is "the file this
 * request names is not in the card's directory" -- func_8003EA24 reaches it
 * when the directory search at func_80044598 misses AND the request is not
 * allowed to create. */
#define MC_OP            0x8009B3DEu   /* $gp + 0x4D6, u8 */
#define MC_STATE         0x8009B3EBu   /* $gp + 0x4E3, u8 */
#define MC_OP_WRITE      2u
#define MC_STATE_NOFILE  0x0Eu

#define SITE_READ_LEN    0x8003F7E4u
#define STOCK_READ_LEN   0x24050680u   /* addiu $a1, $zero, 0x680 */
#define EXT_READ_LEN     (0x24050000u | FILE_LEN_EXT)

#define SITE_WRITE_LEN   0x8003F8B0u
#define STOCK_WRITE_LEN  0x24050D00u   /* addiu $a1, $zero, 0xD00 */
#define EXT_WRITE_LEN    (0x24050000u | FILE_LEN_EXT)

/* "BASLUS-01411-YUGIOH" -- the final H is index 18. */
#define NAME_TAIL        (0x80010384u + 18u)
#define NAME_TAIL_STOCK  'H'
#define NAME_TAIL_MOD    'X'
#define MOD_SAVE_NAME    "BASLUS-01411-YUGIOX"

/* ---- the block ----------------------------------------------------------- */
/*  +0x00 u32 magic   'YGXT'
 *  +0x04 u16 version   format of everything after this field
 *  +0x06 u16 first     first extended id in the block (== PSX_CARD_EXT_FIRST)
 *  +0x08 u16 last      last extended id in the block
 *  +0x0A u16 sum       16-bit sum of every OTHER byte the block uses
 *  +0x0C u8  count[last - first + 1]
 * A torn write leaves the sum wrong, which is the difference between "this
 * save has no extended cards" and "this save's extended cards are damaged" --
 * worth telling the player apart, since only one of them is their doing. */
#define BLOCK_MAGIC      0x54584759u   /* 'Y','G','X','T' little-endian */
#define BLOCK_VERSION    1u
#define BLOCK_HDR        0x0Cu
#define EXT_N            (PSX_CARD_EXT_LAST - PSX_CARD_EXT_FIRST + 1u)

typedef char save_block_fits[(BLOCK_HDR + EXT_N <= EXT_BLOCK_LEN) ? 1 : -1];
/* The block sits between the file's second struct copy and the card header
 * template; overrunning it would write into 0x801D4000. */
typedef char save_block_placed[(EXT_BLOCK + EXT_BLOCK_LEN == 0x801D4000u) ? 1 : -1];

enum { IMP_NONE = 0, IMP_OK, IMP_ABSENT, IMP_VERSION, IMP_DAMAGED, IMP_NARROW };

static int      s_setting;          /* the stored row value                  */
static int      s_enabled;          /* latched for this session              */
static int      s_row = -1;
static int      s_seeded;           /* s_shadow holds a block we wrote       */
static int      s_announced;
static int      s_nofile_said;
static int      s_last_import = IMP_NONE;
static uint8_t  s_shadow[EXT_BLOCK_LEN];

int psx_card_save_ext_enabled(void) { return s_enabled; }

/* ---- patch assertion ----------------------------------------------------- */

/* Same idiom as the rest of the extension: write only when the word still
 * reads stock, every frame. A savestate load restores stock bytes, and these
 * sites have to be back before the next save or the file is written short. */
static void assert_patches(void)
{
    if (psx_mod_read_word(SITE_READ_LEN) == STOCK_READ_LEN)
        psx_mod_write_code_word(SITE_READ_LEN, EXT_READ_LEN);
    if (psx_mod_read_word(SITE_WRITE_LEN) == STOCK_WRITE_LEN)
        psx_mod_write_code_word(SITE_WRITE_LEN, EXT_WRITE_LEN);
    if (psx_mod_read_byte(NAME_TAIL) == (uint8_t)NAME_TAIL_STOCK)
        psx_mod_write_byte(NAME_TAIL, (uint8_t)NAME_TAIL_MOD);
}

/* ---- block <-> mod-side counts ------------------------------------------- */

static uint16_t block_sum(const uint8_t *b, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < BLOCK_HDR; i++)
        if (i < 0x0Au) s += b[i];          /* the sum field itself is skipped */
    for (uint32_t i = 0; i < n; i++)
        s += b[BLOCK_HDR + i];
    return (uint16_t)s;
}

static void put16(uint8_t *b, uint32_t off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *b, uint32_t off)
{
    return (uint16_t)((uint16_t)b[off] | ((uint16_t)b[off + 1] << 8));
}

static uint32_t get32(const uint8_t *b, uint32_t off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8)
         | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static void build_block(uint8_t *out)
{
    memset(out, 0, EXT_BLOCK_LEN);
    out[0] = (uint8_t)BLOCK_MAGIC;
    out[1] = (uint8_t)(BLOCK_MAGIC >> 8);
    out[2] = (uint8_t)(BLOCK_MAGIC >> 16);
    out[3] = (uint8_t)(BLOCK_MAGIC >> 24);
    put16(out, 0x04u, (uint16_t)BLOCK_VERSION);
    put16(out, 0x06u, (uint16_t)PSX_CARD_EXT_FIRST);
    put16(out, 0x08u, (uint16_t)PSX_CARD_EXT_LAST);
    for (uint32_t i = 0; i < EXT_N; i++)
        out[BLOCK_HDR + i] = psx_card_ext_trunk_get(PSX_CARD_EXT_FIRST + i);
    put16(out, 0x0Au, block_sum(out, EXT_N));
}

/* Take the block the game just put in front of us. Anything that is not a
 * clean block of this format means "this save owns no extended cards", and
 * the counts are cleared -- carrying the previous save's clones into a file
 * that never had them is the one bug worth avoiding here. */
static int import_block(const uint8_t *b)
{
    int status = IMP_OK;
    uint32_t take = 0;

    if (get32(b, 0x00u) != BLOCK_MAGIC) {
        status = IMP_ABSENT;
    } else if (get16(b, 0x04u) != (uint16_t)BLOCK_VERSION) {
        status = IMP_VERSION;
    } else {
        const uint16_t first = get16(b, 0x06u);
        const uint16_t last  = get16(b, 0x08u);
        if (first != (uint16_t)PSX_CARD_EXT_FIRST || last < first
            || (uint32_t)(last - first) + 1u > EXT_BLOCK_LEN - BLOCK_HDR) {
            status = IMP_DAMAGED;
        } else {
            const uint32_t n = (uint32_t)(last - first) + 1u;
            if (get16(b, 0x0Au) != block_sum(b, n)) {
                status = IMP_DAMAGED;
            } else {
                take = n < EXT_N ? n : EXT_N;
                if (n > EXT_N) status = IMP_NARROW;
            }
        }
    }

    for (uint32_t i = 0; i < EXT_N; i++)
        psx_card_ext_trunk_set(PSX_CARD_EXT_FIRST + i,
                               i < take ? b[BLOCK_HDR + i] : 0u);
    return status;
}

static void announce_import(int status, const uint8_t *b)
{
    char msg[96];

    if (status == s_last_import) return;
    s_last_import = status;
    switch (status) {
    case IMP_VERSION:
        snprintf(msg, sizeof msg,
                 "Save's extra-card data is format %u, not %u - extras reset",
                 (unsigned)get16(b, 0x04u), (unsigned)BLOCK_VERSION);
        host_osd_push(msg, 6000);
        break;
    case IMP_DAMAGED:
        host_osd_push("Save's extra-card data is damaged - extras reset", 6000);
        break;
    case IMP_NARROW:
        snprintf(msg, sizeof msg,
                 "Save holds cards to %u; this build stops at %u",
                 (unsigned)get16(b, 0x08u), (unsigned)PSX_CARD_EXT_LAST);
        host_osd_push(msg, 6000);
        break;
    default:
        break;      /* OK and ABSENT are both normal; say nothing */
    }
}

/* ---- per-frame ----------------------------------------------------------- */

static void read_block(uint8_t *out)
{
    for (uint32_t i = 0; i < EXT_BLOCK_LEN; i += 4u) {
        const uint32_t w = psx_mod_read_word(EXT_BLOCK + i);
        out[i]     = (uint8_t)w;
        out[i + 1] = (uint8_t)(w >> 8);
        out[i + 2] = (uint8_t)(w >> 16);
        out[i + 3] = (uint8_t)(w >> 24);
    }
}

static void card_save_tick(void)
{
    uint8_t cur[EXT_BLOCK_LEN];
    uint8_t want[EXT_BLOCK_LEN];

    if (!s_enabled) return;
    if (!psx_mod_game_started()) {
        s_seeded = 0;
        s_announced = 0;
        s_nofile_said = 0;
        s_last_import = IMP_NONE;
        return;
    }

    assert_patches();

    if (!s_announced) {
        host_osd_push("Extra cards on: this game saves to " MOD_SAVE_NAME, 7000);
        s_announced = 1;
    }

    /* SAVE on a card that has no mod file shows the game's own "UNABLE TO
     * LOCATE LOAD DATA" box, which is true but says nothing about why. Only
     * the NEW GAME path is allowed to create a save file -- it is the one
     * place that sets the create-permission byte at $gp + 0x4CC (0x8002D4C0),
     * and everything else in the card module only ever clears it. That is
     * stock behaviour on any fresh card; it just arrives unexpectedly here,
     * because the player has a save, only under the other name. Say so.
     *
     * Deliberately NOT fixed by writing that byte ourselves: it also turns
     * off the card module's post-write verify (func_8003ECB0), and a shortcut
     * that quietly weakens checking on the save path is the wrong trade for
     * skipping one campaign start. */
    if (!s_nofile_said
        && psx_mod_read_byte(MC_OP) == (uint8_t)MC_OP_WRITE
        && (psx_mod_read_byte(MC_STATE) & 0x0Fu) == MC_STATE_NOFILE) {
        host_osd_push("No " MOD_SAVE_NAME " on this card yet - "
                      "start a new campaign to create it", 8000);
        s_nofile_said = 1;
    }

    /* Never touch the buffer while the card is mid-transfer: rewriting the
     * block from stale mod-side counts halfway through a load would both lose
     * the file's data and hand import a torn block. */
    if (psx_mod_read_half(MC_BUSY) != 0u) return;

    read_block(cur);

    /* The block only ever changes under us for one reason: the game read a
     * file (or a savestate put one back) on top of it. Nothing in the game
     * writes here otherwise -- it is past both struct copies. */
    if (!s_seeded || memcmp(cur, s_shadow, EXT_BLOCK_LEN) != 0)
        announce_import(import_block(cur), cur);

    build_block(want);
    if (memcmp(cur, want, EXT_BLOCK_LEN) != 0) {
        for (uint32_t i = 0; i < EXT_BLOCK_LEN; i += 4u) {
            const uint32_t w = (uint32_t)want[i]
                             | ((uint32_t)want[i + 1] << 8)
                             | ((uint32_t)want[i + 2] << 16)
                             | ((uint32_t)want[i + 3] << 24);
            const uint32_t have = (uint32_t)cur[i]
                                | ((uint32_t)cur[i + 1] << 8)
                                | ((uint32_t)cur[i + 2] << 16)
                                | ((uint32_t)cur[i + 3] << 24);
            if (have != w) psx_mod_write_word(EXT_BLOCK + i, w);
        }
    }

    memcpy(s_shadow, want, EXT_BLOCK_LEN);
    s_seeded = 1;
}

/* ---- the row ------------------------------------------------------------- */

/* The MODS row is withdrawn while the extension is shelved (2026-08-31, per
 * maintainer: the deck-edit prim-runaway crash needs its first domino found
 * before this ships to anyone). Set to 1 to bring the row back; everything
 * behind it -- the latch, the YUGIOX save file, the stretch -- is unchanged
 * and still keyed on the stored "card_extension" setting, which without the
 * row can never be turned on (s_setting stays 0, so the boot latch stays
 * off and every card_* module asserts nothing). */
#define PSX_CARD_EXT_MENU_ROW 0

#if PSX_CARD_EXT_MENU_ROW
static const char *const EXT_LABELS[] = { "Off", "On" };
static const char *const EXT_HINTS[]  = {
    "722 cards, stock save file",
    "More card ids, in its own save file",
};

static void ext_changed(int value)
{
    s_setting = value ? 1 : 0;
    if (psx_video_menu_is_restoring()) return;
    if (s_setting == s_enabled) return;
    host_osd_push(s_setting
        ? "Extra cards: restart to enable (it uses its own save file)"
        : "Extra cards: restart to disable (back to the stock save file)",
        5000);
}
#endif

static void card_save_start(void)
{
    /* psx_video_menu_apply_restored() has already replayed the stored value
     * into ext_changed by the time start hooks run. */
    s_enabled = s_setting;
}

void psx_card_save_init(void)
{
#if PSX_CARD_EXT_MENU_ROW
    s_row = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "Extra cards", EXT_HINTS[0],
        EXT_LABELS, 2, "card_extension", 0, ext_changed);
    if (s_row >= 0) psx_video_menu_set_row_hints(s_row, EXT_HINTS);
#else
    s_row = -1;
#endif
    (void)psx_game_add_start_hook(card_save_start);
    (void)psx_game_add_frame_hook(card_save_tick);
}
