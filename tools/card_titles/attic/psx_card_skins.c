/* psx_card_skins.c -- EXPERIMENT: replace a stock card's name and library art.
 *
 * Player files, in the player-data folder (next to drop_table_edits.ini):
 *     card_skins/names.ini        <card id> = <new name>      (one per line)
 *     card_skins/<id>.png         library art, any size, scaled to 102x96
 *
 * NAMES. A card's name is string id 0x8000+id: the u16 at 0x801D5800+id*2 is
 * an offset from 0x801D0000 to a 0xFF-terminated string in the game's own
 * frequency-ordered character codes (psx_card_extend.c documents the table).
 * A renamed card gets a fresh string written into 0x801D9C00.. (zero in every
 * sampled state; psx_card_extend.c's own strings stop well below) and its
 * table entry repointed. Both are re-asserted per frame because the name
 * table is re-streamed from disc per screen.
 *
 * ART. The library card page streams the card's 276-sector disc block, unpacks
 * the art to a transient buffer (102x96 8bpp followed by a 256-entry CLUT,
 * measured at 0x801DC000 / 0x801DE640) and uploads it with LoadImage
 * (0x8007F978) as rect (256,256,51,96) plus CLUT rect (512,240,256,1). Instead
 * of reverse-engineering the packing, LoadImage's entry is redirected through
 * a stub that swaps the SOURCE pointer to the mod's DMA-visible buffer when the
 * rect is one of those two and the buffer's flag word is set. The host fills
 * the buffer (PNG -> median-cut 256 colours -> indices + 15-bit CLUT) whenever
 * the library cursor (u16 0x8009B258, 0-based) lands on a skinned card while
 * the library is the mode (0x8009B26C == 0xC4). No disc or save bytes change. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_fusion_font.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../psxrecomp/runtime/third_party/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../psxrecomp/runtime/third_party/stb_truetype.h"
#include <math.h>

/* ---- guest facts ---------------------------------------------------------- */
#define MODE_BYTE        0x8009B26Cu
#define MODE_LIBRARY     0xC4u
#define MODE_CHEST       0xC7u            /* build deck / trunk; Triangle opens the same viewer */
#define LIB_PAGE_CARD    0x801D5608u      /* u16 card id while a library page is open (F128) */
#define CHEST_VIEW_ID    0x8009B246u      /* u16 card id, the viewer mailbox */
#define NAMEOFF_TABLE    0x801D5800u
#define NAME_SEGMENT     0x801D0000u
#define NAMES_BASE       0x801D9C00u      /* .. NAMES_LIMIT, ours */
#define NAMES_LIMIT      0x801DA000u
#define LOADIMAGE        0x8007F978u
#define LOADIMAGE_W0     0x27BDFFE0u      /* addiu sp,sp,-32 */
#define LOADIMAGE_W1     0xAFB00010u      /* sw s0,16(sp)    */
#define STUB_ADDR        0x801CFF00u      /* .. 0x801CFFC0 free (chest stub above) */
#define ART_W 102
#define ART_H 96
#define ART_BYTES (ART_W * ART_H)         /* 9792 */
#define CLUT_BYTES 512
#define BUF_BYTES (16 + ART_BYTES + CLUT_BYTES)
/* Main RAM only: block DMA masks addresses into the 2 MB, so the GPU-DMA
 * aperture is NOT usable as a LoadImage source (measured: it read the card
 * block instead). 0x801D0880..0x801D3200 sits between the live save struct
 * and its mirror and reads zero in every sampled state. */
#define BUF_ADDR         0x801D0900u
typedef char skins_buf_fits[(BUF_ADDR + BUF_BYTES <= 0x801D3200u) ? 1 : -1];
/* The card's baked TITLE: 96x14 4bpp uploaded as rect (256,352,24,14), drawn
 * through the 16-entry greyscale CLUT at (480,248) (index 1 lightest .. 7
 * darkest, 0 transparent). Lives in the reclaimed 0x801CD800..0x801CFE00 gap
 * (psx_card_extend's tables end at 0x801CD7E0; psx_free_duel_rows starts at
 * 0x801CFE00), addressed from the art buffer with one signed immediate. */
#define TITLE_W 96
#define TITLE_H 14
#define TITLE_BYTES (TITLE_W / 2 * TITLE_H)   /* 672 */
#define TITLE_ADDR       (BUF_ADDR - 0x2900u)  /* 0x801CE000 */
typedef char skins_title_fits[(TITLE_ADDR >= 0x801CD800u && TITLE_ADDR + TITLE_BYTES <= 0x801CFE00u) ? 1 : -1];
#define MAX_SKINS 64
#define SKIN_NAME_MAX  40

