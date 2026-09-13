/* texture_pack.c -- see texture_pack.h.
 *
 * THE THREE THINGS THIS KEEPS TRACK OF
 * -------------------------------------
 *   assets   what the title told us its art is called, keyed by content hash.
 *            Pure data, set up once, never invalidated.
 *   regions  where art currently LIVES in VRAM. Built from uploads, torn down
 *            whenever anything overwrites the rectangle. This is the part that
 *            has to be correct or replacements attach to the wrong pixels.
 *   entries  replacement images from the active pack, placed in the atlas.
 *            Loaded lazily, on the first draw that actually wants one, so a
 *            720-card pack does not cost 720 decodes at boot.
 *
 * WHY REGIONS EXPIRE
 * -------------------
 * VRAM is one megabyte that the game reuses constantly: the same address holds
 * a card portrait now and a menu panel a moment later. A region therefore
 * means "as of the last write, these words are that asset", and ANY later
 * write over them ends it. Uploads, fills and VRAM->VRAM copies all invalidate,
 * which is why the facade forwards all three rather than just transfers. Miss
 * one and a pack eventually paints the wrong art onto something -- the failure
 * is rare, timing-dependent and horrible to chase, so the invalidation is
 * deliberately blunt: any overlap at all kills the region.
 *
 * THE RESOLVE CACHE
 * ------------------
 * texpack_on_draw() runs per primitive, and a scene is thousands of them, so
 * the answer is cached against the primitive's texture state. A generation
 * counter bumps on every invalidation, which retires the whole cache at a
 * stroke -- correct by construction and cheaper than tracking which entries a
 * particular rectangle touched.
 *
 * NOTHING HERE PRINTS. psxrecomp/CLAUDE.md rule 3 forbids printf/fprintf in
 * runtime source outright: inspection goes through the TCP debug server, not
 * stdout. So the things worth knowing -- how many assets the title registered,
 * how many regions are live, and every reason a replacement was refused -- are
 * COUNTED here and served by texpack_state_json(), the same shape the rest of
 * this title uses (see psx_card_colors_state_json). A silently skipped
 * replacement is the most likely thing to go wrong with a pack, so the refusal
 * counters are the ones that matter: they turn "my art does not show up" into
 * a number that says which of the three reasons it was.
 */

#include "texture_pack.h"

#include <stdio.h>              /* snprintf only -- see the note below */
#include <stdlib.h>
#include <string.h>
#include <time.h>               /* scan_dir()'s wall-clock budget */

#include "mod_plugins.h"        /* psx_mod_player_data_dir */
/* The stb implementation lives in psx_window_icon.cpp and is built with
 * STBI_NO_STDIO, so the filename-based stbi_load() does not exist -- only
 * stbi_load_from_memory(). These defines must match that TU or the
 * declarations here would promise functions nothing ever compiled. Reading the
 * file first is the better shape anyway: it keeps the size cap and the I/O
 * failure in one place instead of inside the decoder. */
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "../psxrecomp/runtime/third_party/stb_image.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define TP_MKDIR(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  define TP_MKDIR(p) mkdir((p), 0755)
#endif

#define TP_MAX_ASSETS   4096
#define TP_MAX_REGIONS   256
#define TP_MAX_ENTRIES  2048
#define TP_MAX_PACKS      16
#define TP_MAX_FILES    8192
#define TP_NAME_MAX       96
#define TP_PATH_MAX      512
#define TP_CACHE         512
#define TP_ATLAS_DIM    4096

/* ---- assets: what the title named ---------------------------------------- */
typedef struct {
    uint64_t      hash;
    char          name[TP_NAME_MAX];
    int           w, h;           /* size the game samples (TILED, hashed) */
    int           disp_w, disp_h; /* size on disk; == w,h unless retile != 0 */
    int           page_w, page_h; /* whole image after retile */
    int           src_x, src_y;   /* this page's origin within that image */
    TexPackRetile retile;         /* display -> tiled, or 0 for raw assets */
    /* Art the game uploads ONCE, before this framework is watching, can never
     * be caught by hashing transfers -- the only transfer that carried it has
     * already happened. Such an asset instead names the VRAM rectangle it is
     * known to occupy, and is confirmed there by hashing that rectangle and
     * comparing against the disc bytes. Position finds it; content still
     * proves it, so a wrong guess about the position fails loudly rather than
     * binding a region to whatever happens to sit there.
     * anch_w == 0 means "not anchored: match by upload only". */
    int           anch_x, anch_y; /* VRAM words */
    int           anch_w, anch_h; /* VRAM words / rows */
    /* Where the game last read this art's palette from. Some art's CLUT is
     * not stored next to its pixels and has resisted being found on disc, so
     * the exporter would have to write it as a grey index map -- which then
     * REPLACES the properly coloured stock art, leaving the game looking
     * worse than untouched. The renderer knows the answer: every draw carries
     * its CLUT coordinates. Recording them lets the exporter read the real
     * colours out of VRAM instead of guessing at them. */
    /* A SNAPSHOT of the palette, taken when the art was drawn -- not the
     * coordinates it lived at. Coordinates were the original bug: CLUT VRAM is
     * reused constantly, so by the time an export asked, those words held some
     * other screen's palette. ui/panels came back as pure red and green ramps
     * that way, and every colour downstream of it was wrong. */
    int           clut_snap;      /* slot in s_clutsnap, or -1 */
    int           clut_n;         /* entries captured */
    int           clut_seen;
    int           tint;           /* see texpack_set_tinted() */
    int           track_cluts;    /* record per-region palettes for this one */
    int           no_plain;       /* see texpack_set_no_plain_fallback() */
} TpAsset;

static TpAsset  s_asset[TP_MAX_ASSETS];
static int      s_asset_n;

/* Palette snapshots. Only art with no palette in the title's own table ever
 * needs one, which is a handful of UI sheets, so a small pool costs far less
 * than 256 words on every asset. */
#define TP_CLUTSNAP 32
static uint16_t s_clutsnap[TP_CLUTSNAP][256];
static int      s_clutsnap_n;

/* WHICH PALETTE, WHERE.
 *
 * One palette per asset is not enough to describe a sheet the game draws
 * through several: the dialogue frame's carved border is stone and its panel
 * fill is navy, from the same pixels. But every draw tells us both the CLUT
 * and the uv rectangle it covers, so the palettes can be recorded PER REGION
 * and an export composed from them -- each part coloured the way the game
 * actually colours it.
 *
 * Rectangles are in whole-image texel space so the exporter can use them
 * directly. Deduped on rect+palette, since the same draw repeats every frame. */
/* Generous on purpose. The dialogue frame alone is 11 rectangles, and a sheet
 * is only fully described once every screen that draws it has been visited --
 * so this fills up over a session rather than at once, and running out again
 * silently truncates the very thing it is meant to record.
 *
 * 512 was not generous enough: a real session visiting dialog_frame, panels,
 * duel_labels, the font, both logo screens, free_duel/background AND the
 * menu labels all share this one pool, and texpack_state.py --cluts caught
 * it completely full (512/512) with menu_labels_1/2 still only partly
 * captured -- every region recorded after the pool filled was silently
 * dropped, for every asset, not just the one being watched. Raised 8x; each
 * entry is ~550 bytes (mostly the 256-entry uint16 pal[]), so 4096 is ~2.2MB,
 * trivial on a desktop target. */
#define TP_CLUTRECT 4096
static struct {
    int      asset;
    int      x0, y0, x1, y1;      /* inclusive, image texels */
    int      d0, e0, d1, e1;      /* where it last landed on screen */
    int      has_dst;
    int      n;                   /* palette entries */
    uint16_t pal[256];
} s_clutrect[TP_CLUTRECT];
static int s_clutrect_n;

static void note_clut_rect(int asset, int x0, int y0, int x1, int y1,
                           const uint16_t *pal, int n, const int *dst)
{
    if (x1 < x0 || y1 < y0 || n <= 0)
        return;

    /* Swallow rectangles that another already covers, when the palette is the
     * same. The game's uv bounds wobble by a texel between draws -- the same
     * element arrives as 127x31 and again as 128x32 -- and it also draws a
     * piece of a region it draws whole elsewhere. Each of those became its own
     * exported file, so the pack filled with near-duplicates of one element.
     * A contained rectangle under the same palette describes nothing new; a
     * containing one replaces what it covers. Different palettes are always
     * kept apart, since that is a different colouring, not a duplicate. */
    for (int i = 0; i < s_clutrect_n; i++) {
        if (s_clutrect[i].asset != asset || s_clutrect[i].n != n ||
            memcmp(s_clutrect[i].pal, pal, (size_t)n * 2u) != 0)
            continue;
        if (x0 >= s_clutrect[i].x0 && y0 >= s_clutrect[i].y0 &&
            x1 <= s_clutrect[i].x1 && y1 <= s_clutrect[i].y1)
            return;                     /* already covered */
        if (x0 <= s_clutrect[i].x0 && y0 <= s_clutrect[i].y0 &&
            x1 >= s_clutrect[i].x1 && y1 >= s_clutrect[i].y1) {
            s_clutrect[i].x0 = x0; s_clutrect[i].y0 = y0;
            s_clutrect[i].x1 = x1; s_clutrect[i].y1 = y1;
            return;                     /* grew to cover the old one */
        }
    }

    /* Same rectangle, same palette: already known. Same rectangle under a
     * DIFFERENT palette is not a duplicate and is never merged -- it is a
     * different appearance of the same pixels, and choosing between them was
     * the mistake this design replaces. Every heuristic tried (newest, most
     * varied, widest contrast) picked wrongly for some screen, because the
     * game means both: the Konami wordmark in black is as real as the white
     * wash over it. Both are kept; both export; the CLUT of each draw picks
     * the right one at run time. */
    for (int i = 0; i < s_clutrect_n; i++) {
        if (s_clutrect[i].asset == asset && s_clutrect[i].x0 == x0 &&
            s_clutrect[i].y0 == y0 && s_clutrect[i].x1 == x1 &&
            s_clutrect[i].y1 == y1 && s_clutrect[i].n == n &&
            memcmp(s_clutrect[i].pal, pal, (size_t)n * 2u) == 0)
            return;
    }

    if (s_clutrect_n >= TP_CLUTRECT)
        return;
    s_clutrect[s_clutrect_n].asset = asset;
    s_clutrect[s_clutrect_n].x0 = x0; s_clutrect[s_clutrect_n].y0 = y0;
    s_clutrect[s_clutrect_n].x1 = x1; s_clutrect[s_clutrect_n].y1 = y1;
    s_clutrect[s_clutrect_n].n = n;
    memcpy(s_clutrect[s_clutrect_n].pal, pal, (size_t)n * 2u);
    s_clutrect[s_clutrect_n].has_dst = dst ? 1 : 0;
    if (dst) {
        s_clutrect[s_clutrect_n].d0 = dst[0]; s_clutrect[s_clutrect_n].e0 = dst[1];
        s_clutrect[s_clutrect_n].d1 = dst[2]; s_clutrect[s_clutrect_n].e1 = dst[3];
    }
    s_clutrect_n++;
}

