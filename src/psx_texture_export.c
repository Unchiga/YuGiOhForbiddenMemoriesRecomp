/* psx_texture_export.c -- write this disc's stock art into a texture pack, so
 * there is something to paint over.
 *
 * This is the other half of psx_wa_catalog.c. That file hashes each asset and
 * registers it under a name; this one decodes the same asset and writes it to
 * a PNG under that same name, both driven by the one table in
 * psx_wa_catalog.h. Extractor and injector therefore cannot disagree about
 * what anything is called -- which they otherwise would, silently, and the
 * only symptom would be art that never appears.
 *
 * IT NEVER OVERWRITES THE PLAYER'S ART
 * -------------------------------------
 * The whole point of the folder is that the player replaces its contents with
 * HD art, so an exporter that clobbered what it found would destroy exactly
 * the work it exists to enable. An upscale is therefore never touched.
 *
 * Its OWN previous output is another matter. A file at exactly the stock size
 * is one of these exports, not a replacement -- painting over art at 1x would
 * be invisible, so nobody does it -- and refreshing those is what makes a fix
 * to the export actually reach the player. Otherwise every improvement to what
 * comes out of here is skipped in silence and the player upscales a stale
 * file. Re-running is safe, and is how you both top up new assets and pick up
 * corrections to old ones.
 *
 * ON DEMAND, NOT A FRAME HOOK
 * -----------------------------
 * psx_texture_export_one() decodes and writes exactly one asset, synchronously,
 * when its caller asks -- psx_asset_manager.c's Export/Export All buttons are
 * the only callers, and Export All spreads its own ~2700 calls one asset (or
 * a handful) per frame from ITS OWN tick(), the same "don't freeze the game"
 * reasoning this file used to apply itself via an automatic "Mods > Export
 * stock textures" background walker. That walker is gone: driving this from
 * the Asset Manager makes it explicit and on-demand instead of a standing
 * frame hook nothing outside the Asset Manager needs.
 *
 * PNG, WITHOUT A COMPRESSOR
 * --------------------------
 * The IDAT is a zlib stream of STORED (uncompressed) deflate blocks. That is a
 * real PNG every viewer and editor accepts, and it needs nothing linked in.
 * The files are bigger than a compressed PNG -- about 40 KB for a card
 * portrait -- which is irrelevant for something the player is about to replace
 * anyway, and cheaper than taking a dependency for it.
 */

#include "psx_texture_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define TX_MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define TX_MKDIR(p) mkdir((p), 0755)
#endif

#include "psx_wa_catalog.h"
#include "texture_pack.h"

#define PATH_MAX_   1024
/* The largest image in the catalog, in TEXELS: the 256x768 UI sheet. The
 * RGBA scratch below is 768 KB of static storage -- big, but paid once and
 * dwarfed by the 1 MB guest VRAM this runtime already carries. */
#define PIXELS_MAX  (256 * 768)
/* One texture page's worth of rows -- the unit the game uploads and palettises
 * tall art in, so also the unit this exports it in. Must match the band size
 * psx_wa_catalog.c registers with. */
#define PAGE_ROWS   256

static unsigned s_written, s_skipped, s_failed;

/* ---- PNG ------------------------------------------------------------------ */
static uint32_t crc_table(uint32_t c)
{
    for (int k = 0; k < 8; k++)
        c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(c & 1)));
    return c;
}

static uint32_t crc32_run(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--)
        crc = crc_table((crc ^ *p++) & 0xFF) ^ (crc >> 8);
    return crc;
}

