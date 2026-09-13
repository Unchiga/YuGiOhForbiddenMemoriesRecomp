/* texture_pack.h -- draw HD art in place of the game's own 2D textures.
 *
 * WHAT THIS REPLACES, AND WHY IT IS NOT A DISC PATCH
 * ---------------------------------------------------
 * Patching art into the disc image cannot give you HD: the game decompresses
 * its art into RAM and hands it to the GPU at the original size, so a bigger
 * picture on the disc is just a bigger picture the game will not read. The
 * swap has to happen where the art reaches the renderer.
 *
 * Every piece of 2D art travels one road to the screen -- a CPU->VRAM transfer,
 * already decompressed, delivered to the renderer as one bulk rectangle by
 * gr_vram_transfer_in(). Watching that road catches all of it without knowing
 * anything about how the disc is laid out.
 *
 * NAMES, NOT HASHES
 * ------------------
 * An upload is identified by the content hash of its payload, but a hash is a
 * miserable thing to hand a pack author. So the TITLE registers what its art
 * is called -- "this hash is cards/042/image" -- and packs are then ordinary
 * directories of PNGs whose paths mirror those names:
 *
 *     <player data>/textures/cards/042/image.png
 *
 * That registration is the whole game-specific surface. This file knows
 * nothing about MRG files, card ids or where a disc keeps its artwork; it is
 * handed names and hashes and does the rest. A title that registers nothing
 * still gets a working framework, just one where nothing is ever replaced.
 *
 * INTEGER SCALES ONLY
 * --------------------
 * A replacement must be a whole multiple of the source's texel size: 102x96
 * art is replaced at 204x192, 408x384, and so on. Anything else is refused
 * rather than stretched -- counted in texpack_state_json()'s refusal tally,
 * since nothing here may print -- because the shader maps a primitive's
 * uv into the replacement as
 *
 *     atlas_texel = atlas_origin + (uv - region_origin) * scale
 *
 * and a fractional scale makes that inexact. The error shows up as seams along
 * sprite edges -- worst precisely where the art is cut into pieces, which on a
 * PS1 title is everywhere.
 *
 * WHAT A PALETTE SWAP DOES TO THIS
 * ---------------------------------
 * The console recolours art by swapping CLUTs rather than by storing several
 * copies, and a flat RGBA replacement cannot do that. So the lookup key is the
 * texture hash AND the CLUT hash, with a palette-agnostic fallback:
 *
 *     <name>.png              any palette -- one picture for every variant
 *     <name>@<cluthash>.png   this palette only
 *
 * The exact form wins when present. A pack that does not care about recolours
 * ships the plain file and never thinks about it; a pack that does ships one
 * per palette and keeps the variation.
 *
 * BACKENDS
 * ---------
 * Tracking and lookup here are backend-independent, but only the OpenGL
 * backend can currently DRAW a replacement -- it already samples VRAM as an
 * integer texture and decodes CLUTs in the fragment shader, which is the shape
 * a replacement path slots into. On Vulkan and software a pack is inert and
 * the stock art draws.
 */
#ifndef TEXTURE_PACK_H
#define TEXTURE_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle ----------------------------------------------------------- */

/* Set up <player data>/textures, the one pack slot. Safe before the renderer
 * is up, safe to call twice. */
void texpack_init(void);
void texpack_shutdown(void);

/* The renderer's VRAM mirror, so a draw can hash what it samples. Called from
 * gr_init(); until it is set the framework is inert. */
void texpack_set_vram(const uint16_t *vram);

/* Look for any registered anchored art in the VRAM mirror and place the
 * regions that confirm. Cheap once everything has resolved, so calling it
 * once a frame is the intended use. */
void texpack_resolve_anchors(void);