#define J(a) (0x08000000u | (((a) & 0x0FFFFFFFu) >> 2))

/* ---- name encoding (the game's code table) ------------------------------ */
static const char CODE_TABLE[0x5A] = {
    ' ','e','t','a','o','i','n','s','r','h','l','.','d','u','m','c',
    'g','y','w','f','p','b','k', 0 ,'A','v','I','\'','T','S','M',',',
    'D','O','W','H','Y','E','R', 0 , 0 ,'G','L','C','N','B', 0 ,'P',
    '-','F','z','K','j','U','x','q','0','V','2','J','#','1','Q','Z',
     0 ,'3','5','&', 0 ,'7','X', 0 , 0 , 0 ,'4', 0 , 0 , 0 ,'6', 0 ,
     0 , 0 , 0 , 0 , 0 , 0 , 0 ,'8', 0 ,'9'
};
static int encode_char(char c) {
    for (int i = 0; i < (int)sizeof CODE_TABLE; i++)
        if (CODE_TABLE[i] && CODE_TABLE[i] == c) return i;
    return 0; /* unknown -> space */
}

/* ---- config --------------------------------------------------------------- */
typedef struct {
    uint16_t id;
    uint8_t  enc[SKIN_NAME_MAX + 1];   /* 0xFF-terminated, 0 len = no rename */
    uint8_t  enc_len;
    char     ascii[SKIN_NAME_MAX + 1];
    uint32_t str_addr;            /* where the string lives in guest RAM */
    int      has_art;
} Skin;

static Skin     s_skins[MAX_SKINS];
static int      s_nskins;
static int      s_loaded;
static uint32_t s_buf;            /* guest DMA buffer, 0 until allocated */
static int      s_buf_id = -1;    /* card id currently in the buffer */
static char     s_dir[1024];

static Skin *find_skin(uint32_t id) {
    for (int i = 0; i < s_nskins; i++) if (s_skins[i].id == id) return &s_skins[i];
    return NULL;
}
static Skin *add_skin(uint32_t id) {
    Skin *s = find_skin(id);
    if (s) return s;
    if (s_nskins >= MAX_SKINS || id < 1 || id > 722) return NULL;
    s = &s_skins[s_nskins++]; memset(s, 0, sizeof *s); s->id = (uint16_t)id;
    return s;
}

static void load_config(void) {
    const char *dir = psx_mod_player_data_dir();
    if (!dir || !dir[0]) return;
    snprintf(s_dir, sizeof s_dir, "%s/card_skins", dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/names.ini", s_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ';' || *p == '#' || *p == '[' || *p < '0' || *p > '9') continue;
            unsigned id = (unsigned)strtoul(p, &p, 10);
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p);
            while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            Skin *s = add_skin(id);
            if (!s || !*p) continue;
            int n = 0;
            for (; p[n] && n < SKIN_NAME_MAX; n++) { s->enc[n] = (uint8_t)encode_char(p[n]); s->ascii[n] = p[n]; }
            s->ascii[n] = 0; s->enc[n] = 0xFF; s->enc_len = (uint8_t)(n + 1);
        }
        fclose(f);
    }
    /* art: probe <id>.png for every id 1..722 (cheap: one stat-like fopen each) */
    for (unsigned id = 1; id <= 722; id++) {
        snprintf(path, sizeof path, "%s/%u.png", s_dir, id);
        FILE *g = fopen(path, "rb");
        if (!g) continue;
        fclose(g);
        Skin *s = add_skin(id);
        if (s) s->has_art = 1;
    }
    /* lay the strings out back to back */
    uint32_t a = NAMES_BASE;
    for (int i = 0; i < s_nskins; i++) {
        if (!s_skins[i].enc_len) continue;
        if (a + s_skins[i].enc_len > NAMES_LIMIT) { s_skins[i].enc_len = 0; continue; }
        s_skins[i].str_addr = a; a += s_skins[i].enc_len;
    }
    s_loaded = 1;
}

