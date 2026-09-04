/* psx_card_packs.c -- replace a stock card everywhere it appears.
 *
 * PLAYER FILES  (<player-data>/cards/<id>/, id = 1..722)
 *     card.ini     name = Blue-eyes Ultimate Dragon
 *                  description = A delicate elf that|lacks in offense|...
 *                                ("|" breaks a line; no "|" = wrapped at 20)
 *                  attack = 4500          defense = 3800     (0..5110, x10)
 *                  star1 = Sun            star2 = Mars       (name or 1..10)
 *                  type = Dragon                             (name or 0..23)
 *                  level = 12             attribute = Light  (name or 0..7)
 *                  price = 999999         password = 12345678
 *                  Every key is optional; a missing key keeps the stock value.
 *     art.png      the card face art. Any size; scaled to 102x96, 256 colours.
 *     thumb.png    the duel thumbnail. Any size; scaled to 40x32, 64 colours.
 *                  Derived from art.png when absent.
 *     title.png    the baked title strip, 96x14, dark ink on white or with
 *                  alpha. Rendered from `name` when absent (Times New Roman
 *                  Bold from <player-data>/cards/timesbd.ttf, or the old
 *                  card_skins/ copy, or the duel text font as a last resort).
 *
 * WHERE EACH FIELD LIVES, MEASURED (2026-09-04, sector history + RAM):
 *
 *   The card FACE the password screen, the chest's TRIANGLE viewer and the
 *   library page draw is the 7-sector 2D record at disc LBA 10817 + 7*id:
 *   +0 art 102x96 8bpp, +9792 256-entry CLUT, +10304 title 96x14 4bpp,
 *   +10976 40x32 thumbnail + 64-entry CLUT (findings F125/F136). All three
 *   screens stream it fresh each time (the password screen read exactly
 *   LBAs 10831..10837 for card 2), so it is not RAM-resident anywhere.
 *
 *   The DUEL draws hand and field cards from the 40x32 thumbnail, streamed
 *   at duel start from WA_MRG sector id-1 (LBA 10102 + id - 1): the drive
 *   walked sectors 0..712 in one pass for a deck spanning ids 1..713, and
 *   only the deck's thumbnails stayed in RAM (stride 1408 at 0x8015C424).
 *
 *   NAME, ATK/DEF/stars/type and level/attribute are EXE tables (string
 *   0x8000+id via 0x801D5800; stats word 0x801D4244[id-1]; level/attr byte
 *   0x801D5332[id]) read by every screen, the duel's bottom bar included.
 *   The DESCRIPTION is string 0xD100+id: u16 at 0x801C0000 + 2*(0x100+id),
 *   an offset from 0x801C0000 to FE-broken, FF-ended text in the same code.
 *
 *   PRICE and PASSWORD are the 8-byte entries at WA_MRG 0xFB9800 + 8*id
 *   (F140), i.e. sectors 0x1F73..0x1F75 of the password module load.
 *
 * SO: the disc-side fields become SECTOR OVERRIDES (psx_mod_cd_override_*):
 * the runtime serves the replacement bytes for those LBAs and every reader
 * gets them, no per-screen hook, no VRAM rect to know. The EXE-side fields
 * are asserted per frame the way the other mods do it (savestates and the
 * card-extension's table relocation both put stock bytes back). Nothing on
 * the disc image or in the save changes.
 *
 * The earlier psx_card_skins.c did the face by redirecting LoadImage and
 * only reached the library/chest viewer; it is retired to tools/card_titles/
 * attic/, and its PNG quantiser and title renderer live on here. */

#include "psx_card_packs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "cdrom.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_db.h"
#include "psx_card_extend.h"
#include "psx_card_effects.h"
#include "psx_fusion_font.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../psxrecomp/runtime/third_party/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../psxrecomp/runtime/third_party/stb_truetype.h"

/* ---- guest facts ---------------------------------------------------------- */
#define CARD_COUNT     722
#define WA_LBA         10102u                       /* WA_MRG.MRG sector 0 */
#define REC_LBA(id)    (10817u + 7u * (uint32_t)(id))
#define REC_SECTORS    7
#define THUMB_LBA(id)  (WA_LBA + (uint32_t)(id) - 1u)
#define PW_LBA         (WA_LBA + 0x1F73u)           /* price/password table */
#define PW_SECTORS     3
#define SECTOR         2048

#define ART_W 102
#define ART_H 96
#define ART_BYTES  (ART_W * ART_H)                  /* 9792 */
#define ART_CLUT_OFF   9792
#define TITLE_OFF      10304
#define TITLE_W 96
#define TITLE_H 14
#define TITLE_BYTES    (TITLE_W / 2 * TITLE_H)      /* 672 */
#define THUMB_OFF      10976
#define THUMB_W 40
#define THUMB_H 32
#define THUMB_BYTES    (THUMB_W * THUMB_H)          /* 1280 */
#define THUMB_CLUT_OFF (THUMB_OFF + THUMB_BYTES)    /* 12256 */
#define THUMB_TOTAL    (THUMB_BYTES + 128)          /* 1408 */

#define STATS_STOCK    0x801D4244u
#define AUX_STOCK      0x801D5332u
#define NAMEOFF_TABLE  0x801D5800u
#define NAME_SEGMENT   0x801D0000u
/* Our name strings. 0x801D916F.. is zero on the stock EXE; psx_card_extend
 * uses 0x801D9200.. for its clone names and stops well before 0x801D9800. */
#define NAMES_BASE     0x801D9800u
#define NAMES_LIMIT    0x801DA000u
/* Descriptions: offsets are relative to 0x801C0000, so the strings must sit
 * in that 64 KB. The stock texts end at 0x801CD59E; psx_card_extend's
 * relocated tables run 0x801CD5A0..~0x801CEB90 and psx_free_duel_rows
 * starts at 0x801CFE00, which leaves this gap. */
#define DESC_TABLE     0x801C0200u        /* + id*2, entry index 0x100+id */
#define DESC_SEGMENT   0x801C0000u
#define DESC_BASE      0x801CEC00u
#define DESC_LIMIT     0x801CFE00u
#define DESC_COLS      20
#define DESC_LINES     6

/* ---- names ----------------------------------------------------------------
 * The game's frequency-ordered character code (gText_adwGlyphCodeTable,
 * findings in teatools/hextext). 0 = no ASCII equivalent. */
static const char CODE_TABLE[0x5C] = {
    ' ','e','t','a','o','i','n','s','r','h','l','.','d','u','m','c',
    'g','y','w','f','p','b','k','!','A','v','I','\'','T','S','M',',',
    'D','O','W','H','Y','E','R', 0 , 0 ,'G','L','C','N','B','?','P',
    '-','F','z','K','j','U','x','q','0','V','2','J','#','1','Q','Z',
    '"','3','5','&','/','7','X', 0 ,':', 0 ,'4',')','(', 0 ,'6','$',
    '*','>', 0 , 0 ,'<', 0 ,'+','8', 0 ,'9', 0 ,'%'
};
static int encode_char(char c)
{
    for (int i = 0; i < (int)sizeof CODE_TABLE; i++)
        if (CODE_TABLE[i] && CODE_TABLE[i] == c) return i;
    return 0;
}

/* Game text -> ASCII, FE -> '|', unknown glyphs -> '?'. */
static void decode_text(uint32_t addr, char *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0; n + 1 < cap && i < 512; i++) {
        const uint8_t b = psx_mod_read_byte(addr + i);
        if (b == 0xFF) break;
        if (b == 0xFE) { out[n++] = '|'; continue; }
        if (b >= 0xF0) { i++; continue; }          /* control code + operand */
        out[n++] = (b < sizeof CODE_TABLE && CODE_TABLE[b]) ? CODE_TABLE[b] : '?';
    }
    out[n] = 0;
}

/* Description text -> game bytes. "|" or a literal "\n" breaks a line; with
 * no breaks at all the text is word-wrapped at DESC_COLS the way the stock
 * descriptions are laid out. At most DESC_LINES lines. Returns the length
 * including the 0xFF. */