/* Ask for the active pack's FILES to be read again -- call after editing art
 * on disk, or the old decode keeps drawing. Registrations and regions are
 * untouched; only decoded pictures and the atlas are rebuilt, lazily.
 *
 * The request is deferred: texpack_reload_if_pending() performs it, and must
 * be called once per frame from the same thread that draws. Doing the work
 * inside a menu callback would free art out from under a draw in progress. */
void texpack_request_reload(void);
void texpack_reload_if_pending(void);

/* The palette the game last drew `name` with, read out of the VRAM mirror --
 * `entries` 16-bit BGR555 words. For art whose CLUT is not stored beside its
 * pixels, this is the only reliable source of its real colours, and without
 * it such art can only be exported as a grey index map. Returns 0 if the
 * asset has not been drawn yet, since nothing has told us the answer. */
int texpack_observed_clut(const char *name, uint16_t *out, int entries);

/* The same, for one band of a multi-band asset. Art taller than a 256-row
 * texture page is registered as several bands under one name; `src_y` picks
 * the band (0, 256, 512, ...). Bands do not share a palette, so exporting a
 * tall sheet correctly means asking per band. */
int texpack_observed_clut_page(const char *name, int src_y,
                               uint16_t *out, int entries);

/* The palettes this art is drawn with, EACH WITH THE REGION IT APPLIES TO.
 *
 * A sheet the game recolours has no single correct palette -- the dialogue
 * frame's border is stone where its fill is navy -- so one palette per asset
 * cannot describe it and an export through one of them is wrong everywhere
 * else. These are recorded per drawn rectangle instead, in whole-image texel
 * coordinates, so an export can colour each part the way the game does.
 *
 * Only regions actually drawn are known, so this fills in as screens are
 * visited. */
/* Every recorded region palette, as its own debug response -- the main state
 * JSON has no room for 60 of them and silently truncated to a dozen, which
 * read as "this asset has none" when it simply was not in the first twelve. */
int texpack_clut_json(char *out, unsigned cap);

int texpack_clut_rect_count(const char *name);
/* Where that region was last drawn on screen, so an export can lay the pieces
 * out as the player sees them. Returns 0 if it has no recorded destination. */
int texpack_clut_rect_dst(const char *name, int which,
                          int *x0, int *y0, int *x1, int *y1);

int texpack_clut_rect_get(const char *name, int which,
                          int *x0, int *y0, int *x1, int *y1,
                          uint16_t *pal, int entries);

/* Mark art as TINTED, meaning its replacement supplies shape and the game
 * still supplies colour.
 *
 * The console recolours art by swapping CLUTs over one set of pixels, and the
 * text font is the extreme case: one glyph sheet drawn through a bank of
 * palettes, which is how dialogue, menu headings and card names differ in
 * colour at all. A flat RGBA replacement has no palette, so replacing the
 * font normally would freeze every string in the game to one colour and
 * silently disable anything that colours text.
 *
 * A tinted replacement is multiplied by the colour the CLUT would have given,
 * so a white glyph on transparency comes out in whatever colour the game
 * selected -- one file, every colourway preserved. Paint tinted art neutral:
 * its own colour multiplies in, so white means "exactly the game's colour".
 * Only 4/8bpp art has a CLUT; tinting a 16bpp asset does nothing. */
/* Anchor one BAND of an already-registered multi-page asset.
 *
 * texpack_register_asset_anchored() makes a standalone image; this attaches an
 * anchor to a band that is part of a larger one, so the band still maps into
 * the same file. Needed because a tall asset can have one band that matches as
 * a whole page and another the game partly overwrites -- the title logo is
 * both at once. The band's registered extent must equal the anchor rectangle,
 * or the hash covers different bytes than the rectangle does and never
 * matches. */
int texpack_set_anchor(const char *name, int src_y,
                       int vram_x, int vram_y, int vram_w, int vram_h);

int texpack_set_tinted(const char *name, int on);

/* Record this art's palettes PER DRAWN REGION (see texpack_clut_rect_get).
 * Opt-in: the pool is shared, and art whose palette the title already knows
 * would fill it before the art that needs it is ever drawn. */
