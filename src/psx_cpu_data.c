/* psx_cpu_data.c — see psx_cpu_data.h.
 *
 * WHERE A DUELIST LIVES
 * ---------------------
 * DECK. Every opponent has a 6144-byte record on the disc, three sectors at
 * WA_MRG's sector 0x1D33 + 3*id, read into 0x801781D8 before a duel
 * [func_800179F4]. The
 * record is four 722-entry u16 weight arrays 1460 bytes apart: the DECK pool
 * first, then the three drop pools the Drop Table Manager already shows, then
 * a 200-byte rank table and 104 bytes nothing reads. Each array sums to 2048.
 *
 * The stock pools are baked from the player's disc at build time
 * (tools/gen_drop_db.py), so this module can show all thirty-nine without a
 * duel; an EDIT is installed as a sector override on that record, which means
 * the game loads the edited pool through its own loader and nothing has to be
 * written into RAM at the right moment.
 *
 * The LBA is checked, not trusted: install_deck() reads the stock sectors
 * back and refuses unless the three DROP pools inside them match the baked
 * database byte for byte. Two independent derivations of the same bytes --
 * the build-time stream offset and this LBA -- agreeing is what makes the
 * write safe.
 *
 * AI. gDuel_aOpponentData (0x800917F0) is 40 nine-byte profiles indexed by
 * opponent id, in the EXE's data, resident from boot and never reloaded.
 * Ai_GetHandSize returns b[0]; the AI SCRIPTS reach the rest through
 * AiScript_LoadOpponentData, which reads b[field + 1] -- and multiplies b[1]
 * by 100 for field 0. The stock table rises with the campaign (Simon
 * 5,20,10,1,1,0,0,25,50 against Nitemare 20,10,5,2,2,5,5,75,0), so the bytes
 * are the opponent's difficulty knobs.
 *
 * WHAT EACH BYTE DOES, and how that was found. The AI is a bytecode VM: the
 * 67-entry handler table at gAiScript_apfnCommand (0x800916E0) names every
 * opcode, and the duel's script is the 0x1800 bytes at 0x801A8000. Reading
 * the operand shape of each handler out of the decompilation (and, for the
 * seven it has not reached yet, out of this build's own recompiled C, by
 * counting their calls to AiScript_ReadByte) is enough to disassemble it.
 * The field number is INDIRECT -- LoadOpponentData takes mem[] slots, not
 * literals -- so each read was traced back to the Store that set the slot:
 *
 *   b[1]   the script's field 0, x100, compared against a life point total
 *          (10/20/30 = 1000/2000/3000 LP) ahead of a weakest-monster search
 *   b[2]   compared against the AI's remaining deck size immediately before
 *          the fusion / combo search
 *   b[3]   read into the slot the best-combo search is handed
 *   b[4]   minus one, handed to EvaluateFusion: how deep it looks
 *   b[8]   the first thing the duel script reads, before anything happens
 *
 * b[5] and b[6] are equal in every stock profile and rise 0 -> 5 across the
 * campaign, but nothing in the resident script reads them, so they are left
 * named for what they look like rather than dressed up.
 *
 * The script also opens with a chain that singles out opponent ids 15, 35,
 * 36, 37 and 38 -- Pegasus, Heishin 2nd, Seto 3rd, DarkNite, Nitemare -- and
 * sets one flag for them and another for everyone else. That is the game's
 * own list of special opponents, and the community's "Pegasus reads your
 * face-downs" is exactly those five. Nothing in the script reads the flag
 * back, so what it turns on lives in native code and is not exposed here.
 *
 * RECORD. gFreeDuel_aDuelistRecords (0x801D071C) in the save: 40 x {u16 win,
 * u16 loss}, duelist ids from +4. The campaign does not touch it (only
 * FreeDuel_Init does), so it is exactly the Free Duel screen's WIN n LOSS n.
 *
 * NAME. The Free Duel grid prints string 0x8328 + cell for the cell under
 * the cursor (FreeDuel_PlaceCursor; teatools/image-freeduel), and cell = id
 * (0 is the Build Deck tile). Those are entries 808..847 of the u16 offset
 * table at gText_aGlobalOffsets (0x801D5800), the table the card names
 * share, each an offset from 0x801D0000 to an 0xFF-ended string in the
 * game's frequency-ordered glyph code. Checked 2026-09-06 by decoding the
 * SLUS: the forty entries read Build Deck, Simon Muran ... Duel Master K,
 * the order tools/gen_drop_db.py already uses. A rename is done the way a
 * card rename is (psx_card_packs.c): the encoded string is written to
 * reclaimed RAM and the entry repointed at it, both re-asserted per frame,
 * because the table is EXE data a savestate puts back. The strings sit in
 * fixed slots at the top of the name blob's free tail, above the card
 * renames (psx_card_packs.h says where the tail ends and why).
 */

#include "psx_cpu_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "mod_plugins.h"
#include "psx_card_packs.h"
#include "psx_drop_db.h"
#include "psx_duelist_portraits.h"
#include "psx_drop_missing.h"
#include "psx_game_hooks.h"
#include "psx_textfile.h"
#include "psx_ygo_cheats.h"      /* psx_ygo_save_is_live() */

#define NDUEL      PSX_DROP_DB_DUELISTS      /* 39 */
#define NCARDS     PSX_DROP_DB_CARDS         /* 722 */
#define TOTAL      PSX_DROP_DB_TOTAL         /* 2048 */
#define INI_NAME   "cpu_manager.ini"

