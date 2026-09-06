/* psx_fusion_table.c -- see psx_fusion_table.h.
 *
 * ---- where the bytes are --------------------------------------------------
 *
 * The duel loader (Main_RunDuel -> func_800179F4 -> func_80014E1C) streams one
 * 235-sector block per terrain out of WA_MRG.MRG and scatters it through RAM
 * in thirteen stages. Stage 3 is this table: block + 0x24800, exactly 0x10000
 * bytes, landing at 0x8017C2D8. Stage 2 is the equip table, block + 0x22000 ->
 * 0x8017A1D8; psx_card_effects.c already replaces that one and the ritual data
 * the same way, and its PKG_LBA is the same block base as ours.
 *
 *     block k   = disc LBA 15932 + 235*k          (k = terrain 0..6)
 *     fusion    = block + 73 sectors, 32 sectors  (LBA 16005 + 235*k)
 *
 * All seven copies ship byte-identical; the block is loaded once per duel for
 * the terrain the duel STARTS on, so a Field card played mid-duel changes the
 * stat bonuses but never reloads this. We write all seven, which makes the
 * question moot.
 *
 * ---- the format -----------------------------------------------------------
 *
 *     u16 index[723]          index[id] = byte offset of that card's record,
 *                             0 = the card fuses with nothing; index[0] unused
 *     record: u8 count        1..255, or u8 0 then u8 511-count for 256..511
 *             5-byte groups   two (partner, result) entries of 10-bit ids:
 *               g[0] = p1>>8 | (r1>>8)<<2 | (p2>>8)<<4 | (r2>>8)<<6
 *               g[1] = p1&0xFF  g[2] = r1&0xFF  g[3] = p2&0xFF  g[4] = r2&0xFF
 *             ODD count       the final group is written as only THREE bytes
 *
 * A pair is stored once, under the smaller id. Records follow the index in
 * card order. 0xFF fills the tail. Stock: 578 records, 25131 pairs, 65002 of
 * 65536 bytes.
 *
 * The three-byte tail is what produces the fifteen "glitch fusions" the game
 * answers and the table never meant to hold: the guest's loop still tests a
 * second half that is not there and reads the next record's first two bytes.
 * psx_fusion_db.c's header lists them. REPACKING MOVES THEM — a different
 * layout produces a different set of accidents — so this module never assumes
 * what they are: it packs the intended pairs, then decodes the packed bytes
 * back through the guest's own walk and publishes THAT as the recipe list.
 * What the browser shows is therefore what the game will do, by construction.
 *
 * Cross-checked against the disc and against TEA-Online's FUSION MAKER
 * (~/teatools/fusionmaker, whose scripts/fusion_table.py round-trips the stock
 * bytes exactly): same 578/25131/65002, same fifteen.
 */

#include "psx_fusion_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_fusion_db.h"
#include "psx_game_hooks.h"
#include "psx_textfile.h"
#include "psx_tool_window.h"

#define NCARDS      PSX_FUSION_TABLE_CARDS
#define TBL_BYTES   PSX_FUSION_TABLE_BYTES
#define INDEX_BYTES ((NCARDS + 1) * 2)          /* 1446 */
#define SECTOR      2048
#define TERRAINS    7

#define PKG_LBA(k)     (15932u + 235u * (uint32_t)(k))
#define FUSION_LBA(k)  (PKG_LBA(k) + 73u)
#define FUSION_SECTORS (TBL_BYTES / SECTOR)     /* 32 */
#define EQUIP_LBA(k)   (PKG_LBA(k) + 68u)
#define EQUIP_SECTORS  5
#define EQUIP_USED     0x2100
#define FUSION_RAM     0x8017C2D8u

/* The packer refuses at this; the loader copies 0x10000 and offsets are u16,
 * so a record may not START at 0xFFFF or later either. */
#define CAP_BYTES   (TBL_BYTES - 1)

#define S_ARROW_TXT "\xE2\x80\x93"     /* en dash, for the status strings */

/* --- state --------------------------------------------------------------- */

static uint8_t  s_stock[TBL_BYTES];      /* the disc's own bytes */
static int      s_have_stock;
static uint8_t  s_packed[TBL_BYTES];     /* stock + edits, packed */
static int      s_packed_bytes;

/* The intended pair set: what we pack. Sorted by (a, b), one entry per pair. */
typedef struct { uint16_t a, b, r; } Pair;
static Pair    *s_intent;
static int      s_intent_n, s_intent_cap;
/* The stock intended set, kept so an edit can be compared with it and dropped
 * when it puts the pair back the way it was. */