int texpack_clut_json(char *out, unsigned cap)
{
    unsigned n = (unsigned)snprintf(out, cap, "\"rects\":%d,\"list\":[",
                                    s_clutrect_n);
    if (n >= cap) return 0;
    /* "p1" alone (index 1) was a fingerprint, not a diagnostic -- it cannot
     * distinguish a real 16-colour gradient from 16 garbage words that happen
     * to agree at one index. "pal" is the FULL captured palette, entry 0
     * (usually transparent) through entry n-1, hex-encoded in one string so
     * a person or tool can look at the whole thing at once. */
    /* Worst case: 256 entries * 4 hex chars, 8bpp, plus the terminator. Was
     * 513 -- half what 256 entries actually need -- which silently cut the
     * dump off around entry 128 with no sign anything was missing; that was
     * the only reason the wrong-depth capture bug below was still readable
     * as a plausible-looking gradient instead of obviously running past a
     * 16-entry 4bpp palette into more data than a 4bpp asset could have. */
    char palhex[1025];
    for (int i = 0; i < s_clutrect_n; i++) {
        const int ai = s_clutrect[i].asset;
        const int pn = (s_clutrect[i].n > 256) ? 256 : s_clutrect[i].n;
        int ph = 0;
        for (int e = 0; e < pn && ph + 4 < (int)sizeof palhex; e++)
            ph += snprintf(palhex + ph, sizeof palhex - ph, "%04x", s_clutrect[i].pal[e]);
        unsigned k = (unsigned)snprintf(out + n, cap - n,
            "%s{\"name\":\"%s\",\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,"
            "\"p1\":\"%04x\",\"pal\":\"%s\"}",
            i ? "," : "",
            (ai >= 0 && ai < s_asset_n) ? s_asset[ai].name : "?",
            s_clutrect[i].x0, s_clutrect[i].y0,
            s_clutrect[i].x1, s_clutrect[i].y1, s_clutrect[i].pal[1], palhex);
        if (k >= cap - n) break;      /* truncate the list, not the response */
        n += k;
    }
    return (unsigned)snprintf(out + n, cap - n, "]") < cap - n;
}

int texpack_clut_rect_count(const char *name)
{
    if (!name) return 0;
    int k = 0;
    for (int i = 0; i < s_clutrect_n; i++) {
        const int ai = s_clutrect[i].asset;
        if (ai >= 0 && ai < s_asset_n && strcmp(s_asset[ai].name, name) == 0)
            k++;
    }
    return k;
}

int texpack_clut_rect_dst(const char *name, int which,
                          int *x0, int *y0, int *x1, int *y1)
{
    if (!name || which < 0)
        return 0;
    for (int i = 0; i < s_clutrect_n; i++) {
        const int ai = s_clutrect[i].asset;
        if (ai < 0 || ai >= s_asset_n || strcmp(s_asset[ai].name, name) != 0)
            continue;
        if (which-- > 0)
            continue;
        if (!s_clutrect[i].has_dst)
            return 0;
        *x0 = s_clutrect[i].d0; *y0 = s_clutrect[i].e0;
        *x1 = s_clutrect[i].d1; *y1 = s_clutrect[i].e1;
        return 1;
    }
    return 0;
}

int texpack_clut_rect_get(const char *name, int which,
                          int *x0, int *y0, int *x1, int *y1,
                          uint16_t *pal, int entries)
{
    if (!name || which < 0)
        return 0;
    for (int i = 0; i < s_clutrect_n; i++) {
        const int ai = s_clutrect[i].asset;
        if (ai < 0 || ai >= s_asset_n || strcmp(s_asset[ai].name, name) != 0)
            continue;
        if (which-- > 0)
            continue;
        if (s_clutrect[i].n < entries)
            return 0;
        *x0 = s_clutrect[i].x0; *y0 = s_clutrect[i].y0;
        *x1 = s_clutrect[i].x1; *y1 = s_clutrect[i].y1;
        if (pal && entries > 0)
            memcpy(pal, s_clutrect[i].pal, (size_t)entries * 2u);
        return 1;
    }
    return 0;
}

/* ---- regions: where art currently lives in VRAM --------------------------- */
typedef struct {
    int      x, y, w, h;        /* VRAM words */
    int      asset;             /* index into s_asset, or -1 */
} TpRegion;

static TpRegion s_region[TP_MAX_REGIONS];
static int      s_region_n;
static unsigned s_generation = 1;

/* Bumped ONLY when the active pack's file list is (re)scanned (activation or
 * a manual reload) -- see reload_pack_files(). s_generation above is not a
 * substitute: it also bumps on every single VRAM upload and draw-cache
 * invalidation (many times a frame), so a caller that wants "the pack just
 * became ready, re-check what it has" needs its own, far coarser counter.
 * texpack_file_scan_generation() exposes it. */
static unsigned s_file_scan_gen;

/* ---- pack files and loaded entries ---------------------------------------- */
typedef struct {
    char name[TP_NAME_MAX];     /* pack-relative, no extension */
    char path[TP_PATH_MAX];
} TpFile;

typedef struct {
    int      file;              /* index into s_file */
    int      src_x, src_y;      /* which page of that file this entry is */
    int      w, h;              /* replacement pixels */
    int      scale;
    int      atlas_x, atlas_y;
    uint8_t *rgba;              /* owned */
    int      pending;           /* backend has not uploaded it yet */
} TpEntry;

static TpFile   s_file[TP_MAX_FILES];
static int      s_file_n;
static TpEntry  s_entry[TP_MAX_ENTRIES];
static int      s_entry_n;

static char     s_pack_name[TP_MAX_PACKS][TP_NAME_MAX];
static char     s_pack_path[TP_MAX_PACKS][TP_PATH_MAX];
static int      s_pack_n;
static int      s_pack_active = -1;
static int      s_enabled = 1;

static const uint16_t *s_vram;

/* atlas shelf allocator */
static int s_shelf_x, s_shelf_y, s_shelf_h;

/* Why replacements were refused, for texpack_state_json(). */
static unsigned s_stat_decode_fail;    /* PNG would not decode */
static unsigned s_stat_scale_reject;   /* not a whole multiple of the source */
static unsigned s_stat_atlas_full;     /* no room left in the atlas */
static unsigned s_stat_missing;        /* indexed, then deleted from disk */
/* Draw-path counters. Region counts above come from UPLOADS, so they say
 * nothing about whether a draw ever resolved -- these do. */
static unsigned s_stat_resolved;       /* draws that found a region */
static unsigned s_stat_clut_skip;      /* ...whose CLUT could not be captured */
/* Diagnostic only: the raw flag and vertex colour of the LAST successfully
 * replaced primitive. gpu_gl_renderer.c has both already (they are the game's
 * own GPU-command state, unrelated to replacement), and they answer a real
 * question the decomp project's own notes raised -- this dialogue is drawn
 * with SetPolyGT4 (Gouraud-shaded textured quads), which can tint a texture
 * with a per-vertex colour the replacement path has never accounted for. If
 * raw==0 and col is far from white, that tint is the missing piece: the
 * shader already multiplies replacement output by it, so a non-white tint
 * here explains a colour mismatch with no bug in the hash/palette machinery
 * at all. */
static int   s_dbg_last_raw = -1;
static float s_dbg_last_col[3];
void texpack_debug_note_prim(int rawtex, const float col[3])
{
    s_dbg_last_raw = rawtex ? 1 : 0;
    s_dbg_last_col[0] = col[0]; s_dbg_last_col[1] = col[1]; s_dbg_last_col[2] = col[2];
}
static int      s_stat_clut_last[3];   /* last skipped: depth, clut_x, clut_y */
static unsigned s_stat_uploads;        /* transfers seen */
static unsigned s_stat_matched;        /* ...that named a registered asset */
static unsigned s_stat_anchor_ok;      /* anchored assets confirmed in VRAM */
static unsigned s_stat_anchor_miss;    /* ...anchored but not what was there */
static uint64_t s_stat_anchor_hash;    /* what the last miss found instead */
/* Distinct upload SHAPES seen (deduped by w x h), newest kept, with a hit
 * count. This is the honest way to answer "what shape does a background
 * arrive in, and does it ever match" without one giant full-VRAM blit hiding
 * everything smaller. */
#define TP_SHAPES 24
static struct { int w, h; unsigned seen, matched; } s_shape[TP_SHAPES];
static int s_shape_n;

/* Strip coalescer. The game uploads a background not as one 128x512 page but
 * as 32 vertically-stacked 128x16 strips, one transfer each -- so the
 * whole-page hash never matches an upload. This tracks a run of same-x,
 * same-width, vertically-contiguous uploads and keeps a ROLLING hash of the
 * bytes seen so far. Because FNV-1a is incremental, that rolling hash equals a
 * registered asset's hash exactly when the strips have assembled precisely
 * that asset -- at which point the whole page is matched and a region emitted,
 * as if it had arrived in one transfer. */
static int      s_bld_x = -1, s_bld_w, s_bld_y0, s_bld_yend;
static uint64_t s_bld_hash;
/* Diagnostics: the tallest strip-run ever assembled, and of runs that reached
 * at least 512 rows (background height) how many matched a registration. Tells
 * "strips never assemble a full page" (interleaved) from "they assemble but I
 * registered the wrong bytes". */
static int      s_bld_max_h;
static unsigned s_bld_tall_runs, s_bld_tall_matched;
static uint64_t s_bld_tall_hash;   /* hash of the last unmatched >=512 run */
static int      s_bld_tall_x, s_bld_tall_y;

/* Log of finished strip runs -- a run ends when it matches, or when the next
 * upload does not continue it. Recording geometry AND rolling hash is what
 * lets an asset that the game splits across VRAM be identified offline: the
 * hash of each piece can be searched for on the disc. */
#define TP_RUNLOG 10
static struct { int x, y, w, h, matched; uint64_t hash; } s_runlog[TP_RUNLOG];
static int s_runlog_n, s_runlog_head;

/* Textured draws that found no containing region, deduped by texture state.
 * A region can be live and still never be drawn FROM -- if the glyphs sample
 * a page we did not register, this is where that shows up, as a texpage with
 * a high count and no match.
 *
 * A REAL RING, not a first-N-wins cap. 12 slots used to fill permanently
 * with whatever ran first in a session and then silently refuse every new
 * distinct pattern for the rest of it -- diagnosing "what's unresolved on
 * THIS screen, right now" was impossible once a long session had already
 * saturated the table with patterns from screens visited earlier. Evicting
 * the OLDEST slot when full (same head-rotation shape as s_runlog below)
 * means the most recently discovered distinct patterns are always visible,
 * which is what a live "why isn't this asset drawing" check actually needs.
 * Raised the size too: 12 was tight even for a short session once several
 * assets are being investigated in the same run. */
#define TP_UNRES 64
static struct { int bx, by, depth, x0, y0, x1, y1; unsigned n; } s_unres[TP_UNRES];
static int s_unres_n, s_unres_head;