static int encode_desc(const char *text, uint8_t *out, int cap)
{
    char lines[DESC_LINES][DESC_COLS + 1];
    int nl = 0;
    const int has_breaks = strchr(text, '|') != NULL || strstr(text, "\\n") != NULL;
    if (has_breaks) {
        const char *p = text;
        while (*p && nl < DESC_LINES) {
            int k = 0;
            while (*p && *p != '|' && !(p[0] == '\\' && p[1] == 'n')) { if (k < DESC_COLS) lines[nl][k++] = *p; p++; }
            lines[nl][k] = 0; nl++;
            if (*p == '|') p++; else if (*p == '\\') p += 2;
        }
    } else {
        const char *p = text;
        while (*p && nl < DESC_LINES) {
            while (*p == ' ') p++;
            if (!*p) break;
            int k = 0;
            const char *last_space = NULL; const char *q = p;
            while (*q && k < DESC_COLS) { if (*q == ' ') last_space = q; k++; q++; }
            if (*q && last_space && *q != ' ') q = last_space;        /* back off to a word end */
            k = (int)(q - p); if (k > DESC_COLS) k = DESC_COLS;
            memcpy(lines[nl], p, (size_t)k); lines[nl][k] = 0; nl++;
            p = q;
        }
    }
    int n = 0;
    for (int l = 0; l < nl; l++) {
        if (l && n < cap) out[n++] = 0xFE;
        for (const char *s = lines[l]; *s && n < cap; s++) out[n++] = (uint8_t)encode_char(*s);
    }
    if (n < cap) out[n++] = 0xFF; else out[cap - 1] = 0xFF;
    return n;
}

static const char *const TYPE_NAMES[24] = {
    "Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast",
    "Winged Beast", "Fiend", "Fairy", "Insect", "Dinosaur", "Reptile",
    "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro",
    "Rock", "Plant", "Magic", "Trap", "Ritual", "Equip"
};
static const char *const ATTR_NAMES[8] = {
    "Light", "Dark", "Earth", "Water", "Fire", "Wind", "Magic", "Trap"
};
static const char *const STAR_NAMES[11] = {
    "", "Mars", "Jupiter", "Saturn", "Uranus", "Pluto",
    "Neptune", "Mercury", "Sun", "Moon", "Venus"
};
static const char *const FX_NAMES[PSX_CARD_FX_COUNT] = {
    "none", "heal", "damage", "destroy_type", "destroy_atk", "raigeki", "dark_hole", "dragon_jar",
    "stop_defense", "flip", "weaken", "swords", "cursebreaker", "harpie", "field", "ritual"
};
static const char *const FX_LABELS[PSX_CARD_FX_COUNT] = {
    "No effect", "Heal LP", "Damage LP", "Destroy a type", "Destroy by ATK", "Destroy all monsters (Raigeki)",
    "Destroy everything (Dark Hole)", "Destroy Dragons", "Stop Defense", "Flip face-down monsters",
    "Weaken opponent's monsters", "Swords of Revealing Light", "Cursebreaker", "Destroy magic/trap zone (Harpie)",
    "Change the field", "Ritual summon"
};
static const char *const TERRAIN_NAMES[7] = { "None", "Forest", "Wasteland", "Mountain", "Sogen", "Umi", "Yami" };
const char *psx_card_packs_effect_name(int fx)  { return (fx >= 0 && fx < PSX_CARD_FX_COUNT) ? FX_NAMES[fx] : "?"; }
const char *psx_card_packs_effect_label(int fx) { return (fx >= 0 && fx < PSX_CARD_FX_COUNT) ? FX_LABELS[fx] : "?"; }
const char *psx_card_packs_terrain_name(int t)  { return (t >= 0 && t < 7) ? TERRAIN_NAMES[t] : "?"; }
const char *psx_card_packs_type_name(int t)      { return (t >= 0 && t < 24) ? TYPE_NAMES[t] : "?"; }
const char *psx_card_packs_attribute_name(int a) { return (a >= 0 && a < 8) ? ATTR_NAMES[a] : "?"; }
const char *psx_card_packs_star_name(int s)      { return (s >= 1 && s <= 10) ? STAR_NAMES[s] : "?"; }

/* ---- state ---------------------------------------------------------------- */
typedef struct {
    PsxCardPack cfg;
    int      present;                 /* card.ini or a PNG exists */
    /* stock snapshot, taken before the first write */
    int      stock_ok;
    uint32_t stock_word;
    uint8_t  stock_aux;
    uint16_t stock_nameoff;
    uint16_t stock_descoff;
    char     stock_name[PSX_CARD_PACK_NAME_MAX + 1];
    char     stock_desc[PSX_CARD_PACK_DESC_MAX + 1];
    /* the encoded replacement name / description, if any */
    uint8_t  enc[PSX_CARD_PACK_NAME_MAX + 2];
    int      enc_len;                 /* incl. the 0xFF, 0 = no rename */
    uint32_t str_addr;
    uint8_t  denc[PSX_CARD_PACK_DESC_MAX + DESC_LINES + 2];
    int      denc_len;                /* incl. the 0xFF, 0 = stock description */
    uint32_t desc_addr;
    /* disc-side */
    int      rec_override;            /* the 7 record sectors are overridden */
    int      thumb_override;
    /* change detection */
    long     mtime[4];                /* card.ini art thumb title */
} Pack;

static Pack    *s_packs[CARD_COUNT + 1];
static char     s_dir[1024];
static int      s_dir_ok;
static unsigned s_generation;
static uint32_t s_names_next = NAMES_BASE;
static int      s_pw_dirty;           /* the price/password sectors need a rebuild */

/* ---- small file helpers --------------------------------------------------- */
static void pack_path(int id, const char *file, char *out, size_t cap)
{
    if (file) snprintf(out, cap, "%s/%d/%s", s_dir, id, file);
    else      snprintf(out, cap, "%s/%d", s_dir, id);
}

static long file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_mtime ^ (long)(st.st_size << 8);
}

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (32L << 20)) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = sz;
    return buf;
}

/* ---- PNG -> RGB at a fixed size --------------------------------------------
 * Box-filtered when shrinking, nearest when enlarging: card art is usually
 * a bigger scan, and averaging is what keeps it from sparkling. */
static int load_png_rgb(const char *path, int W, int H, uint8_t *out /* W*H*3 */)
{
    long sz;
    unsigned char *file = read_file(path, &sz);
    if (!file) return 0;
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 4);
    free(file);
    if (!img) return 0;
    for (int y = 0; y < H; y++) {
        const float sy0 = (float)y * (float)h / (float)H, sy1 = (float)(y + 1) * (float)h / (float)H;
        for (int x = 0; x < W; x++) {
            const float sx0 = (float)x * (float)w / (float)W, sx1 = (float)(x + 1) * (float)w / (float)W;
            int iy0 = (int)sy0, iy1 = (int)ceilf(sy1), ix0 = (int)sx0, ix1 = (int)ceilf(sx1);
            if (iy1 <= iy0) iy1 = iy0 + 1;
            if (ix1 <= ix0) ix1 = ix0 + 1;
            if (iy1 > h) iy1 = h;
            if (ix1 > w) ix1 = w;
            float r = 0, g = 0, b = 0, n = 0;
            for (int yy = iy0; yy < iy1; yy++)
                for (int xx = ix0; xx < ix1; xx++) {
                    const unsigned char *p = img + ((size_t)yy * (size_t)w + (size_t)xx) * 4u;
                    const float a = (float)p[3] / 255.0f;       /* composite on black */
                    r += (float)p[0] * a; g += (float)p[1] * a; b += (float)p[2] * a; n += 1.0f;
                }
            uint8_t *o = out + ((size_t)y * (size_t)W + (size_t)x) * 3u;
            o[0] = (uint8_t)(r / n + 0.5f); o[1] = (uint8_t)(g / n + 0.5f); o[2] = (uint8_t)(b / n + 0.5f);
        }
    }
    stbi_image_free(img);
    return 1;
}

/* ---- median-cut quantiser --------------------------------------------------
 * RGB -> <= ncol palette indices and a 15-bit CLUT. Index 0 is kept opaque
 * black (0x8000) when it is black, since 0x0000 would be transparent. */