static Pair    *s_stock_pairs;
static int      s_stock_n;
/* What the packed bytes actually answer, glitches included -- published. */
static PsxFusionRecipe *s_eff;
static int              s_eff_n, s_eff_cap;
/* The player's changes, sorted by (a, b). r == 0 means "remove this pair". */
static Pair    *s_edit;
static int      s_edit_n, s_edit_cap;

static PsxFusionEquip *s_equip;
static int             s_equip_n, s_equip_cap, s_equip_groups;

/* "delete every fusion". Storing it as a flag instead of an edit per pair is
 * what keeps fusion_edits.txt readable: an emptied table with three additions
 * is four lines, not twenty-five thousand. */
static int      s_wipe;

static int      s_applied;
static unsigned s_gen = 1;

/* --- little helpers ------------------------------------------------------ */

static int pair_cmp(const void *pa, const void *pb)
{
    const Pair *x = (const Pair *)pa, *y = (const Pair *)pb;
    if (x->a != y->a) return (int)x->a - (int)y->a;
    return (int)x->b - (int)y->b;
}

/* Index of (a, b) in a sorted Pair array, or -1. */
static int pair_find(const Pair *v, int n, int a, int b)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (v[m].a != a) { if (v[m].a < a) lo = m + 1; else hi = m - 1; continue; }
        if (v[m].b == b) return m;
        if (v[m].b < b) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static int grow(void **v, int *cap, int need, size_t elem)
{
    if (need <= *cap) return 1;
    int c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    void *p = realloc(*v, elem * (size_t)c);
    if (!p) return 0;
    *v = p; *cap = c;
    return 1;
}

static void order(int *a, int *b) { if (*b < *a) { const int t = *a; *a = *b; *b = t; } }

/* --- decoding ------------------------------------------------------------- */

/* The pairs a record INTENDS to hold: `count` entries, two per five bytes,
 * reading the final odd entry from the three bytes that are really there. */
static int decode_intended(const uint8_t *tbl, Pair **out, int *out_n, int *out_cap)
{
    int n = 0;
    for (int cid = 1; cid <= NCARDS; cid++) {
        const int off = tbl[cid * 2] | (tbl[cid * 2 + 1] << 8);
        if (off < INDEX_BYTES || off >= TBL_BYTES) continue;
        int p = off, count = tbl[p];
        if (count) p += 1; else { count = 511 - tbl[p + 1]; p += 2; }
        if (count <= 0 || count > 511) continue;
        for (int e = 0; e < count; e++) {
            const int q = p + 5 * (e / 2);
            if (q + 3 > TBL_BYTES) break;
            const uint8_t g0 = tbl[q];
            int partner, result;
            if ((e & 1) == 0) {
                partner = ((g0 << 8) & 0x300) | tbl[q + 1];
                result  = ((g0 << 6) & 0x300) | tbl[q + 2];
            } else {
                if (q + 5 > TBL_BYTES) break;
                partner = ((g0 << 4) & 0x300) | tbl[q + 3];
                result  = ((g0 << 2) & 0x300) | tbl[q + 4];
            }
            if (partner < 1 || partner > NCARDS) continue;
            if (result  < 1 || result  > NCARDS) continue;
            if (!grow((void **)out, out_cap, n + 1, sizeof(Pair))) return 0;
            (*out)[n].a = (uint16_t)cid;
            (*out)[n].b = (uint16_t)partner;
            (*out)[n].r = (uint16_t)result;
            n++;
        }
    }
    /* the game finds only the FIRST entry for a partner; drop later ones so
     * the set is one result per pair, the way every consumer expects */
    qsort(*out, (size_t)n, sizeof(Pair), pair_cmp);
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (w && (*out)[w - 1].a == (*out)[i].a && (*out)[w - 1].b == (*out)[i].b) continue;
        (*out)[w++] = (*out)[i];
    }
    *out_n = w;
    return 1;
}

/* What the guest's walk ANSWERS for this table, which is the intended set plus
 * whatever the three-byte tails happen to make reachable. Only entries the
 * lookup can actually reach: a record is consulted for min(a,b), and the loop
 * returns on its first match. */