/* ---- names: per-frame assert ---------------------------------------------- */
static void assert_names(void) {
    for (int i = 0; i < s_nskins; i++) {
        const Skin *s = &s_skins[i];
        if (!s->enc_len) continue;
        for (uint32_t k = 0; k < s->enc_len; k++)
            if (psx_mod_read_byte(s->str_addr + k) != s->enc[k])
                psx_mod_write_byte(s->str_addr + k, s->enc[k]);
        const uint16_t want = (uint16_t)(s->str_addr - NAME_SEGMENT);
        if (psx_mod_read_half(NAMEOFF_TABLE + s->id * 2u) != want)
            psx_mod_write_half(NAMEOFF_TABLE + s->id * 2u, want);
    }
}

/* ---- art: PNG -> 8bpp + CLUT ---------------------------------------------- */
typedef struct { uint8_t r, g, b; } Rgb;

/* median cut over the image's pixels into <= 256 boxes */
typedef struct { int lo, hi; } Box;
static Rgb  s_px[ART_W * ART_H];
static int  s_order[ART_W * ART_H];
static int  s_axis;
static int cmp_px(const void *a, const void *b) {
    const Rgb *pa = &s_px[*(const int *)a], *pb = &s_px[*(const int *)b];
    int va = s_axis == 0 ? pa->r : s_axis == 1 ? pa->g : pa->b;
    int vb = s_axis == 0 ? pb->r : s_axis == 1 ? pb->g : pb->b;
    return va - vb;
}
static void quantize(uint8_t *idx_out, uint16_t *clut_out) {
    const int n = ART_W * ART_H;
    for (int i = 0; i < n; i++) s_order[i] = i;
    Box boxes[256]; int nb = 1; boxes[0].lo = 0; boxes[0].hi = n;
    while (nb < 256) {
        /* split the box with the largest channel range */
        int best = -1, bestrange = -1, bestaxis = 0;
        for (int b = 0; b < nb; b++) {
            if (boxes[b].hi - boxes[b].lo < 2) continue;
            int mn[3] = {255,255,255}, mx[3] = {0,0,0};
            for (int i = boxes[b].lo; i < boxes[b].hi; i++) {
                const Rgb *p = &s_px[s_order[i]];
                int c[3] = {p->r, p->g, p->b};
                for (int k = 0; k < 3; k++) { if (c[k] < mn[k]) mn[k] = c[k]; if (c[k] > mx[k]) mx[k] = c[k]; }
            }
            for (int k = 0; k < 3; k++)
                if (mx[k] - mn[k] > bestrange) { bestrange = mx[k] - mn[k]; best = b; bestaxis = k; }
        }
        if (best < 0 || bestrange == 0) break;
        s_axis = bestaxis;
        qsort(&s_order[boxes[best].lo], (size_t)(boxes[best].hi - boxes[best].lo), sizeof(int), cmp_px);
        int mid = (boxes[best].lo + boxes[best].hi) / 2;
        boxes[nb].lo = mid; boxes[nb].hi = boxes[best].hi; boxes[best].hi = mid; nb++;
    }
    for (int b = 0; b < nb; b++) {
        long r = 0, g = 0, bl = 0; int cnt = boxes[b].hi - boxes[b].lo;
        for (int i = boxes[b].lo; i < boxes[b].hi; i++) {
            const Rgb *p = &s_px[s_order[i]]; r += p->r; g += p->g; bl += p->b;
            idx_out[s_order[i]] = (uint8_t)b;
        }
        if (cnt) { r /= cnt; g /= cnt; bl /= cnt; }
        uint16_t c = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10));
        if (c == 0) c = 0x8000;   /* opaque black; 0x0000 is transparent */
        clut_out[b] = c;
    }
    for (int b = nb; b < 256; b++) clut_out[b] = 0x8000;
}

static int build_art(uint32_t id, uint8_t *idx, uint16_t *clut) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%u.png", s_dir, id);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (8 << 20)) { fclose(f); return 0; }
    unsigned char *file = (unsigned char *)malloc((size_t)sz);
    if (!file || fread(file, 1, (size_t)sz, f) != (size_t)sz) { free(file); fclose(f); return 0; }
    fclose(f);
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 3);
    free(file);
    if (!img) return 0;
    /* nearest-neighbour scale to 102x96 */
    for (int y = 0; y < ART_H; y++)
        for (int x = 0; x < ART_W; x++) {
            int sx = x * w / ART_W, sy = y * h / ART_H;
            const unsigned char *p = img + (sy * w + sx) * 3;
            s_px[y * ART_W + x].r = p[0]; s_px[y * ART_W + x].g = p[1]; s_px[y * ART_W + x].b = p[2];
        }
    stbi_image_free(img);
    quantize(idx, clut);
    return 1;
}