typedef struct { int lo, hi; } Box;
static const uint8_t *s_qpx;
static int  s_qorder[ART_BYTES];
static int  s_qaxis;
static int cmp_px(const void *a, const void *b)
{
    const uint8_t *pa = s_qpx + (size_t)(*(const int *)a) * 3u;
    const uint8_t *pb = s_qpx + (size_t)(*(const int *)b) * 3u;
    return (int)pa[s_qaxis] - (int)pb[s_qaxis];
}
static void quantize(const uint8_t *rgb, int n, int ncol, uint8_t *idx_out, uint16_t *clut_out)
{
    s_qpx = rgb;
    for (int i = 0; i < n; i++) s_qorder[i] = i;
    Box boxes[256]; int nb = 1; boxes[0].lo = 0; boxes[0].hi = n;
    while (nb < ncol) {
        int best = -1, bestrange = -1, bestaxis = 0;
        for (int b = 0; b < nb; b++) {
            if (boxes[b].hi - boxes[b].lo < 2) continue;
            int mn[3] = {255,255,255}, mx[3] = {0,0,0};
            for (int i = boxes[b].lo; i < boxes[b].hi; i++) {
                const uint8_t *p = rgb + (size_t)s_qorder[i] * 3u;
                for (int k = 0; k < 3; k++) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
            }
            for (int k = 0; k < 3; k++)
                if (mx[k] - mn[k] > bestrange) { bestrange = mx[k] - mn[k]; best = b; bestaxis = k; }
        }
        if (best < 0 || bestrange == 0) break;
        s_qaxis = bestaxis;
        qsort(&s_qorder[boxes[best].lo], (size_t)(boxes[best].hi - boxes[best].lo), sizeof(int), cmp_px);
        const int mid = (boxes[best].lo + boxes[best].hi) / 2;
        boxes[nb].lo = mid; boxes[nb].hi = boxes[best].hi; boxes[best].hi = mid; nb++;
    }
    for (int b = 0; b < nb; b++) {
        long r = 0, g = 0, bl = 0; const int cnt = boxes[b].hi - boxes[b].lo;
        for (int i = boxes[b].lo; i < boxes[b].hi; i++) {
            const uint8_t *p = rgb + (size_t)s_qorder[i] * 3u;
            r += p[0]; g += p[1]; bl += p[2];
            idx_out[s_qorder[i]] = (uint8_t)b;
        }
        if (cnt) { r /= cnt; g /= cnt; bl /= cnt; }
        uint16_t c = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10));
        if (c == 0) c = 0x8000;
        clut_out[b] = c;
    }
    for (int b = nb; b < ncol; b++) clut_out[b] = 0x8000;
}

static void rgb_from_indexed(const uint8_t *idx, const uint8_t *clut_le, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        const unsigned c = (unsigned)clut_le[idx[i] * 2] | ((unsigned)clut_le[idx[i] * 2 + 1] << 8);
        out[i * 3 + 0] = (uint8_t)(((c      ) & 31u) * 255u / 31u);
        out[i * 3 + 1] = (uint8_t)(((c >>  5) & 31u) * 255u / 31u);
        out[i * 3 + 2] = (uint8_t)(((c >> 10) & 31u) * 255u / 31u);
    }
}

/* 102x96 -> 40x32 box filter, for a thumbnail derived from the art. */
static void shrink_rgb(const uint8_t *in, int iw, int ih, uint8_t *out, int ow, int oh)
{
    for (int y = 0; y < oh; y++) {
        const int y0 = y * ih / oh, y1 = (y + 1) * ih / oh;
        for (int x = 0; x < ow; x++) {
            const int x0 = x * iw / ow, x1 = (x + 1) * iw / ow;
            long r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = in + ((size_t)yy * (size_t)iw + (size_t)xx) * 3u;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            uint8_t *o = out + ((size_t)y * (size_t)ow + (size_t)x) * 3u;
            if (n) { o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(b / n); }
        }
    }
}

/* ---- title: name -> 96x14 4bpp ---------------------------------------------
 * HOW THE GAME DRAWS THE STRIP (measured 2026-09-04 from a real window
 * capture against the bitmap in gLibrary_aCardArtRecord): the 16-entry CLUT
 * at VRAM (480,248) is index 1 = 184 grey .. index 7 = 32 grey, and the
 * sprite is drawn SUBTRACTIVELY over the gold frame -- each level's grey is
 * taken away from the gold. So the LIGHT entries make dark ink and the dark
 * entries barely show: level 1 is black-on-gold, level 7 is nearly invisible.
 * The stock titles are authored that way (stems at level 1, faint level-7
 * fringes give the engraved look). A bitmap that used 7 for the ink came out
 * as a pale ghost with a dark rim -- the "unreadable title".
 *
 * Times REGULAR at 13 px (the stock weight; bold read as too heavy once the
 * blend was right), every glyph snapped to a whole pixel so stems fill full
 * columns, coverage mapped hard: >=150 -> level 1 (ink), >=96 -> level 3,
 * >=40 -> level 6 (a faint halo, as the stock strips have), else nothing. Baseline row 11, pen from
 * x=3, names wider than ~90 px squeezed to columns 3..93 like stock.
 * TEA-Online's tool is a browser canvas at 16 px squeezed to 14 rows with
 * the nearest of the same seven greys; nothing there to copy.
 *
 * Font file, first found wins: cards/timesbd.ttf, the old card_skins copy,
 * then cards/times.ttf (regular), else the duel font. */
#define TITLE_BASE_Y  11
#define TITLE_X0      3
#define TITLE_MAX_W   90
#define TITLE_CANVAS  512
#define TITLE_INK     1      /* full coverage: the strongest subtraction */
#define TITLE_EDGE    3
#define TITLE_FAINT   6      /* the faint halo the stock titles carry */
typedef struct { const char *file; float px; } TitleModel;
static const TitleModel TITLE_MODELS[3] = {
    { "%s/cards/times.ttf",        13.0f },   /* the stock weight: 1 px cores with faint halos */
    { "%s/cards/timesbd.ttf",      12.0f },
    { "%s/card_skins/timesbd.ttf", 12.0f },
};
static unsigned char *s_ttf;
static stbtt_fontinfo s_font;
static int s_font_ok = -1;
static const TitleModel *s_title_model = &TITLE_MODELS[0];

static int title_font_load(void)
{
    if (s_font_ok >= 0) return s_font_ok;
    s_font_ok = 0;
    const char *dir = psx_mod_player_data_dir();
    for (int t = 0; t < 3 && !s_ttf; t++) {
        char path[1200]; snprintf(path, sizeof path, TITLE_MODELS[t].file, dir);
        long sz; s_ttf = read_file(path, &sz);
        if (s_ttf) s_title_model = &TITLE_MODELS[t];
    }
    if (!s_ttf) return 0;
    if (!stbtt_InitFont(&s_font, s_ttf, stbtt_GetFontOffsetForIndex(s_ttf, 0))) { free(s_ttf); s_ttf = NULL; return 0; }
    s_font_ok = 1;
    return 1;
}

static int title_quantize(float cov)
{
    const int c = (int)(cov + 0.5f);
    if (c >= 150) return TITLE_INK;
    if (c >= 96)  return TITLE_EDGE;
    if (c >= 40)  return TITLE_FAINT;
    return 0;
}

static void render_title_fallback(const char *text, uint8_t nib[TITLE_H][TITLE_W])
{
    const PsxFusionFont *f = &psx_fusion_font;
    int x = 4;
    for (const char *p = text; *p && x < TITLE_W - 2; p++) {
        if (*p == ' ') { x += 3; continue; }
        const int cell = psx_fusion_font_cell((unsigned char)*p);
        if (cell < 0) { x += 3; continue; }
        const uint8_t *g = f->px + (size_t)cell * (size_t)(f->w * f->h);
        int lo = f->w, hi = -1;
        for (int gy = 0; gy < f->h; gy++)
            for (int gx = 0; gx < f->w; gx++)
                if (g[gy * f->w + gx] >= 2) { if (gx < lo) lo = gx; if (gx > hi) hi = gx; }
        if (hi < 0) { x += 3; continue; }
        for (int gy = 0; gy < f->h && gy + 1 < TITLE_H; gy++)
            for (int gx = lo; gx <= hi; gx++) {
                const int v = g[gy * f->w + gx], dx = x + gx - lo;
                if (v < 2 || dx >= TITLE_W) continue;
                nib[gy + 1][dx] = (uint8_t)(v >= 8 ? TITLE_INK : TITLE_FAINT);
            }
        x += hi - lo + 2;
    }
}

static void pack_nibbles(const uint8_t nib[TITLE_H][TITLE_W], uint8_t *out)
{
    for (int y = 0; y < TITLE_H; y++)
        for (int bx = 0; bx < TITLE_W / 2; bx++)
            out[y * (TITLE_W / 2) + bx] = (uint8_t)(nib[y][bx * 2] | (nib[y][bx * 2 + 1] << 4));
}