static int decode_effective(const uint8_t *tbl)
{
    s_eff_n = 0;
    for (int cid = 1; cid <= NCARDS; cid++) {
        const int off = tbl[cid * 2] | (tbl[cid * 2 + 1] << 8);
        if (off < INDEX_BYTES || off >= TBL_BYTES) continue;
        int p = off, count = tbl[p];
        int intended = 1;
        if (count) p += 1; else { count = 511 - tbl[p + 1]; p += 2; }
        if (count <= 0 || count > 511) continue;
        uint8_t seen[(NCARDS + 8) / 8];
        memset(seen, 0, sizeof seen);
        for (int c = count, q = p; c > 0; c -= 2, q += 5) {
            if (q + 5 > TBL_BYTES) break;
            const uint8_t g0 = tbl[q];
            const int half[2][2] = {
                { ((g0 << 8) & 0x300) | tbl[q + 1], ((g0 << 6) & 0x300) | tbl[q + 2] },
                { ((g0 << 4) & 0x300) | tbl[q + 3], ((g0 << 2) & 0x300) | tbl[q + 4] },
            };
            for (int h = 0; h < 2; h++) {
                /* the second half of the last group of an odd record is the
                 * next record's bytes: reachable, but never intended */
                intended = !(c == 1 && h == 1);
                const int partner = half[h][0], result = half[h][1];
                if (partner < cid || partner > NCARDS) continue;
                if (result < 1 || result > NCARDS) continue;
                if (seen[partner >> 3] & (1u << (partner & 7))) continue;
                seen[partner >> 3] |= (uint8_t)(1u << (partner & 7));
                if (!grow((void **)&s_eff, &s_eff_cap, s_eff_n + 1, sizeof(PsxFusionRecipe))) return 0;
                s_eff[s_eff_n].a = (uint16_t)cid;
                s_eff[s_eff_n].b = (uint16_t)partner;
                s_eff[s_eff_n].r = (uint16_t)result;
                s_eff[s_eff_n].glitch = (uint8_t)!intended;
                s_eff_n++;
            }
        }
    }
    return 1;
}

/* --- packing -------------------------------------------------------------- */

/* Serialise `v` (sorted by a then b) into `out`. Returns the bytes used, or 0
 * with the reason in err. Byte-for-byte the stock layout when v is the stock
 * set, which is the test this has to pass. */
static int pack(const Pair *v, int n, uint8_t *out, char *err, unsigned cap)
{
    memset(out, 0xFF, TBL_BYTES);
    memset(out, 0, INDEX_BYTES);
    int pos = INDEX_BYTES, i = 0;
    for (int cid = 1; cid <= NCARDS; cid++) {
        const int start = i;
        while (i < n && v[i].a == cid) i++;
        const int cnt = i - start;
        if (!cnt) continue;
        if (cnt > 511) {
            if (err) snprintf(err, cap, "Card %d would have %d partners; the format holds 511", cid, cnt);
            return 0;
        }
        const int hdr = cnt < 256 ? 1 : 2;
        const int body = 5 * (cnt / 2) + ((cnt & 1) ? 3 : 0);
        if (pos > 0xFFFF || pos + hdr + body > CAP_BYTES) {
            if (err) snprintf(err, cap, "Table full at card %d: needs more than %d bytes", cid, CAP_BYTES);
            return 0;
        }
        out[cid * 2] = (uint8_t)pos;
        out[cid * 2 + 1] = (uint8_t)(pos >> 8);
        if (cnt < 256) out[pos++] = (uint8_t)cnt;
        else { out[pos++] = 0; out[pos++] = (uint8_t)(511 - cnt); }
        for (int e = 0; e + 1 < cnt; e += 2) {
            const Pair *x = &v[start + e], *y = &v[start + e + 1];
            out[pos++] = (uint8_t)((x->b >> 8) | ((x->r >> 8) << 2) | ((y->b >> 8) << 4) | ((y->r >> 8) << 6));
            out[pos++] = (uint8_t)x->b;
            out[pos++] = (uint8_t)x->r;
            out[pos++] = (uint8_t)y->b;
            out[pos++] = (uint8_t)y->r;
        }
        if (cnt & 1) {
            const Pair *x = &v[start + cnt - 1];
            out[pos++] = (uint8_t)((x->b >> 8) | ((x->r >> 8) << 2));
            out[pos++] = (uint8_t)x->b;
            out[pos++] = (uint8_t)x->r;
        }
    }
    /* The LAST record's three-byte tail reads the two bytes past the end of
     * the data, which is 0xFF fill -- and 0xFF decodes to a partner and a
     * result the game will answer. Stock never notices because its final
     * record happens not to, but an emptied table with one recipe in it does:
     * it grew a phantom "card 1 + card 255 = card 255". Two zero bytes cost
     * nothing and make that decode to id 0, which is no fusion. */
    if (pos + 2 <= TBL_BYTES) { out[pos] = 0; out[pos + 1] = 0; }
    return pos;
}

/* --- building the effective view ------------------------------------------ */

/* intended = stock, with every edit overlaid (r == 0 removes) -- or, once the
 * table has been emptied, nothing but the edits that add something. */