int texpack_set_track_cluts(const char *name, int on);

/* Never resolve to the plain "<name>.png" fallback for this asset -- only to
 * a matching per-draw element or an exact @cluthash file. See the .c for why:
 * a single flat file cannot describe art the game recolours. */
int texpack_set_no_plain_fallback(const char *name, int on);

/* ---- what the title registers -------------------------------------------- */

/* "An upload whose payload hashes to `hash` is the art called `name`."
 *
 * `name` is a pack-relative path without extension, e.g. "cards/042/image".
 * w and h are the source's size in TEXELS, which is what an integer scale is
 * measured against -- not the size of the VRAM rectangle the upload lands on,
 * since a 4bpp texel is a quarter of a VRAM word.
 *
 * Returns 0 if the registry is full. Registering the same hash twice keeps the
 * first name. */
int texpack_register_asset(const char *name, uint64_t hash, int w, int h);

/* Register art that the game uploads BEFORE this framework starts watching --
 * the in-game text font is the case: it is written to VRAM during boot and
 * never transferred again, so no amount of watching transfers will ever see
 * it. Instead of a transfer, such an asset names the VRAM rectangle it
 * occupies (`vram_*`, in 16-bit words and rows) and is looked for there
 * whenever a whole-VRAM write lands.
 *
 * The position only says where to LOOK. The rectangle is still hashed and
 * compared against `hash`, so if the position or size is wrong the asset
 * simply does not resolve -- it never binds a name to whatever art happens to
 * be sitting at those coordinates. `w`,`h` are the texel size as usual. */
int texpack_register_asset_anchored(const char *name, uint64_t hash,
                                    int w, int h,
                                    int vram_x, int vram_y,
                                    int vram_w, int vram_h);

/* A replacement transform: convert the in_w x in_h RGBA picture the player
 * painted into the out_w x out_h RGBA the game actually samples. Used for art
 * the game stores scrambled in VRAM (backgrounds) so the player edits a clean
 * image and this repacks it. RGBA8, tightly packed, top-down. */
typedef void (*TexPackRetile)(const uint8_t *in, int in_w, int in_h,
                              uint8_t *out, int out_w, int out_h);

/* Like texpack_register_asset, but the file on disk is DISPLAY layout
 * (disp_w x disp_h -- what the scene looks like) while the game samples the
 * TILED layout (w x h, which the hash covers). A replacement is validated as
 * an integer multiple of disp_w x disp_h, then `retile` packs it to w x h at
 * that scale before it enters the atlas. */
int         texpack_register_asset_tiled(const char *name, uint64_t hash,
                                         int w, int h, int disp_w, int disp_h,
                                         TexPackRetile retile);

/* The general form. An asset may be one PAGE of a larger image, because a PS1
 * texture page is at most 256 rows: anything taller is uploaded as several
 * pages and must be registered (and matched) per page.
 *
 *   w,h            what THIS page is -- the size the game uploads and samples
 *   disp_w,disp_h  the file on disk, which covers the whole image
 *   page_w,page_h  the whole image after `retile` (== disp when retile is 0)
 *   src_x,src_y    where this page sits inside that whole image
 *
 * Several pages therefore share one file: the loader applies retile once, then
 * crops this page out of the result. A pack author still edits one picture. */
int         texpack_register_asset_page(const char *name, uint64_t hash,
                                        int w, int h,
                                        int disp_w, int disp_h,
                                        int page_w, int page_h,
                                        int src_x, int src_y,
                                        TexPackRetile retile);

/* FNV-1a over a buffer, so a title can hash disc bytes with the same function
 * this hashes uploads with. Both sides MUST agree or nothing ever matches. */
uint64_t texpack_hash(const void *data, unsigned len);