static uint32_t adler32_run(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n--) {
        a += *p++; if (a >= 65521u) a -= 65521u;
        b += a;    if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[4];
    be32(hdr, len);
    if (fwrite(hdr, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;
    uint32_t crc = crc32_run(0xFFFFFFFFu, (const uint8_t *)type, 4);
    if (len) crc = crc32_run(crc, data, len);
    be32(hdr, crc ^ 0xFFFFFFFFu);
    return fwrite(hdr, 1, 4, f) == 4;
}

static int write_png_rgba(const char *path, const uint8_t *rgba, int w, int h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const size_t row = 1u + (size_t)w * 4u;
    const size_t raw_len = row * (size_t)h;

    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * row] = 0;                 /* filter: none */
        memcpy(raw + (size_t)y * row + 1,
               rgba + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
    }

    const size_t blocks = (raw_len + 65534u) / 65535u;
    uint8_t *z = (uint8_t *)malloc(2u + raw_len + blocks * 5u + 4u);
    if (!z) { free(raw); return 0; }

    size_t p = 0, off = 0, left = raw_len;
    z[p++] = 0x78; z[p++] = 0x01;                 /* zlib header */
    while (left) {
        const unsigned n = (unsigned)(left > 65535u ? 65535u : left);
        z[p++] = (uint8_t)(left <= 65535u);       /* BFINAL, stored */
        z[p++] = (uint8_t)n;        z[p++] = (uint8_t)(n >> 8);
        z[p++] = (uint8_t)(~n);     z[p++] = (uint8_t)((~n) >> 8);
        memcpy(z + p, raw + off, n);
        p += n; off += n; left -= n;
    }
    be32(z + p, adler32_run(raw, raw_len)); p += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); free(raw); return 0; }

    uint8_t ihdr[13];
    be32(ihdr + 0, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* 8 bits per channel */
    ihdr[9] = 6;    /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    int ok = fwrite(sig, 1, 8, f) == 8 &&
             chunk(f, "IHDR", ihdr, sizeof ihdr) &&
             chunk(f, "IDAT", z, (uint32_t)p) &&
             chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(z);
    free(raw);
    return ok;
}

/* ---- paths ---------------------------------------------------------------- */
static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* mkdir -p, in place, on a path that uses '/'. */
static void mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        TX_MKDIR(path);
        *p = '/';
    }
    TX_MKDIR(path);
}

/* The ACTIVE pack's real folder, not a hardcoded name -- texpack_active_dir()
 * (texture_pack.h), the one place every window that reads or writes a pack's
 * files resolves this. This used to reimplement the same fallback locally,
 * which is exactly how the Asset Manager and this exporter could once have
 * quietly agreed on two different folders for a second, or differently
 * named, pack; card art and CPU portraits share this same folder now too
 * (psx_card_packs.c, psx_cpu_data.c), so one definition covers all of them. */
static int pack_root(char *out, size_t cap)
{
    texpack_active_dir(out, (unsigned)cap);
    return out[0] != 0;
}

/* One PNG per ELEMENT, cropped to a rectangle the game actually draws.
 *
 * A UI page is a dense sheet with no blank rows, so every way of cutting it
 * into files is arbitrary -- and the cuts I chose put icons in with labels.
 * The game's draws are not arbitrary: each samples exactly one element, so
 * those rectangles are the real boundaries. Named for the rectangle so the
 * injector can find one from a draw without a lookup table.
 *
 * These are cut from the page image that was just written, so they inherit its
 * per-region palettes and are correct wherever it is. Never overwritten: once
 * a file exists it is the player's to paint. */
/* Reassemble the sheet the way the SCREEN shows it.
 *
 * A UI sheet is stored as scattered pieces and drawn as a picture: the
 * dialogue box's frame line and its hazard-stripe header are each built from
 * several quads out of one sheet. Painting that in sheet layout means
 * painting a jigsaw. So every drawn region is copied to where it actually
 * lands, and regions whose destinations touch are assembled into one canvas
 * -- typically one canvas per thing you can point at on screen.
 *
 * A .map file beside each canvas records piece -> placement, so
 * tools/slice_screen.py can cut a painted canvas back into the per-element
 * files the injector already understands. Assembly here, injection
 * unchanged -- this is purely an authoring convenience.
 *
 * Pieces the game scales are skipped: their source and destination differ in
 * size, so pasting one would distort it. */