static int build_intent(void)
{
    if (!grow((void **)&s_intent, &s_intent_cap, s_stock_n + s_edit_n + 1, sizeof(Pair))) return 0;
    if (s_wipe) {
        int n = 0;
        for (int j = 0; j < s_edit_n; j++) if (s_edit[j].r) s_intent[n++] = s_edit[j];
        s_intent_n = n;                        /* s_edit is sorted, so this is */
        return 1;
    }
    int n = 0, i = 0, j = 0;
    while (i < s_stock_n || j < s_edit_n) {
        int take_edit;
        if (i >= s_stock_n)      take_edit = 1;
        else if (j >= s_edit_n)  take_edit = 0;
        else {
            const int c = pair_cmp(&s_stock_pairs[i], &s_edit[j]);
            if (c == 0) i++;                 /* the edit replaces this pair */
            take_edit = c >= 0;
        }
        const Pair *p = take_edit ? &s_edit[j++] : &s_stock_pairs[i++];
        if (!p->r) continue;                 /* a removal */
        s_intent[n++] = *p;
    }
    s_intent_n = n;
    return 1;
}

/* Repack and re-derive everything. 0 with the reason in err leaves the
 * previous packed table and recipe list untouched. */
static int rebuild(char *err, unsigned cap)
{
    static uint8_t scratch[TBL_BYTES];
    if (!s_have_stock) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    if (!build_intent()) { if (err) snprintf(err, cap, "Out of memory"); return 0; }
    const int used = pack(s_intent, s_intent_n, scratch, err, cap);
    if (!used) return 0;
    memcpy(s_packed, scratch, TBL_BYTES);
    s_packed_bytes = used;
    decode_effective(s_packed);
    s_gen++;
    return 1;
}

/* --- the disc ------------------------------------------------------------- */

static void read_equips(void)
{
    static uint8_t buf[EQUIP_SECTORS * SECTOR];
    s_equip_n = 0;
    s_equip_groups = 0;
    for (int s = 0; s < EQUIP_SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(EQUIP_LBA(0) + (uint32_t)s, buf + s * SECTOR)) return;
    for (int p = 0; p + 4 <= EQUIP_USED; ) {
        const int key = buf[p] | (buf[p + 1] << 8);
        if (key == 0) break;
        const int cnt = buf[p + 2] | (buf[p + 3] << 8);
        p += 4;
        if (cnt < 0 || p + 2 * cnt > EQUIP_USED) break;
        s_equip_groups++;
        for (int m = 0; m < cnt; m++) {
            const int mon = buf[p + m * 2] | (buf[p + m * 2 + 1] << 8);
            if (mon < 1 || mon > NCARDS) continue;
            if (!grow((void **)&s_equip, &s_equip_cap, s_equip_n + 1, sizeof(PsxFusionEquip))) return;
            s_equip[s_equip_n].equip = (uint16_t)key;
            s_equip[s_equip_n].mon = (uint16_t)mon;
            s_equip_n++;
        }
        p += 2 * cnt;
    }
}

int psx_fusion_table_refresh(void)
{
    static int stock_cap;
    for (int s = 0; s < FUSION_SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(FUSION_LBA(0) + (uint32_t)s, s_stock + s * SECTOR)) return 0;
    s_have_stock = 1;
    if (!decode_intended(s_stock, &s_stock_pairs, &s_stock_n, &stock_cap)) { s_have_stock = 0; return 0; }
    read_equips();
    return rebuild(NULL, 0);
}

int psx_fusion_table_ready(void)
{
    if (!s_have_stock) (void)psx_fusion_table_refresh();
    return s_have_stock;
}

/* --- queries -------------------------------------------------------------- */

const PsxFusionRecipe *psx_fusion_table_recipes(int *n)
{
    if (!psx_fusion_table_ready()) { if (n) *n = 0; return NULL; }
    if (n) *n = s_eff_n;
    return s_eff;
}

const PsxFusionEquip *psx_fusion_table_equips(int *n, int *groups)
{
    if (!psx_fusion_table_ready()) { if (n) *n = 0; if (groups) *groups = 0; return NULL; }
    if (n) *n = s_equip_n;
    if (groups) *groups = s_equip_groups;
    return s_equip;
}

