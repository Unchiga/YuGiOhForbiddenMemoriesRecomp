/* psx_dialogue.c -- the game's text bank, exported for translation and
 * served back at runtime. See psx_dialogue.h for the player view.
 *
 * WHERE THE TEXT IS
 * -----------------
 * Every textbox the game types (story dialogue, the duelists' lines, the
 * campaign choices, the memory-card and menu messages, the duel result
 * screens) is a string id looked up by TextBox_BuildStep (0x800393B0) through
 * Text_LookupString's three tables:
 *
 *     id >= 0xD000  gText_aBankOffsets[id - 0xD000]  from 0x801C0000  (card descriptions, ids 0xD100+card)
 *     id >= 0x8000  gText_aGlobalOffsets[id - 0x8000] from 0x801D0000  (card / duelist names)
 *     id <  0x8000  gText_aBankOffsets[id (- 0x100 when >= 0x500)] from gText_aBank = 0x801B0000
 *
 * So the dialogue is the 64 KB bank at 0x801B0000, addressed by the u16
 * table at 0x801C0000 (0x4FA entries: 0..0xFF general, 0x100..0x3D2 the
 * card descriptions with the OTHER base, 0x400..0x4F9 the campaign strings
 * the story script names by number). Descriptions stay with psx_card_packs.
 *
 * The bank is not a list of strings. It is 290 FF-terminated RUNS that the
 * control codes jump around in: a choice (FB 80 + targets) branches into the
 * middle of a run, FD goes to a shared tail, FC inserts a sub-string and
 * returns at its FF, and 123 of the 351 ids land mid-run (ids 199..203 are
 * five entry points into one run). Every target is a u16 bank offset, and
 * the bank is full: 0xFB4E of 0x10000 bytes are text, the rest is zero. A
 * translation cannot fit in the u16 world.
 *
 * HOW THE GAME READS IT
 * ---------------------
 * BuildStep keeps a small stack of 32-bit stream pointers at rec+0 indexed
 * by rec+0x58; the loop head at 0x80039658 loads the current pointer and
 * reads one byte (0x80039668 lw v1,0(a0) / 0x80039670 lbu v0,0(v1)). Glyph
 * bytes (< 0xF0) draw one per call; control bytes dispatch through the
 * table at 0x80090F18 and loop. Every jump handler sets that pointer to
 * 0x801B0000 | u16, and every operand read (func_80036D3C and friends) goes
 * through the same pointer. The pointer itself is 32-bit.
 *
 * THE MECHANISM
 * -------------
 * Translated runs live in host-backed guest memory (psx_mod_alloc_guest_
 * memory, Expansion 1, no 64 KB limit). Nothing in the text path is
 * patched: BuildStep has a function-entry hook in game.toml that
 * psx_card_drops relies on, and a function sent to the dirty-RAM
 * interpreter takes its callers with it and loses their hooks (measured:
 * with any of the text handlers patched the Free Duel grid stopped taking
 * X). So the redirect is made of two entry hooks the config already has:
 *
 *   1. BuildStep entry (0x800393B0): when the record's current stream
 *      pointer (slot rec+depth*4, depth at rec+0x58) is a 0x801Bxxxx
 *      address in the map, it is rewritten before the byte is read. The
 *      first call of a box only stores the looked-up pointer and returns,
 *      so the second call's entry catches every string start.
 *   2. Jumps happen INSIDE a call (the control-code loop runs until a
 *      glyph), so a translated copy's pointer operands do not name the
 *      stock target: they name a one-byte TRAMPOLINE in the bank's zero
 *      tail (0xFB4E..0xFF80; psx_card_shop parks its menu at 0xFF80). The
 *      trampoline is a space glyph: BuildStep draws one glyph per call, so
 *      the loop ends there, and the next entry finds the pointer at T+1,
 *      which the map sends to the translated target. The glyph emitter's
 *      entry hook (0x80036C14, also listed) makes that space invisible:
 *      the emitter already queues nothing for a space, and the hook zeroes
 *      the advance width rec+0x5A (restored at the next BuildStep entry)
 *      and un-counts the glyph (rec+0x60), so neither the pen nor the
 *      "typed N glyphs" bookkeeping moves. One frame per jump, hidden by
 *      the typewriter's own delay.
 *
 * Run starts and ANCHORS (every stock offset that an id or a jump lands on
 * inside a translated run) are what the map redirects, so a choice branch
 * into a translated run lands on the translated branch, an FC insert
 * returns into the translated caller, and untranslated runs are simply not
 * mapped. Trampolines are RAM data, re-asserted per frame against
 * savestates; a magic word at the arena's start says whether the guest
 * memory still holds the texts. The run holding the player-name slot
 * (bank+0x125A, written by the name-entry screen) is never relocated.
 *
 * THE FILE
 * --------
 * One block per run: "[@XXXX] ids ..." then the text, one file line per
 * game line (FE). Control codes are "{F8 0A 01}" hex groups; pointer
 * operands inside them are "@XXXX" (the stock offset they name); "{@XXXX}"
 * alone is an anchor; "{FE}" is a line break the block cannot end with
 * otherwise. Glyphs are the game's 92 characters as UTF-8. Encoding a
 * decoded stock run gives the stock bytes back exactly (checked at boot). */

#include "psx_textfile.h"
#include "psx_dialogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"

/* ---- the game side ------------------------------------------------------------ */
#define BANK_BASE    0x801B0000u
#define BANK_SIZE    0x10000u
#define TABLE_ADDR   0x801C0000u
#define TABLE_N      0x4FA               /* the descriptions start right after, at 0x801C09F4 */
#define DESC_FIRST   0x100               /* table indices that are card descriptions (other base) */
#define DESC_LAST    0x3D2

/* Entry hooks the game config lists (no regeneration needed). */
#define BUILDSTEP_ADDR 0x800393B0u
#define EMITTER_ADDR   0x80036C14u
/* trampolines: one byte per jump target, in the bank's zero tail */
#define TRAMP_END      0xFF80u           /* psx_card_shop's menu stream starts here */
#define NAME_SLOT_OFF  0x125Au           /* the name-entry screen writes the player's name here */

#define ARENA_BYTES  0x60000u            /* 384 KB for translated runs (stock text is 64 KB): six 64 KB segments */
#define SEG_BYTES    0x10000u
#define SEG_TRAMP    0x200u              /* the top of each segment holds its jump trampolines */
#define MAX_JUMPS    1024
#define ARENA_MAGIC  0x59474F44u         /* "YGOD" */
#define ARENA_HEAD   16u

#define MAX_RUNS     512
#define MAX_ANCH     192
#define MAX_IDS      64
#define MAX_PTRS     256

/* ---- the character set --------------------------------------------------------
 * gText_adwGlyphCodeTable, frequency ordered; the UTF-8 spellings are the
 * Shift-JIS glyphs the font draws (see teatools/hextext). */
static const char *const CHARSET[0x5C] = {
    " ", "e", "t", "a", "o", "i", "n", "s", "r", "h", "l", ".", "d", "u", "m", "c",
    "g", "y", "w", "f", "p", "b", "k", "!", "A", "v", "I", "'", "T", "S", "M", ",",
    "D", "O", "W", "H", "Y", "E", "R", "\xC2\xAB", "\xC2\xBB", "G", "L", "C", "N", "B", "?", "P",
    "-", "F", "z", "K", "j", "U", "x", "q", "0", "V", "2", "J", "#", "1", "Q", "Z",
    "\"", "3", "5", "&", "/", "7", "X", "\xC2\xB7", ":", "\xE2\x99\x80", "4", ")", "(", "\xE2\x99\x82", "6", "$",
    "*", ">", "\xE2\x8A\x82", "\xE2\x8A\x83", "<", "\xCE\xB1", "+", "8", "\xE2\x86\x90", "9", "\xE2\x86\x92", "%"
};
/* Spellings a translator's editor is likely to produce for glyphs the font
 * has: curly quotes, dashes, square brackets for the chevrons. */
static const struct { const char *s; int code; } ALIASES[] = {
    { "\xE2\x80\x99", 0x1B }, { "\xE2\x80\x98", 0x1B },            /* ’ ‘ */
    { "\xE2\x80\x9C", 0x40 }, { "\xE2\x80\x9D", 0x40 },            /* “ ” */
    { "\xE2\x80\x93", 0x30 }, { "\xE2\x80\x94", 0x30 },            /* – — */
    { "[", 0x27 }, { "]", 0x28 },
};