static void note_unresolved(int bx, int by, int depth,
                            int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < s_unres_n; i++)
        if (s_unres[i].bx == bx && s_unres[i].by == by &&
            s_unres[i].depth == depth) { s_unres[i].n++; return; }
    const int slot = s_unres_head;
    s_unres[slot].bx = bx; s_unres[slot].by = by;
    s_unres[slot].depth = depth;
    s_unres[slot].x0 = x0; s_unres[slot].y0 = y0;
    s_unres[slot].x1 = x1; s_unres[slot].y1 = y1;
    s_unres[slot].n = 1;
    s_unres_head = (s_unres_head + 1) % TP_UNRES;
    if (s_unres_n < TP_UNRES) s_unres_n++;
}

/* VRAM-to-VRAM copies whose SOURCE overlaps a registered region -- see
 * texpack_note_copy()'s header comment in texture_pack.h for why this exists
 * at all. Deduped on (asset, destination): the same background redrawing the
 * same compositing step every frame is one fact, not one entry per frame. */
#define TP_COPYLOG 12
static struct { int asset; int sx, sy, sw, sh; int dx, dy; unsigned n; } s_copylog[TP_COPYLOG];
static int s_copylog_n;

void texpack_note_copy(int sx, int sy, int w, int h, int dx, int dy)
{
    if (w <= 0 || h <= 0 || !s_region_n)
        return;
    const int sx1 = sx + w - 1, sy1 = sy + h - 1;
    for (int i = 0; i < s_region_n; i++) {
        const TpRegion *r = &s_region[i];
        if (sx > r->x + r->w - 1 || sx1 < r->x ||
            sy > r->y + r->h - 1 || sy1 < r->y)
            continue;               /* source does not touch this region */
        int found = 0;
        for (int k = 0; k < s_copylog_n; k++)
            if (s_copylog[k].asset == r->asset &&
                s_copylog[k].dx == dx && s_copylog[k].dy == dy) {
                s_copylog[k].n++;
                found = 1;
                break;
            }
        if (!found && s_copylog_n < TP_COPYLOG) {
            s_copylog[s_copylog_n].asset = r->asset;
            s_copylog[s_copylog_n].sx = sx; s_copylog[s_copylog_n].sy = sy;
            s_copylog[s_copylog_n].sw = w;  s_copylog[s_copylog_n].sh = h;
            s_copylog[s_copylog_n].dx = dx; s_copylog[s_copylog_n].dy = dy;
            s_copylog[s_copylog_n].n = 1;
            s_copylog_n++;
        }
    }
}

static void runlog_add(int x, int y, int w, int h, int matched, uint64_t hash)
{
    if (h <= 0) return;
    int i = s_runlog_head;
    s_runlog[i].x = x; s_runlog[i].y = y; s_runlog[i].w = w; s_runlog[i].h = h;
    s_runlog[i].matched = matched; s_runlog[i].hash = hash;
    s_runlog_head = (s_runlog_head + 1) % TP_RUNLOG;
    if (s_runlog_n < TP_RUNLOG) s_runlog_n++;
}

static void bld_reset(void) { s_bld_x = -1; }

static void note_shape(int w, int h, int matched)
{
    for (int i = 0; i < s_shape_n; i++)
        if (s_shape[i].w == w && s_shape[i].h == h) {
            s_shape[i].seen++;
            if (matched) s_shape[i].matched++;
            return;
        }
    if (s_shape_n < TP_SHAPES) {
        s_shape[s_shape_n].w = w; s_shape[s_shape_n].h = h;
        s_shape[s_shape_n].seen = 1; s_shape[s_shape_n].matched = matched ? 1 : 0;
        s_shape_n++;
    }
}

/* A page of one repeated value carries no identity: 32 KB of zeroes occurs
 * 1117 times in this title's art file alone, so ANY blank VRAM page would
 * "match" whichever blank asset happened to register first and get painted
 * with its replacement. Uniform content is therefore never matched and never
 * registered -- there is nothing there to replace. */
static int uniform16(const uint16_t *p, unsigned n)
{
    for (unsigned i = 1; i < n; i++)
        if (p[i] != p[0])
            return 0;
    return 1;
}