/* ---- the LoadImage stub ----------------------------------------------------
 * Entry (displaced prologue), then: flag word at buf+0 (bit0 art, bit1 title).
 *   rect (256,256) -> a1 = buf+16            (art, 102x96 8bpp)   bit0
 *   rect (256,352) -> a1 = TITLE_ADDR        (title, 96x14 4bpp)  bit1
 *   rect (512,240) -> a1 = buf+16+9792       (art CLUT)           bit0
 * k0/k1 are free here; a1 is consumed by `move s1,a1` right after the
 * displaced words, which is why the swap must happen at the entry. */
static void assert_stub(void) {
    const uint32_t hi = (s_buf >> 16) + ((s_buf & 0x8000u) ? 1u : 0u);
    const uint32_t lo = s_buf & 0xFFFFu;
    const uint32_t w[] = {
        LOADIMAGE_W0, LOADIMAGE_W1,
        0x3C1A0000u | hi,                    /*  2 lui   k0,hi          */
        0x275A0000u | lo,                    /*  3 addiu k0,k0,lo       */
        0x8F5B0000u,                         /*  4 lw    k1,0(k0) flags */
        0x13600022u,                         /*  5 beqz  k1,skip(40)    */
        0x949B0000u,                         /*  6 lhu   k1,0(a0) (ds)  */
        0x277BFF00u,                         /*  7 addiu k1,k1,-256     */
        0x17600013u,                         /*  8 bnez  k1,clut(28)    */
        0x949B0002u,                         /*  9 lhu   k1,2(a0) (ds)  */
        0x277BFF00u,                         /* 10 addiu k1,k1,-256     */
        0x1360000Au,                         /* 11 beqz  k1,art(22)     */
        0x00000000u,                         /* 12                      */
        0x277BFFA0u,                         /* 13 addiu k1,k1,-96      */
        0x17600019u,                         /* 14 bnez  k1,skip(40)    */
        0x00000000u,                         /* 15                      */
        0x8F5B0000u,                         /* 16 title: lw k1,0(k0)   */
        0x337B0002u,                         /* 17 andi  k1,k1,2        */
        0x13600015u,                         /* 18 beqz  k1,skip(40)    */
        0x00000000u,                         /* 19                      */
        J(STUB_ADDR + 4u * 40u),             /* 20 j     skip           */
        0x27450000u | ((TITLE_ADDR - s_buf) & 0xFFFFu), /* 21 addiu a1,k0,title (ds) */
        0x8F5B0000u,                         /* 22 art: lw k1,0(k0)     */
        0x337B0001u,                         /* 23 andi  k1,k1,1        */
        0x1360000Fu,                         /* 24 beqz  k1,skip(40)    */
        0x00000000u,                         /* 25                      */
        J(STUB_ADDR + 4u * 40u),             /* 26 j     skip           */
        0x27450010u,                         /* 27 addiu a1,k0,16 (ds)  */
        0x949B0000u,                         /* 28 clut: lhu k1,0(a0)   */
        0x277BFE00u,                         /* 29 addiu k1,k1,-512     */
        0x17600009u,                         /* 30 bnez  k1,skip(40)    */
        0x949B0002u,                         /* 31 lhu   k1,2(a0) (ds)  */
        0x277BFF10u,                         /* 32 addiu k1,k1,-240     */
        0x17600006u,                         /* 33 bnez  k1,skip(40)    */
        0x00000000u,                         /* 34                      */
        0x8F5B0000u,                         /* 35 lw    k1,0(k0)       */
        0x337B0001u,                         /* 36 andi  k1,k1,1        */
        0x13600002u,                         /* 37 beqz  k1,skip(40)    */
        0x00000000u,                         /* 38                      */
        0x27450000u | (16u + ART_BYTES),     /* 39 addiu a1,k0,16+9792  */
        J(LOADIMAGE + 8u),                   /* 40 skip: j LoadImage+8  */
        0x00000000u,                         /* 41                      */
    };
    for (uint32_t i = 0; i < sizeof w / sizeof w[0]; i++)
        if (psx_mod_read_word(STUB_ADDR + 4u * i) != w[i])
            psx_mod_write_code_word(STUB_ADDR + 4u * i, w[i]);
    if (psx_mod_read_word(LOADIMAGE) == LOADIMAGE_W0 &&
        psx_mod_read_word(LOADIMAGE + 4u) == LOADIMAGE_W1) {
        psx_mod_write_code_word(LOADIMAGE, J(STUB_ADDR));
        psx_mod_write_code_word(LOADIMAGE + 4u, 0u);
    }
}
typedef char skins_stub_fits[(STUB_ADDR + 42u * 4u <= 0x801CFFC0u) ? 1 : -1];