const char *psx_dialogue_glyph(int c) { return (c >= 0 && c < 0x5C) ? CHARSET[c] : NULL; }

int psx_dialogue_encode_char(const char *s, int *len)
{
    int best = -1, bl = 0;
    for (int i = 0; i < 0x5C; i++) {
        const int n = (int)strlen(CHARSET[i]);
        if (n > bl && !strncmp(s, CHARSET[i], (size_t)n)) { best = i; bl = n; }
    }
    if (best < 0) {
        for (unsigned i = 0; i < sizeof ALIASES / sizeof ALIASES[0]; i++) {
            const int n = (int)strlen(ALIASES[i].s);
            if (!strncmp(s, ALIASES[i].s, (size_t)n)) { best = ALIASES[i].code; bl = n; break; }
        }
    }
    if (len) *len = bl;
    return best;
}

/* ---- state ---------------------------------------------------------------------- */
typedef struct { uint16_t off; int is_id; int id; } Anchor;   /* a stock offset inside the run that something lands on */

typedef struct {
    uint32_t start, end;              /* stock bank offsets, end is after the FF */
    int      ids[MAX_IDS]; int nids;  /* every id whose lookup lands in the run */
    Anchor   anch[MAX_ANCH]; int nanch;   /* sorted by off; the run start is not listed */
    char    *stock_text;
    /* translation, NULL/0 when the run is stock */
    uint8_t *enc; int enc_len;        /* the bytes the game reads, FF included */
    int      enc_pos[MAX_ANCH];       /* byte position of each anchor in enc, -1 = missing */
    int      nptr;                    /* pointer operands in enc: position and stock target */
    int      ptr_pos[MAX_PTRS]; uint16_t ptr_val[MAX_PTRS];
    int      locked;                  /* holds runtime-written bytes: never relocated */
    char    *cur_text;
    char    *plain_stock, *plain_cur; /* the readable forms, see "plain text" below */
    uint32_t reloc;                   /* guest address of the copy */
} Run;

static uint8_t   s_bank[BANK_SIZE];
static uint16_t  s_table[TABLE_N];
static uint32_t  s_text_end;          /* first byte past the last run */
static Run       s_runs[MAX_RUNS];
static int       s_nruns;
static int       s_ready;
static unsigned  s_generation;
static char      s_dir[1024], s_file[1200];
static int       s_dir_ok;

/* guest side */
static uint32_t  s_map[0x10000];      /* low 16 bits of a bank address -> guest address of its copy, 0 = none */
/* The bank as it should be while translations are live: the same model
 * TEA's editor uses, everything stays inside the game's own 64 KB text
 * bank and every pointer is shifted when a text grows. The menu texts and
 * the player-name slot before the first story text keep their places; the
 * story texts are laid out again from there. */
static uint8_t   s_image[BANK_SIZE];
static uint16_t  s_table_new[TABLE_N];
static uint16_t  s_newoff[0x10000];   /* stock bank offset of a run start / anchor -> where it is now (0 = none) */
static uint32_t  s_rebuilt_lo, s_rebuilt_hi;   /* the rewritten range of the bank */
static uint32_t  s_used, s_room;      /* bytes the story texts take, and the most they may */
static int       s_active;            /* a rebuilt bank is wanted */
static int       s_patched;           /* it is in RAM */
static uint32_t  s_width_rec, s_width_val; /* the record whose advance width is held at 0, and its value */

static void bump(void) { s_generation++; }

/* ---- control-code grammar ---------------------------------------------------------
 * Byte length of the control code at b[i] (i < n), and the offsets within
 * it (relative to i) of its u16 pointer operands. From the handler tables
 * (teatools/hextext README section 3): the shapes are what the EXE consumes;
 * F8 17/18 lists have no encoded count, so the run of plausible offsets is
 * taken, stopping at the first forward target. `choices` carries the count an
 * FB 02 set for the next FB 80 list. */
static int ctl_len(const uint8_t *b, int i, int n, int *choices, int *ptr_at, int *nptr, int bank_off)
{
    const uint8_t c = b[i];
    int np = 0;
    int len = 1;
#define PTR(o) do { if (np < 16) ptr_at[np++] = (o); } while (0)
    if (c < 0xF0) len = 1;
    else if (c <= 0xF5) len = 2;
    else if (c == 0xF6) len = 3;
    else if (c == 0xF7) {
        const int sub = i + 1 < n ? b[i + 1] : 0;
        if (sub == 0x05)      len = 4 + ((i + 3 < n && (b[i + 3] & 0x80)) ? 3 : 0);
        else if (sub == 0x0F) len = 4;
        else if (sub == 0x0A) len = 2;
        else if (sub == 0x0B) len = 8;
        else if (sub == 0x10) len = 4;
        else len = 2;
    } else if (c == 0xF8) {
        static const signed char SZ[0x20] = { 3,3,3,7,3,2,4,2, 2,2,3,3,2,9,4,2, 4,3,2,2,2,3,2,2, 2,3,2,2,2,2,2,2 };
        const int sub = i + 1 < n ? b[i + 1] : 0;
        if (sub == 0x10) len = 4 + ((i + 3 < n && (b[i + 3] & 0x80)) ? 2 : 0);
        else if (sub == 0x17 || sub == 0x18) {
            int j = i + 2, minfwd = -1;
            const int here = bank_off + i;
            while (j + 1 < n) {
                const int v = b[j] | (b[j + 1] << 8);
                if (v < here - 0x400 || v > here + 0x400) break;
                if (minfwd >= 0 && bank_off + j >= minfwd) break;
                PTR(j - i); j += 2;
                if (v > here && (minfwd < 0 || v < minfwd)) minfwd = v;
            }
            len = j - i;
        } else if (sub == 0x0D) { len = SZ[sub]; PTR(6); }      /* the duel: its intro line's bank offset */
        else if (sub < 0x20) len = SZ[sub];
        else len = 2;
    } else if (c == 0xF9) {
        const int v = i + 2 < n ? (b[i + 1] | (b[i + 2] << 8)) : 0;
        if (v & 0x4000) len = 3; else { len = 5; PTR(3); }
    } else if (c == 0xFA) len = 1;
    else if (c == 0xFB) {
        const int f = i + 1 < n ? b[i + 1] : 0;
        len = 2 + ((f & 8) ? 1 : 0);
        if (f & 0x80) {
            for (int k = 0; k < *choices; k++) { PTR(len); len += 2; }
        } else if (f < 8 && (f & 7) >= 1 && (f & 7) <= 3) *choices = f & 7;
    } else if (c == 0xFC || c == 0xFD) { len = 3; PTR(1); }
    else if (c == 0xFE) len = 1;
    else len = 1;   /* FF */
#undef PTR
    if (i + len > n) len = n - i;
    *nptr = np;
    return len;
}

/* ---- the bank ------------------------------------------------------------------- */
static int cmp_anchor(const void *a, const void *b)
{
    return (int)((const Anchor *)a)->off - (int)((const Anchor *)b)->off;
}

static Run *run_of(uint32_t off)
{
    for (int i = 0; i < s_nruns; i++)
        if (off >= s_runs[i].start && off < s_runs[i].end) return &s_runs[i];
    return NULL;
}

static void add_anchor(Run *r, uint32_t off, int is_id, int id)
{
    if (off == r->start) return;
    for (int k = 0; k < r->nanch; k++)
        if (r->anch[k].off == off) { if (is_id && !r->anch[k].is_id) { r->anch[k].is_id = 1; r->anch[k].id = id; } return; }
    if (r->nanch < MAX_ANCH) { r->anch[r->nanch].off = (uint16_t)off; r->anch[r->nanch].is_id = is_id; r->anch[r->nanch].id = id; r->nanch++; }
}