int psx_fusion_table_result(int a, int b)
{
    if (!psx_fusion_table_ready()) return 0;
    order(&a, &b);
    /* the published list is what the game answers, so ask that */
    int lo = 0, hi = s_eff_n - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (s_eff[m].a != a) { if (s_eff[m].a < a) lo = m + 1; else hi = m - 1; continue; }
        if (s_eff[m].b == b) return s_eff[m].r;
        if (s_eff[m].b < b) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

int psx_fusion_table_stock_result(int a, int b)
{
    if (!psx_fusion_table_ready()) return 0;
    order(&a, &b);
    const int i = pair_find(s_stock_pairs, s_stock_n, a, b);
    return i < 0 ? 0 : s_stock_pairs[i].r;
}

int psx_fusion_table_is_edited(int a, int b)
{
    order(&a, &b);
    return pair_find(s_edit, s_edit_n, a, b) >= 0;
}

int psx_fusion_table_edit_count(void) { return s_edit_n; }
int psx_fusion_table_cleared(void) { return s_wipe; }
unsigned psx_fusion_table_generation(void) { return s_gen; }

void psx_fusion_table_budget(int *used, int *capacity, int *pairs)
{
    if (used)     *used = s_packed_bytes;
    if (capacity) *capacity = CAP_BYTES;
    if (pairs)    *pairs = s_intent_n;
}

/* --- editing -------------------------------------------------------------- */

static int edit_put(int a, int b, int r)
{
    const int i = pair_find(s_edit, s_edit_n, a, b);
    if (i >= 0) { s_edit[i].r = (uint16_t)r; return 1; }
    if (!grow((void **)&s_edit, &s_edit_cap, s_edit_n + 1, sizeof(Pair))) return 0;
    s_edit[s_edit_n].a = (uint16_t)a;
    s_edit[s_edit_n].b = (uint16_t)b;
    s_edit[s_edit_n].r = (uint16_t)r;
    s_edit_n++;
    qsort(s_edit, (size_t)s_edit_n, sizeof(Pair), pair_cmp);
    return 1;
}

static void edit_drop(int a, int b)
{
    const int i = pair_find(s_edit, s_edit_n, a, b);
    if (i < 0) return;
    memmove(&s_edit[i], &s_edit[i + 1], sizeof(Pair) * (size_t)(s_edit_n - i - 1));
    s_edit_n--;
}

/* Push the packed table at a duel that is already running, so an edit does not
 * wait for the next one. Word at a time: 16384 writes, once, on demand. */
static void poke_ram(void)
{
    if (!psx_fusion_db_ready()) return;
    for (int i = 0; i < TBL_BYTES; i += 4) {
        const uint32_t w = (uint32_t)s_packed[i] | ((uint32_t)s_packed[i + 1] << 8)
                         | ((uint32_t)s_packed[i + 2] << 16) | ((uint32_t)s_packed[i + 3] << 24);
        psx_mod_write_word(FUSION_RAM + (uint32_t)i, w);
    }
}

static void install_sectors(void)
{
    for (int k = 0; k < TERRAINS; k++)
        for (int s = 0; s < FUSION_SECTORS; s++)
            psx_mod_cd_override_set(FUSION_LBA(k) + (uint32_t)s, s_packed + s * SECTOR, SECTOR);
}

/* Re-push whatever is current, if the override is on. */
static void refresh_install(void)
{
    if (!s_applied) return;
    install_sectors();
    poke_ram();
}

int psx_fusion_table_edit(int a, int b, int result, char *err, unsigned cap)
{
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    order(&a, &b);
    if (a < 1 || a > NCARDS || b < 1 || b > NCARDS) { if (err) snprintf(err, cap, "Card ids run 1 to %d", NCARDS); return 0; }
    if (result < 0 || result > NCARDS) { if (err) snprintf(err, cap, "Result ids run 1 to %d, or 0 for no fusion", NCARDS); return 0; }

    /* remember enough to put it back if the repack refuses */
    const int had = pair_find(s_edit, s_edit_n, a, b);
    const int had_r = had >= 0 ? s_edit[had].r : 0;

    /* An edit that puts a pair back the way it was is not an edit -- unless the
     * table was emptied, where "the way it was" is "absent" and setting the
     * stock result is a real addition. */
    const int base = s_wipe ? 0 : psx_fusion_table_stock_result(a, b);
    if (result == base) edit_drop(a, b);
    else if (!edit_put(a, b, result)) { if (err) snprintf(err, cap, "Out of memory"); return 0; }

    if (!rebuild(err, cap)) {
        if (had >= 0) (void)edit_put(a, b, had_r); else edit_drop(a, b);
        (void)rebuild(NULL, 0);
        return 0;
    }
    refresh_install();
    (void)psx_fusion_table_save(NULL, 0);
    return 1;
}

int psx_fusion_table_clear_all(char *err, unsigned cap)
{
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    const int had = s_intent_n;
    s_wipe = 1;
    s_edit_n = 0;                       /* nothing left for an edit to modify */
    if (!rebuild(err, cap)) { s_wipe = 0; (void)rebuild(NULL, 0); return 0; }
    refresh_install();
    (void)psx_fusion_table_save(NULL, 0);
    psx_tool_log("Fusion table: emptied (%d recipes removed)", had);
    if (err) snprintf(err, cap, "All %d fusions deleted. Nothing combines until you add or import some " S_ARROW_TXT " Restore stock brings the game's table back",
                      had);
    return 1;
}

int psx_fusion_table_clear_card(int id, char *err, unsigned cap)
{
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    if (id < 1 || id > NCARDS) { if (err) snprintf(err, cap, "Card ids run 1 to %d", NCARDS); return 0; }
    /* Collect first: edit_put re-sorts s_edit, and s_intent is rebuilt from it. */
    int n = 0;
    for (int i = 0; i < s_intent_n; i++) if (s_intent[i].a == id || s_intent[i].b == id) n++;
    if (!n) { if (err) snprintf(err, cap, "That card has no fusions to delete"); return 1; }
    Pair *hit = (Pair *)malloc(sizeof(Pair) * (size_t)n);
    if (!hit) { if (err) snprintf(err, cap, "Out of memory"); return 0; }
    int k = 0;
    for (int i = 0; i < s_intent_n && k < n; i++)
        if (s_intent[i].a == id || s_intent[i].b == id) hit[k++] = s_intent[i];
    for (int i = 0; i < k; i++) {
        if (s_wipe) edit_drop(hit[i].a, hit[i].b);
        else        (void)edit_put(hit[i].a, hit[i].b, 0);
    }
    free(hit);
    if (!rebuild(err, cap)) return 0;
    refresh_install();
    (void)psx_fusion_table_save(NULL, 0);
    if (err) snprintf(err, cap, "Deleted %d fusion%s", k, k == 1 ? "" : "s");
    return 1;
}

void psx_fusion_table_reset(void)
{
    s_wipe = 0;
    s_edit_n = 0;
    (void)rebuild(NULL, 0);
    refresh_install();
    (void)psx_fusion_table_save(NULL, 0);
}

/* --- installing ----------------------------------------------------------- */

int psx_fusion_table_apply(char *err, unsigned cap)
{
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    if (!s_packed_bytes && !rebuild(err, cap)) return 0;
    install_sectors();
    poke_ram();
    s_applied = 1;
    psx_tool_log("Fusion table: %d edits installed, %d bytes of %d", s_edit_n, s_packed_bytes, CAP_BYTES);
    if (err) snprintf(err, cap, "Fusion edits are on: %d change%s, %d bytes of %d used",
                      s_edit_n, s_edit_n == 1 ? "" : "s", s_packed_bytes, CAP_BYTES);
    return 1;
}

void psx_fusion_table_revert(void)
{
    for (int k = 0; k < TERRAINS; k++)
        for (int s = 0; s < FUSION_SECTORS; s++)
            psx_mod_cd_override_clear(FUSION_LBA(k) + (uint32_t)s);
    s_applied = 0;
    /* a duel in progress keeps the edited copy until it reloads; put the stock
     * bytes back so "off" means off right now as well as next duel */
    if (psx_fusion_db_ready() && s_have_stock) {
        for (int i = 0; i < TBL_BYTES; i += 4) {
            const uint32_t w = (uint32_t)s_stock[i] | ((uint32_t)s_stock[i + 1] << 8)
                             | ((uint32_t)s_stock[i + 2] << 16) | ((uint32_t)s_stock[i + 3] << 24);
            psx_mod_write_word(FUSION_RAM + (uint32_t)i, w);
        }
    }
}

int psx_fusion_table_applied(void) { return s_applied; }

/* The row is the PLAYER'S preference and nothing else writes it. Installing
 * and removing the override used to move it in sympathy, which was wrong the
 * moment edits started applying on their own: taking the override out because
 * the last edit was deleted, or because Restore stock ran, would flip the row
 * to OFF and quietly block the NEXT edit from applying. Mechanism does not get
 * to edit the preference that drives it. */
/* NO MENU ROW. Editing a recipe IS the request to use it -- a switch in a
 * different menu that has to be found and thrown before your own change does
 * anything is indistinguishable, from the player's side, from the feature
 * being broken, and that is how two people read it. Restore stock and Delete
 * all live in the Fusion Manager, beside the editing, and are the way back. */
void psx_fusion_table_register_menu(void) { }

/* --- the file ------------------------------------------------------------- */

static void edits_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/fusion_edits.txt", dir);
    else               snprintf(out, cap, "fusion_edits.txt");
}