/* A multi-region canvas is named by WHERE it landed on screen (screen_X_Y),
 * because that is the only thing known about it in general -- two different
 * screens sharing one asset produce two different, un-guessable positions.
 * But for the handful the disc has already been checked against, the size is
 * a stable fingerprint (a given picture is always the same number of pixels,
 * regardless of which savestate or session captured it), so those get a name
 * a pack author can actually recognise instead of coordinates. Anything not
 * listed here -- including Konami's two screens, not yet captured live at
 * the time this table was written -- keeps the coordinate name; add a row
 * once its canvas size is confirmed by decoding it and looking, the same bar
 * every entry in psx_wa_catalog.c's own table has to clear. */
static const struct { const char *asset; int w, h; const char *label; } SCREEN_NAMES[] = {
    { "ui/logo/front",         321, 169, "logo" },
    { "ui/logo/front",         233,  17, "press_start" },
    { "free_duel/background",  321, 241, "background" },
};
#define SCREEN_NAME_N ((int)(sizeof SCREEN_NAMES / sizeof SCREEN_NAMES[0]))

static const char *screen_label(const char *name, int w, int h)
{
    for (int i = 0; i < SCREEN_NAME_N; i++)
        if (!strcmp(SCREEN_NAMES[i].asset, name) &&
            SCREEN_NAMES[i].w == w && SCREEN_NAMES[i].h == h)
            return SCREEN_NAMES[i].label;
    return NULL;
}