/* ---- hashing -------------------------------------------------------------- */
#define FNV_OFFSET 1469598103934665603ULL
static uint64_t fnv_update(uint64_t h, const void *data, unsigned len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (unsigned i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t texpack_hash(const void *data, unsigned len)
{
    /* FNV-1a. The title hashes disc bytes with this same function, so both
     * sides must see the same byte order -- which they do, since a PS1 upload
     * payload is little-endian halfwords and so is the disc it came from.
     * Incremental (fnv_update) so a run of strip uploads can be hashed as one
     * page -- see the strip coalescer in texpack_on_upload. */
    return fnv_update(FNV_OFFSET, data, len);
}

int texpack_register_asset_page(const char *name, uint64_t hash,
                                int w, int h, int disp_w, int disp_h,
                                int page_w, int page_h,
                                int src_x, int src_y, TexPackRetile retile)
{
    if (!name || !*name || w <= 0 || h <= 0 || disp_w <= 0 || disp_h <= 0)
        return 0;
    if (s_asset_n >= TP_MAX_ASSETS)
        return 0;
    for (int i = 0; i < s_asset_n; i++)
        if (s_asset[i].hash == hash)
            return 1;
    TpAsset *a = &s_asset[s_asset_n++];
    a->hash = hash; a->w = w; a->h = h;
    a->disp_w = disp_w; a->disp_h = disp_h;
    a->page_w = page_w; a->page_h = page_h;
    a->src_x = src_x; a->src_y = src_y;
    a->retile = retile;
    a->anch_x = a->anch_y = a->anch_w = a->anch_h = 0;
    a->clut_snap = -1;
    a->clut_n = a->clut_seen = 0;
    a->tint = 0;
    a->track_cluts = 0;
    a->no_plain = 0;
    snprintf(a->name, sizeof a->name, "%s", name);
    return 1;
}

int texpack_register_asset_tiled(const char *name, uint64_t hash,
                                 int w, int h, int disp_w, int disp_h,
                                 TexPackRetile retile)
{
    if (!name || !*name || w <= 0 || h <= 0 || disp_w <= 0 || disp_h <= 0)
        return 0;
    if (s_asset_n >= TP_MAX_ASSETS)
        return 0;
    for (int i = 0; i < s_asset_n; i++)
        if (s_asset[i].hash == hash)
            return 1;           /* first name for a hash wins */
    TpAsset *a = &s_asset[s_asset_n++];
    a->hash = hash;
    a->w = w;
    a->h = h;
    a->disp_w = disp_w;
    a->disp_h = disp_h;
    a->page_w = w;
    a->page_h = h;
    a->src_x = 0;
    a->src_y = 0;
    a->retile = retile;
    a->anch_x = a->anch_y = a->anch_w = a->anch_h = 0;
    a->clut_snap = -1;
    a->clut_n = a->clut_seen = 0;
    a->tint = 0;
    a->track_cluts = 0;
    a->no_plain = 0;
    snprintf(a->name, sizeof a->name, "%s", name);
    return 1;
}

int texpack_register_asset(const char *name, uint64_t hash, int w, int h)
{
    return texpack_register_asset_tiled(name, hash, w, h, w, h, 0);
}

int texpack_register_asset_anchored(const char *name, uint64_t hash,
                                    int w, int h,
                                    int vram_x, int vram_y,
                                    int vram_w, int vram_h)
{
    if (vram_w <= 0 || vram_h <= 0)
        return 0;
    const int before = s_asset_n;
    if (!texpack_register_asset(name, hash, w, h))
        return 0;
    if (s_asset_n == before)
        return 1;               /* this hash was already named; leave it be */
    TpAsset *a = &s_asset[s_asset_n - 1];
    a->anch_x = vram_x; a->anch_y = vram_y;
    a->anch_w = vram_w; a->anch_h = vram_h;
    return 1;
}

static int asset_by_hash(uint64_t hash)
{
    for (int i = 0; i < s_asset_n; i++)
        if (s_asset[i].hash == hash)
            return i;
    return -1;
}

/* ---- pack discovery ------------------------------------------------------- */
static void add_file(const char *rel, const char *full)
{
    if (s_file_n >= TP_MAX_FILES)
        return;
    TpFile *f = &s_file[s_file_n++];
    snprintf(f->name, sizeof f->name, "%s", rel);
    snprintf(f->path, sizeof f->path, "%s", full);
}

/* Wall-clock budget for one whole scan_dir() tree walk, reset by whoever
 * starts one (reload_pack_files()). A player's pack folder can be huge, or
 * sit on a slow or cloud-synced drive where every single stat()/readdir()
 * call carries real latency -- a starvation-watchdog abort seconds after
 * launch, from exactly this walk running unbounded during boot, is what
 * this guards against (found 2026-09-13). Once the deadline passes every
 * level bails out without recursing further, so a slow or huge tree yields
 * whatever it found in the time it had rather than blocking indefinitely;
 * the pack is still usable for everything the partial scan did reach. */
#define TP_SCAN_BUDGET_SECONDS 3
static int   s_scan_over_budget;
static time_t s_scan_deadline;

/* Walk a pack directory, recording every .png under it by its path relative to
 * the pack root and minus the extension -- which is exactly the name the title
 * registered, so lookup is a string compare and nothing has to agree on a
 * naming convention twice. */
static void scan_dir(const char *root, const char *sub)
{
    if (s_scan_over_budget)
        return;
    if (time(NULL) >= s_scan_deadline) { s_scan_over_budget = 1; return; }

    char dir[TP_PATH_MAX];
    if (sub && *sub)
        snprintf(dir, sizeof dir, "%s/%s", root, sub);
    else
        snprintf(dir, sizeof dir, "%s", root);

#ifdef _WIN32
    char glob[TP_PATH_MAX];
    snprintf(glob, sizeof glob, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (s_scan_over_budget) break;
        if (time(NULL) >= s_scan_deadline) { s_scan_over_budget = 1; break; }
        const char *nm = fd.cFileName;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        char rel[TP_NAME_MAX];
        if (sub && *sub) snprintf(rel, sizeof rel, "%s/%s", sub, nm);
        else             snprintf(rel, sizeof rel, "%s", nm);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(root, rel);
        } else {
            size_t n = strlen(rel);
            if (n > 4 && !strcmp(rel + n - 4, ".png")) {
                char full[TP_PATH_MAX];
                snprintf(full, sizeof full, "%s/%s", root, rel);
                rel[n - 4] = '\0';
                add_file(rel, full);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (s_scan_over_budget) break;
        if (time(NULL) >= s_scan_deadline) { s_scan_over_budget = 1; break; }
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        char rel[TP_NAME_MAX];
        if (sub && *sub) snprintf(rel, sizeof rel, "%s/%s", sub, nm);
        else             snprintf(rel, sizeof rel, "%s", nm);
        char full[TP_PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", root, rel);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(root, rel);
        } else {
            size_t n = strlen(rel);
            if (n > 4 && !strcmp(rel + n - 4, ".png")) {
                rel[n - 4] = '\0';
                add_file(rel, full);
            }
        }
    }
    closedir(dp);
#endif
}

/* One pack, always: <player-data>/textures, registered so psx_wa_catalog.c's
 * build_catalog() auto-activates it at boot ("one pack directory means that
 * pack is the one you meant") -- the same folder texpack_active_dir()'s
 * fallback already names on its own, so this just makes it live instead of
 * only ever agreed-upon by construction.
 *
 * This used to be unconditional here too, until 2026-09-13: a player whose
 * player-data lives under a slow or cloud-synced folder hit a starvation-
 * watchdog abort seconds after launch, from scan_dir() -- the recursive walk
 * that activating a pack triggers -- blocking during boot with no bound on
 * how long it could take. Pulling activation out entirely (require opening
 * the Textures page first) traded that crash for HD replacement silently
 * never turning on for a player who never happened to open that page, which
 * is worse for everyone who was never at risk in the first place. The actual
 * fix is scan_dir()'s own wall-clock budget (TP_SCAN_BUDGET_SECONDS, above)
 * -- bounded, so this registration is safe to be unconditional again. */
static void discover_packs(void)
{
    const char *base = psx_mod_player_data_dir();
    if (!base || !*base)
        return;
    char root[TP_PATH_MAX];
    snprintf(root, sizeof root, "%s/textures", base);
    TP_MKDIR(root);
    snprintf(s_pack_name[0], TP_NAME_MAX, "textures");
    snprintf(s_pack_path[0], TP_PATH_MAX, "%s", root);
    s_pack_n = 1;
}

void texpack_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    discover_packs();
}

void texpack_shutdown(void)
{
    for (int i = 0; i < s_entry_n; i++) {
        free(s_entry[i].rgba);
        s_entry[i].rgba = NULL;
    }
    s_entry_n = 0;
    s_file_n = 0;
    s_region_n = 0;
}

static void rescan_anchored(int vw, int vh, const uint16_t *data);

void texpack_set_vram(const uint16_t *vram) { s_vram = vram; }

/* Anchored art is resident: it is in VRAM continuously, not delivered by an
 * event we can hook. So it is looked for against the live mirror rather than
 * only when a whole-VRAM write happens to arrive -- a session that never
 * restores a savestate would otherwise never resolve it at all. Assets
 * already placed are skipped, so the steady-state cost is a scan of a handful
 * of registrations. */
/* Ask for this art's palettes to be recorded per drawn region.
 *
 * Off by default, because the pool is shared and small: with every card
 * claiming a slot it filled long before the UI was ever drawn, and the assets
 * that actually need this -- the ones whose palette the title cannot supply --
 * silently got none. Cards do not need it; their palette sits beside their
 * pixels. */
/* Refuse to fall back to the plain whole-sheet file for this asset.
 *
 * That file can only ever hold ONE colouring of a sheet the game draws with
 * several -- an old file left over from a since-deleted composite pipeline,
 * or a single hand-painted sheet, would otherwise silently apply itself to a
 * draw under a different palette and look wrong with no indication why.
 * Without a matching per-draw element such a draw stays stock, which is
 * always correctly coloured, rather than a guess. */
int texpack_set_no_plain_fallback(const char *name, int on)
{
    if (!name)
        return 0;
    int hit = 0;
    for (int i = 0; i < s_asset_n; i++)
        if (strcmp(s_asset[i].name, name) == 0) {
            s_asset[i].no_plain = on ? 1 : 0;
            hit = 1;
        }
    return hit;
}

int texpack_set_track_cluts(const char *name, int on)
{
    if (!name)
        return 0;
    int hit = 0;
    for (int i = 0; i < s_asset_n; i++)
        if (strcmp(s_asset[i].name, name) == 0) {
            s_asset[i].track_cluts = on ? 1 : 0;
            hit = 1;
        }
    return hit;
}

int texpack_set_anchor(const char *name, int src_y,
                       int vram_x, int vram_y, int vram_w, int vram_h)
{
    if (!name || vram_w <= 0 || vram_h <= 0)
        return 0;
    for (int i = 0; i < s_asset_n; i++) {
        if (s_asset[i].src_y != src_y || strcmp(s_asset[i].name, name) != 0)
            continue;
        s_asset[i].anch_x = vram_x; s_asset[i].anch_y = vram_y;
        s_asset[i].anch_w = vram_w; s_asset[i].anch_h = vram_h;
        return 1;
    }
    return 0;
}

int texpack_set_tinted(const char *name, int on)
{
    if (!name)
        return 0;
    int hit = 0;
    for (int i = 0; i < s_asset_n; i++)
        if (strcmp(s_asset[i].name, name) == 0) {
            s_asset[i].tint = on ? 1 : 0;
            hit = 1;
        }
    return hit;
}

int texpack_observed_clut(const char *name, uint16_t *out, int entries)
{
    return texpack_observed_clut_page(name, 0, out, entries);
}

/* Art taller than a texture page is registered as several bands sharing one
 * name, and those bands need not share a palette -- the UI sheet is glyphs on
 * top and stone panelling below, drawn through entirely different CLUTs. So
 * the band has to be part of the key, or every band reports whichever one
 * happened to be drawn first and the rest export in the wrong colours. */
int texpack_observed_clut_page(const char *name, int src_y,
                               uint16_t *out, int entries)
{
    if (!name || !out || entries <= 0 || !s_vram)
        return 0;
    for (int i = 0; i < s_asset_n; i++) {
        const TpAsset *a = &s_asset[i];
        if (!a->clut_seen || a->src_y != src_y || strcmp(a->name, name) != 0)
            continue;
        if (a->clut_snap < 0 || a->clut_n < entries)
            return 0;
        memcpy(out, s_clutsnap[a->clut_snap],
               (size_t)entries * sizeof(uint16_t));
        return 1;
    }
    return 0;
}

void texpack_resolve_anchors(void)
{
    if (s_vram && s_asset_n)
        rescan_anchored(1024, 512, s_vram);
}

int         texpack_pack_count(void)        { return s_pack_n; }
const char *texpack_pack_name(int i)
{
    return (i >= 0 && i < s_pack_n) ? s_pack_name[i] : NULL;
}
const char *texpack_pack_path(int i)
{
    return (i >= 0 && i < s_pack_n) ? s_pack_path[i] : NULL;
}
int         texpack_active_pack(void)       { return s_pack_active; }
int         texpack_enabled(void)           { return s_enabled; }

/* discover_packs() registers <player-data>/textures as pack 0 once
 * texpack_init() has run, so the active branch below is the normal case --
 * this fallback mainly covers a call that lands before that (or before
 * player-data is known at all, where "." is at least something). */
void texpack_active_dir(char *out, unsigned cap)
{
    if (!out || !cap) return;
    const char *path = (s_pack_active >= 0) ? s_pack_path[s_pack_active] : NULL;
    if (path && *path) { snprintf(out, cap, "%s", path); return; }
    const char *base = psx_mod_player_data_dir();
    snprintf(out, cap, "%s/textures", base && *base ? base : ".");
}

void texpack_set_enabled(int on)
{
    on = on ? 1 : 0;
    if (on == s_enabled)
        return;
    s_enabled = on;
    /* The resolve cache holds decisions made under the old setting, so it has
     * to be retired -- otherwise cached hits would keep drawing replacements
     * after the toggle went off. Bumping the generation retires all of it at
     * once, the same way an invalidation does. */
    s_generation++;
}
int         texpack_atlas_dim(void)         { return TP_ATLAS_DIM; }


/* Throw away everything derived from the pack's FILES and read the directory
 * again. Registrations and regions survive -- those describe the game's own
 * art and have not changed -- so only the decoded pictures and the atlas are
 * rebuilt, lazily, as things are next drawn.
 *
 * This is what makes an edited PNG appear without restarting: entries cache
 * decoded pixels for the life of the process, so without dropping them a file
 * changed on disk is never read a second time. */
static void reload_pack_files(void)
{
    for (int i = 0; i < s_entry_n; i++) {
        free(s_entry[i].rgba);
        s_entry[i].rgba = NULL;
    }
    s_entry_n = 0;
    s_file_n = 0;
    s_shelf_x = s_shelf_y = s_shelf_h = 0;
    s_generation++;
    if (s_pack_active >= 0) {
        s_scan_over_budget = 0;
        s_scan_deadline = time(NULL) + TP_SCAN_BUDGET_SECONDS;
        scan_dir(s_pack_path[s_pack_active], NULL);
        s_stat_decode_fail = s_stat_scale_reject = s_stat_atlas_full = 0;
        s_stat_missing = 0;
    }
    s_file_scan_gen++;
}

/* Reloading walks the pack directory and frees decoded art, so it must not run
 * from a menu callback while the renderer is resolving a draw. The request is
 * a flag; texpack_reload_if_pending() does the work from the frame hook, the
 * same split every other toggle in this title uses. */
static int s_reload_req;

void texpack_request_reload(void) { s_reload_req = 1; }

void texpack_reload_if_pending(void)
{
    if (!s_reload_req)
        return;
    s_reload_req = 0;
    reload_pack_files();
}

void texpack_set_active_pack(int index)
{
    if (index == s_pack_active)
        return;
    s_pack_active = (index >= 0 && index < s_pack_n) ? index : -1;
    reload_pack_files();
}

void texpack_set_active_dir(const char *path)
{
    if (!path || !*path)
        return;
    for (int i = 0; i < s_pack_n; i++)
        if (!strcmp(s_pack_path[i], path)) { texpack_set_active_pack(i); return; }
    const int slot = (s_pack_n < TP_MAX_PACKS) ? s_pack_n++ : TP_MAX_PACKS - 1;
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *base_bs = strrchr(path, '\\');
    if (base_bs && (!base || base_bs > base)) base = base_bs;
#endif
    snprintf(s_pack_name[slot], TP_NAME_MAX, "%s", base ? base + 1 : path);
    snprintf(s_pack_path[slot], TP_PATH_MAX, "%s", path);
    /* Force texpack_set_active_pack's reload below even when `slot` happens
     * to already equal s_pack_active (the array-full reuse case): the path
     * just written into it changed, but the index did not. */
    s_pack_active = -1;
    texpack_set_active_pack(slot);
}

/* ---- regions -------------------------------------------------------------- */
static int rects_overlap(const TpRegion *r, int x, int y, int w, int h)
{
    return !(x >= r->x + r->w || x + w <= r->x ||
             y >= r->y + r->h || y + h <= r->y);
}

static void drop_overlapping(int x, int y, int w, int h)
{
    for (int i = 0; i < s_region_n; ) {
        if (rects_overlap(&s_region[i], x, y, w, h))
            s_region[i] = s_region[--s_region_n];
        else
            i++;
    }
}

void texpack_invalidate_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int before = s_region_n;
    drop_overlapping(x, y, w, h);
    if (s_region_n != before)
        s_generation++;
}

void texpack_invalidate_all(void)
{
    s_region_n = 0;
    s_generation++;
    bld_reset();
}

static void coalesce_strip(int x, int y, int w, int h, const uint16_t *data);

/* A whole-VRAM write (savestate restore, boot handoff) replaces everything at
 * once. Discarding our regions there is correct but leaves art that the game
 * uploaded ONCE and never re-uploads -- the UI font sheet above all --
 * permanently unmatchable: the only transfer that ever carried it has already
 * happened. The blit itself contains that art, though, so rather than throw it
 * away we re-derive regions from it: hash every 64x256 texture-page slot and
 * re-register the ones we recognise. 32 slots, 1 MB of hashing, once. */
#define TP_PAGE_W 64
#define TP_PAGE_H 256
/* Scratch for one anchored rectangle: a full texture page is the largest one
 * that can sensibly be claimed, so it is also the ceiling. */
#define TP_ANCH_MAX (TP_PAGE_W * TP_PAGE_H)

/* Confirm each anchored asset at the rectangle it claims. Hashing the
 * rectangle and comparing against the registration is what keeps this honest:
 * the position says where to look, the content still decides. A miss records
 * what WAS there, because the usual cause is a rectangle of the wrong height
 * and that hash identifies the right one. */
/* Index of asset's live region, or -1. */
static int anchor_region_index(int asset)
{
    for (int i = 0; i < s_region_n; i++)
        if (s_region[i].asset == asset)
            return i;
    return -1;
}

static void rescan_anchored(int vw, int vh, const uint16_t *data)
{
    static uint16_t rect[TP_ANCH_MAX];
    if (!data)
        return;
    for (int i = 0; i < s_asset_n; i++) {
        const TpAsset *a = &s_asset[i];
        if (!a->anch_w)
            continue;
        const size_t n = (size_t)a->anch_w * (size_t)a->anch_h;
        if (n > TP_ANCH_MAX ||
            a->anch_x + a->anch_w > vw || a->anch_y + a->anch_h > vh)
            continue;

        /* Re-hashed EVERY scan, even once already live.
         *
         * An anchor claims a FIXED VRAM rectangle, but nothing keeps that
         * rectangle exclusive to this asset -- the game is free to reuse the
         * same texture page for a totally different screen's art, and
         * nothing routes that change through texpack_on_upload() if it
         * happens by a GPU fill/blit rather than a tracked CPU->VRAM
         * transfer. The old code took "already live" as permanent proof of
         * occupancy and never looked again, so once some OTHER menu's
         * graphics moved into an anchored rectangle, every draw that
         * legitimately sampled it kept being served this asset's stale
         * content forever -- a hazard-stripe dialog border came out in
         * duel_labels' navy-and-white because duel_labels' anchor had
         * silently kept "owning" VRAM that the game had long since
         * repurposed. Checking every time costs a few small hashes per
         * frame, which is cheap next to getting this wrong permanently. */
        for (int r = 0; r < a->anch_h; r++)
            memcpy(rect + (size_t)r * a->anch_w,
                   data + (size_t)(a->anch_y + r) * vw + a->anch_x,
                   (size_t)a->anch_w * sizeof(uint16_t));
        const uint64_t got = texpack_hash(rect, (unsigned)(n * 2));
        const int live = anchor_region_index(i);

        if (got != a->hash) {
            s_stat_anchor_miss++;
            s_stat_anchor_hash = got;
            if (live >= 0) {
                /* No longer this asset's content: release the claim so
                 * whatever is actually there now can be matched normally
                 * (by upload hash) or simply draws stock, either of which
                 * is correct -- unlike continuing to hand out this asset's
                 * art for someone else's VRAM. */
                s_region[live] = s_region[--s_region_n];
            }
            continue;
        }
        if (live >= 0)
            continue;           /* already claimed and still correct */

        if (s_region_n >= TP_MAX_REGIONS)
            s_region_n = TP_MAX_REGIONS - 1;
        TpRegion *rg = &s_region[s_region_n++];
        rg->x = a->anch_x; rg->y = a->anch_y;
        rg->w = a->anch_w; rg->h = a->anch_h;
        rg->asset = i;
        s_stat_anchor_ok++;
        s_stat_matched++;
    }
}

static void rescan_vram(int w, int h, const uint16_t *data)
{
    static uint16_t page[TP_PAGE_W * TP_PAGE_H];
    for (int py = 0; py + TP_PAGE_H <= h; py += TP_PAGE_H) {
        for (int px = 0; px + TP_PAGE_W <= w; px += TP_PAGE_W) {
            for (int r = 0; r < TP_PAGE_H; r++)
                memcpy(page + (size_t)r * TP_PAGE_W,
                       data + (size_t)(py + r) * w + px,
                       TP_PAGE_W * sizeof(uint16_t));
            if (uniform16(page, TP_PAGE_W * TP_PAGE_H))
                continue;       /* blank page: see uniform16 */
            const int asset = asset_by_hash(
                texpack_hash(page, sizeof page));
            if (asset < 0)
                continue;
            if (s_region_n >= TP_MAX_REGIONS)
                s_region_n = TP_MAX_REGIONS - 1;
            TpRegion *rg = &s_region[s_region_n++];
            rg->x = px; rg->y = py;
            rg->w = TP_PAGE_W; rg->h = TP_PAGE_H;
            rg->asset = asset;
            s_stat_matched++;
        }
    }
    rescan_anchored(w, h, data);
}

void texpack_on_upload(int x, int y, int w, int h, const uint16_t *data)
{
    if (w <= 0 || h <= 0 || !data)
        return;

    /* Whatever was here is gone regardless of whether we can name the new
     * contents, so the invalidation happens before the lookup, not after it. */
    drop_overlapping(x, y, w, h);
    s_generation++;
    s_stat_uploads++;

    if (!s_asset_n)
        return;                 /* title registered nothing; nothing to name */

    /* A blit big enough to be the whole framebuffer is not one asset -- it is
     * everything at once. Recover what we can recognise inside it. */
    if (w >= 512 && h >= 512) {
        rescan_vram(w, h, data);
        bld_reset();
        return;
    }

    /* A uniform upload identifies nothing, but it must still be FED to the
     * coalescer: a background arrives as 16-row strips and some of those rows
     * are legitimately blank, so returning early here broke the rolling hash
     * mid-page and the whole background stopped assembling. Only the match is
     * suppressed, never the accumulation. (Blank pages cannot match anyway --
     * psx_wa_catalog.c refuses to register uniform bands.) */
    const uint64_t hash = texpack_hash(data, (unsigned)(w * h) * 2u);
    const int asset = uniform16(data, (unsigned)(w * h))
                    ? -1 : asset_by_hash(hash);
    note_shape(w, h, asset >= 0);
    if (asset < 0) {
        coalesce_strip(x, y, w, h, data);   /* maybe a piece of a strip run */
        return;
    }
    bld_reset();                /* a whole-upload match breaks any strip run */

    if (s_region_n >= TP_MAX_REGIONS)
        s_region_n = TP_MAX_REGIONS - 1;   /* newest wins over the oldest */
    TpRegion *r = &s_region[s_region_n++];
    r->x = x; r->y = y; r->w = w; r->h = h;
    r->asset = asset;
    s_stat_matched++;
}

/* Fold one upload into the vertical-strip run and, if the run has now
 * assembled a registered asset, emit a region for the whole page. Called for
 * every upload; a run that does not continue simply restarts here. */
static void coalesce_strip(int x, int y, int w, int h, const uint16_t *data)
{
    if (x == s_bld_x && w == s_bld_w && y == s_bld_yend) {
        /* continues the current run */
    } else {
        if (s_bld_x >= 0)      /* the run that just ended, unmatched */
            runlog_add(s_bld_x, s_bld_y0, s_bld_w,
                       s_bld_yend - s_bld_y0, 0, s_bld_hash);
        s_bld_x = x; s_bld_w = w; s_bld_y0 = y;
        s_bld_yend = y; s_bld_hash = FNV_OFFSET;
    }
    s_bld_hash = fnv_update(s_bld_hash, data, (unsigned)(w * h) * 2u);
    s_bld_yend = y + h;
    if (s_bld_yend - s_bld_y0 > s_bld_max_h) s_bld_max_h = s_bld_yend - s_bld_y0;

    const int asset = asset_by_hash(s_bld_hash);
    if (s_bld_yend - s_bld_y0 >= 512) {
        s_bld_tall_runs++;
        if (asset >= 0) s_bld_tall_matched++;
        else { s_bld_tall_hash = s_bld_hash;
               s_bld_tall_x = s_bld_x; s_bld_tall_y = s_bld_y0; }
    }
    if (asset < 0)
        return;

    const int ph = s_bld_yend - s_bld_y0;
    if (s_region_n >= TP_MAX_REGIONS)
        s_region_n = TP_MAX_REGIONS - 1;
    TpRegion *r = &s_region[s_region_n++];
    r->x = s_bld_x; r->y = s_bld_y0; r->w = s_bld_w; r->h = ph;
    r->asset = asset;
    s_stat_matched++;
    runlog_add(s_bld_x, s_bld_y0, s_bld_w, ph, 1, s_bld_hash);
    bld_reset();                /* page complete; next strip starts fresh */
}

/* ---- entries: loading a replacement --------------------------------------- */
static int find_file(const char *name)
{
    for (int i = 0; i < s_file_n; i++)
        if (!strcmp(s_file[i].name, name))
            return i;
    return -1;
}

int texpack_has_replacement(const char *name)
{
    if (!s_enabled || s_pack_active < 0 || !name || !*name)
        return 0;
    return find_file(name) >= 0;
}

unsigned texpack_file_scan_generation(void) { return s_file_scan_gen; }

static int atlas_place(int w, int h, int *ox, int *oy)
{
    if (w > TP_ATLAS_DIM || h > TP_ATLAS_DIM)
        return 0;
    if (s_shelf_x + w > TP_ATLAS_DIM) {         /* next shelf */
        s_shelf_y += s_shelf_h;
        s_shelf_x = 0;
        s_shelf_h = 0;
    }
    if (s_shelf_y + h > TP_ATLAS_DIM)
        return 0;                               /* atlas full */
    *ox = s_shelf_x;
    *oy = s_shelf_y;
    s_shelf_x += w;
    if (h > s_shelf_h)
        s_shelf_h = h;
    return 1;
}

/* Whole-file read, then decode. TP_FILE_MAX caps it so a stray large file in
 * a pack directory cannot be turned into an allocation by naming alone. */
#define TP_FILE_MAX (64u * 1024u * 1024u)

static stbi_uc *load_png(const char *path, int *w, int *h, int *comp)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Indexed at scan time, gone now. Counted apart from a decode failure
         * because the two mean opposite things: one is a broken PNG, the other
         * is a pack that has changed under us and wants reloading. */
        s_stat_missing++;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0 || (unsigned long)len > TP_FILE_MAX) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return NULL; }
    stbi_uc *px = stbi_load_from_memory(buf, (int)len, w, h, comp, 4);
    free(buf);
    return px;
}