/* Same idea, widened to 128 bits, for identifying a PALETTE specifically.
 * A palette is 32-512 bytes of highly structured BGR555 data, and across a
 * whole session's worth of distinct UI colourings, two genuinely different
 * palettes were found to collide under plain 64-bit FNV-1a -- a dialogue
 * border's real palette and an unrelated one exported earlier landed on the
 * identical digest, so the wrong file was served with full confidence. */
void texpack_hash128(const void *data, unsigned len, uint64_t out[2]);

/* ---- packs --------------------------------------------------------------- */

int         texpack_pack_count(void);
const char *texpack_pack_name(int index);
/* The pack's full directory on disk -- <player-data>/textures, normally --
 * but a title reading/writing a pack's files (an asset-browsing tool, say)
 * needs the real path rather than rebuilding that guess itself. NULL out of
 * range. */
const char *texpack_pack_path(int index);
int         texpack_active_pack(void);          /* -1 when none */
void        texpack_set_active_pack(int index); /* -1 disables replacement */
/* Point the active pack at an arbitrary absolute folder -- what a "choose a
 * different folder" control wants, as opposed to texpack_set_active_pack's
 * index into the packs already found. Reuses the matching pack slot if this
 * path is already one of them, else claims (or replaces) a synthetic slot for
 * it, so texpack_active_dir() and everything built on it immediately agree on
 * exactly this folder. */
void        texpack_set_active_dir(const char *path);

/* The active pack's own folder -- texpack_pack_path(texpack_active_pack()),
 * resolved the one place instead of at every caller, and never empty: with
 * nothing active yet it falls back to <player-data>/textures, the same
 * folder texpack_init() sets up as the one pack slot, so there is always a
 * real place to write to. This is the single definition of "the pack folder"
 * that the raw VRAM injector, the exporter, and any higher-level replacement
 * that wants to live beside it (card art, CPU portraits) all share -- so two
 * windows never quietly agree on two different folders. */
void        texpack_active_dir(char *out, unsigned cap);

/* Player-facing on/off. Distinct from selecting no pack: this leaves the pack
 * loaded and its atlas populated, so toggling is instant and free -- which is
 * what you want when comparing replaced art against the original. */
void        texpack_set_enabled(int on);
int         texpack_enabled(void);

/* "Is there a replacement ready for this asset right now?" -- 1 only when
 * replacement is enabled, a pack is active, and that pack actually ships the
 * file. A title uses this to stand its OWN art-replacement paths down when the
 * pack already covers an asset, so the two do not fight over the same pixels. */
int         texpack_has_replacement(const char *name);

/* Bumped only when the active pack's file list is (re)scanned -- activation,
 * a manual reload, or (once one exists) the pack directory itself changing --
 * never by ordinary draws or uploads. A title that gates its OWN replacement
 * path on texpack_has_replacement() at load time (see that function's own
 * comment) can be asked before the pack has finished its first scan, and a
 * per-file mtime watch never re-asks once its own file hasn't changed. Poll
 * this alongside that watch and re-run the query when it changes, so "the
 * pack only became ready a moment after I first checked" self-corrects
 * instead of sticking with a stale false forever. */
unsigned    texpack_file_scan_generation(void);

/* ---- VRAM observation (called by the renderer facade) -------------------- */

/* One completed CPU->VRAM transfer. The rectangle becomes a candidate source
 * region and anything it overlaps stops being one. x/y/w are in VRAM 16-bit
 * words, matching gr_vram_transfer_in(). */
void texpack_on_upload(int x, int y, int w, int h, const uint16_t *data);

/* VRAM changed some other way -- a fill, a VRAM->VRAM copy, a savestate.
 * Anything the rectangle touches stops being a valid source. */
void texpack_invalidate_rect(int x, int y, int w, int h);
void texpack_invalidate_all(void);

/* ---- draw-time lookup ---------------------------------------------------- */