/* The disc record, id 1..39. 0x1D33 is the first duelist block as counted
 * INSIDE WA_MRG.MRG, and WA_MRG itself starts at sector 10102 of the user
 * data stream (the number tools/live_probe.py has called WA all along), so
 * the absolute sector is the sum. Derived independently of
 * tools/gen_drop_db.py's byte offset into the same stream, and checked
 * against it in install_deck() before anything is written. */
#define WA_MRG_LBA    10102u
#define DUELIST_LBA0  0x1D33u
#define REC_LBA(id)   (WA_MRG_LBA + DUELIST_LBA0 + 3u * (uint32_t)(id))
#define REC_SECTORS   3u
#define SECTOR        2048u
#define ARRAY_BYTES   (NCARDS * 2u)          /* 1444 of the 1460-byte stride */
#define ARRAY_STRIDE  1460u
#define DECK_OFF      0u
#define DROP_OFF(t)   (ARRAY_STRIDE * (uint32_t)((t) + 1))

/* the AI table */
#define AI_TABLE      0x800917F0u
#define AI_STRIDE     PSX_CPU_AI_BYTES

/* the save's records */
#define RECORDS       0x801D071Cu

/* the names: entry 808 + id of the offset table, offsets from NAME_SEGMENT */
#define NAMEOFF_TABLE 0x801D5800u
#define NAME_SEGMENT  0x801D0000u
#define NAME_ENTRY0   808u
/* Where the renamed strings go: one fixed slot per duelist, PSX_CPU_NAME_MAX
 * glyphs and the 0xFF, from PSX_CPU_NAMES_BASE up to 0x801DA000. The first
 * pick was the page at 0x801DA100: zero in the SLUS image, zero at the title,
 * and then eight bytes appeared at 0x801DA000 the moment a save was loaded
 * (2026-09-06) -- it is D_801DA000, a table of 0x88-byte records that
 * func_80036C14 writes, so the tail of the name blob it is. */
#define NAME_ARENA    PSX_CPU_NAMES_BASE
#define NAME_SLOT     (PSX_CPU_NAME_MAX + 1u)
#define NAME_SLOT_ADDR(d) (NAME_ARENA + (uint32_t)(d) * NAME_SLOT)
typedef char cpu_name_arena_fits[(NAME_ARENA + 39u * NAME_SLOT <= 0x801DA000u) ? 1 : -1];
typedef char cpu_name_arena_above_packs[(NAME_ARENA >= PSX_CARD_PACKS_NAMES_LIMIT) ? 1 : -1];

/* the portrait tiles: WA_MRG sector 0x1EAA, forty 2432-byte tiles */
#define TILE_LBA      17952u
#define TILE_BYTES    2432u
#define TILE_W        48
#define TILE_PIXELS   (TILE_W * TILE_W)          /* 2304 indices */
#define TILE_CLUT     64
#define TILES         40u
#define TILE_SECTORS  ((TILES * TILE_BYTES + SECTOR - 1u) / SECTOR)   /* 48 */

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    uint16_t deck[NCARDS];       /* the edited pool, only when deck_set */
    uint8_t  deck_set;
    uint8_t  portrait_set;       /* duelists/<id>/portrait.png exists */
    uint8_t  ai[PSX_CPU_AI_BYTES];
    uint8_t  ai_set;
    uint8_t  installed;          /* the override is in place for this duelist */
    char     name[PSX_CPU_NAME_MAX + 1];
    uint8_t  name_set;
    uint8_t  enc[PSX_CPU_NAME_MAX + 1];   /* the name in the game's code, 0xFF-ended */
    uint8_t  enc_len;
} CpuEdit;

static CpuEdit  g_edit[NDUEL];
static uint8_t  g_ai_stock[NDUEL][PSX_CPU_AI_BYTES];
static int      g_ai_stock_ready;
static uint16_t g_name_stock[NDUEL];     /* the stock offset-table entries */
static int      g_name_stock_ready;
static int      g_loaded;
static int      g_dirty;
static unsigned g_gen = 1;
static char     g_ini_path[1024];
static char     g_status[96] = "not loaded";
static int      g_installs;      /* sector overrides written, for the read-back */
static int      g_refused;       /* records whose sectors did not match the DB */

/* ---- the AI profile -------------------------------------------------------- */

static const char *const AI_LABEL[PSX_CPU_AI_BYTES] = {
    "Hand size", "Life point line", "Fusion deck gate", "Combo width",
    "Fusion depth", "Rank 1", "Rank 2", "Opening value", "Field 7"
};
static const char *const AI_HINT[PSX_CPU_AI_BYTES] = {
    "How many cards this opponent plays with: Ai_GetHandSize returns exactly this. 5 for the first duelists, 20 for the last",
    "Multiplied by 100 and compared against a life point total, so 10 means 1000 LP. The comparison guards a weakest-monster search",
    "Compared against how many cards are left in the AI's deck, just before it looks for a fusion. Stock 5, 10 or 20",
    "Read just before the best-combo search and handed to it. Stock 1 to 3",
    "Minus one, this is what the fusion evaluator is given: how far the AI looks for a fusion. Stock 1 to 3",
    "Rises with the campaign, 0 at the start and 5 for DarkNite and Nitemare. Nothing in the resident duel script reads it",
    "The same as Rank 1 in every stock duelist. Nothing in the resident duel script reads it either",
    "The first thing the duel script reads about the opponent, before anything happens. Stock 25, 50 or 75",
    "Stock 0, 25, 50 or 75. No use of it was found in the resident script"
};

