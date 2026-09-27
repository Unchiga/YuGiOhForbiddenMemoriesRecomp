/* psx_wa_catalog.h -- what art this disc holds, and what it is called.
 *
 * ONE TABLE, TWO CONSUMERS
 * -------------------------
 * The asset table below is read by both halves of the texture-pack feature:
 *
 *   psx_wa_catalog.c    hashes each asset's pixels and registers the hash
 *                       under its name, so an upload can be recognised.
 *   psx_texture_export.c decodes each asset to a PNG under that same name,
 *                       so the player has a canvas to paint over.
 *
 * They MUST agree on names, or the exporter writes files the injector will
 * never look for -- a failure with no error message, just art that quietly
 * does nothing. Driving both from one table makes disagreement impossible
 * rather than merely unlikely.
 *
 * WHERE THE OFFSETS COME FROM
 * ----------------------------
 * WA_MRG.MRG starts at disc LBA 10102. The per-card record is 0x3800 from
 * 0x169000 -- padded to a sector boundary, NOT the 0x31E0 its contents
 * occupy -- holding a 102x96 portrait at +0 and a 40x32 thumbnail at +0x2AE0.
 * Duelist icons are packed flat from 0xF55000, 48x48 with a 64-entry palette,
 * so their stride is exactly content: 48*48 + 64*2 = 2432.
 *
 * Every image is pixels first, palette immediately after -- TIM with the
 * header stripped. Each entry was checked by decoding it and looking at the
 * result, not by trusting a table.
 *
 * SU.MRG starts at disc LBA 954 -- a separate archive from WA_MRG, holding
 * (per the decomp notes) "the main-menu package": the SAVE/LOAD/TRADE/... menu
 * text, requested as one async transfer of 0x73 sectors at boot. An entry
 * whose `mrg` is 1 reads from here instead, at a `base` relative to THIS
 * archive's start, not WA_MRG's.
 */
#ifndef PSX_WA_CATALOG_H
#define PSX_WA_CATALOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Names are FLAT within a family -- "cards/001", "cards/001_thumb",
 * "icons/duelist_001", "backgrounds/f1_03_a" -- so a pack is a handful of
 * directories with files in them, not hundreds of directories holding one
 * file each. The exporter appends ".png"; the injector looks up the bare
 * name.
 *
 * EVERYTHING IS IN UPLOAD LAYOUT
 * -------------------------------
 * Each asset is exported exactly as the game uploads it to VRAM, because that
 * is the unit of replacement: the injector recognises an upload by hashing
 * its payload, and the shader maps the VRAM region linearly into the
 * replacement image. The campaign backgrounds are the case where this
 * matters: they are stored and uploaded as TILED 128x512 plates that the
 * game draws in bands, so the exported PNG looks cut into strips. That is
 * not a bug -- an untiled "pretty" 320x160 export could never be matched to
 * an upload or mapped by the shader. Paint over the strips where they are.
 *
 * One family of images: `count` records addressed from `base`, each holding
 * one image at `off` inside it and (when it has one) a BGR555 palette at
 * `clut_off`, both relative to the record start.
 *
 *   bcd          0: records sit at base + index*stride.
 *                1: the index is BCD-composed first -- index 10 becomes 0x10
 *                   -- before multiplying. This is the game's OWN background
 *                   addressing (the routine at 0x8002DF2C), ported from
 *                   BGEx's calclba and verified against TCRF's quoted
 *                   addresses for all three families.
 *   bpp          8 or 4. A 4bpp image packs two texels per byte, low nibble
 *                first, and its hash covers the PACKED bytes -- again,
 *                because that is what the game uploads.
 *   clut_entries 0 means the image carries no usable palette here. 4bpp
 *                assets with 0 decode as a grey ramp on transparency
 *                (index 0 is the hole): honest structure, no invented
 *                colours. Their REPLACEMENTS still draw in full colour --
 *                the shader samples the replacement's own RGBA -- so an
 *                unresolved palette blocks pretty export, not reinjection.
 */