/* Partition the bank into runs, then find every offset something lands on. */
static void build_runs(void)
{
    /* The stock text is dense; it ends where the zero tail begins (64 zero
     * bytes in a row). Other mods park their own streams in that tail --
     * psx_card_shop's four-line shopkeeper menu at +0xFF80 -- and those are
     * not texts to translate. */
    s_text_end = BANK_SIZE;
    for (uint32_t i = 0x100; i + 64 <= BANK_SIZE; i++) {
        int zeros = 1;
        for (uint32_t k = 0; k < 64; k++) if (s_bank[i + k]) { zeros = 0; break; }
        if (zeros) { s_text_end = i; break; }
    }
    while (s_text_end > 0 && s_bank[s_text_end - 1] != 0xFF) s_text_end--;
    /* An FF ends a run only at an instruction boundary: "F8 06 F8 FF" is a
     * signed left padding of -8, not an end. So the split walks the control
     * codes the way the typewriter does. */
    s_nruns = 0;
    {
        uint32_t s = 0, i = 0;
        int choices = 2;
        while (i < s_text_end && s_nruns < MAX_RUNS) {
            if (s_bank[i] == 0xFF) {
                Run *r = &s_runs[s_nruns++];
                memset(r, 0, sizeof *r);
                r->start = s; r->end = i + 1;
                s = i + 1; i = s; choices = 2;
                continue;
            }
            int ptr_at[16], np = 0;
            const int len = ctl_len(s_bank + s, (int)(i - s), (int)(s_text_end - s), &choices, ptr_at, &np, (int)s);
            i += (uint32_t)(len > 0 ? len : 1);
        }
    }
    /* ids: table entries that are not descriptions and not the unused 0 */
    for (int id = 0; id < TABLE_N; id++) {
        if (id >= DESC_FIRST && id <= DESC_LAST) continue;
        const uint32_t off = s_table[id];
        if (!off || off >= s_text_end) continue;
        Run *r = run_of(off);
        if (!r) continue;
        if (r->nids < MAX_IDS) r->ids[r->nids++] = id;
        add_anchor(r, off, 1, id);
    }
    /* jump targets */
    for (int i = 0; i < s_nruns; i++) {
        const Run *r = &s_runs[i];
        const uint8_t *b = s_bank + r->start;
        const int n = (int)(r->end - r->start);
        int choices = 2;
        for (int p = 0; p < n;) {
            int ptr_at[16], np = 0;
            const int len = ctl_len(b, p, n, &choices, ptr_at, &np, (int)r->start);
            for (int k = 0; k < np; k++) {
                const int o = p + ptr_at[k];
                if (o + 1 >= n) continue;
                const uint32_t v = (uint32_t)(b[o] | (b[o + 1] << 8));
                if (!v || v >= s_text_end) continue;
                Run *t = run_of(v);
                if (t) add_anchor(t, v, 0, 0);
            }
            if (b[p] == 0xFF) break;
            p += len > 0 ? len : 1;
        }
    }
    for (int i = 0; i < s_nruns; i++)
        qsort(s_runs[i].anch, (size_t)s_runs[i].nanch, sizeof(Anchor), cmp_anchor);
    { Run *r = run_of(NAME_SLOT_OFF); if (r) r->locked = 1; }
}

/* ---- decode: bytes -> file text -----------------------------------------------------
 * anchors: (offset-in-bytes, stock offset name) pairs to place, sorted by
 * position. Returns a malloc'd UTF-8 string. */
typedef struct { int pos; uint16_t name; int is_ptr; } Mark;   /* an anchor, or (is_ptr) a pointer operand */

static void cat(char **out, size_t *n, size_t *cap, const char *s)
{
    const size_t l = strlen(s);
    if (*n + l + 1 > *cap) { *cap = (*cap + l + 1) * 2; *out = (char *)realloc(*out, *cap); }
    memcpy(*out + *n, s, l + 1); *n += l;
}

static char *decode_bytes(const uint8_t *b, int n, const Mark *marks, int nmarks, int bank_off)
{
    /* only anchors are placed; pointer marks are skipped */
    size_t cap = (size_t)n * 4 + 64, len = 0;
    char *out = (char *)malloc(cap);
    out[0] = 0;
    int choices = 2, m = 0;
    char tmp[64];
    /* trailing FE run: those cannot be file newlines (a block's trailing
     * blank lines are dropped), so they are written as {FE} */
    int tail_fe = n;    /* index of the first FE in the trailing run */
    while (tail_fe > 0 && b[tail_fe - 1] == 0xFF) tail_fe--;
    { int t = tail_fe; while (t > 0 && b[t - 1] == 0xFE) t--; tail_fe = t; }
    for (int p = 0; p < n;) {
        while (m < nmarks && marks[m].pos <= p) {
            if (marks[m].pos == p && !marks[m].is_ptr) { snprintf(tmp, sizeof tmp, "{@%04X}", marks[m].name); cat(&out, &len, &cap, tmp); }
            m++;
        }
        const uint8_t c = b[p];
        if (c == 0xFF) break;
        if (c < 0x5C) { cat(&out, &len, &cap, CHARSET[c]); p++; continue; }
        if (c < 0xF0) { snprintf(tmp, sizeof tmp, "{%02X}", c); cat(&out, &len, &cap, tmp); p++; continue; }
        if (c == 0xFE) { cat(&out, &len, &cap, p >= tail_fe ? "{FE}" : "\n"); p++; continue; }
        int ptr_at[16], np = 0;
        int l = ctl_len(b, p, n, &choices, ptr_at, &np, bank_off);
        if (l < 1) l = 1;
        /* an anchor inside the group means the grouping was wrong there:
         * cut the group at the anchor and re-sync */
        for (int k = m; k < nmarks && marks[k].pos < p + l; k++)
            if (marks[k].pos > p && !marks[k].is_ptr) { l = marks[k].pos - p; np = 0; break; }
        cat(&out, &len, &cap, "{");
        for (int k = 0; k < l;) {
            int is_ptr = 0;
            for (int q = 0; q < np; q++) if (ptr_at[q] == k) is_ptr = 1;
            if (k) cat(&out, &len, &cap, " ");
            if (is_ptr && k + 1 < l) { snprintf(tmp, sizeof tmp, "@%04X", b[p + k] | (b[p + k + 1] << 8)); cat(&out, &len, &cap, tmp); k += 2; }
            else { snprintf(tmp, sizeof tmp, "%02X", b[p + k]); cat(&out, &len, &cap, tmp); k++; }
        }
        cat(&out, &len, &cap, "}");
        p += l;
    }
    return out;
}

static char *decode_stock_run(const Run *r)
{
    Mark marks[MAX_ANCH];
    for (int k = 0; k < r->nanch; k++) { marks[k].pos = (int)(r->anch[k].off - r->start); marks[k].name = r->anch[k].off; marks[k].is_ptr = 0; }
    return decode_bytes(s_bank + r->start, (int)(r->end - r->start), marks, r->nanch, (int)r->start);
}