/* Where a primitive should read its replacement, in atlas texels. A
 * primitive's uv is relative to its texture page while the replacement is
 * relative to the source region, so the shader needs the region's origin
 * expressed in this primitive's own texel space to line them up. */
typedef struct {
    float atlas_x, atlas_y;   /* the replacement's top-left in the atlas */
    float org_u, org_v;       /* the source region's origin, in prim texels */
    float scale;              /* replacement pixels per source texel (>= 1) */
    /* How the replacement is to be drawn:
     *   0 FLAT     the file's own RGB. HD colour, but the palette is frozen.
     *   1 TINTED   the game's CLUT colour, the file's alpha. Correct under
     *              every palette, but only cutout edges can sharpen.
     *   2 INDEXED  the file holds INDICES, not colour, and the shader looks
     *              them up in the game's CLUT per pixel -- what the console
     *              itself does, just at the replacement's resolution. Sharp
     *              AND correct under every palette; limited to the palette's
     *              own colours. */
    int   mode;
} TexPackHit;

/* Resolve one textured primitive: 1 and fills `out` when a replacement
 * applies, 0 when the stock path should draw.
 *
 * base_x/base_y are the texture page origin in VRAM words, depth is 0/1/2 for
 * 4/8/16bpp, clut_x/clut_y locate the palette, and lim is the inclusive
 * sampled uv bound {lo_u, lo_v, hi_u, hi_v} from psx_uv_tri_limits(). Results
 * are cached, so calling this per primitive is cheap after the first draw of
 * each distinct asset. */
/* `dst` is the primitive's SCREEN bounding box {x0,y0,x1,y1}, or NULL. It is
 * not used for resolving -- it is recorded, so an export can reassemble a
 * sheet the way the screen shows it instead of the way VRAM stores it. */
int texpack_on_draw(int base_x, int base_y, int depth,
                    int clut_x, int clut_y, const int lim[4],
                    const int dst[4], TexPackHit *out);

/* A VRAM-to-VRAM copy (GP0(80h)) never goes through texpack_on_draw -- it
 * moves raw words, not a textured primitive, so it has no depth/clut/uv to
 * report and nothing here would notice it happened. That is a real blind
 * spot: an asset whose content the game COPIES to a different address
 * before actually drawing it from there is invisible to every mechanism in
 * this file, anchored or not -- the anchor still matches because the source
 * bytes sit untouched, but no primitive ever samples that address directly,
 * which looks identical to "genuinely never drawn" from texpack_on_draw's
 * side. ui/menu_labels_1 is the confirmed case this exists for: LIVE at its
 * anchor, zero real draws there across two isolated sessions.
 *
 * Called from gpu_copy_rect() with the raw GP0(80h) rectangle in VRAM words,
 * both endpoints. Records it (in a small ring, see the debug server's
 * "copies" list) only when the SOURCE overlaps a currently registered
 * region, so this stays cheap on every frame's unrelated copies and only
 * ever holds copies that might explain why a specific asset never resolves. */
void texpack_note_copy(int sx, int sy, int w, int h, int dx, int dy);

/* ---- unknown-asset capture -------------------------------------------------
 *
 * A discovery aid, separate from replacement. texpack_on_draw's "no region
 * claims this primitive" case currently just counts it (see the debug
 * server's "unres" list) -- everything past that point (which disc bytes
 * this is, what colour it really draws in) has to be reconstructed by hand:
 * take a savestate, pull VRAM, hash-match it against a disc dump, guess a
 * bit depth, guess a palette, look, guess again. Every one of those steps is
 * information texpack_on_draw ALREADY HAS at the moment it decides "no match"
 * -- the exact VRAM rectangle, the exact live CLUT -- and is about to throw
 * away. This keeps it instead: decode the rectangle through its own live
 * palette, right there, and queue it for a game layer to save. No disc
 * offset, no hash-matching, no guessing; the picture comes from what was
 * actually on screen, in the colour it was actually shown in. */