int psx_fusion_table_save(char *err, unsigned cap)
{
    char path[1200];
    edits_path(path, sizeof path);
    if (!s_edit_n && !s_wipe) { psx_remove_utf8(path); if (err) snprintf(err, cap, "No edits to save"); return 1; }
    return psx_fusion_table_export(path, 1, err, cap);
}

int psx_fusion_table_load(char *err, unsigned cap)
{
    char path[1200];
    edits_path(path, sizeof path);
    return psx_fusion_table_import(path, err, cap);
}

/* Card names would make this friendlier but also unparseable -- several of
 * them carry digits ("Winged Dragon #1") -- so the three ids come FIRST, as
 * their own fields, and the names ride along after them as a comment the
 * reader ignores. A plain `a,b,result` CSV (what TEA-Online's tool writes)
 * therefore imports unchanged. */
int psx_fusion_table_export(const char *path, int edits_only, char *err, unsigned cap)
{
    if (!path || !path[0]) { if (err) snprintf(err, cap, "No file name"); return 0; }
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) { if (err) snprintf(err, cap, "Cannot write %s", path); return 0; }
    fprintf(f, "# Yu-Gi-Oh! Forbidden Memories - fusion %s\n", edits_only ? "edits" : "table");
    fprintf(f, "# card1\tcard2\tresult   (ids 1..%d; result 0 removes the fusion)\n", NCARDS);
    if (edits_only) {
        fprintf(f, "# %d change%s to the game's own table\n", s_edit_n, s_edit_n == 1 ? "" : "s");
        if (s_wipe)
            fprintf(f, "#\n# 'clear' empties the table first: what follows is the whole of it.\nclear\n");
        fprintf(f, "\n");
        for (int i = 0; i < s_edit_n; i++)
            fprintf(f, "%d\t%d\t%d\n", s_edit[i].a, s_edit[i].b, s_edit[i].r);
    } else {
        fprintf(f, "# %d recipes, the whole table as the game will read it\n\n", s_intent_n);
        for (int i = 0; i < s_intent_n; i++)
            fprintf(f, "%d\t%d\t%d\n", s_intent[i].a, s_intent[i].b, s_intent[i].r);
    }
    const int bad = ferror(f);
    fclose(f);
    if (bad) { if (err) snprintf(err, cap, "Write failed part-way through %s", path); return 0; }
    if (err) snprintf(err, cap, "Wrote %d %s to %s", edits_only ? s_edit_n : s_intent_n,
                      edits_only ? "edits" : "recipes", path);
    return 1;
}