/* ---- encode: file text -> bytes -----------------------------------------------------
 * Fills enc (FF appended) and the position of every anchor named in the
 * text. Returns the length, or -1 with the problem in err. */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int encode_text(const char *text, uint8_t *enc, int cap, Mark *marks, int *nmarks, int maxmarks,
                       char *err, unsigned errcap, int *longest_line)
{
    int n = 0, line = 1, col = 0, longest = 0;
    *nmarks = 0;
    for (const char *p = text; *p;) {
        if (n + 4 >= cap) { snprintf(err, errcap, "line %d: the text is too long", line); return -1; }
        if (*p == '\n') { enc[n++] = 0xFE; p++; line++; if (col > longest) longest = col; col = 0; continue; }
        if (*p == '\r') { p++; continue; }
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) { snprintf(err, errcap, "line %d: '{' without a closing '}'", line); return -1; }
            const char *q = p + 1;
            while (q < e && *q == ' ') q++;
            /* "{@XXXX}" alone is an anchor: a place, not bytes */
            if (q < e && *q == '@') {
                const char *z = q + 1; int v = 0, d = 0;
                while (z < e && hexval(*z) >= 0) { v = v * 16 + hexval(*z); z++; d++; }
                const char *t = z; while (t < e && *t == ' ') t++;
                if (t == e) {
                    if (d < 1 || d > 4) { snprintf(err, errcap, "line %d: bad {@XXXX} marker", line); return -1; }
                    if (*nmarks < maxmarks) { marks[*nmarks].pos = n; marks[*nmarks].name = (uint16_t)v; marks[*nmarks].is_ptr = 0; (*nmarks)++; }
                    p = e + 1;
                    continue;
                }
            }
            int tokens = 0;
            while (q < e) {
                while (q < e && *q == ' ') q++;
                if (q >= e) break;
                if (n + 3 >= cap) { snprintf(err, errcap, "line %d: the text is too long", line); return -1; }
                if (*q == '@') {
                    int v = 0, d = 0;
                    q++;
                    while (q < e && hexval(*q) >= 0) { v = v * 16 + hexval(*q); q++; d++; }
                    if (d < 1 || d > 4) { snprintf(err, errcap, "line %d: bad @XXXX pointer in {...}", line); return -1; }
                    if (*nmarks < maxmarks) { marks[*nmarks].pos = n; marks[*nmarks].name = (uint16_t)v; marks[*nmarks].is_ptr = 1; (*nmarks)++; }
                    enc[n++] = (uint8_t)(v & 0xFF); enc[n++] = (uint8_t)(v >> 8);
                } else {
                    const int h = hexval(q[0]), l = q + 1 < e ? hexval(q[1]) : -1;
                    if (h < 0 || l < 0) { snprintf(err, errcap, "line %d: {...} must hold hex bytes or @XXXX pointers", line); return -1; }
                    const int byte = h * 16 + l;
                    /* a bare {FF} would end the text early; as an operand (F8 06 F8 FF = pad -8) it is data */
                    if (byte == 0xFF && !tokens && q + 2 >= e) { snprintf(err, errcap, "line %d: {FF} ends a text; a block ends by itself", line); return -1; }
                    enc[n++] = (uint8_t)byte;
                    if (byte == 0xFE || byte == 0xFA) { if (col > longest) longest = col; col = 0; }
                    q += 2;
                }
                if (q < e && *q != ' ') { snprintf(err, errcap, "line %d: {...} tokens must be separated by spaces", line); return -1; }
                tokens++;
            }
            if (!tokens) { snprintf(err, errcap, "line %d: empty {}", line); return -1; }
            p = e + 1;
            continue;
        }
        if (*p == '}') { snprintf(err, errcap, "line %d: '}' without a '{'", line); return -1; }
        int cl = 0;
        const int code = psx_dialogue_encode_char(p, &cl);
        if (code < 0) {
            /* show the offending character */
            char ch[8] = { 0 }; int k = 0;
            ch[k++] = p[0];
            while (k < 4 && (p[k] & 0xC0) == 0x80) { ch[k] = p[k]; k++; }
            snprintf(err, errcap, "line %d: the game's font has no '%s'", line, ch);
            return -1;
        }
        enc[n++] = (uint8_t)code; col++;
        p += cl;
    }
    if (col > longest) longest = col;
    enc[n++] = 0xFF;
    if (longest_line) *longest_line = longest;
    return n;
}

/* ---- plain text: what translators see ---------------------------------------------
 * The raw form above is byte-exact but unreadable. The plain form hides the
 * control codes behind small numbered markers: "{1}", "{2}"... in the order
 * they occur in the ORIGINAL text ("{name}" for the player's name), a page
 * break is a blank line and a line break a line break. Importing maps the
 * markers back to the original codes, then wraps what does not fit the box:
 * a line past PSX_DIALOGUE_COLS breaks at a space, a page past
 * PSX_DIALOGUE_LINES lines is split into more pages. */
#define NAME_INSERT "{FC @125A}"

/* the first campaign text's offset: menu and duel labels sit before it */
static uint32_t story_first(void)
{
    static uint32_t first;
    if (first) return first;
    first = 0xFFFFu;
    for (int i = 0; i < s_nruns; i++)
        for (int k = 0; k < s_runs[i].nids; k++)
            if (s_runs[i].ids[k] >= 0x400 && s_runs[i].start < first) { first = s_runs[i].start; break; }
    return first;
}
static int run_is_story(const Run *r)
{
    for (int k = 0; k < r->nids; k++) if (r->ids[k] >= 0x400) return 1;
    if (r->nids == 0) return r->start >= story_first();   /* reached by jumps: story when it sits among the story texts */
    return 0;
}

/* the "{...}" groups of a raw text, page breaks left out */
static int raw_codes(const char *raw, char ***out)
{
    int n = 0, cap = 0;
    char **codes = NULL;
    for (const char *p = raw; *p;) {
        if (*p != '{') { p++; continue; }
        const char *e = strchr(p, '}');
        if (!e) break;
        const size_t l = (size_t)(e - p) + 1;
        if (!(l == 4 && !strncmp(p, "{FA}", 4))) {
            if (n == cap) { cap = cap ? cap * 2 : 16; codes = (char **)realloc(codes, sizeof(char *) * (size_t)cap); }
            codes[n] = (char *)malloc(l + 1); memcpy(codes[n], p, l); codes[n][l] = 0; n++;
        }
        p = e + 1;
    }
    *out = codes;
    return n;
}
static void free_codes(char **codes, int n) { for (int i = 0; i < n; i++) free(codes[i]); free(codes); }

static char *plain_from_raw(const char *raw)
{
    size_t cap = strlen(raw) + 64, len = 0;
    char *out = (char *)malloc(cap);
    out[0] = 0;
    int k = 0;
    char tmp[16];
    for (const char *p = raw; *p;) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) { cat(&out, &len, &cap, p); break; }
            const size_t l = (size_t)(e - p) + 1;
            if (l == 4 && !strncmp(p, "{FA}", 4)) cat(&out, &len, &cap, "\n\n");
            else {
                k++;
                if (l == strlen(NAME_INSERT) && !strncmp(p, NAME_INSERT, l)) cat(&out, &len, &cap, "{name}");
                else { snprintf(tmp, sizeof tmp, "{%d}", k); cat(&out, &len, &cap, tmp); }
            }
            p = e + 1;
            continue;
        }
        const char one[2] = { *p, 0 };
        cat(&out, &len, &cap, one);
        p++;
    }
    return out;
}

/* one visible character or one "{marker}" of a plain line */
static size_t plain_token(const char *p, int *width)
{
    if (*p == '{') { const char *e = strchr(p, '}'); *width = 0; return e ? (size_t)(e - p) + 1 : 1; }
    int cl = 0;
    const int code = psx_dialogue_encode_char(p, &cl);
    *width = 1;
    if (code >= 0 && cl > 0) return (size_t)cl;
    /* an unknown character: step over the whole UTF-8 sequence, encode_text will name it */
    size_t l = 1; while ((p[l] & 0xC0) == 0x80) l++;
    return l;
}

/* Wrap one line into `lines` (each a malloc'd string); returns how many. */
static int wrap_line(const char *line, char **lines, int max)
{
    int n = 0;
    const char *p = line;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p, *last_space = NULL; int w = 0;
        while (*q) {
            int tw = 0; const size_t tl = plain_token(q, &tw);
            if (w + tw > PSX_DIALOGUE_COLS) break;
            if (*q == ' ') last_space = q;
            w += tw; q += tl;
        }
        const char *end = q;
        if (*q) {
            /* the line runs on: break at the last space that fits, else mid-word */
            if (last_space && last_space > p) end = last_space;
            /* markers right after the break stay with the text before it */
            while (*end == '{') { const char *e = strchr(end, '}'); if (!e) break; end = e + 1; }
        } else end = q;
        size_t l = (size_t)(end - p);
        while (l && p[l - 1] == ' ') l--;
        lines[n] = (char *)malloc(l + 1); memcpy(lines[n], p, l); lines[n][l] = 0; n++;
        p = end;
    }
    if (!n) { lines[0] = strdup(""); n = 1; }
    return n;
}