static void render_title(const char *text, uint8_t *out /* TITLE_BYTES */)
{
    uint8_t nib[TITLE_H][TITLE_W];
    memset(nib, 0, sizeof nib);
    if (!title_font_load()) {
        render_title_fallback(text, nib);
        pack_nibbles(nib, out);
        return;
    }
    static unsigned char canvas[TITLE_H][TITLE_CANVAS];
    static unsigned char glyph[64 * 64];
    memset(canvas, 0, sizeof canvas);
    const float scale = stbtt_ScaleForMappingEmToPixels(&s_font, s_title_model->px);
    float pen = (float)TITLE_X0;
    int ink_lo = TITLE_CANVAS, ink_hi = -1;
    for (const char *p = text; *p; p++) {
        const int cp = (unsigned char)*p;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        if (cp != ' ') {
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&s_font, cp, scale, scale, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw <= 64 && gh <= 64) {
                stbtt_MakeCodepointBitmap(&s_font, glyph, gw, gh, gw, scale, scale, cp);
                const int bx = (int)floorf(pen + 0.5f) + x0, by = TITLE_BASE_Y + y0;
                for (int gy = 0; gy < gh; gy++) {
                    const int y = by + gy;
                    if (y < 0 || y >= TITLE_H) continue;
                    for (int gx = 0; gx < gw; gx++) {
                        const int x = bx + gx;
                        const unsigned char v = glyph[gy * gw + gx];
                        if (x < 0 || x >= TITLE_CANVAS || !v) continue;
                        if (v > canvas[y][x]) canvas[y][x] = v;
                        if (x < ink_lo) ink_lo = x;
                        if (x > ink_hi) ink_hi = x;
                    }
                }
            }
        }
        pen += (float)adv * scale;
        if (p[1]) pen += (float)stbtt_GetCodepointKernAdvance(&s_font, cp, (unsigned char)p[1]) * scale;
        pen = floorf(pen + 0.5f);               /* whole-pixel advances: stems on full columns */
    }
    if (ink_hi >= ink_lo) {
        const int natural = ink_hi - ink_lo + 1;
        if (natural <= TITLE_MAX_W) {
            for (int y = 0; y < TITLE_H; y++)
                for (int x = 0; x < TITLE_W; x++)
                    nib[y][x] = (uint8_t)title_quantize((float)canvas[y][x]);
        } else {
            static float sq[TITLE_H][TITLE_MAX_W];
            const float step = (float)natural / (float)TITLE_MAX_W;
            float peak = 1.0f;
            for (int y = 0; y < TITLE_H; y++)
                for (int x = 0; x < TITLE_MAX_W; x++) {
                    const float sx = (float)ink_lo + ((float)x + 0.5f) * step - 0.5f;
                    int i0 = (int)floorf(sx); const float t = sx - (float)i0; int i1 = i0 + 1;
                    if (i0 < 0) i0 = 0;
                    if (i1 > TITLE_CANVAS - 1) i1 = TITLE_CANVAS - 1;
                    sq[y][x] = (float)canvas[y][i0] * (1.0f - t) + (float)canvas[y][i1] * t;
                    if (sq[y][x] > peak) peak = sq[y][x];
                }
            const float gain = 255.0f / peak;
            for (int y = 0; y < TITLE_H; y++)
                for (int x = 0; x < TITLE_MAX_W; x++)
                    nib[y][TITLE_X0 + x] = (uint8_t)title_quantize(sq[y][x] * gain);
        }
    }
    pack_nibbles(nib, out);
}

/* title.png: 96x14, ink darkness = level. Alpha counts as ink coverage when
 * present, so a transparent-background strip and a white-background one both
 * work. */
static int load_title_png(const char *path, uint8_t *out)
{
    long sz;
    unsigned char *file = read_file(path, &sz);
    if (!file) return 0;
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 4);
    free(file);
    if (!img) return 0;
    uint8_t nib[TITLE_H][TITLE_W];
    memset(nib, 0, sizeof nib);
    for (int y = 0; y < TITLE_H && y < h; y++)
        for (int x = 0; x < TITLE_W && x < w; x++) {
            const unsigned char *p = img + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            const int lum = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
            const int cov = (255 - lum) * p[3] / 255;
            nib[y][x] = (uint8_t)title_quantize((float)cov);
        }
    stbi_image_free(img);
    pack_nibbles(nib, out);
    return 1;
}

/* ---- ini ------------------------------------------------------------------- */
static int match_name(const char *v, const char *const *names, int n, int first)
{
    for (int i = 0; i < n; i++) {
        const char *a = v, *b = names[i];
        if (!*b) continue;
        while (*a && *b && ((*a | 32) == (*b | 32) || (*a == ' ' && *b == '-') || (*a == '-' && *b == ' '))) { a++; b++; }
        if (!*a && !*b) return first + i;
    }
    return -1;
}

static int parse_enum(const char *v, const char *const *names, int n, int first, int lo, int hi)
{
    if (*v >= '0' && *v <= '9') {
        const int x = atoi(v);
        return (x >= lo && x <= hi) ? x : -1;
    }
    const int m = match_name(v, names, n, first);
    return (m >= lo && m <= hi) ? m : -1;
}

static void cfg_reset(PsxCardPack *c, int id)
{
    memset(c, 0, sizeof *c);
    c->id = id;
    c->attack = c->defense = c->star1 = c->star2 = c->type = c->level = c->attribute = c->price = -1;
    psx_card_packs_effects_reset(c);
}

void psx_card_packs_effects_reset(PsxCardPack *c)
{
    c->effect = c->amount = c->target = c->terrain = c->equip_bonus = c->trap_atk_max = -1;
    c->equips_set = 0; c->equip_types = 0; c->equip_n = 0;
    c->boost_set = 0;
    for (int t = 0; t < 20; t++) c->boost[t] = PSX_CARD_PACK_BOOST_UNSET;
    c->ritual_set = 0;
    c->ritual_mat[0] = c->ritual_mat[1] = c->ritual_mat[2] = c->ritual_result = -1;
}

int psx_card_packs_parse_effect(const char *v)
{
    for (int i = 0; i < PSX_CARD_FX_COUNT; i++) {
        const char *a = v, *b = FX_NAMES[i];
        while (*a && *b && ((*a | 32) == (*b | 32) || ((*a == ' ' || *a == '-') && *b == '_'))) { a++; b++; }
        if (!*a && !*b) return i;
    }
    if (*v >= '0' && *v <= '9') { const int x = atoi(v); return (x >= 0 && x < PSX_CARD_FX_COUNT) ? x : -1; }
    return -1;
}

static void trim(char *s);
static void seterr(char *err, unsigned cap, const char *m) { if (err && cap) snprintf(err, cap, "%s", m); }

/* Split v on commas into trimmed tokens; returns the count (max n). */
static int split_list(const char *v, char tok[][48], int n)
{
    int k = 0;
    const char *p = v;
    while (*p && k < n) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        int m = 0;
        while (*p && *p != ',' && m < 47) tok[k][m++] = *p++;
        tok[k][m] = 0;
        trim(tok[k]);
        if (tok[k][0]) k++;
    }
    return k;
}

int psx_card_packs_parse_equips(const char *v, PsxCardPack *c, char *err, unsigned errcap)
{
    static char tok[300][48];
    const int n = split_list(v, tok, 300);
    uint32_t types = 0; int ids = 0; uint16_t list[PSX_CARD_PACK_EQUIP_MAX];
    for (int i = 0; i < n; i++) {
        const char *t = tok[i];
        if (!strcmp(t, "all") || !strcmp(t, "All") || !strcmp(t, "ALL")) { types |= PSX_CARD_PACK_EQUIP_ALL; continue; }
        if (!strcmp(t, "none") || !strcmp(t, "None") || !strcmp(t, "NONE")) continue;
        if (*t >= '0' && *t <= '9') {
            const int x = atoi(t);
            if (x < 1 || x > CARD_COUNT) { seterr(err, errcap, "card ids are 1 to 722"); return 0; }
            if (ids < PSX_CARD_PACK_EQUIP_MAX) list[ids++] = (uint16_t)x;
            continue;
        }
        const int ty = match_name(t, TYPE_NAMES, 20, 0);
        if (ty < 0) { char m[96]; snprintf(m, sizeof m, "'%s' is not a monster type or a card id", t); seterr(err, errcap, m); return 0; }
        types |= 1u << ty;
    }
    c->equips_set = 1;
    c->equip_types = types;
    c->equip_n = ids;
    memcpy(c->equip_ids, list, (size_t)ids * sizeof list[0]);
    return 1;
}