/* ---- title: name -> 96x14 4bpp ---------------------------------------------
 * Measured against all 722 stock titles (tools/... see findings F127): the
 * originals are Times New Roman BOLD at 12 px (em), baseline on row 11, pen
 * starting at x=3, greyscale coverage mapped onto the 8-step CLUT ramp, and
 * any name wider than ~90 px squeezed horizontally to fit columns 3..93.
 * Konami's rasteriser adds an etched contrast we do not reproduce; shapes,
 * weight and spacing match to within a pixel. The font is the player's own
 * copy (card_skins/timesbd.ttf); without it the duel text font is used. */
#define TITLE_PX      12.0f
#define TITLE_BASE_Y  11
#define TITLE_X0      3
#define TITLE_MAX_W   90
#define TITLE_CANVAS  512
static unsigned char *s_ttf;      /* file bytes, or NULL */
static stbtt_fontinfo s_font;
static int s_font_ok = -1;        /* -1 untried */
/* coverage (0..255) thresholds for CLUT levels 1..7, fitted by quantile
 * matching over the stock titles' ink pixels */
static const int TITLE_LUT_THR[7] = { 1, 60, 76, 127, 152, 164, 244 };

static int title_font_load(void) {
    if (s_font_ok >= 0) return s_font_ok;
    s_font_ok = 0;
    char path[1200]; snprintf(path, sizeof path, "%s/timesbd.ttf", s_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (16 << 20)) { fclose(f); return 0; }
    s_ttf = (unsigned char *)malloc((size_t)sz);
    if (!s_ttf || fread(s_ttf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(s_ttf); s_ttf = NULL; return 0; }
    fclose(f);
    if (!stbtt_InitFont(&s_font, s_ttf, stbtt_GetFontOffsetForIndex(s_ttf, 0))) { free(s_ttf); s_ttf = NULL; return 0; }
    s_font_ok = 1;
    return 1;
}

static int title_quantize(float cov) {
    const int c = (int)(cov + 0.5f);
    int lvl = 0;
    for (int k = 0; k < 7; k++) if (c >= TITLE_LUT_THR[k]) lvl = k + 1;
    return lvl;
}

/* Fallback: the 8x12 duel text font, core -> 7, edges -> 3. */
static void render_title_fallback(const char *text, uint8_t nib[TITLE_H][TITLE_W]) {
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
                nib[gy + 1][dx] = (uint8_t)(v >= 8 ? 7 : 3);
            }
        x += hi - lo + 2;
    }
}