/* plain -> raw for one run; NULL with the reason in err */
static char *raw_from_plain(const Run *r, const char *plain, char *err, unsigned errcap)
{
    char **codes = NULL;
    const int ncodes = raw_codes(r->stock_text, &codes);
    /* pages of lines */
    enum { MAXL = 256 };
    char *lines[MAXL]; int page_of[MAXL]; int nl = 0, page = 0, blank = 1;
    for (const char *p = plain; *p;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char buf[1024];
        if (l >= sizeof buf) l = sizeof buf - 1;
        memcpy(buf, p, l); buf[l] = 0;
        if (l && buf[l - 1] == '\r') buf[--l] = 0;
        p += (e ? (size_t)(e - p) + 1 : strlen(p));
        if (!l) { if (!blank) { page++; blank = 1; } continue; }
        blank = 0;
        char *w[16];
        const int k = wrap_line(buf, w, 16);
        for (int i = 0; i < k; i++) { if (nl < MAXL) { lines[nl] = w[i]; page_of[nl] = page; nl++; } else free(w[i]); }
    }
    /* pages of plain words that do not fit are split; a page holding any
     * game code (a choice list, a jump) is left whole: the game lays those
     * out itself and a wait in the middle of a choice breaks it */
    int page_has_code[MAXL]; memset(page_has_code, 0, sizeof page_has_code);
    for (int i = 0; i < nl; i++) if (strchr(lines[i], '{') && page_of[i] < MAXL) page_has_code[page_of[i]] = 1;
    size_t cap = strlen(plain) * 2 + 256, len = 0;
    char *out = (char *)malloc(cap); out[0] = 0;
    int cur_page = -1, in_page = 0, mark = 0;
    for (int i = 0; i < nl; i++) {
        if (page_of[i] != cur_page || (in_page >= PSX_DIALOGUE_LINES && !page_has_code[page_of[i]])) {
            if (cur_page >= 0) cat(&out, &len, &cap, "{FA}");
            cur_page = page_of[i]; in_page = 0;
        } else cat(&out, &len, &cap, "\n");
        in_page++;
        /* markers back to codes */
        for (const char *q = lines[i]; *q;) {
            if (*q == '{') {
                const char *e = strchr(q, '}');
                if (!e) { snprintf(err, errcap, "'{' without a closing '}'"); goto fail; }
                if (!strncmp(q, "{name}", 6)) { cat(&out, &len, &cap, NAME_INSERT); q = e + 1; continue; }
                char *z = NULL; const long v = strtol(q + 1, &z, 10);
                if (z != e || v < 1 || v > ncodes) { char m[96]; snprintf(m, sizeof m, "unknown marker %.*s (this text has {1}..{%d})", (int)(e - q + 1), q, ncodes); snprintf(err, errcap, "%s", m); goto fail; }
                cat(&out, &len, &cap, codes[v - 1]);
                q = e + 1; mark++;
                continue;
            }
            const char one[2] = { *q, 0 };
            cat(&out, &len, &cap, one);
            q++;
        }
    }
    for (int i = 0; i < nl; i++) free(lines[i]);
    free_codes(codes, ncodes);
    return out;
fail:
    for (int i = 0; i < nl; i++) free(lines[i]);
    free_codes(codes, ncodes);
    free(out);
    return NULL;
}

/* ---- the rebuilt bank ----------------------------------------------------------------- */
/* Rewrite the pointer operands of one run's bytes (already at `dst`) through s_newoff. */
static void fix_pointers(uint8_t *dst, int n, int bank_off)
{
    int choices = 2;
    for (int p = 0; p < n;) {
        const uint8_t c = dst[p];
        if (c == 0xFF) break;
        if (c < 0xF0 || c == 0xFE || c == 0xFA) { p++; continue; }
        int ptr_at[16], np = 0;
        const int l = ctl_len(dst, p, n, &choices, ptr_at, &np, bank_off + p);
        for (int q = 0; q < np; q++) {
            const int at = p + ptr_at[q];
            if (at + 1 >= n) continue;
            const uint16_t old = (uint16_t)(dst[at] | (dst[at + 1] << 8));
            const uint16_t now = s_newoff[old];
            if (now) { dst[at] = (uint8_t)(now & 0xFF); dst[at + 1] = (uint8_t)(now >> 8); }
        }
        p += l < 1 ? 1 : l;
    }
}

/* Lay the story texts out again in the bank, translations in place of
 * their originals, and shift the string table and every pointer. Returns 1,
 * or 0 with the reason (the texts do not fit). */
static int rebuild_bank(char *err, unsigned errcap)
{
    int any = 0;
    for (int i = 0; i < s_nruns; i++) if (s_runs[i].enc) { any = 1; break; }
    memcpy(s_image, s_bank, BANK_SIZE);
    memcpy(s_table_new, s_table, sizeof s_table_new);
    memset(s_newoff, 0, sizeof s_newoff);
    const uint32_t first = story_first();
    s_rebuilt_lo = first; s_rebuilt_hi = first;
    s_room = TRAMP_END - first;
    s_used = 0;
    if (!any) { s_active = 0; return 1; }
    /* pass 1: where everything goes */
    uint32_t cursor = first;
    for (int i = 0; i < s_nruns; i++) {
        Run *r = &s_runs[i];
        if (r->start < first) {
            s_newoff[r->start] = (uint16_t)r->start;
            for (int k = 0; k < r->nanch; k++) s_newoff[r->anch[k].off] = r->anch[k].off;
            continue;
        }
        const uint32_t len = r->enc ? (uint32_t)r->enc_len : r->end - r->start;
        r->reloc = cursor;
        s_newoff[r->start] = (uint16_t)cursor;
        for (int k = 0; k < r->nanch; k++) {
            if (r->enc) s_newoff[r->anch[k].off] = (uint16_t)(r->enc_pos[k] >= 0 ? cursor + (uint32_t)r->enc_pos[k] : cursor);
            else s_newoff[r->anch[k].off] = (uint16_t)(cursor + (r->anch[k].off - r->start));
        }
        cursor += len;
    }
    s_used = cursor - first;
    if (cursor > TRAMP_END) {
        snprintf(err, errcap, "the texts are %u bytes too long for the game's text bank (it holds %u bytes of story text; shorten some texts)", cursor - TRAMP_END, s_room);
        s_active = 0;
        return 0;
    }
    /* pass 2: the bytes, pointers shifted */
    uint8_t *tmp = (uint8_t *)calloc(BANK_SIZE, 1);
    for (int i = 0; i < s_nruns; i++) {
        Run *r = &s_runs[i];
        if (r->start < first) {
            fix_pointers(s_image + r->start, (int)(r->end - r->start), (int)r->start);
            continue;
        }
        const uint32_t len = r->enc ? (uint32_t)r->enc_len : r->end - r->start;
        memcpy(tmp + r->reloc, r->enc ? r->enc : s_bank + r->start, (size_t)len);
        fix_pointers(tmp + r->reloc, (int)len, (int)r->reloc);
    }
    memcpy(s_image + first, tmp + first, (size_t)(TRAMP_END - first));   /* the tail beyond the texts is zero, as stock */
    free(tmp);
    s_rebuilt_hi = cursor;
    /* the string table */
    for (int i = 0; i < TABLE_N; i++) {
        if (i >= DESC_FIRST && i <= DESC_LAST) continue;
        const uint16_t v = s_table[i];
        if (v >= first && s_newoff[v]) s_table_new[i] = s_newoff[v];
    }
    s_active = 1;
    return 1;
}

static void write_bank(void)
{
    for (uint32_t o = s_rebuilt_lo & ~3u; o < TRAMP_END; o += 4) {
        const uint32_t w = (uint32_t)s_image[o] | ((uint32_t)s_image[o + 1] << 8) | ((uint32_t)s_image[o + 2] << 16) | ((uint32_t)s_image[o + 3] << 24);
        psx_mod_write_word(BANK_BASE + o, w);
    }
    for (int i = 0; i < TABLE_N; i++)
        if (s_table_new[i] != s_table[i]) psx_mod_write_half(TABLE_ADDR + (uint32_t)i * 2u, s_table_new[i]);
}

/* Per frame: the rebuilt bank must be in RAM (a savestate puts the stock
 * bytes back), and two duel-intro defaults the code carries as plain
 * numbers (0x71D0 / 0x7270, see func_80030F40) follow their texts. */
#define INTRO_DEFAULT_A 0x71D0u
#define INTRO_DEFAULT_B 0x7270u
#define INTRO_VAR       0x8009B36Au
static void assert_bank(void)
{
    if (!s_active) { s_patched = 0; return; }
    const uint32_t probe = s_rebuilt_lo & ~3u;
    const uint32_t want = (uint32_t)s_image[probe] | ((uint32_t)s_image[probe + 1] << 8) | ((uint32_t)s_image[probe + 2] << 16) | ((uint32_t)s_image[probe + 3] << 24);
    const uint32_t tail = (s_rebuilt_hi - 4u) & ~3u;
    const uint32_t want2 = (uint32_t)s_image[tail] | ((uint32_t)s_image[tail + 1] << 8) | ((uint32_t)s_image[tail + 2] << 16) | ((uint32_t)s_image[tail + 3] << 24);
    int fresh = psx_mod_read_word(BANK_BASE + probe) != want || psx_mod_read_word(BANK_BASE + tail) != want2;
    if (!fresh) {
        for (int i = 0x400; i < TABLE_N; i += 37)
            if (psx_mod_read_half(TABLE_ADDR + (uint32_t)i * 2u) != s_table_new[i]) { fresh = 1; break; }
    }
    if (fresh) write_bank();
    s_patched = 1;
    const uint16_t v = psx_mod_read_half(INTRO_VAR);
    if ((v == INTRO_DEFAULT_A || v == INTRO_DEFAULT_B) && s_newoff[v] && s_newoff[v] != v) psx_mod_write_half(INTRO_VAR, s_newoff[v]);
}