static void write_screens(const char *root, const char *name,
                          const PsxWaAsset *a, int index, int dw, int dh)
{
    if (dw != a->w || dh != a->h)
        return;

    const int entries = (a->bpp == 4) ? 16 : 256;
    const int nr = texpack_clut_rect_count(name);
    if (nr <= 0)
        return;

    /* Wide enough for an 8bpp palette: some UI sheets are 256-entry. */
#define SCR_MAX 256
    static int grp[SCR_MAX];
    static int sx0[SCR_MAX], sy0[SCR_MAX], sx1[SCR_MAX], sy1[SCR_MAX];
    static int dx0[SCR_MAX], dy0[SCR_MAX], dx1[SCR_MAX], dy1[SCR_MAX];
    static uint16_t pals[SCR_MAX][256];
    int n = 0;

    for (int i = 0; i < nr && n < SCR_MAX; i++) {
        uint16_t rp[256];
        int a0, b0, a1, b1, c0, e0, c1, e1;
        if (!texpack_clut_rect_get(name, i, &a0, &b0, &a1, &b1, rp, entries))
            continue;
        if (!texpack_clut_rect_dst(name, i, &c0, &e0, &c1, &e1))
            continue;
        /* Reject only art the game genuinely SCALES. The two rectangles are
         * measured differently -- lim is inclusive texel bounds, so a 32-wide
         * piece reads 0..31, while the screen box comes from vertex positions
         * and reads 100..132 -- so an unscaled draw differs by exactly one in
         * each axis. Demanding equality rejected every piece the first time
         * this was written, so no canvas was ever produced. */
        const int sdw = (a1 - a0) - (c1 - c0), sdh = (b1 - b0) - (e1 - e0);
        if (sdw < -1 || sdw > 1 || sdh < -1 || sdh > 1)
            continue;
        sx0[n]=a0; sy0[n]=b0; sx1[n]=a1; sy1[n]=b1;
        dx0[n]=c0; dy0[n]=e0; dx1[n]=c1; dy1[n]=e1;
        memcpy(pals[n], rp, (size_t)entries * 2u);
        grp[n] = n;
        n++;
    }
    if (!n)
        return;

    /* Group by destination adjacency: pieces of one picture touch or tile
     * end to end. Scattered SOURCE positions alone prove nothing -- a frame's
     * border segments or an icon strip can legitimately be sourced from all
     * over the sheet -- so source position is not checked for an ordinary
     * touch/tile.
     *
     * But when two pieces' destinations actually OVERLAP in pixels (not just
     * sit within the gap), tiling is not what is happening: real composite
     * art tiles its parts edge to edge, it does not draw one part over
     * another. An overlap paired with sources nowhere near each other on the
     * sheet is the signature of two UNRELATED elements the game simply drew
     * at overlapping screen positions (a duel-HUD icon and a number badge,
     * say) -- merging those pastes fragments of one over the other. Require
     * the sources to be plausibly related, too, before allowing that merge;
     * an ordinary non-overlapping touch is unaffected. */
    for (int pass = 0; pass < n; pass++)
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                if (grp[i] == grp[j]) continue;
                const int gap = 4;
                if (!(dx0[i] - gap <= dx1[j] && dx0[j] - gap <= dx1[i] &&
                      dy0[i] - gap <= dy1[j] && dy0[j] - gap <= dy1[i]))
                    continue;
                const int dst_overlaps =
                    dx0[i] <= dx1[j] && dx0[j] <= dx1[i] &&
                    dy0[i] <= dy1[j] && dy0[j] <= dy1[i];
                if (dst_overlaps) {
                    const int src_gap = 24;
                    const int src_near =
                        sx0[i] - src_gap <= sx1[j] && sx0[j] - src_gap <= sx1[i] &&
                        sy0[i] - src_gap <= sy1[j] && sy0[j] - src_gap <= sy1[i];
                    if (!src_near)
                        continue;   /* overlaps on screen, unrelated on the sheet */
                }
                const int lo = grp[i] < grp[j] ? grp[i] : grp[j];
                const int hi = grp[i] < grp[j] ? grp[j] : grp[i];
                for (int k = 0; k < n; k++) if (grp[k] == hi) grp[k] = lo;
            }

    static uint8_t whole[PIXELS_MAX * 4];
    static uint8_t canvas[PIXELS_MAX * 4];

    for (int g = 0; g < n; g++) {
        int members = 0, X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
        for (int i = 0; i < n; i++) {
            if (grp[i] != g) continue;
            if (!members) { X0=dx0[i]; Y0=dy0[i]; X1=dx1[i]; Y1=dy1[i]; }
            else {
                if (dx0[i] < X0) X0 = dx0[i];
                if (dy0[i] < Y0) Y0 = dy0[i];
                if (dx1[i] > X1) X1 = dx1[i];
                if (dy1[i] > Y1) Y1 = dy1[i];
            }
            members++;
        }
        if (!members) continue;
        const int cw = X1 - X0 + 1, chh = Y1 - Y0 + 1;
        if (cw <= 0 || chh <= 0 || (size_t)cw * (size_t)chh * 4u > sizeof canvas)
            continue;

        char cp[PATH_MAX_ + 160], mp[PATH_MAX_ + 160], stem[64];
        const char *label = screen_label(name, cw, chh);
        if (label)
            snprintf(stem, sizeof stem, "%s", label);
        else
            snprintf(stem, sizeof stem, "screen_%d_%d", X0, Y0);
        if (snprintf(cp, sizeof cp, "%s/%s/%s.png", root, name, stem) >= (int)sizeof cp)
            continue;
        snprintf(mp, sizeof mp, "%s/%s/%s.map", root, name, stem);
        if (exists(cp))
            continue;

        memset(canvas, 0, (size_t)cw * (size_t)chh * 4u);
        FILE *mf = fopen(mp, "wb");
        if (mf)
            fprintf(mf, "# canvas %dx%d at screen %d,%d of %s\n"
                        "# src_x src_y w h  canvas_x canvas_y  cluthash\n",
                    cw, chh, X0, Y0, name);

        for (int i = 0; i < n; i++) {
            if (grp[i] != g) continue;
            if (!psx_wa_catalog_decode_pal(a, index, whole, pals[i], entries))
                continue;
            const int w = sx1[i] - sx0[i] + 1, h = sy1[i] - sy0[i] + 1;
            for (int y = 0; y < h; y++) {
                const uint8_t *sp = whole + ((size_t)(sy0[i] + y) * dw + sx0[i]) * 4u;
                uint8_t *cq = canvas + ((size_t)(dy0[i] - Y0 + y) * cw
                                        + (dx0[i] - X0)) * 4u;
                for (int x = 0; x < w; x++)
                    if (sp[x * 4 + 3])          /* layered: keep what is there */
                        memcpy(cq + x * 4, sp + x * 4, 4);
            }
            uint64_t ph[2];
            texpack_hash128(pals[i], (unsigned)entries * 2u, ph);
            if (mf)
                fprintf(mf, "%d %d %d %d  %d %d  %016llx%016llx\n",
                        sx0[i], sy0[i], w, h, dx0[i] - X0, dy0[i] - Y0,
                        (unsigned long long)ph[0], (unsigned long long)ph[1]);
        }
        if (mf) fclose(mf);

        char dir[PATH_MAX_ + 160];
        snprintf(dir, sizeof dir, "%s", cp);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir_p(dir); }
        if (write_png_rgba(cp, canvas, cw, chh)) s_written++;
        else                                     s_failed++;
    }
}