/* --- start-up -------------------------------------------------------------
 *
 * None of this can happen in the constructor: the disc is not mounted during
 * static init, and the menu row's stored value is replayed later still (the
 * on_change would fire against an unread table and silently do nothing). So
 * the first frame that can read the disc does the whole thing once. */

static void tick(void)
{
    static int done;
    char msg[256];
    if (!done) {
        if (!psx_mod_game_started()) return;
        if (!psx_fusion_table_refresh()) return;      /* no disc yet: try again */
        done = 1;
        if (psx_fusion_table_load(msg, sizeof msg) && s_edit_n)
            psx_tool_log("Fusion table: %s", msg);
    }

    /* RECONCILE every frame: the override is installed exactly when there is
     * something to install. Keeping "are there edits" and "are the sectors
     * in" as two pieces of state that someone remembers to move together is
     * what produced a table that needed toggling off and on to take. */
    if (s_have_stock) {
        /* Having edits is what puts them in the game -- nobody should have to
         * find a switch in another menu to make their own change take effect,
         * which is the one failure mode that looks exactly like the feature
         * being broken. The row only ever takes them back OUT. */
        const int want = s_edit_n > 0 || s_wipe;
        if (want && !s_applied)      (void)psx_fusion_table_apply(msg, sizeof msg);
        else if (!want && s_applied) psx_fusion_table_revert();
    }
}

PSX_MOD_CONSTRUCTOR(psx_fusion_table_install)
{
    psx_fusion_table_register_menu();
    (void)psx_game_add_frame_hook(tick);
}

/* Three integers per line, separated by anything that is not a digit or a
 * minus. A line that does not start with one is a comment. */
static int three_ints(const char *ln, int *a, int *b, int *c)
{
    const char *p = ln;
    int v[3], k = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p && k < 3) {
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; if (n > 99999) return 0; }
            v[k++] = n;
        } else p++;
    }
    if (k < 3) return 0;
    *a = v[0]; *b = v[1]; *c = v[2];
    return 1;
}