const char *psx_cpu_ai_label(int f) { return (f >= 0 && f < PSX_CPU_AI_BYTES) ? AI_LABEL[f] : "?"; }
const char *psx_cpu_ai_hint(int f)  { return (f >= 0 && f < PSX_CPU_AI_BYTES) ? AI_HINT[f] : ""; }

static uint32_t ai_addr(int duelist) { return AI_TABLE + (uint32_t)(duelist + 1) * AI_STRIDE; }

/* The table is EXE data: resident from boot, never reloaded. Snapshot it once
 * so "stock" survives our own writes. */
static void ai_snapshot(void)
{
    if (g_ai_stock_ready || !psx_mod_game_started()) return;
    int nonzero = 0;
    for (int d = 0; d < NDUEL; d++)
        for (int f = 0; f < PSX_CPU_AI_BYTES; f++) {
            const uint8_t v = psx_mod_read_byte(ai_addr(d) + (uint32_t)f);
            g_ai_stock[d][f] = v;
            nonzero += v != 0;
        }
    /* Every duelist has a hand size, so an all-zero read is the EXE not being
     * there yet rather than a table of zeros. */
    if (nonzero) g_ai_stock_ready = 1;
}

int psx_cpu_ai_live(int duelist, uint8_t out[PSX_CPU_AI_BYTES])
{
    if (duelist < 0 || duelist >= NDUEL || !psx_mod_game_started()) return 0;
    for (int f = 0; f < PSX_CPU_AI_BYTES; f++)
        out[f] = psx_mod_read_byte(ai_addr(duelist) + (uint32_t)f);
    return 1;
}

int psx_cpu_ai_stock(int duelist, uint8_t out[PSX_CPU_AI_BYTES])
{
    ai_snapshot();
    if (duelist < 0 || duelist >= NDUEL || !g_ai_stock_ready) return 0;
    memcpy(out, g_ai_stock[duelist], PSX_CPU_AI_BYTES);
    return 1;
}

int psx_cpu_ai_edit(int duelist, uint8_t out[PSX_CPU_AI_BYTES])
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !g_edit[duelist].ai_set) return 0;
    if (out) memcpy(out, g_edit[duelist].ai, PSX_CPU_AI_BYTES);
    return 1;
}

int psx_cpu_ai_set(int duelist, int field, int value)
{
    psx_cpu_ensure_loaded();
    ai_snapshot();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    if (value < 0) return psx_cpu_ai_clear(duelist);
    if (field < 0 || field >= PSX_CPU_AI_BYTES || value > 255) return 0;
    CpuEdit *e = &g_edit[duelist];
    if (!e->ai_set) {
        uint8_t base[PSX_CPU_AI_BYTES];
        if (!psx_cpu_ai_live(duelist, base) && !psx_cpu_ai_stock(duelist, base)) return 0;
        memcpy(e->ai, base, PSX_CPU_AI_BYTES);
        e->ai_set = 1;
    }
    if (e->ai[field] == (uint8_t)value) return 1;
    e->ai[field] = (uint8_t)value;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_cpu_ai_clear(int duelist)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !g_edit[duelist].ai_set) return 0;
    g_edit[duelist].ai_set = 0;
    g_dirty = 1;
    g_gen++;
    return 1;
}

/* Push the edits (and only the edits) into the live table. Stock duelists are
 * put back from the snapshot, so clearing an edit takes effect at once. */
static void ai_apply(void)
{
    ai_snapshot();
    if (!g_ai_stock_ready) return;
    for (int d = 0; d < NDUEL; d++) {
        const uint8_t *want = g_edit[d].ai_set ? g_edit[d].ai : g_ai_stock[d];
        for (int f = 0; f < PSX_CPU_AI_BYTES; f++) {
            const uint32_t a = ai_addr(d) + (uint32_t)f;
            if (psx_mod_read_byte(a) != want[f]) psx_mod_write_byte(a, want[f]);
        }
    }
}

/* ---- the deck pool --------------------------------------------------------- */

static void deck_stock_into(int duelist, uint16_t *w)
{
    memset(w, 0, NCARDS * sizeof *w);
    const PsxDropDbDuelist *d = &PSX_DROP_DB[duelist];
    for (int i = 0; i < d->deck_n; i++) {
        const uint16_t c = d->deck[i].card;
        if (c >= 1 && c <= NCARDS) w[c - 1] = d->deck[i].weight;
    }
}

static void deck_effective(int duelist, uint16_t *w)
{
    if (g_edit[duelist].deck_set) memcpy(w, g_edit[duelist].deck, NCARDS * sizeof *w);
    else                          deck_stock_into(duelist, w);
}

int psx_cpu_deck_stock_weight(int duelist, int card)
{
    if (duelist < 0 || duelist >= NDUEL || card < 1 || card > NCARDS) return 0;
    const PsxDropDbDuelist *d = &PSX_DROP_DB[duelist];
    for (int i = 0; i < d->deck_n; i++)
        if (d->deck[i].card == card) return d->deck[i].weight;
    return 0;
}

int psx_cpu_deck_weight(int duelist, int card)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || card < 1 || card > NCARDS) return 0;
    if (!g_edit[duelist].deck_set) return psx_cpu_deck_stock_weight(duelist, card);
    return g_edit[duelist].deck[card - 1];
}

int psx_cpu_deck_edited(int duelist)
{
    psx_cpu_ensure_loaded();
    return (duelist >= 0 && duelist < NDUEL) ? g_edit[duelist].deck_set : 0;
}