static void write_elements(const char *root, const char *name,
                           const PsxWaAsset *a, int index, int dw, int dh)
{
    if (dw != a->w || dh != a->h)
        return;                 /* retiled art: image coords are not page ones */

    const int entries = (a->bpp == 4) ? 16 : 256;
    const int nr = texpack_clut_rect_count(name);
    static uint8_t crop[PIXELS_MAX * 4];

    for (int i = 0; i < nr; i++) {
        uint16_t rp[256];
        int x0, y0, x1, y1;
        if (!texpack_clut_rect_get(name, i, &x0, &y0, &x1, &y1, rp, entries))
            continue;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= dw) x1 = dw - 1;
        if (y1 >= dh) y1 = dh - 1;
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        if (w <= 0 || h <= 0 || (size_t)w * (size_t)h * 4u > sizeof crop)
            continue;

        /* The palette is part of the identity, not something to choose
         * between. The same rectangle is drawn through more than one CLUT --
         * the Konami wordmark once in black and again as part of a white wash
         * -- and every rule I tried for picking "the real one" (newest, most
         * varied, highest contrast) got some case wrong. So both are exported,
         * named by their palette, and the injector picks by the CLUT the draw
         * actually uses. No guessing, and nothing lost. */
        uint64_t ph[2];
        texpack_hash128(rp, (unsigned)entries * 2u, ph);
        char ep[PATH_MAX_ + 160];
        if (snprintf(ep, sizeof ep, "%s/%s/%d_%d_%dx%d@%016llx%016llx.png",
                     root, name, x0, y0, w, h,
                     (unsigned long long)ph[0], (unsigned long long)ph[1])
                >= (int)sizeof ep)
            continue;
        if (exists(ep))
            continue;

        /* Decoded through this region's own palette rather than cut from the
         * composed page: the page can only show one of the colourings. */
        static uint8_t whole[PIXELS_MAX * 4];
        if (!psx_wa_catalog_decode_pal(a, index, whole, rp, entries))
            continue;
        for (int y = 0; y < h; y++)
            memcpy(crop + (size_t)y * w * 4u,
                   whole + ((size_t)(y0 + y) * dw + x0) * 4u,
                   (size_t)w * 4u);

        char dir[PATH_MAX_ + 160];
        snprintf(dir, sizeof dir, "%s", ep);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir_p(dir); }
        if (write_png_rgba(ep, crop, w, h)) s_written++;
        else                                s_failed++;
    }
}

/* ---- one asset ------------------------------------------------------------ */
/* See psx_texture_export_one() (the public wrapper right below) for the
 * return-value contract; `root` is the caller's chosen destination, not
 * necessarily the active pack's own folder. */