/* the stock bytes back (a cleared translation) */
static void restore_bank(void)
{
    for (uint32_t o = s_rebuilt_lo & ~3u; o < TRAMP_END; o += 4) {
        const uint32_t w = (uint32_t)s_bank[o] | ((uint32_t)s_bank[o + 1] << 8) | ((uint32_t)s_bank[o + 2] << 16) | ((uint32_t)s_bank[o + 3] << 24);
        psx_mod_write_word(BANK_BASE + o, w);
    }
    for (int i = 0; i < TABLE_N; i++) psx_mod_write_half(TABLE_ADDR + (uint32_t)i * 2u, s_table[i]);
    const uint16_t v = psx_mod_read_half(INTRO_VAR);
    for (uint32_t o = 0; o < 0x10000u; o++) if (s_newoff[o] == v && o != v && (o == INTRO_DEFAULT_A || o == INTRO_DEFAULT_B)) { psx_mod_write_half(INTRO_VAR, (uint16_t)o); break; }
}

/* ---- apply a parsed block --------------------------------------------------------- */
static void run_clear(Run *r)
{
    free(r->enc); r->enc = NULL; r->enc_len = 0;
    free(r->cur_text); r->cur_text = NULL;
    free(r->plain_cur); r->plain_cur = NULL;
    r->reloc = 0;
}

/* Set a run's text. Returns 1 translated, 0 stock (identical bytes), -1 error. */
static int run_set_text(Run *r, const char *text, char *err, unsigned errcap, int *longest, int *missing)
{
    static uint8_t enc[BANK_SIZE];
    Mark marks[MAX_ANCH + MAX_PTRS]; int nm = 0;
    const int n = encode_text(text, enc, (int)sizeof enc, marks, &nm, MAX_ANCH + MAX_PTRS, err, errcap, longest);
    if (n < 0) return -1;
    if (n == (int)(r->end - r->start) && !memcmp(enc, s_bank + r->start, (size_t)n)) { run_clear(r); return 0; }
    if (r->locked) { snprintf(err, errcap, "this text holds the player's name slot and cannot be changed"); return -1; }
    run_clear(r);
    r->nptr = 0;
    for (int j = 0; j < nm; j++) if (marks[j].is_ptr && r->nptr < MAX_PTRS) { r->ptr_pos[r->nptr] = marks[j].pos; r->ptr_val[r->nptr] = marks[j].name; r->nptr++; }
    r->enc = (uint8_t *)malloc((size_t)n); memcpy(r->enc, enc, (size_t)n); r->enc_len = n;
    *missing = 0;
    for (int k = 0; k < r->nanch; k++) {
        r->enc_pos[k] = -1;
        for (int j = 0; j < nm; j++) if (!marks[j].is_ptr && marks[j].name == r->anch[k].off) { r->enc_pos[k] = marks[j].pos; break; }
        if (r->enc_pos[k] < 0) (*missing)++;
    }
    /* the canonical text: what the bytes decode to, anchors where they landed */
    Mark placed[MAX_ANCH]; int np = 0;
    for (int k = 0; k < r->nanch; k++) if (r->enc_pos[k] >= 0) { placed[np].pos = r->enc_pos[k]; placed[np].name = r->anch[k].off; placed[np].is_ptr = 0; np++; }
    /* keep them sorted by position for the decoder */
    for (int a = 1; a < np; a++) for (int b2 = a; b2 > 0 && placed[b2 - 1].pos > placed[b2].pos; b2--) { Mark t = placed[b2]; placed[b2] = placed[b2 - 1]; placed[b2 - 1] = t; }
    r->cur_text = decode_bytes(r->enc, r->enc_len, placed, np, (int)r->start);
    r->plain_cur = plain_from_raw(r->cur_text);
    return 1;
}

/* ---- file: export ----------------------------------------------------------------- */
static void write_header(FILE *f, const char *what)
{
    char stamp[64];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    fprintf(f,
        "; Yu-Gi-Oh! Forbidden Memories -- in-game text: story dialogue, duelist lines, menus, duel messages\n"
        "; exported %s%s\n"
        ";\n"
        "; HOW TO EDIT THIS FILE\n"
        ";  - Each block starts with \"[@XXXX] ids ...\": where the text sits in the game. Never change that line.\n"
        ";    Below it is the text, one file line per line in the game's box. Lines starting with \";\" are comments.\n"
        ";  - Translate the words. Leave everything in braces {...} exactly as it is: those are the game's control\n"
        ";    codes (colours, pauses, portraits, music, jumps between texts). {FA} is a page break (wait for X).\n"
        ";    A block ends at the next \"[@\" line; blank lines before it are dropped, so a line break the game\n"
        ";    needs at the very end of a text is written {FE}.\n"
        ";  - {@XXXX} marks a spot the game jumps to (a choice branch, a shared ending, a string another text\n"
        ";    inserts). Keep each marker in front of the same sentence in your translation; a marker that is\n"
        ";    missing keeps the ORIGINAL text for that branch. {FC @XXXX} inserts another text; {FC @125A} is the\n"
        ";    player's name; {F8 0A 01} sets the colour (0 white 1 yellow 2 blue 3 green 4 grey 5 orange 6 red).\n"
        ";  - BOX SIZE: the story box shows %d lines of up to %d characters per page (the original never goes past\n"
        ";    %d; a longer line runs out of the box). Duel and menu boxes are narrower: keep those lines as long\n"
        ";    as the original. Names inserted with {FC @125A} take up to 6 characters.\n"
        ";  - Characters the font has: A-Z a-z 0-9 space . , ! ? ' \" - & / # $ %% * + : ( ) < > \xC2\xAB \xC2\xBB \xC2\xB7 "
        "\xE2\x99\x80 \xE2\x99\x82 \xE2\x8A\x82 \xE2\x8A\x83 \xCE\xB1 \xE2\x86\x90 \xE2\x86\x92\n"
        ";    No accents or other letters: the game's font has none. {XX} (two hex digits) is a glyph the font\n"
        ";    has no character for; leave it.\n"
        ";  - Save as UTF-8. Import in the game: MODS > Dialogue manager > Import..., or drop it back as\n"
        ";    <player-data>/dialogue/dialogue.txt and restart.\n"
        ";\n",
        stamp, what, PSX_DIALOGUE_LINES, PSX_DIALOGUE_COLS, PSX_DIALOGUE_COLS);
}

/* The translator's file: the story texts in plain form. */
int psx_dialogue_export(const char *path, char *err, unsigned errcap)
{
    if (!s_ready) { snprintf(err, errcap, "The game's text is not loaded yet"); return 0; }
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) { snprintf(err, errcap, "Cannot write %s", path); return 0; }
    char stamp[64];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&t));
    int tr = 0, n = 0;
    for (int i = 0; i < s_nruns; i++) if (run_is_story(&s_runs[i])) { n++; tr += s_runs[i].enc != NULL; }
    fprintf(f,
        "; Yu-Gi-Oh! Forbidden Memories -- the campaign's dialogue, for translating\n"
        "; exported %s%s\n"
        ";\n"
        "; Every text starts with a line like \"[1885]\" (its place in the game: leave those lines as they are)\n"
        "; and is followed by the words. Write your translation in place of the words:\n"
        ";  - a line break in the file is a line break in the game's box, and a blank line starts a new\n"
        ";    page (the game waits for X). You do not have to count: a story box shows %d lines of %d\n"
        ";    characters, and anything longer is wrapped and paged for you when the file is imported.\n"
        ";  - {1}, {2}, ... are the game's own codes (a picture, a sound, a choice, a jump to another text).\n"
        ";    Keep each one, in its order, next to the same words; the text around them is yours to change.\n"
        ";    {name} is where the player's name goes.\n"
        ";  - The font has A-Z a-z 0-9 and . , ! ? ' \" - & / # $ %% * + : ( ) < > \xC2\xAB \xC2\xBB \xC2\xB7 "
        "\xE2\x99\x80 \xE2\x99\x82 \xE2\x8A\x82 \xE2\x8A\x83 \xCE\xB1 \xE2\x86\x90 \xE2\x86\x92; no accents.\n"
        ";  - Lines starting with ; are notes. Save as UTF-8, then VIEW > Dialogue manager > Import in the game.\n"
        ";\n", stamp, tr ? " (with your translations)" : " (the original text)", PSX_DIALOGUE_LINES, PSX_DIALOGUE_COLS);
    for (int i = 0; i < s_nruns; i++) {
        const Run *r = &s_runs[i];
        if (!run_is_story(r)) continue;
        fprintf(f, "[%04X]\n", r->start);
        if (r->locked) fprintf(f, "; the player's name is typed into this text by the game: leave it as it is\n");
        fputs(r->plain_cur ? r->plain_cur : r->plain_stock, f);
        fputs("\n\n", f);
    }
    fclose(f);
    snprintf(err, errcap, "Exported %d texts%s to %s", n, tr ? " (with your translations)" : "", path);
    return 1;
}