typedef struct {
    const char *name_fmt;   /* e.g. "cards/%03d" -- takes the id */
    uint32_t    base;       /* byte offset of record 0 in WA_MRG */
    uint32_t    stride;     /* record size in bytes */
    uint32_t    off;        /* image position within the record */
    uint32_t    clut_off;   /* palette position within the record */
    int         count;
    int         first;      /* id of record 0 */
    int         w, h;       /* texels */
    int         bpp;        /* 8 or 4 */
    int         clut_entries;
    int         bcd;        /* record index is BCD-composed (backgrounds) */
    /* VRAM anchor, in 16-bit words and rows. Non-zero anch_w means this art
     * is uploaded during boot -- before the injector is watching transfers --
     * so it is found by looking at the place it lives instead of by catching
     * the transfer that put it there. The bytes are still hashed and checked
     * against the disc, so a wrong anchor resolves to nothing rather than to
     * the wrong picture. See texpack_register_asset_anchored(). */
    int         anch_x, anch_y, anch_w, anch_h;
    /* 1: this art is recoloured by CLUT swapping, so a replacement must
     * supply SHAPE only and let the game keep supplying colour. The font is
     * the case that forces it -- one glyph sheet drawn through a bank of
     * palettes -- and a flat replacement would freeze every string in the
     * game to a single colour. Tinted art exports as a white mask on
     * transparency, since that is the neutral the tint multiplies against.
     * See texpack_set_tinted(). */
    /* 0: default -- tinted if it is UI, flat otherwise.
     * 1: always tinted, wherever it lives.
     * -1: never tinted, even though it is UI. For UI art a pack actually
     *     PAINTS, where the replacement's own colour is the whole point and
     *     freezing the palette is an acceptable price. */
    int         tint;
    /* Which band the anchor above describes, for art tall enough to be several
     * pages. The title logo needs it: its second band matches as a whole page
     * while the game overwrites part of the first. Defaults to band 0. */
    int         anch_band;
    /* Which MRG archive `base` is relative to. 0: WA_MRG.MRG (disc LBA
     * 10102) -- everything above this field. 1: SU.MRG (disc LBA 954), the
     * main-menu package -- a second archive entirely, found by hash-matching
     * a title-screen VRAM dump against a raw disc read rather than against
     * WA_MRG's bytes, because it is not in WA_MRG at all. */
    int         mrg;
    /* 1: this asset's own clut_off/clut_entries has been checked BY EYE
     * against a decode of the WHOLE sheet (every row, not just the region
     * that motivated finding the palette) and it renders coherently
     * throughout -- safe for export_one() to use as a REFERENCE render when
     * nothing has been captured live yet (see psx_texture_export.c).
     *
     * 0 (default): do NOT assume that. A tint == 0 asset's catalogued
     * palette is very often confirmed correct for only PART of the sheet --
     * dialog_frame's is the border's stone-brown, proven wrong for the
     * navy fill, which is the entire reason it is tint == 0 and not -1.
     * Auto-generating a whole-sheet reference from a partial palette is
     * exactly the "pixels times palette" ambiguity export_one()'s own
     * header warns against: it looks complete and is quietly wrong.
     * Set this only after doing what confirmed ui/menu_labels_1/2/3 --
     * decoding the FULL page and looking, not just the row that justified
     * adding the palette in the first place. */
    int         ref_whole;
    /* 0 (default): rows are packed tight at w*bpp/8 with no gap, as every
     * entry above assumes. Nonzero: the underlying disc/VRAM data is laid
     * out with THIS MANY TEXELS between the start of one row and the next,
     * even though the entry's own `w` may be narrower -- i.e. this entry
     * crops a `w`-wide column out of a wider physical row instead of owning
     * the row outright. Exists for a hash/anchor rectangle that must be
     * NARROWER than a sheet's full row width: register_one()'s anchor hash
     * otherwise always spans the whole row, so if part of that row is
     * permanently something else's content (a different asset's VRAM reuse)
     * the anchor can never validate at all. See ui/duel_panels_lp for the
     * motivating case -- the same "crop out of a wider layout" problem
     * campaign_characters' bg_retile_cols solves for whole VRAM pages,
     * applied at row granularity instead of page granularity. */
    int         row_stride;
} PsxWaAsset;

/* WHEN TO SET tint
 * ----------------
 * Whenever this table has no palette it can trust. A tinted asset exports as a
 * white MASK and takes its colour from the game at draw time, so:
 *
 *   - it needs no palette at all, since only the mask's alpha survives export;
 *   - at 1x it reproduces the stock art exactly, so a replacement that has not
 *     been upscaled yet is invisible rather than damaging;
 *   - a sheet drawn through MANY CLUTs comes out right, because the colour is
 *     resolved per texel from whichever palette the game chose.
 *
 * That last point is why guessing a single palette failed for the UI sheets.
 * Asking the renderer which CLUT an asset "was drawn with" has no answer when
 * every sprite on the sheet uses a different one, and taking the last one seen
 * repainted the whole chest screen in one sprite's colours. */