static void render_title(const char *text, uint8_t *out /* TITLE_BYTES */) {
    uint8_t nib[TITLE_H][TITLE_W];
    memset(nib, 0, sizeof nib);
    if (!title_font_load()) {
        render_title_fallback(text, nib);
    } else {
        static unsigned char canvas[TITLE_H][TITLE_CANVAS];
        static unsigned char glyph[64 * 64];
        memset(canvas, 0, sizeof canvas);
        const float scale = stbtt_ScaleForMappingEmToPixels(&s_font, TITLE_PX);
        float pen = (float)TITLE_X0;
        int ink_lo = TITLE_CANVAS, ink_hi = -1;
        for (const char *p = text; *p; p++) {
            const int cp = (unsigned char)*p;
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
            if (cp != ' ') {
                const float xf = pen - floorf(pen);
                int x0, y0, x1, y1;
                stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, xf, 0.0f, &x0, &y0, &x1, &y1);
                const int gw = x1 - x0, gh = y1 - y0;
                if (gw > 0 && gh > 0 && gw <= 64 && gh <= 64) {
                    stbtt_MakeCodepointBitmapSubpixel(&s_font, glyph, gw, gh, gw, scale, scale, xf, 0.0f, cp);
                    const int bx = (int)floorf(pen) + x0, by = TITLE_BASE_Y + y0;
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
        }
        if (ink_hi >= ink_lo) {
            const int natural = ink_hi - ink_lo + 1;
            if (natural <= TITLE_MAX_W) {
                for (int y = 0; y < TITLE_H; y++)
                    for (int x = 0; x < TITLE_W; x++)
                        nib[y][x] = (uint8_t)title_quantize((float)canvas[y][x]);
            } else {
                /* squeeze the ink run to TITLE_MAX_W columns starting at x=3
                 * (bilinear), then stretch contrast back so the darkest pixel
                 * is full coverage again -- averaging alone washes a long
                 * title out below the grey thresholds. */
                static float sq[TITLE_H][TITLE_MAX_W];
                const float step = (float)natural / (float)TITLE_MAX_W;
                float peak = 1.0f;
                for (int y = 0; y < TITLE_H; y++)
                    for (int x = 0; x < TITLE_MAX_W; x++) {
                        const float sx = (float)ink_lo + ((float)x + 0.5f) * step - 0.5f;
                        int i0 = (int)floorf(sx); float t = sx - (float)i0; int i1 = i0 + 1;
                        if (i0 < 0) i0 = 0; if (i1 > TITLE_CANVAS - 1) i1 = TITLE_CANVAS - 1;
                        sq[y][x] = (float)canvas[y][i0] * (1.0f - t) + (float)canvas[y][i1] * t;
                        if (sq[y][x] > peak) peak = sq[y][x];
                    }
                const float gain = 255.0f / peak;
                for (int y = 0; y < TITLE_H; y++)
                    for (int x = 0; x < TITLE_MAX_W; x++)
                        nib[y][TITLE_X0 + x] = (uint8_t)title_quantize(sq[y][x] * gain);
            }
        }
    }
    for (int y = 0; y < TITLE_H; y++)
        for (int bx = 0; bx < TITLE_W / 2; bx++)
            out[y * (TITLE_W / 2) + bx] = (uint8_t)(nib[y][bx * 2] | (nib[y][bx * 2 + 1] << 4));
}

static void fill_buffer(int id) {
    static uint8_t  idx[ART_BYTES];
    static uint16_t clut[256];
    const Skin *sk = find_skin((uint32_t)id);
    uint32_t flags = 0;
    if (sk && sk->has_art && build_art((uint32_t)id, idx, clut)) flags |= 1u;
    if (sk && sk->ascii[0]) flags |= 2u;
    if (flags & 1u) for (uint32_t i = 0; i < ART_BYTES; i += 4)
        psx_mod_write_word(s_buf + 16u + i,
            (uint32_t)idx[i] | ((uint32_t)idx[i+1] << 8) | ((uint32_t)idx[i+2] << 16) | ((uint32_t)idx[i+3] << 24));
    if (flags & 1u) for (uint32_t i = 0; i < 256; i += 2)
        psx_mod_write_word(s_buf + 16u + ART_BYTES + i * 2u, (uint32_t)clut[i] | ((uint32_t)clut[i+1] << 16));
    if (flags & 2u) {
        static uint8_t title[TITLE_BYTES];
        render_title(sk->ascii, title);
        for (uint32_t i = 0; i < TITLE_BYTES; i += 4)
            psx_mod_write_word(TITLE_ADDR + i,
                (uint32_t)title[i] | ((uint32_t)title[i+1] << 8) | ((uint32_t)title[i+2] << 16) | ((uint32_t)title[i+3] << 24));
    }
    psx_mod_write_word(s_buf + 4u, (uint32_t)id);
    psx_mod_write_word(s_buf + 8u, flags);
    psx_mod_write_word(s_buf, flags);
    s_buf_id = id;
}

static void card_skins_tick(void) {
    if (!psx_mod_game_started()) return;
    if (!s_loaded) load_config();
    if (!s_nskins) return;
    assert_names();
    if (!s_buf) { s_buf = BUF_ADDR; psx_mod_write_word(s_buf, 0); }
    assert_stub();
    int want = -1;
    {
        const uint8_t mode = psx_mod_read_byte(MODE_BYTE);
        int id = -1;
        if (mode == MODE_LIBRARY) id = (int)psx_mod_read_half(LIB_PAGE_CARD);
        else if (mode == MODE_CHEST) id = (int)psx_mod_read_half(CHEST_VIEW_ID);
        const Skin *s = id > 0 ? find_skin((uint32_t)id) : NULL;
        if (s && (s->has_art || s->ascii[0])) want = id;
    }
    if (want < 0) { if (psx_mod_read_word(s_buf)) psx_mod_write_word(s_buf, 0); return; }
    if (want != s_buf_id) fill_buffer(want);
    else if (!psx_mod_read_word(s_buf)) psx_mod_write_word(s_buf, psx_mod_read_word(s_buf + 8u));
}

PSX_MOD_CONSTRUCTOR(psx_card_skins_install) {
    (void)psx_game_add_frame_hook(card_skins_tick);
}