/* Every text, control codes and all: the byte-exact form (debug). */
int psx_dialogue_export_raw(const char *path, char *err, unsigned errcap)
{
    if (!s_ready) { snprintf(err, errcap, "The game's text is not loaded yet"); return 0; }
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) { snprintf(err, errcap, "Cannot write %s", path); return 0; }
    int tr = psx_dialogue_translated_count();
    char what[96];
    snprintf(what, sizeof what, tr ? " (%d of %d texts translated)" : " (the original texts)", tr, s_nruns);
    write_header(f, what);
    for (int i = 0; i < s_nruns; i++) {
        const Run *r = &s_runs[i];
        fprintf(f, "[@%04X]", r->start);
        if (r->nids) { fprintf(f, " ids"); for (int k = 0; k < r->nids; k++) fprintf(f, " %d", r->ids[k]); }
        else fprintf(f, " (reached only by jumps)");
        fputc('\n', f);
        if (r->locked) fprintf(f, "; the player's name is typed into this text by the game: leave it as it is\n");
        if (r->nanch) {
            fprintf(f, "; markers:");
            for (int k = 0; k < r->nanch; k++) {
                if (r->anch[k].is_id) fprintf(f, " {@%04X}=id %d", r->anch[k].off, r->anch[k].id);
                else fprintf(f, " {@%04X}=jump target", r->anch[k].off);
            }
            fputc('\n', f);
        }
        fputs(r->cur_text ? r->cur_text : r->stock_text, f);
        fputs("\n\n", f);
    }
    fclose(f);
    snprintf(err, errcap, "Exported %d texts%s to %s", s_nruns, tr ? " (with your translations)" : "", path);
    return 1;
}

/* ---- file: import ----------------------------------------------------------------- */
static Run *run_by_key(uint32_t key)
{
    for (int i = 0; i < s_nruns; i++) if (s_runs[i].start == key) return &s_runs[i];
    return NULL;
}