int psx_cpu_deck_set(int duelist, int card, int weight)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || card < 1 || card > NCARDS) return 0;
    if (weight < 0 || weight > TOTAL) return 0;
    uint16_t w[NCARDS];
    deck_effective(duelist, w);
    const uint16_t pin_card = (uint16_t)card, pin_w = (uint16_t)weight;
    /* The one piece of renormalising arithmetic in the build: pin this card,
     * rescale the rest, keep the 2048 the loader expects. */
    if (psx_drop_pins_rescale(w, &pin_card, &pin_w, 1) != 1) return 0;
    memcpy(g_edit[duelist].deck, w, sizeof w);
    g_edit[duelist].deck_set = 1;
    g_edit[duelist].installed = 0;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_cpu_deck_clear(int duelist)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !g_edit[duelist].deck_set) return 0;
    g_edit[duelist].deck_set = 0;
    g_edit[duelist].installed = 0;
    for (uint32_t s = 0; s < REC_SECTORS; s++)
        psx_mod_cd_override_clear(REC_LBA(duelist + 1) + s);
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_cpu_deck_list(int duelist, uint16_t *cards, uint16_t *weights, int cap)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    uint16_t w[NCARDS];
    deck_effective(duelist, w);
    int n = 0;
    for (int c = 1; c <= NCARDS && n < cap; c++) {
        if (!w[c - 1]) continue;
        cards[n] = (uint16_t)c;
        weights[n] = w[c - 1];
        n++;
    }
    return n;
}

/* Write the duelist's record back to the disc with the edited deck pool in
 * it. The stock sectors are read first and CHECKED against the baked drop
 * database: the three drop pools inside the record must match, or this is not
 * the record we think it is and nothing is written. */
static int install_deck(int duelist)
{
    static uint8_t rec[REC_SECTORS * SECTOR];
    const uint32_t lba = REC_LBA(duelist + 1);
    for (uint32_t s = 0; s < REC_SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(lba + s, rec + s * SECTOR)) return 0;

    const PsxDropDbDuelist *db = &PSX_DROP_DB[duelist];
    for (int t = 0; t < PSX_DROP_DB_TIERS; t++) {
        uint16_t want[NCARDS];
        memset(want, 0, sizeof want);
        for (int i = 0; i < db->count[t]; i++) {
            const uint16_t c = db->tier[t][i].card;
            if (c >= 1 && c <= NCARDS) want[c - 1] = db->tier[t][i].weight;
        }
        if (memcmp(rec + DROP_OFF(t), want, ARRAY_BYTES) != 0) { g_refused++; return 0; }
    }

    uint16_t w[NCARDS];
    deck_effective(duelist, w);
    memcpy(rec + DECK_OFF, w, ARRAY_BYTES);
    for (uint32_t s = 0; s < REC_SECTORS; s++)
        if (!psx_mod_cd_override_set(lba + s, rec + s * SECTOR, SECTOR)) return 0;
    g_installs++;
    return 1;
}

/* ---- the portrait ----------------------------------------------------------
 *
 * Replacing one is the card-art pipeline pointed at a different target: the
 * PNG is scaled to 48x48, quantised to 64 colors and written back as indices
 * plus a CLUT. The whole 48-sector block is rebuilt from the stock sectors
 * every time, so the overrides are always stock plus exactly the portraits
 * the player has replaced -- there is no accumulated state to get wrong.
 *
 * The STP bit goes on every CLUT entry because the stock tiles carry it (the
 * portraits are solid squares, and black without STP keys out), and
 * psx_duelist_portraits.c refuses a block that does not have it. */

static void portrait_dir(int duelist, char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    snprintf(out, cap, "%s/duelists/%d", dir && dir[0] ? dir : ".", duelist + 1);
}

static void portrait_png(int duelist, char *out, size_t cap)
{
    char d[1024];
    portrait_dir(duelist, d, sizeof d);
    snprintf(out, cap, "%s/portrait.png", d);
}