int psx_card_packs_parse_boost(const char *v, PsxCardPack *c, char *err, unsigned errcap)
{
    static char tok[40][48];
    const int n = split_list(v, tok, 40);
    int b[20];
    for (int t = 0; t < 20; t++) b[t] = PSX_CARD_PACK_BOOST_UNSET;
    for (int i = 0; i < n; i++) {
        char *t = tok[i];
        /* "<type> <+/-N>": the number is the last token */
        char *num = t + strlen(t);
        while (num > t && num[-1] != ' ') num--;
        if (num == t) { seterr(err, errcap, "each entry is a type and a number, like Dragon +500"); return 0; }
        const int val = atoi(num);
        if (val < -1280 || val > 1270) { seterr(err, errcap, "a boost is -1280 to 1270"); return 0; }
        char name[48]; snprintf(name, sizeof name, "%.*s", (int)(num - t), t); trim(name);
        int ty = -1;
        if (!strcmp(name, "all") || !strcmp(name, "All")) { for (int k = 0; k < 20; k++) b[k] = val / 10 * 10; continue; }
        if (*name >= '0' && *name <= '9') ty = atoi(name); else ty = match_name(name, TYPE_NAMES, 20, 0);
        if (ty < 0 || ty > 19) { char m[96]; snprintf(m, sizeof m, "'%s' is not a monster type", name); seterr(err, errcap, m); return 0; }
        b[ty] = val / 10 * 10;
    }
    c->boost_set = 1;
    memcpy(c->boost, b, sizeof b);
    return 1;
}

int psx_card_packs_parse_ritual(const char *v, PsxCardPack *c, char *err, unsigned errcap)
{
    int m[3], r;
    char buf[128]; snprintf(buf, sizeof buf, "%s", v);
    for (char *q = buf; *q; q++) if (*q == '-' || *q == '>' || *q == '=' || *q == ',') *q = ' ';
    if (sscanf(buf, "%d %d %d %d", &m[0], &m[1], &m[2], &r) != 4) { seterr(err, errcap, "a recipe is three material ids and a result, like 1, 1, 1 -> 380"); return 0; }
    for (int i = 0; i < 3; i++) if (m[i] < 1 || m[i] > CARD_COUNT) { seterr(err, errcap, "card ids are 1 to 722"); return 0; }
    if (r < 1 || r > CARD_COUNT) { seterr(err, errcap, "card ids are 1 to 722"); return 0; }
    c->ritual_set = 1;
    c->ritual_mat[0] = m[0]; c->ritual_mat[1] = m[1]; c->ritual_mat[2] = m[2]; c->ritual_result = r;
    return 1;
}

void psx_card_packs_format_equips(const PsxCardPack *c, char *out, unsigned cap)
{
    unsigned n = 0; out[0] = 0;
    if (c->equip_types & PSX_CARD_PACK_EQUIP_ALL) n += (unsigned)snprintf(out + n, cap - n, "all");
    else for (int t = 0; t < 20; t++) if (c->equip_types & (1u << t)) n += (unsigned)snprintf(out + n, cap - n, "%s%s", n ? ", " : "", TYPE_NAMES[t]);
    for (int i = 0; i < c->equip_n && n + 8 < cap; i++) n += (unsigned)snprintf(out + n, cap - n, "%s%d", n ? ", " : "", c->equip_ids[i]);
    if (!n) snprintf(out, cap, "none");
}

void psx_card_packs_format_boost(const PsxCardPack *c, char *out, unsigned cap)
{
    unsigned n = 0; out[0] = 0;
    for (int t = 0; t < 20; t++) {
        if (c->boost[t] == PSX_CARD_PACK_BOOST_UNSET || c->boost[t] == 0) continue;
        n += (unsigned)snprintf(out + n, cap - n, "%s%s %+d", n ? ", " : "", TYPE_NAMES[t], c->boost[t]);
        if (n >= cap) break;
    }
    if (!n) snprintf(out, cap, "none");
}