/* Load a file as the replacement for asset `a`. Returns an entry index, or -1.
 *
 * The file on disk is DISPLAY layout (a->disp_w x a->disp_h). For a raw asset
 * that equals the sampled size and the loaded pixels are used as-is. For a
 * tiled asset (a->retile != 0) the loaded picture is a clean scene and has to
 * be repacked into the a->w x a->h layout the game samples, at the same
 * integer scale, before it enters the atlas. */
static int entry_load(int file, const TpAsset *a)
{
    /* Keyed on the PAGE, not the file: several pages of a tall image share one
     * file, and matching on file alone would hand every page the first page's
     * pixels. */
    for (int i = 0; i < s_entry_n; i++)
        if (s_entry[i].file == file &&
            s_entry[i].src_x == a->src_x && s_entry[i].src_y == a->src_y)
            return i;
    if (s_entry_n >= TP_MAX_ENTRIES)
        return -1;

    int w = 0, h = 0, comp = 0;
    stbi_uc *px = load_png(s_file[file].path, &w, &h, &comp);
    if (!px) {
        s_stat_decode_fail++;
        return -1;
    }
    /* Validate against the DISPLAY size -- that is what the file is. */
    if (w % a->disp_w || h % a->disp_h ||
        w / a->disp_w != h / a->disp_h || w < a->disp_w) {
        s_stat_scale_reject++;      /* see the header on why fractional fails */
        stbi_image_free(px);
        return -1;
    }
    const int scale = w / a->disp_w;

    /* 1. retile the WHOLE image, if this family is stored scrambled. */
    stbi_uc *page = px;
    int pw = w, ph = h;
    if (a->retile) {
        pw = a->page_w * scale;
        ph = a->page_h * scale;
        page = (stbi_uc *)malloc((size_t)pw * (size_t)ph * 4u);
        if (!page) { stbi_image_free(px); return -1; }
        a->retile(px, w, h, page, pw, ph);
        stbi_image_free(px);
    }

    /* 2. crop out just this page. A PS1 texture page is at most 256 rows, so a
     *    taller image arrives as several uploads and each is its own asset
     *    cropped from the same picture. */
    const int aw = a->w * scale, ah = a->h * scale;
    const int sx = a->src_x * scale, sy = a->src_y * scale;
    stbi_uc *atlas_px;
    if (sx == 0 && sy == 0 && aw == pw && ah == ph) {
        atlas_px = page;                 /* whole image is the page */
    } else {
        if (sx + aw > pw || sy + ah > ph) {
            if (a->retile) free(page); else stbi_image_free(page);
            s_stat_scale_reject++;
            return -1;
        }
        atlas_px = (stbi_uc *)malloc((size_t)aw * (size_t)ah * 4u);
        if (!atlas_px) {
            if (a->retile) free(page); else stbi_image_free(page);
            return -1;
        }
        for (int y = 0; y < ah; y++)
            memcpy(atlas_px + (size_t)y * aw * 4u,
                   page + ((size_t)(sy + y) * pw + sx) * 4u,
                   (size_t)aw * 4u);
        if (a->retile) free(page); else stbi_image_free(page);
    }

    int ax, ay;
    if (!atlas_place(aw, ah, &ax, &ay)) {
        s_stat_atlas_full++;
        free(atlas_px);
        return -1;
    }

    TpEntry *e = &s_entry[s_entry_n];
    e->file = file;
    e->src_x = a->src_x;
    e->src_y = a->src_y;
    e->w = aw;
    e->h = ah;
    e->scale = scale;
    e->atlas_x = ax;
    e->atlas_y = ay;
    e->rgba = atlas_px;
    e->pending = 1;
    return s_entry_n++;
}