static int file_exists(const char *p)
{
    FILE *f = psx_fopen_utf8(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int psx_cpu_portrait_edited(int duelist)
{
    psx_cpu_ensure_loaded();
    return (duelist >= 0 && duelist < NDUEL) ? g_edit[duelist].portrait_set : 0;
}

/* Rebuild the whole tile block from stock, paint every replaced portrait into
 * it, and override the sectors. Returns how many portraits were painted, or
 * -1 when the stock sectors could not be read. */
static int portraits_install(void)
{
    static uint8_t block[TILE_SECTORS * SECTOR];
    int painted = 0, any = 0;
    for (int d = 0; d < NDUEL; d++) any += g_edit[d].portrait_set != 0;
    if (!any) {
        for (uint32_t sct = 0; sct < TILE_SECTORS; sct++) psx_mod_cd_override_clear(TILE_LBA + sct);
        return 0;
    }
    for (uint32_t sct = 0; sct < TILE_SECTORS; sct++)
        if (!psx_mod_cd_read_stock_sector(TILE_LBA + sct, block + sct * SECTOR)) return -1;
    for (int d = 0; d < NDUEL; d++) {
        if (!g_edit[d].portrait_set) continue;
        char png[1200];
        portrait_png(d, png, sizeof png);
        static uint8_t rgb[TILE_PIXELS * 3];
        if (!psx_card_packs_load_png_rgb(png, TILE_W, TILE_W, rgb)) continue;
        static uint8_t idx[TILE_PIXELS];
        uint16_t clut[TILE_CLUT];
        psx_card_packs_quantize(rgb, TILE_PIXELS * 3, TILE_CLUT, idx, clut);
        uint8_t *tile = block + (uint32_t)(d + 1) * TILE_BYTES;
        for (int i = 0; i < TILE_PIXELS; i++) tile[i] = (uint8_t)(idx[i] & 63u);
        for (int k = 0; k < TILE_CLUT; k++) {
            const uint16_t c = (uint16_t)(clut[k] | 0x8000u);   /* STP, as stock */
            tile[TILE_PIXELS + 2 * k]     = (uint8_t)(c & 0xFFu);
            tile[TILE_PIXELS + 2 * k + 1] = (uint8_t)(c >> 8);
        }
        {   /* hand the same pixels to the windows, in the colours the game
             * will draw: they decode the STOCK sectors and cannot see an
             * override. */
            static uint32_t argb[TILE_PIXELS];
            for (int i = 0; i < TILE_PIXELS; i++) {
                const unsigned c = clut[idx[i] & 63u];
                const unsigned r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
                argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            psx_duelist_portraits_override(d, argb);
        }
        painted++;
    }
    for (uint32_t sct = 0; sct < TILE_SECTORS; sct++)
        if (!psx_mod_cd_override_set(TILE_LBA + sct, block + sct * SECTOR, SECTOR)) return -1;
    psx_duelist_portraits_reload();
    return painted;
}

int psx_cpu_portrait_set(int duelist, const char *png_path, char *msg, unsigned cap)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !png_path || !png_path[0]) {
        if (msg && cap) snprintf(msg, cap, "No picture to use");
        return 0;
    }
    static uint8_t rgb[TILE_PIXELS * 3];
    if (!psx_card_packs_load_png_rgb(png_path, TILE_W, TILE_W, rgb)) {
        if (msg && cap) snprintf(msg, cap, "That file is not a picture this can read");
        return 0;
    }
    /* Keep the player's own PNG: it is what survives a restart, and what
     * they can replace by hand. */
    char dir[1024], dest[1200];
    const char *pd = psx_mod_player_data_dir();
    char base[1100];
    snprintf(base, sizeof base, "%s/duelists", pd && pd[0] ? pd : ".");
#ifdef _WIN32
    (void)_mkdir(base);
#else
    (void)mkdir(base, 0755);
#endif
    portrait_dir(duelist, dir, sizeof dir);
#ifdef _WIN32
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
    portrait_png(duelist, dest, sizeof dest);
    {   /* copy it in, unless it is already the file we would write */
        FILE *in = psx_fopen_utf8(png_path, "rb");
        if (!in) { if (msg && cap) snprintf(msg, cap, "Could not read that file"); return 0; }
        FILE *out = strcmp(png_path, dest) ? psx_fopen_utf8(dest, "wb") : NULL;
        if (out) {
            char buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
            fclose(out);
        }
        fclose(in);
    }
    g_edit[duelist].portrait_set = 1;
    const int painted = portraits_install();
    g_gen++;
    if (msg && cap) {
        if (painted < 0) snprintf(msg, cap, "The portrait sectors could not be written");
        else snprintf(msg, cap, "Portrait replaced for %.24s", PSX_DROP_DB[duelist].name);
    }
    return painted >= 0;
}

int psx_cpu_portrait_clear(int duelist)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !g_edit[duelist].portrait_set) return 0;
    char png[1200];
    portrait_png(duelist, png, sizeof png);
    (void)psx_remove_utf8(png);
    g_edit[duelist].portrait_set = 0;
    psx_duelist_portraits_override(duelist, NULL);
    (void)portraits_install();
    g_gen++;
    return 1;
}

/* ---- the name --------------------------------------------------------------
 *
 * The stock offsets are snapshotted once the table is resident (a zero entry
 * would be a Build Deck tile with no name, so all-nonzero is the test), and
 * put back when an edit is cleared. The edit itself is one fixed slot in
 * NAME_ARENA, re-asserted every frame: the table is re-streamed with the
 * EXE data a savestate restores, and the slot with it. */
static uint32_t nameoff_addr(int duelist) { return NAMEOFF_TABLE + (NAME_ENTRY0 + (uint32_t)duelist + 1u) * 2u; }

static void name_snapshot(void)
{
    if (g_name_stock_ready || !psx_mod_game_started()) return;
    for (int d = 0; d < NDUEL; d++) {
        const uint16_t off = psx_mod_read_half(nameoff_addr(d));
        if (!off) return;
        g_name_stock[d] = off;
    }
    g_name_stock_ready = 1;
}

static int name_encode(const char *name, uint8_t *out, int cap)
{
    int n = 0;
    for (const char *p = name; *p && n + 1 < cap; p++) {
        const int code = *p == ' ' ? 0 : psx_card_packs_encode_char(*p);
        if (*p != ' ' && !code) return -1;      /* a glyph the font lacks */
        out[n++] = (uint8_t)code;
    }
    out[n++] = 0xFF;
    return n;
}