void texpack_set_capture_enabled(int on);
int  texpack_capture_enabled(void);

typedef struct {
    int base_x, base_y, depth, clut_x, clut_y;  /* same meaning as texpack_on_draw */
    int lim[4];
    int dst[4];             /* screen bbox, or all zero if draw passed NULL */
    int w, h;                /* the captured rectangle, in texels */
    const uint8_t *rgba;     /* w*h*4; BORROWED -- valid for the process's
                             * life, do not free. Kept here (not handed off)
                             * because a game layer needs to see the WHOLE
                             * accumulated pile repeatedly to group pieces
                             * that sit next to each other on screen (one
                             * panel's worth of icons, one logo's worth of
                             * bands), the same way write_screens() already
                             * groups a known asset's recorded regions --
                             * a one-shot consume-once queue cannot do that,
                             * since the piece that completes a group might
                             * not arrive until several frames after the
                             * first one in it. */

    /* The SAME rectangle, word-aligned and in the SAME shape
     * rescan_anchored() reads and hashes for an anchor check: anch_w/anch_h
     * VRAM words starting at anch_x/anch_y, raw (not palette-decoded), row-
     * major. A game layer that saves this alongside the picture can hash it
     * with texpack_hash() and hand the result straight to
     * texpack_register_asset_anchored() -- a real, permanently re-matchable
     * catalog entry, with NO disc offset ever needed, because the hash this
     * produces is bit-for-bit what rescan_anchored() will compute the next
     * time this exact content is on screen. Without this, a hash computed
     * from the decoded RGBA (or from lim[], which is texel-granular and not
     * necessarily word-aligned) would not match anything real -- it would
     * just be a number that happens to describe a picture nobody ever
     * re-derives the same way twice. */
    int anch_x, anch_y, anch_w, anch_h;
    const uint16_t *raw;     /* anch_w*anch_h words; BORROWED, do not free */
} TexPackCapture;

/* How many distinct captures exist so far this session. Grows as new (rect,
 * palette) combinations are seen; never shrinks except when the fixed pool
 * (TP_CAP_ALL in texture_pack.c) is full, at which point capture silently
 * stops accepting new ones rather than evicting what a game layer may already
 * be part-way through grouping -- same "keep what is already recorded"
 * choice s_clutrect's own pool-full case makes. */
int texpack_capture_count(void);

/* Read capture `i` (0-indexed, stable for the life of the process). 1 on
 * success, 0 if `i` is out of range. */
int texpack_capture_get(int i, TexPackCapture *out);

/* ---- atlas (for the drawing backend) ------------------------------------- */

int texpack_atlas_dim(void);   /* side of the square RGBA8 atlas, in pixels */


/* ---- diagnostics ---------------------------------------------------------- */

/* Debug-server state line, in this title's usual shape. Carries the counts and
 * -- the useful part -- why replacements were refused, since a pack that
 * silently does nothing is the normal failure and stdout is not available to
 * explain it (psxrecomp/CLAUDE.md rule 3). */
int texpack_state_json(char *out, unsigned cap);

/* Diagnostic only: record the raw flag / vertex colour of a primitive that is
 * about to be (or was) HD-replaced, so texpack_state_json can report what the
 * game's own GPU command actually carried. Called from the renderer, which
 * already has both as ordinary GPU state. */
void texpack_debug_note_prim(int rawtex, const float col[3]);

/* Per-VRAM-page hashes and what they resolve to, for locating art the game
 * draws from but nothing has registered. */
int texpack_vram_pages_json(char *out, unsigned cap);

/* Hand the backend the next replacement still needing upload into its atlas
 * texture, as tightly packed RGBA8. 0 when nothing is pending. Call in a loop
 * before drawing. */
int texpack_take_pending(int *x, int *y, int *w, int *h, const uint8_t **rgba);

#ifdef __cplusplus
}
#endif

#endif /* TEXTURE_PACK_H */