int texpack_take_pending(int *x, int *y, int *w, int *h, const uint8_t **rgba)
{
    for (int i = 0; i < s_entry_n; i++) {
        if (!s_entry[i].pending)
            continue;
        s_entry[i].pending = 0;
        *x = s_entry[i].atlas_x;
        *y = s_entry[i].atlas_y;
        *w = s_entry[i].w;
        *h = s_entry[i].h;
        *rgba = s_entry[i].rgba;
        return 1;
    }
    return 0;
}

/* ---- draw-time resolve ---------------------------------------------------- */
/* The resolve cache. The key MUST include the sampled uv bounds, not just the
 * texture state: one 64-word texpage spans two 256-row VRAM pages, so a quad
 * drawing the left half and a quad drawing the right half share base_x,
 * base_y, depth and CLUT entirely -- they differ only in which texels they
 * read. Keyed without `lim`, the second quad inherited the first quad's
 * answer, sampled past the end of that page's atlas entry, and discarded as
 * transparent: a background whose left third replaced correctly while the rest
 * went black. */
typedef struct {
    unsigned   gen;
    int        base_x, base_y, depth, clut_x, clut_y;
    int        lim[4];
    int        hit;
    TexPackHit out;
} TpCache;

static TpCache s_cache[TP_CACHE];

/* VRAM is addressed with WRAPAROUND, which is what the GPU does -- the shader
 * has always used `ivec2(x & 1023, y & 511)`. Returning zero past the edge
 * instead made this disagree with the renderer for any CLUT near the right of
 * VRAM: a 256-entry palette at x=896 needs 1152 columns, so it wraps to the
 * start of its own line. Reading zeros there produced a wrong CLUT hash and,
 * worse, made the capture guard below reject those palettes entirely. */
static uint16_t vram_at(int x, int y)
{
    if (!s_vram)
        return 0;
    return s_vram[(y & 511) * 1024 + (x & 1023)];
}

/* The CLUT a primitive draws through, hashed, so a pack can hold one image per
 * palette variant. 4bpp reads 16 entries, 8bpp 256, and a 16bpp primitive has
 * no CLUT at all. */
/* A palette's identity, as 128 bits.
 *
 * 64 bits is not enough: this is a hash of only 32-512 bytes of highly
 * structured data (BGR555 palette entries), sampled across a whole session's
 * worth of distinct UI colourings, and two GENUINELY DIFFERENT palettes were
 * found to collide under plain FNV-1a-64 -- a dialogue border's real yellow
 * palette and an unrelated blue one exported earlier landed on the identical
 * 64-bit digest, so the injector confidently served the wrong file. Two
 * independent 64-bit FNV-1a runs (second one salted so it is not simply the
 * same computation twice) make that practically impossible without needing a
 * cryptographic hash. */
void texpack_hash128(const void *data, unsigned len, uint64_t out[2])
{
    out[0] = fnv_update(FNV_OFFSET, data, len);
    uint64_t h = fnv_update(FNV_OFFSET ^ 0x9E3779B97F4A7C15ULL, data, len);
    out[1] = fnv_update(h, "\x5A", 1);
}

static void clut_hash(int depth, int clut_x, int clut_y, uint64_t out[2])
{
    int n = (depth == 0) ? 16 : (depth == 1) ? 256 : 0;
    if (!n) { out[0] = out[1] = 0; return; }
    uint16_t buf[256];
    for (int i = 0; i < n; i++)
        buf[i] = vram_at(clut_x + i, clut_y);
    texpack_hash128(buf, (unsigned)n * 2u, out);
}

/* ---- unknown-asset capture -------------------------------------------------
 * See texture_pack.h. Everything here runs only when texpack_on_draw has
 * already decided "no region claims this primitive" -- it never competes
 * with, slows down, or changes the result of the replacement path above.
 *
 * Kept as a flat, ever-growing, NEVER-consumed pile rather than a drained
 * queue: a game layer groups pieces that sit next to each other on screen
 * (one panel's worth of icons, one logo's worth of bands) the same way
 * write_screens() groups a known asset's recorded regions, and the piece
 * that completes a group may not turn up until several frames after the
 * first one in it -- so nothing here can be thrown away just because it was
 * looked at once already. */
#define TP_CAP_ALL     128  /* distinct captures kept for the session */
#define TP_CAP_MAX_DIM 256  /* a texture page is never wider or taller */

static int      s_capture_enabled;

typedef struct {
    int base_x, base_y, depth, clut_x, clut_y;
    int lim[4];
    int dst[4];
    int w, h;
    uint8_t *rgba;
    int anch_x, anch_y, anch_w, anch_h;
    uint16_t *raw;
    uint64_t key;    /* dedup identity: see maybe_capture */
} TpCapSlot;
static TpCapSlot s_capture_all[TP_CAP_ALL];
static int       s_capture_all_n;

void texpack_set_capture_enabled(int on) { s_capture_enabled = on ? 1 : 0; }
int  texpack_capture_enabled(void)       { return s_capture_enabled; }

static int capture_already_seen(uint64_t key)
{
    /* Linear, but TP_CAP_ALL is small and this only runs on the already-rare
     * "no region matched" path, not per frame. */
    for (int i = 0; i < s_capture_all_n; i++)
        if (s_capture_all[i].key == key)
            return 1;
    return 0;
}

/* Decode the rectangle this primitive samples through its own live CLUT --
 * the same unpacking texpack_on_draw already knows (tpw texels per VRAM
 * word), just writing pixels out instead of only reading the palette. 0x0000
 * is the PS1's own transparency convention, followed everywhere else in this
 * project's disc-side decoding (psx_wa_catalog.c), so it is followed here
 * too: a capture reads exactly like a disc-decoded asset would. */