int psx_cpu_name_set(int duelist, const char *name)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL) return 0;
    if (!name || !name[0]) return psx_cpu_name_clear(duelist);
    char trimmed[PSX_CPU_NAME_MAX + 1];
    {   /* the ini writer trims, so what is kept is what would be read back */
        while (*name == ' ') name++;
        snprintf(trimmed, sizeof trimmed, "%s", name);
        size_t n = strlen(trimmed);
        while (n && trimmed[n - 1] == ' ') trimmed[--n] = 0;
        if (!n) return psx_cpu_name_clear(duelist);
    }
    CpuEdit *e = &g_edit[duelist];
    uint8_t enc[PSX_CPU_NAME_MAX + 1];
    const int len = name_encode(trimmed, enc, (int)sizeof enc);
    if (len < 0) return 0;
    if (e->name_set && !strcmp(e->name, trimmed)) return 1;
    snprintf(e->name, sizeof e->name, "%s", trimmed);
    memcpy(e->enc, enc, (size_t)len);
    e->enc_len = (uint8_t)len;
    e->name_set = 1;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_cpu_name_clear(int duelist)
{
    psx_cpu_ensure_loaded();
    if (duelist < 0 || duelist >= NDUEL || !g_edit[duelist].name_set) return 0;
    g_edit[duelist].name_set = 0;
    g_edit[duelist].enc_len = 0;
    g_dirty = 1;
    g_gen++;
    return 1;
}

int psx_cpu_name_edited(int duelist)
{
    psx_cpu_ensure_loaded();
    return (duelist >= 0 && duelist < NDUEL) ? g_edit[duelist].name_set : 0;
}

const char *psx_cpu_display_name(int duelist)
{
    if (duelist < 0 || duelist >= NDUEL) return "?";
    psx_cpu_ensure_loaded();
    return g_edit[duelist].name_set ? g_edit[duelist].name : PSX_DROP_DB[duelist].name;
}

/* Per frame: the edited strings and their table entries, the stock entries
 * for everyone else. Cheap: a handful of reads that match. */
static void names_apply(void)
{
    name_snapshot();
    if (!g_name_stock_ready) return;
    for (int d = 0; d < NDUEL; d++) {
        const CpuEdit *e = &g_edit[d];
        const uint32_t oa = nameoff_addr(d);
        if (!e->name_set) {
            if (psx_mod_read_half(oa) != g_name_stock[d]) psx_mod_write_half(oa, g_name_stock[d]);
            continue;
        }
        const uint32_t sa = NAME_SLOT_ADDR(d);
        for (int k = 0; k < e->enc_len; k++)
            if (psx_mod_read_byte(sa + (uint32_t)k) != e->enc[k]) psx_mod_write_byte(sa + (uint32_t)k, e->enc[k]);
        const uint16_t want = (uint16_t)(sa - NAME_SEGMENT);
        if (psx_mod_read_half(oa) != want) psx_mod_write_half(oa, want);
    }
}

/* ---- the record ------------------------------------------------------------ */

int psx_cpu_record(int duelist, int *wins, int *losses)
{
    if (duelist < 0 || duelist >= NDUEL || !psx_ygo_save_is_live()) return 0;
    const uint32_t a = RECORDS + (uint32_t)(duelist + 1) * 4u;
    if (wins)   *wins   = (int)psx_mod_read_half(a);
    if (losses) *losses = (int)psx_mod_read_half(a + 2u);
    return 1;
}

int psx_cpu_record_set(int duelist, int wins, int losses)
{
    if (duelist < 0 || duelist >= NDUEL || !psx_ygo_save_is_live()) return 0;
    if (wins < 0) wins = 0;
    if (losses < 0) losses = 0;
    if (wins > 999) wins = 999;          /* the screen's own ceiling */
    if (losses > 999) losses = 999;
    const uint32_t a = RECORDS + (uint32_t)(duelist + 1) * 4u;
    psx_mod_write_half(a, (uint16_t)wins);
    psx_mod_write_half(a + 2u, (uint16_t)losses);
    return 1;
}

/* ---- the file --------------------------------------------------------------
 *
 *     [Duelist Name]
 *     ai = 5, 20, 10, 1, 1, 0, 0, 25, 50
 *     <card id> = <deck weight>
 */

static void ini_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", dir, INI_NAME);
    else               snprintf(out, cap, "%s", INI_NAME);
}

void psx_cpu_share_dir(char *out, unsigned cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (!out || !cap) return;
    if (dir && dir[0]) snprintf(out, cap, "%s/cpu_decks", dir);
    else               snprintf(out, cap, "cpu_decks");
#ifdef _WIN32
    (void)_mkdir(out);
#else
    (void)mkdir(out, 0755);
#endif
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int read_ini(const char *path)
{
    FILE *f = psx_fopen_utf8(path, "r");
    if (!f) return -1;
    for (int d = 0; d < NDUEL; d++) {
        g_edit[d].deck_set = 0;
        g_edit[d].ai_set = 0;
        g_edit[d].installed = 0;
        g_edit[d].name_set = 0;
        g_edit[d].enc_len = 0;
    }
    char line[256];
    int cur = -1, n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            cur = -1;
            for (int d = 0; d < NDUEL; d++)
                if (!strcmp(PSX_DROP_DB[d].name, s + 1)) { cur = d; break; }
            continue;
        }
        if (cur < 0) continue;
        if (!strncmp(s, "name", 4) && (s[4] == ' ' || s[4] == '=' || s[4] == '\t')) {
            char *v = strchr(s, '=');
            if (!v) continue;
            v = trim(v + 1);
            /* through the setter, so a glyph the font lacks is dropped here
             * rather than drawn as a blank in the grid */
            const int was_dirty = g_dirty;
            if (psx_cpu_name_set(cur, v)) n++;
            g_dirty = was_dirty;
            continue;
        }
        int a[PSX_CPU_AI_BYTES];
        if (sscanf(s, "ai = %d , %d , %d , %d , %d , %d , %d , %d , %d",
                   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8]) == PSX_CPU_AI_BYTES) {
            for (int i = 0; i < PSX_CPU_AI_BYTES; i++)
                g_edit[cur].ai[i] = (uint8_t)(a[i] < 0 ? 0 : a[i] > 255 ? 255 : a[i]);
            g_edit[cur].ai_set = 1;
            n++;
            continue;
        }
        int card = 0, weight = 0;
        if (sscanf(s, "%d = %d", &card, &weight) != 2) continue;
        if (card < 1 || card > NCARDS || weight < 0 || weight > TOTAL) continue;
        if (!g_edit[cur].deck_set) {
            deck_stock_into(cur, g_edit[cur].deck);
            g_edit[cur].deck_set = 1;
        }
        g_edit[cur].deck[card - 1] = (uint16_t)weight;
        n++;
    }
    fclose(f);
    /* A hand-written pool that does not total 2048 is not loadable, so it is
     * rescaled here rather than refused: the file stays hand-editable and the
     * loader still gets what it needs. */
    for (int d = 0; d < NDUEL; d++) {
        if (!g_edit[d].deck_set) continue;
        long sum = 0;
        for (int c = 0; c < NCARDS; c++) sum += g_edit[d].deck[c];
        if (sum == TOTAL || sum <= 0) continue;
        long acc = 0;
        int last = -1;
        for (int c = 0; c < NCARDS; c++) {
            if (!g_edit[d].deck[c]) continue;
            const long v = (long)g_edit[d].deck[c] * TOTAL / sum;
            g_edit[d].deck[c] = (uint16_t)v;
            acc += v;
            last = c;
        }
        if (last >= 0 && acc != TOTAL) g_edit[d].deck[last] = (uint16_t)(g_edit[d].deck[last] + (TOTAL - acc));
    }
    return n;
}