void psx_card_packs_format_ritual(const PsxCardPack *c, char *out, unsigned cap)
{
    if (!c->ritual_set) { out[0] = 0; return; }
    snprintf(out, cap, "%d, %d, %d -> %d", c->ritual_mat[0], c->ritual_mat[1], c->ritual_mat[2], c->ritual_result);
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

static int read_ini(int id, PsxCardPack *c)
{
    char path[1200];
    pack_path(id, "card.ini", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '[' || !*p || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        trim(key);
        while (*val == ' ' || *val == '\t') val++;
        trim(val);
        for (char *k = key; *k; k++) if (*k >= 'A' && *k <= 'Z') *k = (char)(*k + 32);   /* not |32: that turns '_' into DEL */
        if (!strcmp(key, "name")) {
            snprintf(c->name, sizeof c->name, "%s", val);
        } else if (!strcmp(key, "description") || !strcmp(key, "desc") || !strcmp(key, "text")) {
            snprintf(c->description, sizeof c->description, "%s", val);
        } else if (!strcmp(key, "attack") || !strcmp(key, "atk")) {
            const int v = atoi(val); if (v >= 0 && v <= 5110) c->attack = v / 10 * 10;
        } else if (!strcmp(key, "defense") || !strcmp(key, "defence") || !strcmp(key, "def")) {
            const int v = atoi(val); if (v >= 0 && v <= 5110) c->defense = v / 10 * 10;
        } else if (!strcmp(key, "star1") || !strcmp(key, "gs1")) {
            c->star1 = parse_enum(val, STAR_NAMES, 11, 0, 1, 10);
        } else if (!strcmp(key, "star2") || !strcmp(key, "gs2")) {
            c->star2 = parse_enum(val, STAR_NAMES, 11, 0, 1, 10);
        } else if (!strcmp(key, "type")) {
            c->type = parse_enum(val, TYPE_NAMES, 24, 0, 0, 23);
        } else if (!strcmp(key, "level")) {
            const int v = atoi(val); if (v >= 0 && v <= 15) c->level = v;
        } else if (!strcmp(key, "attribute") || !strcmp(key, "attr")) {
            c->attribute = parse_enum(val, ATTR_NAMES, 8, 0, 0, 7);
        } else if (!strcmp(key, "price") || !strcmp(key, "cost")) {
            const int v = atoi(val); if (v >= 0 && v <= 999999) c->price = v;
        } else if (!strcmp(key, "password")) {
            int ok = strlen(val) == 8;
            for (int i = 0; ok && i < 8; i++) if (val[i] < '0' || val[i] > '9') ok = 0;
            if (ok) memcpy(c->password, val, 9);
        } else if (!strcmp(key, "effect")) {
            c->effect = psx_card_packs_parse_effect(val);
        } else if (!strcmp(key, "amount")) {
            const int v = atoi(val); if (v >= -9999 && v <= 25500) c->amount = v;
        } else if (!strcmp(key, "target")) {
            c->target = parse_enum(val, TYPE_NAMES, 20, 0, 0, 19);
        } else if (!strcmp(key, "terrain") || !strcmp(key, "field")) {
            c->terrain = parse_enum(val, TERRAIN_NAMES, 7, 0, 1, 6);
        } else if (!strcmp(key, "equip_bonus") || !strcmp(key, "bonus")) {
            const int v = atoi(val); if (v >= -9990 && v <= 9990) c->equip_bonus = v / 10 * 10;
        } else if (!strcmp(key, "equips")) {
            (void)psx_card_packs_parse_equips(val, c, NULL, 0);
        } else if (!strcmp(key, "boost") || !strcmp(key, "boosts")) {
            (void)psx_card_packs_parse_boost(val, c, NULL, 0);
        } else if (!strcmp(key, "trap_atk_max") || !strcmp(key, "trap_atk")) {
            const int v = atoi(val); if (v >= 0 && v <= 25500) c->trap_atk_max = v / 100 * 100;
        } else if (!strcmp(key, "ritual") || !strcmp(key, "recipe")) {
            (void)psx_card_packs_parse_ritual(val, c, NULL, 0);
        }
    }
    fclose(f);
    return 1;
}

/* ---- stock snapshot --------------------------------------------------------- */
static int take_stock(Pack *pk)
{
    if (pk->stock_ok) return 1;
    if (!psx_card_db_ready()) return 0;
    const int id = pk->cfg.id;
    pk->stock_word    = psx_mod_read_word(STATS_STOCK + (uint32_t)(id - 1) * 4u);
    pk->stock_aux     = psx_mod_read_byte(AUX_STOCK + (uint32_t)id);
    pk->stock_nameoff = psx_mod_read_half(NAMEOFF_TABLE + (uint32_t)id * 2u);
    pk->stock_descoff = psx_mod_read_half(DESC_TABLE + (uint32_t)id * 2u);
    snprintf(pk->stock_name, sizeof pk->stock_name, "%s", psx_card_db_name(id));
    decode_text(DESC_SEGMENT + pk->stock_descoff, pk->stock_desc, sizeof pk->stock_desc);
    pk->stock_ok = 1;
    return 1;
}

/* ---- the disc-side build ---------------------------------------------------- */
static void bump(void) { s_generation++; }

static int read_stock_record(int id, uint8_t *rec /* 7*2048 */)
{
    for (int s = 0; s < REC_SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(REC_LBA(id) + (uint32_t)s, rec + s * SECTOR)) return 0;
    return 1;
}

/* Build and install the record + thumbnail sectors for a pack. Returns 1 if
 * any disc-side override is now in place. */
static int build_disc_side(Pack *pk)
{
    const int id = pk->cfg.id;
    static uint8_t rec[REC_SECTORS * SECTOR];
    static uint8_t art_rgb[ART_BYTES * 3], thumb_rgb[THUMB_BYTES * 3];
    static uint8_t idx[ART_BYTES];
    static uint16_t clut[256];
    char path[1200];
    int have_art = 0, have_thumb = 0, have_title = 0;

    if (!read_stock_record(id, rec)) return 0;

    pack_path(id, "art.png", path, sizeof path);
    if (load_png_rgb(path, ART_W, ART_H, art_rgb)) {
        quantize(art_rgb, ART_BYTES, 256, idx, clut);
        memcpy(rec, idx, ART_BYTES);
        for (int i = 0; i < 256; i++) { rec[ART_CLUT_OFF + i * 2] = (uint8_t)clut[i]; rec[ART_CLUT_OFF + i * 2 + 1] = (uint8_t)(clut[i] >> 8); }
        have_art = 1;
    }
    pack_path(id, "thumb.png", path, sizeof path);
    if (load_png_rgb(path, THUMB_W, THUMB_H, thumb_rgb)) have_thumb = 1;
    else if (have_art) { shrink_rgb(art_rgb, ART_W, ART_H, thumb_rgb, THUMB_W, THUMB_H); have_thumb = 1; }
    if (have_thumb) {
        quantize(thumb_rgb, THUMB_BYTES, 64, idx, clut);
        memcpy(rec + THUMB_OFF, idx, THUMB_BYTES);
        for (int i = 0; i < 64; i++) { rec[THUMB_CLUT_OFF + i * 2] = (uint8_t)clut[i]; rec[THUMB_CLUT_OFF + i * 2 + 1] = (uint8_t)(clut[i] >> 8); }
    }
    pack_path(id, "title.png", path, sizeof path);
    if (load_title_png(path, rec + TITLE_OFF)) have_title = 1;
    else if (pk->cfg.name[0]) { render_title(pk->cfg.name, rec + TITLE_OFF); have_title = 1; }

    pk->cfg.has_art = have_art; pk->cfg.has_thumb = have_thumb; pk->cfg.has_title = have_title;

    if (have_art || have_thumb || have_title) {
        for (int s = 0; s < REC_SECTORS; s++)
            psx_mod_cd_override_set(REC_LBA(id) + (uint32_t)s, rec + s * SECTOR, SECTOR);
        pk->rec_override = 1;
    } else if (pk->rec_override) {
        for (int s = 0; s < REC_SECTORS; s++) psx_mod_cd_override_clear(REC_LBA(id) + (uint32_t)s);
        pk->rec_override = 0;
    }
    if (have_thumb) {
        static uint8_t sec[SECTOR];
        if (psx_mod_cd_read_stock_sector(THUMB_LBA(id), sec)) {
            memcpy(sec, rec + THUMB_OFF, THUMB_TOTAL);
            psx_mod_cd_override_set(THUMB_LBA(id), sec, SECTOR);
            pk->thumb_override = 1;
        }
    } else if (pk->thumb_override) {
        psx_mod_cd_override_clear(THUMB_LBA(id));
        pk->thumb_override = 0;
    }
    return pk->rec_override || pk->thumb_override;
}

/* The price/password table is one block for all cards, so it is rebuilt
 * from stock whenever any pack's price or password changes. */
static void rebuild_password_table(void)
{
    static uint8_t tab[PW_SECTORS * SECTOR];
    int any = 0;
    for (int s = 0; s < PW_SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(PW_LBA + (uint32_t)s, tab + s * SECTOR)) return;
    for (int id = 1; id <= CARD_COUNT; id++) {
        const Pack *pk = s_packs[id];
        if (!pk || !pk->present) continue;
        uint8_t *e = tab + id * 8;
        if (pk->cfg.price >= 0) {
            const uint32_t v = (uint32_t)pk->cfg.price;
            e[0] = (uint8_t)v; e[1] = (uint8_t)(v >> 8); e[2] = (uint8_t)(v >> 16); e[3] = (uint8_t)(v >> 24);
            any = 1;
        }
        if (pk->cfg.password[0]) {
            const uint32_t v = (uint32_t)strtoul(pk->cfg.password, NULL, 16);   /* digits as BCD */
            e[4] = (uint8_t)v; e[5] = (uint8_t)(v >> 8); e[6] = (uint8_t)(v >> 16); e[7] = (uint8_t)(v >> 24);
            any = 1;
        }
    }
    for (int s = 0; s < PW_SECTORS; s++) {
        if (any) psx_mod_cd_override_set(PW_LBA + (uint32_t)s, tab + s * SECTOR, SECTOR);
        else     psx_mod_cd_override_clear(PW_LBA + (uint32_t)s);
    }
    s_pw_dirty = 0;
}

/* ---- names in guest RAM ------------------------------------------------------
 * Strings are laid out back to back from NAMES_BASE in pack order and never
 * moved; a rename that no longer fits is dropped with a note. Re-laid out
 * from scratch on every reload, which is rare and cheap. */
static void layout_names(void)
{
    uint32_t a = NAMES_BASE;
    for (int id = 1; id <= CARD_COUNT; id++) {
        Pack *pk = s_packs[id];
        if (!pk || !pk->present) continue;
        pk->enc_len = 0;
        if (!pk->cfg.name[0]) continue;
        int n = 0;
        for (; pk->cfg.name[n] && n < PSX_CARD_PACK_NAME_MAX; n++) pk->enc[n] = (uint8_t)encode_char(pk->cfg.name[n]);
        pk->enc[n] = 0xFF;
        if (a + (uint32_t)n + 1u > NAMES_LIMIT) continue;    /* out of room: keep stock name */
        pk->enc_len = n + 1;
        pk->str_addr = a;
        a += (uint32_t)n + 1u;
    }
    s_names_next = a;
    uint32_t d = DESC_BASE;
    for (int id = 1; id <= CARD_COUNT; id++) {
        Pack *pk = s_packs[id];
        if (!pk || !pk->present) continue;
        pk->denc_len = 0;
        if (!pk->cfg.description[0]) continue;
        const int n = encode_desc(pk->cfg.description, pk->denc, (int)sizeof pk->denc);
        if (d + (uint32_t)n > DESC_LIMIT) continue;                  /* out of room: keep stock */
        pk->denc_len = n;
        pk->desc_addr = d;
        d += (uint32_t)n;
    }
}

/* ---- apply / restore ---------------------------------------------------------- */
static void restore_ram(Pack *pk)
{
    if (!pk->stock_ok) return;
    const int id = pk->cfg.id;
    psx_mod_write_word(STATS_STOCK + (uint32_t)(id - 1) * 4u, pk->stock_word);
    psx_mod_write_byte(AUX_STOCK + (uint32_t)id, pk->stock_aux);
    psx_mod_write_half(NAMEOFF_TABLE + (uint32_t)id * 2u, pk->stock_nameoff);
    psx_mod_write_half(DESC_TABLE + (uint32_t)id * 2u, pk->stock_descoff);
    const uint32_t sb = psx_card_extend_stats_base(), ab = psx_card_extend_aux_base();
    if (sb != STATS_STOCK) psx_mod_write_word(sb + (uint32_t)(id - 1) * 4u, pk->stock_word);
    if (ab != AUX_STOCK)   psx_mod_write_byte(ab + (uint32_t)id, pk->stock_aux);
}

static uint32_t target_word(const Pack *pk)
{
    uint32_t w = pk->stock_word;
    const PsxCardPack *c = &pk->cfg;
    if (c->attack >= 0)  w = (w & ~0x1FFu)         | ((uint32_t)(c->attack / 10) & 0x1FFu);
    if (c->defense >= 0) w = (w & ~(0x1FFu << 9))  | (((uint32_t)(c->defense / 10) & 0x1FFu) << 9);
    if (c->star2 >= 0)   w = (w & ~(0xFu << 18))   | (((uint32_t)c->star2 & 0xFu) << 18);
    if (c->star1 >= 0)   w = (w & ~(0xFu << 22))   | (((uint32_t)c->star1 & 0xFu) << 22);
    if (c->type >= 0)    w = (w & ~(0x1Fu << 26))  | (((uint32_t)c->type & 0x1Fu) << 26);
    return w;
}

static uint8_t target_aux(const Pack *pk)
{
    uint8_t a = pk->stock_aux;
    if (pk->cfg.level >= 0)     a = (uint8_t)((a & 0xF0u) | ((uint32_t)pk->cfg.level & 0xFu));
    if (pk->cfg.attribute >= 0) a = (uint8_t)((a & 0x0Fu) | (((uint32_t)pk->cfg.attribute & 0xFu) << 4));
    return a;
}

/* Per frame, for every present pack whose stock snapshot exists. */
static void assert_ram(void)
{
    const uint32_t sb = psx_card_extend_stats_base(), ab = psx_card_extend_aux_base();
    for (int id = 1; id <= CARD_COUNT; id++) {
        Pack *pk = s_packs[id];
        if (!pk || !pk->present) continue;
        if (!take_stock(pk)) continue;
        const uint32_t w = target_word(pk);
        const uint8_t  a = target_aux(pk);
        if (psx_mod_read_word(STATS_STOCK + (uint32_t)(id - 1) * 4u) != w) psx_mod_write_word(STATS_STOCK + (uint32_t)(id - 1) * 4u, w);
        if (sb != STATS_STOCK && psx_mod_read_word(sb + (uint32_t)(id - 1) * 4u) != w) psx_mod_write_word(sb + (uint32_t)(id - 1) * 4u, w);
        if (psx_mod_read_byte(AUX_STOCK + (uint32_t)id) != a) psx_mod_write_byte(AUX_STOCK + (uint32_t)id, a);
        if (ab != AUX_STOCK && psx_mod_read_byte(ab + (uint32_t)id) != a) psx_mod_write_byte(ab + (uint32_t)id, a);
        if (pk->enc_len) {
            for (int k = 0; k < pk->enc_len; k++)
                if (psx_mod_read_byte(pk->str_addr + (uint32_t)k) != pk->enc[k])
                    psx_mod_write_byte(pk->str_addr + (uint32_t)k, pk->enc[k]);
            const uint16_t want = (uint16_t)(pk->str_addr - NAME_SEGMENT);
            if (psx_mod_read_half(NAMEOFF_TABLE + (uint32_t)id * 2u) != want)
                psx_mod_write_half(NAMEOFF_TABLE + (uint32_t)id * 2u, want);
        } else if (psx_mod_read_half(NAMEOFF_TABLE + (uint32_t)id * 2u) != pk->stock_nameoff) {
            psx_mod_write_half(NAMEOFF_TABLE + (uint32_t)id * 2u, pk->stock_nameoff);
        }
        if (pk->denc_len) {
            for (int k = 0; k < pk->denc_len; k++)
                if (psx_mod_read_byte(pk->desc_addr + (uint32_t)k) != pk->denc[k])
                    psx_mod_write_byte(pk->desc_addr + (uint32_t)k, pk->denc[k]);
            const uint16_t want = (uint16_t)(pk->desc_addr - DESC_SEGMENT);
            if (psx_mod_read_half(DESC_TABLE + (uint32_t)id * 2u) != want)
                psx_mod_write_half(DESC_TABLE + (uint32_t)id * 2u, want);
        } else if (psx_mod_read_half(DESC_TABLE + (uint32_t)id * 2u) != pk->stock_descoff) {
            psx_mod_write_half(DESC_TABLE + (uint32_t)id * 2u, pk->stock_descoff);
        }
    }
}

static void note_mtimes(Pack *pk)
{
    static const char *const files[4] = { "card.ini", "art.png", "thumb.png", "title.png" };
    char path[1200];
    for (int i = 0; i < 4; i++) { pack_path(pk->cfg.id, files[i], path, sizeof path); pk->mtime[i] = file_mtime(path); }
}

static int mtimes_changed(const Pack *pk)
{
    static const char *const files[4] = { "card.ini", "art.png", "thumb.png", "title.png" };
    char path[1200];
    for (int i = 0; i < 4; i++) {
        pack_path(pk->cfg.id, files[i], path, sizeof path);
        if (file_mtime(path) != pk->mtime[i]) return 1;
    }
    return 0;
}

/* (Re)load one card's folder. A folder with nothing usable in it means "stock". */
static void load_pack(int id)
{
    if (id < 1 || id > CARD_COUNT || !s_dir_ok) return;
    Pack *pk = s_packs[id];
    if (!pk) {
        pk = (Pack *)calloc(1, sizeof *pk);
        if (!pk) return;
        s_packs[id] = pk;
    }
    const int had_pw = pk->present && (pk->cfg.price >= 0 || pk->cfg.password[0]);
    /* Stock is snapshotted before anything is written; if the tables are not
     * resident yet the per-frame pass takes it later, before its first write. */
    if (pk->present) restore_ram(pk);
    cfg_reset(&pk->cfg, id);
    const int ini = read_ini(id, &pk->cfg);
    char path[1200];
    pack_path(id, "art.png", path, sizeof path);   const int art = file_mtime(path) != 0;
    pack_path(id, "thumb.png", path, sizeof path); const int thumb = file_mtime(path) != 0;
    pack_path(id, "title.png", path, sizeof path); const int title = file_mtime(path) != 0;
    pk->present = ini || art || thumb || title;
    take_stock(pk);
    note_mtimes(pk);
    if (pk->present) {
        build_disc_side(pk);
    } else {
        if (pk->rec_override) for (int s = 0; s < REC_SECTORS; s++) psx_mod_cd_override_clear(REC_LBA(id) + (uint32_t)s);
        if (pk->thumb_override) psx_mod_cd_override_clear(THUMB_LBA(id));
        pk->rec_override = pk->thumb_override = 0;
    }
    if (had_pw || pk->cfg.price >= 0 || pk->cfg.password[0]) s_pw_dirty = 1;
    layout_names();
    psx_card_db_invalidate();
    bump();
}

static void scan_all(void)
{
    for (int id = 1; id <= CARD_COUNT; id++) {
        char path[1200];
        pack_path(id, NULL, path, sizeof path);
        struct stat st;
        const int exists = stat(path, &st) == 0;
        if (exists || (s_packs[id] && s_packs[id]->present)) load_pack(id);
    }
}

/* ---- public ------------------------------------------------------------------- */
const char *psx_card_packs_dir(void) { return s_dir_ok ? s_dir : ""; }
unsigned psx_card_packs_generation(void) { return s_generation; }

int psx_card_packs_get(int id, PsxCardPack *out)
{
    if (id < 1 || id > CARD_COUNT || !s_packs[id] || !s_packs[id]->present) return 0;
    if (out) *out = s_packs[id]->cfg;
    return 1;
}

int psx_card_packs_stock(int id, PsxCardStock *out)
{
    if (id < 1 || id > CARD_COUNT || !out || !psx_card_db_ready()) return 0;
    memset(out, 0, sizeof *out);
    uint32_t w; uint8_t a; const char *name;
    Pack *pk = s_packs[id];
    if (pk && pk->stock_ok) { w = pk->stock_word; a = pk->stock_aux; name = pk->stock_name; }
    else {
        w = psx_mod_read_word(STATS_STOCK + (uint32_t)(id - 1) * 4u);
        a = psx_mod_read_byte(AUX_STOCK + (uint32_t)id);
        name = psx_card_db_name(id);
    }
    snprintf(out->name, sizeof out->name, "%s", name);
    if (pk && pk->stock_ok) snprintf(out->description, sizeof out->description, "%s", pk->stock_desc);
    else decode_text(DESC_SEGMENT + psx_mod_read_half(DESC_TABLE + (uint32_t)id * 2u), out->description, sizeof out->description);
    out->attack = (int)(w & 0x1FFu) * 10;
    out->defense = (int)((w >> 9) & 0x1FFu) * 10;
    out->star2 = (int)((w >> 18) & 0xFu);
    out->star1 = (int)((w >> 22) & 0xFu);
    out->type = (int)((w >> 26) & 0x1Fu);
    out->level = (int)(a & 0xFu);
    out->attribute = (int)(a >> 4);
    /* price / password from the stock table sector holding entry id */
    {
        uint8_t sec[SECTOR];
        const uint32_t off = (uint32_t)id * 8u;
        if (psx_mod_cd_read_stock_sector(PW_LBA + off / SECTOR, sec)) {
            const uint8_t *e = sec + off % SECTOR;
            out->price = (int)((uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24));
            const uint32_t pw = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
            if (pw != 0xFFFFFFFEu && pw != 0xFFFFFFFFu) snprintf(out->password, sizeof out->password, "%08X", pw);
        }
    }
    psx_card_effects_stock(id, out);
    return 1;
}

int psx_card_packs_save(const PsxCardPack *c)
{
    if (!c || c->id < 1 || c->id > CARD_COUNT || !s_dir_ok) return 0;
    char path[1200];
    MKDIR(s_dir);
    pack_path(c->id, NULL, path, sizeof path);
    MKDIR(path);
    pack_path(c->id, "card.ini", path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "; card %d -- written by the Card Manager; hand edits are picked up live\n", c->id);
    if (c->name[0])        fprintf(f, "name = %s\n", c->name);
    if (c->description[0]) fprintf(f, "description = %s\n", c->description);
    if (c->attack >= 0)    fprintf(f, "attack = %d\n", c->attack);
    if (c->defense >= 0)   fprintf(f, "defense = %d\n", c->defense);
    if (c->star1 >= 1)     fprintf(f, "star1 = %s\n", psx_card_packs_star_name(c->star1));
    if (c->star2 >= 1)     fprintf(f, "star2 = %s\n", psx_card_packs_star_name(c->star2));
    if (c->type >= 0)      fprintf(f, "type = %s\n", psx_card_packs_type_name(c->type));
    if (c->level >= 0)     fprintf(f, "level = %d\n", c->level);
    if (c->attribute >= 0) fprintf(f, "attribute = %s\n", psx_card_packs_attribute_name(c->attribute));
    if (c->price >= 0)     fprintf(f, "price = %d\n", c->price);
    if (c->password[0])    fprintf(f, "password = %s\n", c->password);
    if (c->effect >= 0)    fprintf(f, "effect = %s\n", psx_card_packs_effect_name(c->effect));
    if (c->amount >= 0 || (c->effect == PSX_CARD_FX_WEAKEN && c->amount != -1)) fprintf(f, "amount = %d\n", c->amount);
    if (c->target >= 0)    fprintf(f, "target = %s\n", psx_card_packs_type_name(c->target));
    if (c->terrain >= 1)   fprintf(f, "terrain = %s\n", psx_card_packs_terrain_name(c->terrain));
    if (c->equip_bonus >= 0) fprintf(f, "equip_bonus = %d\n", c->equip_bonus);
    if (c->equips_set)     { char b[2048]; psx_card_packs_format_equips(c, b, sizeof b); fprintf(f, "equips = %s\n", b); }
    if (c->boost_set)      { char b[512];  psx_card_packs_format_boost(c, b, sizeof b);  fprintf(f, "boost = %s\n", b); }
    if (c->trap_atk_max >= 0) fprintf(f, "trap_atk_max = %d\n", c->trap_atk_max);
    if (c->ritual_set)     { char b[64];   psx_card_packs_format_ritual(c, b, sizeof b); fprintf(f, "ritual = %s\n", b); }
    fclose(f);
    load_pack(c->id);
    return 1;
}

int psx_card_packs_remove(int id)
{
    if (id < 1 || id > CARD_COUNT || !s_dir_ok) return 0;
    static const char *const files[4] = { "card.ini", "art.png", "thumb.png", "title.png" };
    char path[1200];
    for (int i = 0; i < 4; i++) { pack_path(id, files[i], path, sizeof path); remove(path); }
    pack_path(id, NULL, path, sizeof path);
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
    load_pack(id);
    return 1;
}

void psx_card_packs_reload(int id)
{
    if (id > 0) load_pack(id); else scan_all();
    if (s_pw_dirty) rebuild_password_table();
}

int psx_card_packs_art_rgb(int id, uint8_t *out)
{
    if (id < 1 || id > CARD_COUNT || !out) return 0;
    static uint8_t rec[REC_SECTORS * SECTOR];
    /* the override when present, else stock: read through the same LBAs */
    for (int s = 0; s < REC_SECTORS; s++) {
        const uint32_t lba = REC_LBA(id) + (uint32_t)s;
        int ok = 0;
        if (s_packs[id] && s_packs[id]->rec_override) {
            ok = cdrom_override_get(lba, rec + s * SECTOR);
        }
        if (!ok && !psx_mod_cd_read_stock_sector(lba, rec + s * SECTOR)) return 0;
    }
    rgb_from_indexed(rec, rec + ART_CLUT_OFF, ART_BYTES, out);
    return 1;
}

int psx_card_packs_thumb_rgb(int id, uint8_t *out)
{
    if (id < 1 || id > CARD_COUNT || !out) return 0;
    static uint8_t sec[SECTOR];
    int ok = 0;
    if (s_packs[id] && s_packs[id]->thumb_override) {
        ok = cdrom_override_get(THUMB_LBA(id), sec);
    }
    if (!ok && !psx_mod_cd_read_stock_sector(THUMB_LBA(id), sec)) return 0;
    rgb_from_indexed(sec, sec + THUMB_BYTES, THUMB_BYTES, out);
    return 1;
}

int psx_card_packs_state_json(char *out, unsigned cap)
{
    unsigned n = (unsigned)snprintf(out, cap, "\"dir\":\"%s\",\"generation\":%u,\"overrides\":%u,\"packs\":[",
                                    s_dir, s_generation, cdrom_override_count());
    int first = 1;
    for (int id = 1; id <= CARD_COUNT && n + 64 < cap; id++) {
        const Pack *pk = s_packs[id];
        if (!pk || !pk->present) continue;
        n += (unsigned)snprintf(out + n, cap - n, "%s{\"id\":%d,\"name\":\"%s\",\"rec\":%d,\"thumb\":%d,\"renamed\":%d,\"desc\":%d}",
                                first ? "" : ",", id, pk->cfg.name, pk->rec_override, pk->thumb_override, pk->enc_len > 0, pk->denc_len > 0);
        first = 0;
    }
    n += (unsigned)snprintf(out + n, cap - n, "]");
    return n < cap;
}

/* ---- the frame hook ------------------------------------------------------------ */
static void card_packs_tick(void)
{
    static unsigned frames;
    static int booted;
    if (!psx_mod_game_started()) return;
    if (!booted) {
        const char *dir = psx_mod_player_data_dir();
        if (!dir || !dir[0]) return;
        snprintf(s_dir, sizeof s_dir, "%s/cards", dir);
        s_dir_ok = 1;
        booted = 1;
        scan_all();
        rebuild_password_table();
    }
    frames++;
    /* Hot reload: known packs every second, the whole tree every ten. */
    if ((frames % 60u) == 0u) {
        int changed = 0;
        for (int id = 1; id <= CARD_COUNT; id++)
            if (s_packs[id] && s_packs[id]->present && mtimes_changed(s_packs[id])) { load_pack(id); changed = 1; }
        if ((frames % 600u) == 0u) {
            for (int id = 1; id <= CARD_COUNT; id++) {
                if (s_packs[id] && s_packs[id]->present) continue;
                char path[1200];
                pack_path(id, NULL, path, sizeof path);
                struct stat st;
                if (stat(path, &st) == 0) { load_pack(id); changed = 1; }
            }
        }
        if (changed || s_pw_dirty) rebuild_password_table();
    }
    assert_ram();
}

PSX_MOD_CONSTRUCTOR(psx_card_packs_install)
{
    (void)psx_game_add_frame_hook(card_packs_tick);
}