static void maybe_capture(int base_x, int base_y, int depth,
                         int clut_x, int clut_y, const int lim[4],
                         const int dst[4])
{
    if (!s_capture_enabled || !s_vram || !lim)
        return;

    const uint64_t key = (uint64_t)(unsigned)base_x * 1000003u
                       ^ (uint64_t)(unsigned)base_y * 999983u << 12
                       ^ (uint64_t)(unsigned)depth  * 65599u  << 24
                       ^ (uint64_t)(unsigned)clut_x * 31u     << 32
                       ^ (uint64_t)(unsigned)clut_y * 131u    << 40
                       ^ (uint64_t)(unsigned)(lim[0] * 5 + lim[1] * 11
                                             + lim[2] * 7 + lim[3] * 13);
    if (capture_already_seen(key))
        return;

    const int tpw = (depth == 0) ? 4 : (depth == 1) ? 2 : 1;
    const int w = lim[2] - lim[0] + 1, h = lim[3] - lim[1] + 1;
    if (w <= 0 || h <= 0 || w > TP_CAP_MAX_DIM || h > TP_CAP_MAX_DIM)
        return;

    const int cn = (depth == 0) ? 16 : (depth == 1) ? 256 : 0;
    uint16_t pal[256];
    if (cn && clut_x >= 0 && clut_y >= 0)
        for (int e = 0; e < cn; e++)
            pal[e] = vram_at(clut_x + e, clut_y);

    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba)
        return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int tx = lim[0] + x, ty = lim[1] + y;
            uint16_t v;
            if (depth == 2) {
                v = vram_at(base_x + tx, base_y + ty);
            } else {
                const uint16_t word = vram_at(base_x + tx / tpw, base_y + ty);
                const unsigned idx = (depth == 0) ? (word >> ((tx & 3) * 4)) & 0xFu
                                                  : (word >> ((tx & 1) * 8)) & 0xFFu;
                v = (cn && (int)idx < cn) ? pal[idx] : 0;
            }
            uint8_t *q = rgba + (size_t)(y * w + x) * 4u;
            q[0] = (uint8_t)(((v      ) & 31) * 255 / 31);
            q[1] = (uint8_t)(((v >>  5) & 31) * 255 / 31);
            q[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
            q[3] = v ? 255 : 0;
        }
    }

    /* The SAME rectangle, word-aligned, raw (not palette-decoded) -- exactly
     * what rescan_anchored() reads and hashes for an anchor check. lim[] is
     * texel-granular and need not fall on a word boundary, so the anchor's
     * word span is the words CONTAINING lim[0]..lim[2], which for a 16bpp
     * primitive (tpw 1) is the same range either way. */
    const int word0 = lim[0] / tpw, word1 = lim[2] / tpw;
    const int anch_x = base_x + word0, anch_y = base_y + lim[1];
    const int anch_w = word1 - word0 + 1, anch_h = h;
    uint16_t *raw = (uint16_t *)malloc((size_t)anch_w * (size_t)anch_h * sizeof(uint16_t));
    if (!raw) { free(rgba); return; }
    for (int r = 0; r < anch_h; r++)
        for (int c = 0; c < anch_w; c++)
            raw[(size_t)r * anch_w + c] = vram_at(anch_x + c, anch_y + r);

    if (s_capture_all_n >= TP_CAP_ALL) {
        /* Pool full: stop accepting new ones rather than evicting what a
         * game layer may already be part-way through grouping -- the same
         * "keep what is already recorded" choice s_clutrect's own pool-full
         * case makes, and for the same reason (silently truncating on this
         * side of the join would be worse than silently truncating here). */
        free(rgba); free(raw);
        return;
    }
    TpCapSlot *slot = &s_capture_all[s_capture_all_n++];
    slot->base_x = base_x; slot->base_y = base_y; slot->depth = depth;
    slot->clut_x = clut_x; slot->clut_y = clut_y;
    slot->lim[0] = lim[0]; slot->lim[1] = lim[1];
    slot->lim[2] = lim[2]; slot->lim[3] = lim[3];
    if (dst) { slot->dst[0]=dst[0]; slot->dst[1]=dst[1]; slot->dst[2]=dst[2]; slot->dst[3]=dst[3]; }
    else     { slot->dst[0]=slot->dst[1]=slot->dst[2]=slot->dst[3]=0; }
    slot->w = w; slot->h = h; slot->rgba = rgba; slot->key = key;
    slot->anch_x = anch_x; slot->anch_y = anch_y;
    slot->anch_w = anch_w; slot->anch_h = anch_h; slot->raw = raw;
}

int texpack_capture_count(void) { return s_capture_all_n; }

int texpack_capture_get(int i, TexPackCapture *out)
{
    if (!out || i < 0 || i >= s_capture_all_n)
        return 0;
    const TpCapSlot *s = &s_capture_all[i];
    out->base_x = s->base_x; out->base_y = s->base_y; out->depth = s->depth;
    out->clut_x = s->clut_x; out->clut_y = s->clut_y;
    memcpy(out->lim, s->lim, sizeof out->lim);
    memcpy(out->dst, s->dst, sizeof out->dst);
    out->w = s->w; out->h = s->h; out->rgba = s->rgba;   /* borrowed */
    out->anch_x = s->anch_x; out->anch_y = s->anch_y;
    out->anch_w = s->anch_w; out->anch_h = s->anch_h;
    out->raw = s->raw;                                    /* borrowed */
    return 1;
}

int texpack_on_draw(int base_x, int base_y, int depth,
                    int clut_x, int clut_y, const int lim[4],
                    const int dst[4], TexPackHit *out)
{
    if (!s_enabled || s_pack_active < 0 || !s_region_n || !lim)
        return 0;

    unsigned slot = (unsigned)(base_x * 31 + base_y * 17 + depth * 7
                               + clut_x * 3 + clut_y
                               + lim[0] * 5 + lim[1] * 11) % TP_CACHE;
    TpCache *c = &s_cache[slot];
    if (c->gen == s_generation && c->base_x == base_x && c->base_y == base_y &&
        c->depth == depth && c->clut_x == clut_x && c->clut_y == clut_y &&
        c->lim[0] == lim[0] && c->lim[1] == lim[1] &&
        c->lim[2] == lim[2] && c->lim[3] == lim[3]) {
        if (c->hit)
            *out = c->out;
        return c->hit;
    }

    c->gen = s_generation;
    c->base_x = base_x; c->base_y = base_y; c->depth = depth;
    c->clut_x = clut_x; c->clut_y = clut_y;
    c->lim[0] = lim[0]; c->lim[1] = lim[1];
    c->lim[2] = lim[2]; c->lim[3] = lim[3];
    c->hit = 0;

    /* texels per VRAM word at this depth */
    const int tpw = (depth == 0) ? 4 : (depth == 1) ? 2 : 1;

    /* the VRAM words this primitive samples */
    const int x0 = base_x + lim[0] / tpw;
    const int x1 = base_x + lim[2] / tpw;
    const int y0 = base_y + lim[1];
    const int y1 = base_y + lim[3];

    /* newest region that fully contains them */
    int found = -1;
    for (int i = s_region_n - 1; i >= 0; i--) {
        const TpRegion *r = &s_region[i];
        if (x0 >= r->x && x1 < r->x + r->w &&
            y0 >= r->y && y1 < r->y + r->h) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        note_unresolved(base_x, base_y, depth, x0, y0, x1, y1);
        maybe_capture(base_x, base_y, depth, clut_x, clut_y, lim, dst);
        return 0;
    }

    const TpRegion *r = &s_region[found];
    TpAsset *a = &s_asset[r->asset];

    /* The containment test above is purely geometric: it asks whether this
     * primitive's WORD-space sample rectangle falls inside the region, not
     * whether the primitive is actually drawing this asset's content. A
     * region wide enough for one asset can geometrically contain a totally
     * unrelated primitive sampling at a DIFFERENT bit depth -- same VRAM
     * words, different meaning entirely, because tpw (texels per word)
     * changes with depth. That mismatch is invisible to x0/x1/y0/y1, which
     * are already computed through this primitive's own tpw.
     *
     * ui/menu_labels_1 (bpp 4, expects depth 0) is the confirmed case: a
     * depth-1 primitive geometrically inside the same anchored region got
     * captured as its "palette" -- 256 real VRAM words, smoothly graduated
     * because they are real image content, just not this asset's palette at
     * all. The result was legible-shaped but wrong-coloured "static" on
     * every export, and no amount of checking the disc bytes or the decode
     * math would ever have found it, because both were already correct. */
    /* TpAsset carries no bpp field of its own, but an ANCHORED asset's own
     * anch_w (VRAM words) against its w (texels) already says what depth it
     * was registered at -- texels per word is exactly what tpw above is,
     * just computed the other way round. Non-anchored (upload-hash-matched)
     * assets are not checked here: their region identity already came from
     * an actual content hash on the real upload, not a geometric guess, so
     * the specific failure mode this guards against does not apply to them
     * the same way. */
    if (a->anch_w > 0) {
        const int reg_tpw = a->w / a->anch_w;
        const int expect_depth = (reg_tpw == 4) ? 0 : (reg_tpw == 2) ? 1 : (reg_tpw == 1) ? 2 : -1;
        if (expect_depth >= 0 && depth != expect_depth) {
            note_unresolved(base_x, base_y, depth, x0, y0, x1, y1);
            maybe_capture(base_x, base_y, depth, clut_x, clut_y, lim, dst);
            return 0;
        }
    }

    /* Capture the palette CONTENTS while they are still the ones being used.
     * Noted even when no replacement exists: the exporter needs this precisely
     * for the art nobody has painted over yet. */
    s_stat_resolved++;
    const int cn = (depth == 0) ? 16 : (depth == 1) ? 256 : 0;
    if (!(cn && s_vram && clut_x >= 0 && clut_y >= 0)) {
        s_stat_clut_skip++;
        s_stat_clut_last[0] = depth;
        s_stat_clut_last[1] = clut_x;
        s_stat_clut_last[2] = clut_y;
    }
    if (cn && s_vram && clut_x >= 0 && clut_y >= 0) {
        if (a->clut_snap < 0 && s_clutsnap_n < TP_CLUTSNAP)
            a->clut_snap = s_clutsnap_n++;
        if (a->clut_snap >= 0) {
            /* Entry by entry, wrapped -- a 256-entry CLUT starting near the
             * right edge runs off it and continues at column 0 of the same
             * line, so a flat copy would read past the row and a bounds check
             * would refuse it. */
            for (int e = 0; e < cn; e++)
                s_clutsnap[a->clut_snap][e] = vram_at(clut_x + e, clut_y);
            a->clut_n = cn;
            a->clut_seen = 1;
            /* ...and the same palette against the rectangle it applies to,
             * in whole-image texels: what this primitive samples, shifted by
             * where the region sits in the page and which band it is. */
            const int ou = (r->x - base_x) * tpw, ov = r->y - base_y;
            if (a->track_cluts)
                note_clut_rect(r->asset,
                               lim[0] - ou + a->src_x, lim[1] - ov + a->src_y,
                               lim[2] - ou + a->src_x, lim[3] - ov + a->src_y,
                               s_clutsnap[a->clut_snap], cn, dst);
        }
    }

    uint64_t ch2[2];
    clut_hash(depth, clut_x, clut_y, ch2);

    /* PER-ELEMENT file first, if the pack has one.
     *
     * A UI page is a dense sheet of unrelated icons and labels with no blank
     * rows between them, so any split of the page into files is arbitrary --
     * and mine put icons in with labels. The game's own draw calls are not
     * arbitrary: each one samples exactly one element. So a pack may supply a
     * file per element, named for the rectangle the draw covers, and it wins
     * over the whole-page file.
     *
     * The name has to be derived the same way the exporter derives it, from
     * the same lim, or nothing matches. */
    const int e_ou = (r->x - base_x) * tpw, e_ov = r->y - base_y;
    const int ix0 = lim[0] - e_ou + a->src_x, iy0 = lim[1] - e_ov + a->src_y;
    const int ix1 = lim[2] - e_ou + a->src_x, iy1 = lim[3] - e_ov + a->src_y;
    if (ix1 >= ix0 && iy1 >= iy0) {
        char sub[TP_NAME_MAX];
        /* Palette-specific element first: the same rectangle is drawn through
         * different CLUTs and each has its own file. Falls back to the plain
         * name for packs that supply just one. */
        int sfile = -1, spal = 0;
        if ((ch2[0] || ch2[1]) &&
            snprintf(sub, sizeof sub, "%s/%d_%d_%dx%d@%016llx%016llx", a->name,
                     ix0, iy0, ix1 - ix0 + 1, iy1 - iy0 + 1,
                     (unsigned long long)ch2[0], (unsigned long long)ch2[1])
                < (int)sizeof sub) {
            sfile = find_file(sub);
            spal = (sfile >= 0);
        }
        if (snprintf(sub, sizeof sub, "%s/%d_%d_%dx%d", a->name, ix0, iy0,
                     ix1 - ix0 + 1, iy1 - iy0 + 1) < (int)sizeof sub) {
            if (sfile < 0) sfile = find_file(sub);
            if (sfile >= 0) {
                TpAsset sa = *a;            /* a standalone image, not a page */
                sa.w = sa.disp_w = sa.page_w = ix1 - ix0 + 1;
                sa.h = sa.disp_h = sa.page_h = iy1 - iy0 + 1;
                sa.src_x = sa.src_y = 0;
                sa.retile = 0;
                const int se = entry_load(sfile, &sa);
                if (se >= 0) {
                    c->out.atlas_x = (float)s_entry[se].atlas_x;
                    c->out.atlas_y = (float)s_entry[se].atlas_y;
                    c->out.org_u   = (float)(ix0 + e_ou - a->src_x);
                    c->out.org_v   = (float)(iy0 + e_ov - a->src_y);
                    c->out.scale   = (float)s_entry[se].scale;
                    /* Palette-specific art can only match under its own CLUT,
                     * so drawing it flat is always colour-correct -- and flat
                     * is what lets a tinted sheet's elements be true HD instead
                     * of HD-alpha-only. The palette-agnostic fallback keeps
                     * the asset's tint semantics. */
                    /* a->tint is tri-state (-1/0/1): a plain truthiness check
                     * here would treat -1 (explicitly opted OUT of tinting) as
                     * tinted, since -1 is non-zero in C. Only tint==1 counts. */
                    c->out.mode    = spal ? 0 : (a->tint > 0 ? 1 : 0);
                    c->hit = 1;
                    *out = c->out;
                    return 1;
                }
            }
        }
    }

    /* Most specific file wins:
     *   <name>@<cluthash>  painted for exactly this palette
     *   <name>.idx         an INDEX map -- works under every palette
     *   <name>             ordinary art, drawn flat or tinted per the asset
     *
     * The .idx form is what lets a replacement behave like the game's own art
     * on a sheet drawn through several CLUTs: it keeps the indirection the
     * console has and a painted RGBA file throws away. */
    int file = -1;
    /* Tri-state: only tint==1 means tinted. A plain truthiness check would
     * also fire for tint==-1 (explicitly opted out), since -1 is non-zero. */
    int mode = a->tint > 0 ? 1 : 0;
    if (ch2[0] || ch2[1]) {
        char keyed[TP_NAME_MAX];
        snprintf(keyed, sizeof keyed, "%s@%016llx%016llx", a->name,
                 (unsigned long long)ch2[0], (unsigned long long)ch2[1]);
        file = find_file(keyed);
    }
    if (file < 0) {
        char ix[TP_NAME_MAX];
        snprintf(ix, sizeof ix, "%s.idx", a->name);
        file = find_file(ix);
        if (file >= 0)
            mode = 2;
    }
    /* The plain whole-sheet fallback is skipped for per-palette UI art
     * (not tinted, but still UI). That file can only ever hold ONE colouring,
     * which is the exact ambiguity this design exists to avoid -- an old file
     * left over from the deleted composite/heuristic pipeline, or a single
     * hand-painted sheet, would otherwise silently apply itself to draws under
     * a DIFFERENT palette and look wrong with no indication why. Without a
     * matching per-draw element, such a draw is stock, which is always
     * correctly coloured, rather than a guess. */
    if (file < 0 && !a->no_plain)
        file = find_file(a->name);
    if (file < 0)
        return 0;

    const int entry = entry_load(file, a);
    if (entry < 0)
        return 0;

    const TpEntry *e = &s_entry[entry];
    c->out.atlas_x = (float)e->atlas_x;
    c->out.atlas_y = (float)e->atlas_y;
    c->out.org_u   = (float)((r->x - base_x) * tpw);
    c->out.org_v   = (float)(r->y - base_y);
    c->out.scale   = (float)e->scale;
    c->out.mode    = mode;
    c->hit = 1;
    *out = c->out;
    return 1;
}