void psx_cpu_ensure_loaded(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    ini_path(g_ini_path, sizeof g_ini_path);
    for (int d = 0; d < NDUEL; d++) {
        char png[1200];
        portrait_png(d, png, sizeof png);
        g_edit[d].portrait_set = (uint8_t)file_exists(png);
    }
    const int n = read_ini(g_ini_path);
    snprintf(g_status, sizeof g_status, n < 0 ? "no ini" : "%d entries from ini", n < 0 ? 0 : n);
    g_dirty = 0;
    g_gen++;
}

static int write_to(const char *path)
{
    FILE *f = psx_fopen_utf8(path, "w");
    if (!f) return 0;
    fprintf(f,
"; Yu-Gi-Oh! Forbidden Memories - Recompiled : CPU duelists\n"
";\n"
"; Written by the CPU Manager (VIEW > CPU MANAGER); hand-editing works too.\n"
"; One section per duelist:\n"
";\n"
";     name = Dingus                        what the FREE DUEL grid calls them (letters,\n"
";                                          digits and . , ! ? ' - & / : ( ) only, %d at most)\n"
";     ai = 5, 20, 10, 1, 1, 0, 0, 25, 50    the nine AI profile bytes\n"
";     <card id> = <weight>                  their deck pool, out of 2048\n"
";\n"
"; A deck pool listed here REPLACES that duelist's: every card they can draw\n"
"; has to be in it, and the weights are rescaled to 2048 when they are not.\n"
"; The section header is always the disc's own name, so a renamed duelist\n"
"; can still be found. Delete a section (or the file) to put a duelist back\n"
"; to the disc's own.\n"
"\n", PSX_CPU_NAME_MAX);
    for (int d = 0; d < NDUEL; d++) {
        if (!g_edit[d].deck_set && !g_edit[d].ai_set && !g_edit[d].name_set) continue;
        fprintf(f, "[%s]\n", PSX_DROP_DB[d].name);
        if (g_edit[d].name_set) fprintf(f, "name = %s\n", g_edit[d].name);
        if (g_edit[d].ai_set) {
            fprintf(f, "ai = ");
            for (int i = 0; i < PSX_CPU_AI_BYTES; i++)
                fprintf(f, "%d%s", g_edit[d].ai[i], i + 1 < PSX_CPU_AI_BYTES ? ", " : "\n");
        }
        if (g_edit[d].deck_set)
            for (int c = 1; c <= NCARDS; c++)
                if (g_edit[d].deck[c - 1])
                    fprintf(f, "%-3d = %4d\n", c, g_edit[d].deck[c - 1]);
        fprintf(f, "\n");
    }
    fclose(f);
    return 1;
}

int psx_cpu_save(void)
{
    psx_cpu_ensure_loaded();
    if (!write_to(g_ini_path)) { snprintf(g_status, sizeof g_status, "save FAILED"); return 0; }
    g_dirty = 0;
    snprintf(g_status, sizeof g_status, "saved");
    return 1;
}

int      psx_cpu_dirty(void)      { return g_dirty; }
unsigned psx_cpu_generation(void) { return g_gen; }

int psx_cpu_export_file(const char *path, char *msg, unsigned cap)
{
    psx_cpu_ensure_loaded();
    if (!path || !path[0]) { if (msg && cap) snprintf(msg, cap, "No file to export to"); return 0; }
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    const char *base = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    if (!strchr(base, '.')) { const size_t n = strlen(p); snprintf(p + n, sizeof p - n, ".ini"); }
    if (!write_to(p)) { if (msg && cap) snprintf(msg, cap, "Could not write that file"); return 0; }
    int decks = 0, ai = 0, names = 0;
    for (int d = 0; d < NDUEL; d++) { decks += g_edit[d].deck_set != 0; ai += g_edit[d].ai_set != 0; names += g_edit[d].name_set != 0; }
    base = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    if (msg && cap)
        snprintf(msg, cap, "Exported %d deck%s, %d AI profile%s and %d name%s as %.40s",
                 decks, decks == 1 ? "" : "s", ai, ai == 1 ? "" : "s", names, names == 1 ? "" : "s", base);
    return 1;
}