/* The table, and how many entries it has. */
const PsxWaAsset *psx_wa_catalog_table(int *count);

/* The CURATED path a row's file lives under -- "UI/Deck/panels", not the raw
 * "ui/panels" name_fmt encodes -- so a pack author's folder tree reads the
 * same as the Asset Manager's own browsing tree instead of the disc-derived
 * family grouping. This is the SINGLE source both sides of that agreement
 * read from: registration (register_one(), this file), export (export_one(),
 * psx_texture_export.c) and the Asset Manager's own path builders all call
 * this, so there is exactly one table to keep in sync, not three ways for
 * "where the manager looks" and "where the exporter writes" to drift apart.
 *
 * Returns 1 and fills `out` with "Top/Sub/leaf" when this row's name_fmt has
 * a curated home; 0 when it does not yet (a row not listed in the table
 * below), in which case the caller falls back to the raw name_fmt path --
 * every row still exports and matches SOMEWHERE, just not yet under a
 * curated name, exactly like an unlisted CatRule branch used to just not
 * show up in the menu. */
int psx_wa_catalog_display_path(const PsxWaAsset *a, int index,
                                char *out, unsigned cap);

/* Read `len` bytes at a WA_MRG byte offset, walking 2048-byte sectors.
 * Reads STOCK sectors, so it is unaffected by card-manager overrides. */
int psx_wa_catalog_read(uint32_t off, uint8_t *out, uint32_t len);

/* Decode one image to RGBA8. `out` needs w*h*4 bytes. A palette entry that is
 * exactly 0x0000 is fully transparent -- the PS1 rule the rest of this repo
 * already follows. */
int psx_wa_catalog_decode(const PsxWaAsset *a, int index, uint8_t *out);

/* As above, but with `pal` (pal_n BGR555 words) standing in for the asset's
 * palette. This is how art whose CLUT is not stored beside its pixels gets
 * exported in real colour: the renderer observes the palette the game
 * actually draws it with (texpack_observed_clut) and passes it here. Pass
 * NULL to use the table's own palette. */
int psx_wa_catalog_decode_pal(const PsxWaAsset *a, int index, uint8_t *out,
                              const uint16_t *pal, int pal_n);

/* Decode only rows [row0, row0+nrows) into `out` at that row offset, leaving
 * the rest untouched. Art taller than a 256-row texture page is uploaded and
 * palettised one band at a time, so exporting it correctly means decoding it
 * one band at a time too -- each with the palette that band is really drawn
 * with. Does NOT untile; backgrounds must go through decode_pal. */
int psx_wa_catalog_decode_rows(const PsxWaAsset *a, int index, uint8_t *out,
                               int row0, int nrows,
                               const uint16_t *pal, int pal_n);

/* Whether this asset is drawn TINTED: the replacement supplies shape and the
 * game keeps supplying colour. True for every UI asset by name, plus any row
 * that sets `tint`. Both the exporter (which must write a white mask) and the
 * registration (which must tell the injector) go through this, so the two
 * cannot disagree about what is tinted. */
int psx_wa_catalog_tinted(const PsxWaAsset *a);

/* 1 for UI art -- the family that gets a palette strip exported beside it,
 * because it is the art most likely to want an index-map replacement. */
int psx_wa_catalog_ui(const PsxWaAsset *a);

/* The asset's raw indices, one byte per texel (w*h of them). Exact, straight
 * from the disc -- the only reliable way to build an index map, since indices
 * cannot be recovered from a painted image. */
int psx_wa_catalog_indices(const PsxWaAsset *a, int index,
                           uint8_t *out, unsigned cap);

/* The asset's palette in INDEX ORDER (BGR555), which is what an index map is
 * quantised against. Returns the number of entries written, 0 if this asset
 * has no palette in the table. */
int psx_wa_catalog_palette(const PsxWaAsset *a, int index,
                           uint16_t *out, int max_entries);


/* The size the exporter writes to disk and the injector validates against.
 * Equals w,h except for tiled backgrounds, which export as the untiled scene. */
/* Takes the record index because one family's size varies per record: family
 * 2's first background layer is a parallax strip that is 512, 344 or 320 wide
 * depending on which one it is. */
void psx_wa_catalog_disp_size(const PsxWaAsset *a, int index, int *w, int *h);

/* Debug-server state line. */
int psx_wa_catalog_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_WA_CATALOG_H */