/* ---- diagnostics ---------------------------------------------------------- */

/* Hash of every 64x256 texture-page slot currently in VRAM, with the asset it
 * resolves to (empty when unknown). Reported on demand rather than only on a
 * whole-VRAM blit, so what a screen ACTUALLY has resident can be compared
 * against the disc -- which is how a page the game draws from but we never
 * registered gets identified. */
int texpack_vram_pages_json(char *out, unsigned cap)
{
    static uint16_t page[TP_PAGE_W * TP_PAGE_H];
    unsigned n = (unsigned)snprintf(out, cap, "\"pages\":[");
    if (n >= cap) return 0;
    int first = 1;
    for (int py = 0; py + TP_PAGE_H <= 512; py += TP_PAGE_H) {
        for (int px = 0; px + TP_PAGE_W <= 1024; px += TP_PAGE_W) {
            if (!s_vram) break;
            for (int r = 0; r < TP_PAGE_H; r++)
                memcpy(page + (size_t)r * TP_PAGE_W,
                       s_vram + (size_t)(py + r) * 1024 + px,
                       TP_PAGE_W * sizeof(uint16_t));
            const uint64_t h = texpack_hash(page, sizeof page);
            const int a = asset_by_hash(h);
            unsigned k = (unsigned)snprintf(out + n, cap - n,
                "%s{\"x\":%d,\"y\":%d,\"h\":\"%016llx\",\"a\":\"%s\"}",
                first ? "" : ",", px, py, (unsigned long long)h,
                a >= 0 ? s_asset[a].name : "");
            if (k >= cap - n) return 0;
            n += k; first = 0;
        }
    }
    return (unsigned)snprintf(out + n, cap - n, "]") < cap - n;
}
int texpack_state_json(char *out, unsigned cap)
{
    const char *pack = (s_pack_active >= 0) ? s_pack_name[s_pack_active] : "";
    unsigned n = (unsigned)snprintf(
        out, cap,
        "\"enabled\":%d,\"packs\":%d,\"active\":%d,\"pack\":\"%s\",\"files\":%d,"
        "\"assets\":%d,\"regions\":%d,\"loaded\":%d,"
        "\"uploads\":%u,\"matched\":%u,"
        "\"anchor\":{\"ok\":%u,\"miss\":%u,\"hash\":\"%016llx\"},"
        "\"draw\":{\"resolved\":%u,\"clut_skip\":%u,\"rects\":%d,"
        "\"last_depth\":%d,\"last_cx\":%d,\"last_cy\":%d,"
        "\"last_raw\":%d,\"last_col\":[%.3f,%.3f,%.3f]},"
        "\"runs\":{\"max_h\":%d,\"tall\":%u,\"tall_matched\":%u,"
        "\"hash\":\"%016llx\",\"x\":%d,\"y\":%d},"
        "\"refused\":{\"decode\":%u,\"scale\":%u,\"atlas_full\":%u,"
        "\"missing\":%u},\"unres\":[",
        s_enabled, s_pack_n, s_pack_active, pack, s_file_n,
        s_asset_n, s_region_n, s_entry_n,
        s_stat_uploads, s_stat_matched,
        s_stat_anchor_ok, s_stat_anchor_miss,
        (unsigned long long)s_stat_anchor_hash,
        s_stat_resolved, s_stat_clut_skip, s_clutrect_n,
        s_stat_clut_last[0], s_stat_clut_last[1], s_stat_clut_last[2],
        s_dbg_last_raw, (double)s_dbg_last_col[0], (double)s_dbg_last_col[1],
        (double)s_dbg_last_col[2],
        s_bld_max_h, s_bld_tall_runs, s_bld_tall_matched,
        (unsigned long long)s_bld_tall_hash, s_bld_tall_x, s_bld_tall_y,
        s_stat_decode_fail, s_stat_scale_reject, s_stat_atlas_full,
        s_stat_missing);
    if (n >= cap)
        return 0;

    for (int i = 0; i < s_unres_n; i++) {
        unsigned k = (unsigned)snprintf(out + n, cap - n,
            "%s{\"bx\":%d,\"by\":%d,\"d\":%d,\"x0\":%d,\"y0\":%d,"
            "\"x1\":%d,\"y1\":%d,\"n\":%u}",
            i ? "," : "", s_unres[i].bx, s_unres[i].by, s_unres[i].depth,
            s_unres[i].x0, s_unres[i].y0, s_unres[i].x1, s_unres[i].y1,
            s_unres[i].n);
        if (k >= cap - n) return 0;
        n += k;
    }
    n += (unsigned)snprintf(out + n, cap - n, "],\"copies\":[");
    if (n >= cap) return 0;
    for (int i = 0; i < s_copylog_n; i++) {
        const int ai = s_copylog[i].asset;
        unsigned k = (unsigned)snprintf(out + n, cap - n,
            "%s{\"name\":\"%s\",\"sx\":%d,\"sy\":%d,\"sw\":%d,\"sh\":%d,"
            "\"dx\":%d,\"dy\":%d,\"n\":%u}",
            i ? "," : "",
            (ai >= 0 && ai < s_asset_n) ? s_asset[ai].name : "?",
            s_copylog[i].sx, s_copylog[i].sy, s_copylog[i].sw, s_copylog[i].sh,
            s_copylog[i].dx, s_copylog[i].dy, s_copylog[i].n);
        if (k >= cap - n) return 0;
        n += k;
    }
    n += (unsigned)snprintf(out + n, cap - n, "],\"shapes\":[");
    if (n >= cap) return 0;

    for (int i = 0; i < s_shape_n; i++) {
        unsigned k = (unsigned)snprintf(out + n, cap - n,
            "%s{\"w\":%d,\"h\":%d,\"seen\":%u,\"matched\":%u}",
            i ? "," : "", s_shape[i].w, s_shape[i].h,
            s_shape[i].seen, s_shape[i].matched);
        if (k >= cap - n) return 0;
        n += k;
    }
    n += (unsigned)snprintf(out + n, cap - n, "],\"runlog\":[");
    if (n >= cap) return 0;
    for (int i = 0; i < s_runlog_n; i++) {
        int j = (s_runlog_head - s_runlog_n + i + TP_RUNLOG * 2) % TP_RUNLOG;
        unsigned k = (unsigned)snprintf(out + n, cap - n,
            "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"m\":%d,\"hash\":\"%016llx\"}",
            i ? "," : "", s_runlog[j].x, s_runlog[j].y, s_runlog[j].w,
            s_runlog[j].h, s_runlog[j].matched,
            (unsigned long long)s_runlog[j].hash);
        if (k >= cap - n) return 0;
        n += k;
    }
    n += (unsigned)snprintf(out + n, cap - n, "],\"live\":[");
    if (n >= cap) return 0;

    /* Naming the live regions is what turns "a hash matched" into "the RIGHT
     * asset matched" -- the check worth having before anything is drawn. */
    for (int i = 0; i < s_region_n; i++) {
        const TpRegion *r = &s_region[i];
        const char *nm = (r->asset >= 0) ? s_asset[r->asset].name : "?";
        unsigned k = (unsigned)snprintf(
            out + n, cap - n,
            "%s{\"name\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
            i ? "," : "", nm, r->x, r->y, r->w, r->h);
        if (k >= cap - n)
            return 0;
        n += k;
    }
    return (unsigned)snprintf(out + n, cap - n, "]") < cap - n;
}