int psx_cpu_import_file(const char *path, char *msg, unsigned cap)
{
    psx_cpu_ensure_loaded();
    if (!path || !path[0] || read_ini(path) < 0) {
        if (msg && cap) snprintf(msg, cap, "Could not read that file");
        return 0;
    }
    /* The file REPLACES the edits, so a deck that was installed before the
     * import and is not in the file must come off the disc too: the sector
     * store keeps an override until it is cleared, and read_ini() only
     * forgets the flag. (Found in the 2026-09-06 double-check: an import
     * without Simon's section left Simon's edited pool in play.) */
    for (int d = 0; d < NDUEL; d++) {
        g_edit[d].installed = 0;
        if (g_edit[d].deck_set) continue;
        for (uint32_t sct = 0; sct < REC_SECTORS; sct++)
            psx_mod_cd_override_clear(REC_LBA(d + 1) + sct);
    }
    g_gen++;
    /* An import sticks, the way every other manager's does. */
    const int kept = psx_cpu_save();
    int decks = 0, ai = 0, names = 0;
    for (int d = 0; d < NDUEL; d++) { decks += g_edit[d].deck_set != 0; ai += g_edit[d].ai_set != 0; names += g_edit[d].name_set != 0; }
    if (msg && cap) {
        if (!kept) snprintf(msg, cap, "Imported, but %s could not be written", INI_NAME);
        else snprintf(msg, cap, "Imported %d deck%s, %d AI profile%s and %d name%s, and kept them",
                      decks, decks == 1 ? "" : "s", ai, ai == 1 ? "" : "s", names, names == 1 ? "" : "s");
    }
    return 1;
}

/* ---- the tick --------------------------------------------------------------
 *
 * Cheap: one generation compare when nothing has changed. The AI table is
 * pushed every time the edits change (and once at boot, when the EXE's data
 * has arrived); a deck override is installed once per edited duelist, because
 * the sector store keeps it until it is cleared. */
static void tick(void)
{
    static unsigned seen_gen;
    static int seen_ai_ready;
    if (!psx_mod_game_started()) return;
    psx_cpu_ensure_loaded();
    ai_snapshot();
    names_apply();        /* every frame: the table comes back stock with the EXE data */
    if (g_gen == seen_gen && g_ai_stock_ready == seen_ai_ready) return;
    seen_gen = g_gen;
    seen_ai_ready = g_ai_stock_ready;
    ai_apply();
    for (int d = 0; d < NDUEL; d++) {
        if (!g_edit[d].deck_set || g_edit[d].installed) continue;
        if (install_deck(d)) g_edit[d].installed = 1;
    }
    {   /* the portraits, once, when the disc answers */
        static int done;
        int want = 0;
        for (int d = 0; d < NDUEL; d++) want += g_edit[d].portrait_set != 0;
        if (want && !done && portraits_install() >= 0) done = 1;
        if (!want) done = 0;
    }
}

int psx_cpu_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    psx_cpu_ensure_loaded();
    int decks = 0, ai = 0, portraits = 0, names = 0;
    for (int d = 0; d < NDUEL; d++) {
        decks += g_edit[d].deck_set != 0;
        ai += g_edit[d].ai_set != 0;
        portraits += g_edit[d].portrait_set != 0;
        names += g_edit[d].name_set != 0;
    }
    unsigned n = (unsigned)snprintf(out, cap,
        "\"decks\":%d,\"ai\":%d,\"portraits\":%d,\"names\":%d,\"dirty\":%d,\"gen\":%u,\"installs\":%d,\"refused\":%d,"
        "\"ai_ready\":%d,\"names_ready\":%d,\"name_arena\":\"%08X\",\"save_live\":%d,\"status\":\"%s\",\"duelists\":[",
        decks, ai, portraits, names, g_dirty, g_gen, g_installs, g_refused, g_ai_stock_ready,
        g_name_stock_ready, NAME_ARENA, psx_ygo_save_is_live(), g_status);
    int first = 1;
    for (int d = 0; d < NDUEL && n + 260u < cap; d++) {
        if (!g_edit[d].deck_set && !g_edit[d].ai_set && !g_edit[d].name_set) continue;
        uint8_t live[PSX_CPU_AI_BYTES] = {0};
        psx_cpu_ai_live(d, live);
        n += (unsigned)snprintf(out + n, cap - n,
            "%s{\"d\":%d,\"id\":%d,\"name\":\"%s\",\"shown\":\"%s\",\"nameoff\":%u,\"deck\":%d,\"installed\":%d,\"ai_set\":%d,"
            "\"live\":[%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
            first ? "" : ",", d, d + 1, PSX_DROP_DB[d].name, psx_cpu_display_name(d),
            psx_mod_game_started() ? psx_mod_read_half(nameoff_addr(d)) : 0u,
            g_edit[d].deck_set, g_edit[d].installed, g_edit[d].ai_set,
            live[0], live[1], live[2], live[3], live[4], live[5], live[6], live[7], live[8]);
        first = 0;
    }
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

void psx_cpu_data_install(void)
{
    (void)psx_game_add_frame_hook(tick);
}

PSX_MOD_CONSTRUCTOR(psx_cpu_data_ctor)
{
    psx_cpu_data_install();
}