int psx_fusion_table_import(const char *path, char *err, unsigned cap)
{
    if (!path || !path[0]) { if (err) snprintf(err, cap, "No file name"); return 0; }
    if (!psx_fusion_table_ready()) { if (err) snprintf(err, cap, "The disc's fusion table has not been read"); return 0; }
    size_t len = 0;
    char *text = psx_read_text_utf8(path, &len, 8u << 20);
    if (!text) { if (err) snprintf(err, cap, "Cannot read %s", path); return 0; }

    /* keep what is there: a failed import must change nothing */
    Pair *keep = NULL;
    const int keep_n = s_edit_n;
    if (keep_n) {
        keep = (Pair *)malloc(sizeof(Pair) * (size_t)keep_n);
        if (!keep) { free(text); if (err) snprintf(err, cap, "Out of memory"); return 0; }
        memcpy(keep, s_edit, sizeof(Pair) * (size_t)keep_n);
    }

    const int had_wipe = s_wipe;
    int applied = 0, skipped = 0, lines = 0, cleared = 0;
    char *save = NULL;
    for (char *ln = text; ln && *ln; ) {
        char *nl = strpbrk(ln, "\r\n");
        if (nl) { *nl = 0; save = nl + 1; } else save = NULL;
        int a, b, r;
        {   /* a bare "clear" line empties the table before the rest is read */
            const char *q = ln;
            while (*q == ' ' || *q == '\t') q++;
            if (!strncmp(q, "clear", 5) && (q[5] == 0 || q[5] == ' ' || q[5] == '\t')) {
                s_wipe = 1;
                s_edit_n = 0;
                cleared = 1;
                ln = save;
                while (ln && (*ln == '\n' || *ln == '\r')) ln++;
                continue;
            }
        }
        if (three_ints(ln, &a, &b, &r)) {
            lines++;
            order(&a, &b);
            if (a < 1 || a > NCARDS || b < 1 || b > NCARDS || r < 0 || r > NCARDS) skipped++;
            else if (r == (s_wipe ? 0 : psx_fusion_table_stock_result(a, b))) { edit_drop(a, b); applied++; }
            else if (edit_put(a, b, r)) applied++;
            else skipped++;
        }
        ln = save;
        while (ln && (*ln == '\n' || *ln == '\r')) ln++;
    }
    free(text);

    char why[256] = "";
    if (!rebuild(why, sizeof why)) {
        s_edit_n = keep_n;
        s_wipe = had_wipe;
        if (keep_n) memcpy(s_edit, keep, sizeof(Pair) * (size_t)keep_n);
        free(keep);
        (void)rebuild(NULL, 0);
        if (err) snprintf(err, cap, "Nothing imported: %s", why);
        return 0;
    }
    free(keep);
    refresh_install();
    (void)psx_fusion_table_save(NULL, 0);
    if (err) {
        if (!lines && cleared) snprintf(err, cap, "Table emptied by %s; it holds no fusions at all", path);
        else if (!lines) snprintf(err, cap, "No recipes in %s: each line needs card1, card2 and the result as numbers", path);
        else if (skipped) snprintf(err, cap, "Imported %d of %d (%d out of range); %d edit%s in total",
                                   applied, lines, skipped, s_edit_n, s_edit_n == 1 ? "" : "s");
        else snprintf(err, cap, "Imported %d recipe%s; %d edit%s in total",
                      applied, applied == 1 ? "" : "s", s_edit_n, s_edit_n == 1 ? "" : "s");
    }
    return lines > 0 || cleared;
}

/* Everything back the way the disc has it. The edits are written to a backup
 * beside the live file first: this is one click in the title bar, and the
 * only thing standing between a misclick and a lost afternoon. */
int psx_fusion_table_restore_stock(char *err, unsigned cap)
{
    const int had = s_edit_n;
    char path[1200], backup[1200], why[256];
    int saved = 0;

    if (had) {
        const char *dir = psx_mod_player_data_dir();
        if (dir && dir[0]) snprintf(backup, sizeof backup, "%s/fusion_edits_backup.txt", dir);
        else               snprintf(backup, sizeof backup, "fusion_edits_backup.txt");
        saved = psx_fusion_table_export(backup, 1, why, sizeof why);
    }

    s_edit_n = 0;
    s_wipe = 0;
    (void)rebuild(NULL, 0);
    psx_fusion_table_revert();
    edits_path(path, sizeof path);
    psx_remove_utf8(path);
    psx_tool_log("Fusion table: restored stock, %d edit%s dropped%s",
                 had, had == 1 ? "" : "s", saved ? " (backed up)" : "");
    if (err) {
        if (!had)
            snprintf(err, cap, "Already the game's own table; the override is off");
        else if (saved)
            snprintf(err, cap, "Dropped %d edit%s and put the game's own table back - the old set is in fusion_edits_backup.txt, Import brings it back",
                     had, had == 1 ? "" : "s");
        else
            snprintf(err, cap, "Dropped %d edit%s and put the game's own table back (the backup could not be written)",
                     had, had == 1 ? "" : "s");
    }
    return 1;
}