static int export_one(const PsxWaAsset *a, int index, const char *root)
{
    char path[PATH_MAX_ + 128], name[96];

    /* The curated path when this row has one, exactly matching what
     * register_one() (psx_wa_catalog.c) registered it under and what the
     * Asset Manager's own browsing tree shows -- see
     * psx_wa_catalog_display_path()'s header comment. Falls back to the raw
     * disc-family name for anything not curated yet, same as registration. */
    if (!psx_wa_catalog_display_path(a, index, name, sizeof name))
        snprintf(name, sizeof name, a->name_fmt, a->first + index);
    if (snprintf(path, sizeof path, "%s/%s.png", root, name) >= (int)sizeof path) {
        s_failed++;
        return 0;
    }

    int dw, dh;
    psx_wa_catalog_disp_size(a, index, &dw, &dh);

    /* UI art is DUMPED, not reconstructed.
     *
     * A UI sheet's identity is pixels TIMES palette: the same bytes are the
     * black wordmark under one CLUT and the white wash under another, and
     * every attempt to compose "the" sheet out of that -- observed palettes,
     * dominant-coverage fallbacks, contrast heuristics, region repaints,
     * hole-filling -- guessed wrongly for some screen, because the game means
     * all of its colourings at once. So for UI art this writes one file per
     * element per palette, each pixel-for-pixel what a draw actually showed
     * (write_elements), and no whole-sheet file at all. What lands on disk is
     * what was on screen; there is nothing left to infer.
     *
     * The explicitly tinted sheets (the font) skip BOTH and keep only the
     * whole-page export: their workflow is a single alpha mask painted over
     * the full sheet, and that canvas has one honest colouring by
     * construction, precisely because it carries no baked colour of its own.
     * An element or assembled-screen file draws FLAT the moment its exact
     * palette recurs -- correct for art with one true colouring, but it
     * would let a single painted glyph freeze to one colour forever, which
     * is the opposite of what tinting the font buys: dialogue white, a card
     * name in whatever colour its type gives it, from the one file.
     *
     * tint == -1 ALSO skips this, same as tint == 1, and for a different
     * reason: -1 means "never tinted, single fixed colouring" (dialog_frame,
     * the boot logo's hieroglyph layer, ui/panels) -- art with exactly ONE
     * true palette, just not yet a known one. That is not the "pixels times
     * palette" ambiguity this branch exists for; it is the same shape as a
     * card or a background, and belongs on the plain path below with them.
     * The condition used to be `tint != 1`, which caught tint == -1 too and
     * multi-region-dumped these into scattered per-rectangle files with no
     * single picture to look at or paint -- correct only for genuinely
     * multi-palette UI (duel_labels, the menu labels, guardian-star-turned-
     * monster-type icons, card_sleeves/canvas), which all default to
     * tint == 0 and are unaffected by this narrowing. */
    if (psx_wa_catalog_ui(a) && a->tint == 0) {
        write_elements(root, name, a, index, dw, dh);
        /* write_screens() (the assembled screen_X_Y.png + .map convenience
         * canvas) is no longer called here -- nothing in the runtime ever
         * read it, and the individual rect@hash.png files write_elements()
         * just wrote above are the exact same content the injector actually
         * resolves. Disabled per explicit request rather than left running
         * to produce files nobody consumes; the function itself is kept
         * (see its own header) in case the paint-a-whole-canvas authoring
         * workflow is wanted back, at which point tools/slice_screen.py
         * still knows how to cut its output apart. */
        /* Nothing captured yet, but the table might still know a real disc
         * colouring anyway -- some multi-context sheets (the menu labels) do
         * have one confirmed default palette even though other rows/states
         * genuinely need live capture. This is a REFERENCE only, named so it
         * can never satisfy the runtime's exact-match replacement lookup
         * (a plain "<name>.png" at the pack root, or a "rect@hash.png"
         * element) -- it exists purely so a pack author opens an empty
         * folder and sees real colours to paint from instead of nothing. */
        if (texpack_clut_rect_count(name) == 0 && a->clut_entries > 0 &&
            a->ref_whole && dw == a->w && dh == a->h) {
            char rp[PATH_MAX_ + 160];
            if (snprintf(rp, sizeof rp, "%s/%s/_reference.png", root, name)
                    < (int)sizeof rp && !exists(rp)) {
                static uint8_t ref[PIXELS_MAX * 4];
                if (psx_wa_catalog_decode(a, index, ref)) {
                    char dir[PATH_MAX_ + 160];
                    snprintf(dir, sizeof dir, "%s", rp);
                    char *slash = strrchr(dir, '/');
                    if (slash) { *slash = '\0'; mkdir_p(dir); }
                    (void)write_png_rgba(rp, ref, dw, dh);
                }
            }
        }
        /* A sheet can be PARTIALLY captured -- some regions genuinely seen
         * and correctly coloured, others (a different screen's furniture on
         * the same page, never visited this session) still blank -- and
         * until now a partial sheet got nothing for the blank part: the
         * block above only fires when NO region has been captured, so one
         * captured corner silenced the reference for the whole rest of the
         * sheet. ui/panels is the concrete case: the Deck screen's CHEST/
         * ORDER/DECK furniture captures fine, but the in-duel LP boxes live
         * on rows the Deck screen never draws, so they stayed permanently
         * invisible even though the disc bytes for them are right there.
         *
         * This is what psx_wa_catalog_decode_rows() ALREADY does when a
         * catalog entry has no known palette (ui/panels: clut_entries == 0)
         * -- render the index value as a grey ramp instead of inventing a
         * colour. That is safe to always emit, unlike the coloured
         * reference above: a grey ramp can never be confidently WRONG the
         * way a borrowed palette can (the exact "looks complete, quietly
         * wrong" failure ref_whole exists to prevent), so it does not need
         * ref_whole's human-verified opt-in, and unlike the coloured
         * reference it is not silenced by other regions already being
         * captured -- a pack author can see the shape of every uncaptured
         * piece, not just the ones on an empty sheet. */
        /* Extended (2026-09-11) to fire for clut_entries > 0 sheets too, not
         * just clut_entries == 0 -- duel_labels/duel_panels/monster_types all
         * have a real (16-entry) table palette, so they never qualified for
         * this grey fallback before, and any of them whose anchor never
         * validates live (ui/icons/monster_types: permanently blocked by a
         * VRAM reuse collision every session so far) got nothing at all: no
         * coloured reference (needs ref_whole, which is unset -- duel_labels'
         * own comment says outright that ONE static palette is wrong for at
         * least one of its two contexts, so setting it there would be
         * exactly the "looks complete, quietly wrong" mistake ref_whole
         * exists to prevent), and no grey one either (gated to
         * clut_entries == 0 only, until now).
         *
         * Pulls raw indices (psx_wa_catalog_indices), not decode_pal/
         * decode_rows: those two read the table's OWN clut_off palette
         * automatically whenever clut_entries > 0, which would silently
         * reintroduce the exact "confidently wrong colour" problem this
         * block exists to avoid for sheets ref_whole is correctly withheld
         * from -- the whole point is index-only structure, regardless of
         * whether a (possibly wrong-for-this-region) table palette exists. */
        if (dw == a->w && dh == a->h) {
            char rp[PATH_MAX_ + 160];
            if (snprintf(rp, sizeof rp, "%s/%s/_reference.png", root, name)
                    < (int)sizeof rp && !exists(rp)) {
                static uint8_t idx[PIXELS_MAX];
                static uint8_t ref[PIXELS_MAX * 4];
                const uint32_t npix = (uint32_t)dw * (uint32_t)dh;
                if (npix <= sizeof idx &&
                    psx_wa_catalog_indices(a, index, idx, sizeof idx)) {
                    const int max_idx = (a->bpp == 4) ? 15 : 255;
                    for (uint32_t i = 0; i < npix; i++) {
                        uint8_t *q = ref + (size_t)i * 4u;
                        const uint8_t g = (uint8_t)((int)idx[i] * 255 / max_idx);
                        q[0] = q[1] = q[2] = g;
                        q[3] = idx[i] ? 255 : 0;
                    }
                    char dir[PATH_MAX_ + 160];
                    snprintf(dir, sizeof dir, "%s", rp);
                    char *slash = strrchr(dir, '/');
                    if (slash) { *slash = '\0'; mkdir_p(dir); }
                    (void)write_png_rgba(rp, ref, dw, dh);
                }
            }
        }
        return 1;
    }

    /* Never overwrite: an existing file is the player's, whatever its size. */
    if (exists(path)) { s_skipped++; return 1; }

    /* TINTED art (the font: tint == 1, never caught by the tint == 0 UI
     * branch above since that returned already) is a WHITE MASK on
     * transparency by design -- see psx_wa_catalog.h's "WHEN TO SET tint".
     * The colour the game shows it in comes from the CLUT the renderer swaps
     * in per draw, never from this file. Nothing below this point ever
     * checked psx_wa_catalog_tinted(a): the table's own disc palette (font
     * has a real one, clut_entries == 16) was being decoded and baked in
     * unconditionally, so the export came out whatever colour the disc
     * happens to store the glyphs in -- red, here -- instead of neutral
     * white.
     *
     * Alpha comes from the INDEX VALUE itself, not a binary in/out test: a
     * PS1 font's index levels ARE its anti-aliasing (0 transparent, 15/255
     * fully opaque, everything between a soft edge) -- collapsing that to a
     * hard on/off mask would throw the edge smoothing away and hand a pack
     * author jagged glyphs to repaint from. Scaled to the bpp's real index
     * range so a 4bpp sheet's 0-15 and an 8bpp sheet's 0-255 both reach full
     * opacity, not just 4bpp topping out at 15/255. */
    if (psx_wa_catalog_tinted(a)) {
        static uint8_t idx[PIXELS_MAX];
        static uint8_t mask[PIXELS_MAX * 4];
        const uint32_t npix = (uint32_t)dw * (uint32_t)dh;
        if (npix > sizeof idx || (size_t)npix * 4u > sizeof mask ||
            !psx_wa_catalog_indices(a, index, idx, sizeof idx)) {
            s_failed++;
            return 0;
        }
        const int max_idx = (a->bpp == 4) ? 15 : 255;
        for (uint32_t i = 0; i < npix; i++) {
            uint8_t *q = mask + (size_t)i * 4u;
            const uint8_t alpha = (uint8_t)((int)idx[i] * 255 / max_idx);
            q[0] = q[1] = q[2] = 255;
            q[3] = alpha;
        }
        char dir[PATH_MAX_ + 128];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdir_p(dir); }
        if (write_png_rgba(path, mask, dw, dh)) { s_written++; return 1; }
        s_failed++;
        return 0;
    }

    /* The table's palette when it has one, else the one palette the renderer
     * saw -- good enough for the single-colouring art that reaches here. */
    uint16_t pal[256];
    int pal_n = 0;
    if (!a->clut_entries) {
        pal_n = (a->bpp == 4) ? 16 : 256;
        if (!texpack_observed_clut(name, pal, pal_n))
            pal_n = 0;          /* never drawn yet: grey ramp */
    }

    static uint8_t rgba[PIXELS_MAX * 4];
    if ((size_t)dw * (size_t)dh * 4u > sizeof rgba) {
        s_failed++;
        return 0;
    }

    /* Art taller than one texture page is uploaded and palettised a band at a
     * time; backgrounds untile as a whole and are one palette by construction. */
    const int bands = (dh == a->h) ? (a->h + PAGE_ROWS - 1) / PAGE_ROWS : 1;
    if (bands > 1) {
        for (int b = 0; b < bands; b++) {
            const int row0 = b * PAGE_ROWS;
            const int rows = (a->h - row0 < PAGE_ROWS) ? a->h - row0 : PAGE_ROWS;
            uint16_t bp[256];
            int bn = (a->bpp == 4) ? 16 : 256;
            if (!texpack_observed_clut_page(name, row0, bp, bn))
                bn = 0;
            if (!psx_wa_catalog_decode_rows(a, index, rgba, row0, rows,
                                            bn ? bp : (pal_n ? pal : NULL),
                                            bn ? bn : pal_n)) {
                s_failed++;
                return 0;
            }
        }
    } else if (!psx_wa_catalog_decode_pal(a, index, rgba,
                                          pal_n ? pal : NULL, pal_n)) {
        s_failed++;
        return 0;
    }

    char dir[PATH_MAX_ + 128];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }

    if (write_png_rgba(path, rgba, dw, dh)) { s_written++; return 1; }
    s_failed++;
    return 0;
}

int psx_texture_export_one(const PsxWaAsset *a, int index, const char *root)
{
    if (!a || !root || !*root) return 0;
    return export_one(a, index, root);
}

int  psx_texture_export_write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    return write_png_rgba(path, rgba, w, h);
}
void psx_texture_export_mkdir_p(char *path) { mkdir_p(path); }