static int copy_file(const char *from, const char *to)
{
    if (!strcmp(from, to)) return 1;
    FILE *a = psx_fopen_utf8(from, "rb"); if (!a) return 0;
    FILE *b = psx_fopen_utf8(to, "wb");   if (!b) { fclose(a); return 0; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    return 1;
}

/* Parse and apply. persist = copy the file into the player folder. */
static int import_file(const char *path, int persist, char *err, unsigned errcap)
{
    if (!s_ready) { snprintf(err, errcap, "The game's text is not loaded yet"); return 0; }
    size_t got = 0;
    char *data = psx_read_text_utf8(path, &got, 16 * 1024 * 1024);
    if (!data) { snprintf(err, errcap, "Cannot read %s (missing, or over 16 MB)", path); return 0; }
    if (!got) { free(data); snprintf(err, errcap, "%s is empty", path); return 0; }
    char *p = data;

    /* pass 1: collect blocks (key, text) -- the text keeps its line breaks */
    typedef struct { uint32_t key; char *text; int line; int plain; } Block;
    Block *blocks = (Block *)calloc(MAX_RUNS + 8, sizeof(Block));
    int nb = 0, line = 0, blocks_seen = 0;
    char *text = NULL; size_t tn = 0, tcap = 0;
    uint32_t key = 0; int key_line = 0, have = 0, plain = 0;
    char warn[2048]; unsigned wn = 0; warn[0] = 0;
    int unknown = 0;
#define WARN(...) do { if (wn < sizeof warn - 1) wn += (unsigned)snprintf(warn + wn, sizeof warn - wn, __VA_ARGS__); } while (0)
    while (*p) {
        char *e = strchr(p, '\n');
        const size_t ll = e ? (size_t)(e - p) : strlen(p);
        char *ln = p; p += ll + (e ? 1 : 0);
        line++;
        size_t l = ll;
        if (l && ln[l - 1] == '\r') l--;
        ln[l] = 0;
        if (ln[0] == '[' && (ln[1] == '@' || hexval(ln[1]) >= 0)) {
            if (have) {
                while (tn && text[tn - 1] == '\n') text[--tn] = 0;
                if (nb < MAX_RUNS + 8) { blocks[nb].key = key; blocks[nb].text = text ? text : strdup(""); blocks[nb].line = key_line; blocks[nb].plain = plain; nb++; }
                else free(text);
                text = NULL; tn = tcap = 0;
            }
            plain = ln[1] != '@';
            uint32_t v = 0; int d = 0; const char *q = ln + (plain ? 1 : 2);
            while (hexval(*q) >= 0) { v = v * 16u + (uint32_t)hexval(*q); q++; d++; }
            if (d < 1 || d > 4 || *q != ']') { WARN("line %d: bad block header; ", line); have = 0; continue; }
            key = v; key_line = line; have = 1; blocks_seen++;
            continue;
        }
        if (!have) continue;
        if (ln[0] == ';') continue;
        if (tn + l + 2 > tcap) { tcap = (tcap + l + 2) * 2; text = (char *)realloc(text, tcap); if (!tn) text[0] = 0; }
        memcpy(text + tn, ln, l); tn += l; text[tn++] = '\n'; text[tn] = 0;
    }
    if (have) {
        while (tn && text[tn - 1] == '\n') text[--tn] = 0;
        if (nb < MAX_RUNS + 8) { blocks[nb].key = key; blocks[nb].text = text ? text : strdup(""); blocks[nb].line = key_line; blocks[nb].plain = plain; nb++; }
        else free(text);
    }
    if (!blocks_seen) {
        free(blocks); free(data);
        snprintf(err, errcap, "%s holds no \"[XXXX]\" text blocks; is it a dialogue export?", path);
        return 0;
    }

    /* pass 2: encode every block; nothing is applied when a block fails */
    int translated = 0, stock = 0, errors = 0, wide = 0, missing_total = 0;
    uint32_t total = ARENA_HEAD;
    /* remember the previous state so a failed import leaves it alone */
    Run *snap = (Run *)malloc(sizeof(Run) * (size_t)s_nruns);
    memcpy(snap, s_runs, sizeof(Run) * (size_t)s_nruns);
    for (int i = 0; i < s_nruns; i++) { s_runs[i].enc = NULL; s_runs[i].cur_text = NULL; s_runs[i].enc_len = 0; s_runs[i].reloc = 0; }
    for (int b = 0; b < nb; b++) {
        Run *r = run_by_key(blocks[b].key);
        if (!r) { unknown++; if (unknown <= 3) WARN("line %d: no text at [@%04X]; ", blocks[b].line, blocks[b].key); continue; }
        char why[160]; int longest = 0, missing = 0;
        char *raw = NULL;
        if (blocks[b].plain) {
            if (!run_is_story(r)) { unknown++; if (unknown <= 3) WARN("line %d: [%04X] is a menu or duel label, not dialogue: skipped; ", blocks[b].line, r->start); continue; }
            /* untouched plain text is the original, byte for byte */
            if (!strcmp(blocks[b].text, r->plain_stock)) { run_clear(r); stock++; continue; }
            raw = raw_from_plain(r, blocks[b].text, why, sizeof why);
            if (!raw) { errors++; if (errors <= 4) WARN("[%04X] %s (block at line %d); ", r->start, why, blocks[b].line); continue; }
        }
        const int rc = run_set_text(r, raw ? raw : blocks[b].text, why, sizeof why, &longest, &missing);
        free(raw);
        if (rc < 0) { errors++; if (errors <= 4) WARN("[@%04X] %s (block at line %d); ", r->start, why, blocks[b].line); continue; }
        if (rc == 0) { stock++; continue; }
        translated++;
        total += (uint32_t)r->enc_len;
        if (longest > PSX_DIALOGUE_COLS) { wide++; if (wide <= 3) WARN("[@%04X] has a %d-character line (box: %d); ", r->start, longest, PSX_DIALOGUE_COLS); }
        if (missing) { missing_total += missing; if (missing_total <= 3) WARN("[@%04X] lost %d {@XXXX} marker(s): those branches keep the original; ", r->start, missing); }
    }
    for (int b = 0; b < nb; b++) free(blocks[b].text);
    free(blocks); free(data);
    if (errors) {
        /* roll back */
        for (int i = 0; i < s_nruns; i++) run_clear(&s_runs[i]);
        memcpy(s_runs, snap, sizeof(Run) * (size_t)s_nruns);
        free(snap);
        snprintf(err, errcap, "Nothing imported: %d text%s could not be read. %s", errors, errors == 1 ? "" : "s", warn);
        return 0;
    }
    (void)total;
    {
        char why[256];
        if (!rebuild_bank(why, sizeof why)) {
            /* roll back to what was live */
            for (int i = 0; i < s_nruns; i++) run_clear(&s_runs[i]);
            memcpy(s_runs, snap, sizeof(Run) * (size_t)s_nruns);
            free(snap);
            (void)rebuild_bank(why, sizeof why);
            snprintf(err, errcap, "Nothing imported: %s", why);
            return 0;
        }
    }
    for (int i = 0; i < s_nruns; i++) { free(snap[i].enc); free(snap[i].cur_text); free(snap[i].plain_cur); }
    free(snap);
    if (s_active) write_bank(); else restore_bank();
    bump();
    if (persist && s_dir_ok) {
        MKDIR(s_dir);
        if (translated) { if (!copy_file(path, s_file)) WARN("could not keep a copy as %s; ", s_file); }
        else remove(s_file);
    }
    snprintf(err, errcap, "Imported %d translated text%s (%d unchanged%s%s)%s%s",
             translated, translated == 1 ? "" : "s", stock,
             unknown ? ", some unknown" : "", wide ? ", some lines too long" : "",
             wn ? ". " : "", warn);
    return 1;
#undef WARN
}

int psx_dialogue_import(const char *path, char *err, unsigned errcap) { return import_file(path, 1, err, errcap); }

void psx_dialogue_clear(void)
{
    for (int i = 0; i < s_nruns; i++) run_clear(&s_runs[i]);
    { char why[64]; (void)rebuild_bank(why, sizeof why); }   /* nothing left: the stock bank comes back */
    restore_bank();
    if (s_dir_ok) remove(s_file);
    bump();
}

const char *psx_dialogue_file(void) { return s_dir_ok ? s_file : ""; }

/* ---- reading the bank ---------------------------------------------------------------- */
static int bank_resident(void)
{
    /* the first campaign string is at bank+0x128B and the general table's
     * first used entry at +0x11 in the retail EXE; both must hold */
    return psx_mod_read_half(TABLE_ADDR + 0x400u * 2u) == 0x128Bu
        && psx_mod_read_half(TABLE_ADDR + 1u * 2u) == 0x0011u
        && psx_mod_read_byte(BANK_BASE + 0x11u) == 0xFFu;
}

static void read_bank(void)
{
    for (uint32_t i = 0; i < BANK_SIZE; i++) s_bank[i] = psx_mod_read_byte(BANK_BASE + i);
    for (int i = 0; i < TABLE_N; i++) s_table[i] = psx_mod_read_half(TABLE_ADDR + (uint32_t)i * 2u);
    build_runs();
    int bad = 0;
    for (int i = 0; i < s_nruns; i++) {
        Run *r = &s_runs[i];
        r->stock_text = decode_stock_run(r);
        r->plain_stock = plain_from_raw(r->stock_text);
        /* the codec must give the stock bytes back */
        static uint8_t enc[BANK_SIZE];
            Mark marks[MAX_ANCH + MAX_PTRS]; int nm = 0; char why[128]; int lg = 0;
        const int n = encode_text(r->stock_text, enc, (int)sizeof enc, marks, &nm, MAX_ANCH + MAX_PTRS, why, sizeof why, &lg);
        if (n != (int)(r->end - r->start) || memcmp(enc, s_bank + r->start, (size_t)n)) {
            bad++;
            if (bad <= 5) fprintf(stderr, "Dialogue: run [@%04X] does not round-trip (%s)\n", r->start, n < 0 ? why : "bytes differ");
        }
    }
    s_ready = 1;
    fprintf(stderr, "Dialogue: %d texts, %u bytes of bank%s\n", s_nruns, s_text_end, bad ? " (round-trip problems, see above)" : "");
}

/* ---- public ------------------------------------------------------------------------ */
int  psx_dialogue_ready(void) { return s_ready; }
int  psx_dialogue_count(void) { return s_ready ? s_nruns : 0; }
unsigned psx_dialogue_generation(void) { return s_generation; }

int psx_dialogue_translated_count(void)
{
    int n = 0;
    for (int i = 0; i < s_nruns; i++) n += s_runs[i].enc != NULL;
    return n;
}

int psx_dialogue_run(int index, PsxDialogueRun *out)
{
    if (!s_ready || index < 0 || index >= s_nruns || !out) return 0;
    const Run *r = &s_runs[index];
    out->key = r->start;
    out->nids = r->nids;
    out->ids = r->ids;
    out->stock = r->stock_text;
    out->current = r->cur_text ? r->cur_text : r->stock_text;
    out->plain_stock = r->plain_stock;
    out->plain_current = r->plain_cur ? r->plain_cur : r->plain_stock;
    out->story = run_is_story(r);
    out->translated = r->enc != NULL;
    out->bytes = r->enc ? r->enc_len : (int)(r->end - r->start);
    return 1;
}

int psx_dialogue_state_json(char *out, unsigned cap)
{
    unsigned n = (unsigned)snprintf(out, cap,
        "\"ready\":%d,\"runs\":%d,\"translated\":%d,\"active\":%d,\"patched\":%d,\"generation\":%u,"
        "\"used\":%u,\"room\":%u,\"rebuilt\":\"%04X-%04X\",\"file\":\"%s\",\"keys\":[",
        s_ready, s_nruns, psx_dialogue_translated_count(), s_active, s_patched, s_generation,
        s_used, s_room, s_rebuilt_lo, s_rebuilt_hi, s_file);
    int first = 1;
    for (int i = 0; i < s_nruns && n + 24 < cap; i++) {
        if (!s_runs[i].enc) continue;
        n += (unsigned)snprintf(out + n, cap - n, "%s\"%04X\"", first ? "" : ",", s_runs[i].start);
        first = 0;
    }
    if (n + 2 >= cap) return 0;
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

/* ---- the frame hook ------------------------------------------------------------------ */
static void dialogue_tick(void)
{
    static int booted;
    if (!psx_mod_game_started()) return;
    if (!booted) {
        const char *dir = psx_mod_player_data_dir();
        if (!dir || !dir[0]) return;
        snprintf(s_dir, sizeof s_dir, "%s/dialogue", dir);
        snprintf(s_file, sizeof s_file, "%s/dialogue.txt", s_dir);
        s_dir_ok = 1;
        booted = 1;
    }
    if (!s_ready) {
        if (!bank_resident()) return;
        read_bank();
        /* the kept translation, if any */
        struct stat st;
        if (stat(s_file, &st) == 0) {
            char msg[512];
            if (import_file(s_file, 0, msg, sizeof msg)) fprintf(stderr, "Dialogue: %s\n", msg);
            else fprintf(stderr, "Dialogue: kept translation not applied: %s\n", msg);
        }
    }
    assert_bank();
}

PSX_MOD_CONSTRUCTOR(psx_dialogue_install)
{
    (void)psx_game_add_frame_hook(dialogue_tick);
}
