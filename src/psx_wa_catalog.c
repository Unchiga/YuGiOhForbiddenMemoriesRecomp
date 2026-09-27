/* psx_wa_catalog.c -- tell the texture-pack framework what this title's art is
 * called, so a pack author edits cards/001/image.png and not a hash.
 *
 * This is the whole game-specific surface of texture packs. texture_pack.c
 * knows nothing about MRG files or card ids; it is handed
 *
 *     "an upload whose payload hashes to H is the art called N"
 *
 * and does the rest. Everything below is just working out H and N for the art
 * this disc holds.
 *
 * WHERE THE OFFSETS COME FROM
 * ----------------------------
 * WA_MRG.MRG starts at disc LBA 10102, and the per-card record layout is the
 * one tools/wa_assets.py documents and verifies by eye: records of 0x3800 from
 * 0x169000, each holding a 102x96 8bpp portrait at +0 and a 40x32 thumbnail at
 * +0x2AE0, pixel data first and CLUT after (TIM with the header stripped).
 *
 * The record stride is 0x3800 and NOT the 0x31E0 its contents occupy -- each
 * record is padded out to a sector boundary. Getting that wrong still decodes
 * card 1 correctly and turns every later card into noise, which is exactly how
 * it was caught.
 *
 * WHAT IS HASHED, AND THE RISK IN IT
 * -----------------------------------
 * The hash covers the PIXEL BYTES only -- 102*96 for a portrait -- because
 * that is what the game uploads to VRAM. The CLUT that follows on disc travels
 * separately and lands somewhere else in VRAM, so including it would
 * guarantee a mismatch.
 *
 * That leaves one assumption this cannot check without running: that the game
 * uploads a portrait as ONE transfer of exactly those bytes. If it stages them
 * through a larger buffer, or uploads a sub-rectangle at a time, the payload
 * hash will not match and the card simply never resolves. That is why
 * psx_wa_catalog_state_json() reports how many assets were registered
 * alongside texpack_state_json()'s live region count: registered-but-never-
 * matched is the signature of exactly this, and it is a fact about the game's
 * loader, not a bug in the hashing.
 */

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_card_packs.h"
#include "psx_video_menu.h"
#include "psx_wa_catalog.h"
#include "texture_pack.h"

#define WA_LBA        10102u        /* WA_MRG.MRG's first sector */
#define SU_LBA          954u        /* SU.MRG's first sector -- the main-menu
                                      * package, a separate archive entirely */
#define SECTOR        2048u
#define CARD_COUNT    722

/* The one table both halves of the feature read. See psx_wa_catalog.h for why
 * that matters more than it looks. */
static const PsxWaAsset ASSETS[] = {
    /* name_fmt               base        stride     off       clut_off  cnt first  w    h  bpp clut bcd */

    /* Cards: 0x3800 records (sector-padded, NOT the 0x31E0 of content).
     * Portrait, its 40x32 duel thumbnail, and the 96x14 name strip -- a 4bpp
     * coverage mask the game tints at draw time. A replacement freezes that
     * tint, which for the name strip is usually the point (a redrawn logo)
     * but is worth knowing. */
    { "cards/art/%03d",         0x169000u,  0x3800u, 0x0000u,   9792u,  722, 1, 102,  96, 8, 256, 0 },
    { "cards/mini/%03d",   0x169000u,  0x3800u, 0x2AE0u,  0x2FE0u, 722, 1,  40,  32, 8, 256, 0 },
    { "cards/titles/%03d",       0x169000u,  0x3800u, 0x2840u,       0u, 722, 1,  96,  14, 4,   0, 0,
      0, 0, 0, 0, 1 },

    /* The DUEL's own separate copy of this same 40x32 thumbnail: one flat
     * sector per card (WA_MRG "sector id-1" -- psx_card_packs.c's
     * THUMB_LBA), not inside the art record at all, a completely different
     * on-disc region that happens to hold the same logical picture.
     * Byte-verified against the mounted disc image (2026-09-14): 1280 index
     * bytes then a 64-entry CLUT with no gap, matching the layout
     * psx_card_packs.c already reads/writes there. Registered separately
     * from "cards/mini" above -- but curated to the exact same "Card
     * assets/card thumbnails" folder below, so the one uploaded file backs
     * both -- which is what lets a card actually in your hand or on the
     * field during a duel go through the raw injector too, instead of the
     * quantized disc override psx_card_packs.c otherwise has no HD path to
     * skip for it. */
    { "cards/duel_thumb/%03d",  0x0u,       0x800u,  0x0000u,    1280u, 722, 1,  40,  32, 8,  64, 0 },

    /* Duelist icons pack flat: the record IS the content, 48*48 + 64*2. The
     * 64-entry palette is what pins that -- no pixel indexes past 63, the
     * invariant that caught a wrong stride here once already. */
    { "free_duel/portrait_%03d", 0xF55000u,    2432u, 0x0000u,   2304u,   40, 0,  48,  48, 8,  64, 0 },

    /* Campaign backgrounds and cutscene plates.
     *
     * Addressed LINEARLY: record = base + index*stride. This used to use the
     * game's BCD scheme (bcd=1), which was wrong, and wrong in a way that hid
     * itself -- BCD is the identity for 0..9, so the first ten backgrounds
     * looked perfect while index 10 fetched record 16 and index 41 fetched
     * record 65, off the end of the family and into the next one's data. That
     * is where the "noise" came from.
     *
     * The layout proves the linear reading: 50 static records of 0x10800 from
     * 0x10EA800 land exactly on 0x1423800, family 1's base, and 42 of 0x28800
     * from there land exactly on 0x1AC8800, family 2's. Three families tile
     * the region end to end with nothing left over. Static records 0 and
     * 42..49 are blank, so this row starts at record 1 and takes 41.
     *
     * Each record ends with a CLUT SECTOR, and the layers index into it:
     * layer a at +0, b at +0x200, c at +0x400. Family 2's b was reading a's
     * palette and its c was reading b's.
     *
     * Layer c is 4bpp with 16 colours, not another 8bpp page -- exporting it
     * as 8bpp read twice the bytes at half the width.
     *
     * TILED: each plate is the 128x512 page the game uploads, drawn in bands.
     * Export looks striped on purpose -- see the header.
     *
     * TWO KNOWN LIMITS. (1) TCRF documents 4 CLUTs per background; we export
     * through the first, so images whose game code selects a different CLUT
     * export with wrong colours (a green cast). This affects the EXPORT
     * reference only -- reinjection hashes the pixel bytes and the shader
     * draws the replacement's own RGBA, so a background still replaces
     * correctly regardless of which CLUT is unknown. (2) BGEx notes some
     * records repeat identical images under different indices; identical
     * bytes register once under the first name, so replace that one. The
     * base is 0x10FB000 -- TCRF's stated start, verified: index 0 there is a
     * real background, while one stride earlier is empty. */
    { "backgrounds/static/%02d",   0x10FB000u, 0x10800u, 0x00000u, 0x10000u, 41, 0, 128, 512, 8, 256, 0 },
    { "backgrounds/comp/%02d_layer1", 0x1423800u, 0x28800u, 0x00000u, 0x28000u, 42, 0, 128, 512, 8, 256, 0 },
    { "backgrounds/comp/%02d_layer2", 0x1423800u, 0x28800u, 0x10000u, 0x28200u, 42, 0, 128, 512, 8, 256, 0 },
    { "backgrounds/comp/%02d_layer3", 0x1423800u, 0x28800u, 0x20000u, 0x28400u, 42, 0, 256, 256, 4,  16, 0 },
    /* Same "BGEx" duplication the comment above documents for
     * backgrounds/static, confirmed here too: records 00, 10, 11 and 12 are
     * byte-identical on disc across all three layers (md5-verified 2026-09-11
     * against WA_MRG.MRG directly -- every layer and the whole 0x38800-byte
     * record match). Not a cataloguing bug; Konami really did reuse one
     * cutscene picture across four index slots. Same consequence as the
     * static case: the injector matches by content hash, so a replacement
     * painted for 00 applies to all four, and 10/11/12 cannot be given
     * separately different art because the game never draws different
     * pixels for them. */
    { "backgrounds/mov/%02d_layer1", 0x1AC8800u, 0x38800u, 0x00000u, 0x38000u, 13, 0, 128,1024, 8, 256, 0 },
    { "backgrounds/mov/%02d_layer2", 0x1AC8800u, 0x38800u, 0x20000u, 0x38200u, 13, 0, 128, 512, 8, 256, 0 },
    { "backgrounds/mov/%02d_layer3", 0x1AC8800u, 0x38800u, 0x30000u, 0x38400u, 13, 0, 256, 256, 4,  16, 0 },

    /* The dialogue box's carved stone frame and panel fills -- one 256x256
     * 4bpp page, the one the text-box border actually draws from (VRAM word
     * x=832 on a dialogue screen). Its base is exactly the start of
     * memories-decomp's confirmed "Campaign-scene package" (see
     * notes/mrg-files.md there), whose very next phase -- 0xF33800, a
     * documented 256x1-entry palette stage -- decodes this page in full
     * stone-brown colour, confirmed by eye against a local WA_MRG.MRG copy.
     *
     * That stone-brown palette is real, but it is only the BORDER's palette,
     * not the whole page's -- this is in fact THE motivating case for the
     * per-region capture design (see texture_pack.c's own header: "the
     * dialogue frame's carved border is stone and its panel fill is navy,
     * from the same pixels"). Set to tint -1 earlier this session on the
     * wrong assumption that the whole page shares that one palette; in game
     * the panel FILL is dark blue and only the carved border is stone, so a
     * flat decode through the border's palette washes stone-brown over the
     * fill too. Back to tint 0 (default) so write_elements()/write_screens()
     * capture the border and the fill through their own live CLUTs
     * separately -- needs a dialogue box actually on screen, with HD
     * textures on, before "Export stock textures" produces the two
     * correctly-coloured pieces instead of one wrongly-flat one. */
    { "ui/dialog_frame",    0xF2B800u, 0x8000u, 0x0000u, 0x8000u,   1, 0, 256, 256, 4,  16, 0,
      0, 0, 0, 0, 0},

    /* The menu panel sheet -- the CHEST / ORDER / DECK furniture.
     *
     * This used to be one 256x768 "ui/font_sheet" asset covering 0xB48000 to
     * 0xB60000. That was wrong in two ways. The block is loaded by a single
     * read of 0x36 sectors and lands as THREE separate texture pages -- the
     * fonts at (640,0), the duel UI at (704,0), this at (768,0) -- and each
     * page carries its own palette, so decoding all 768 rows through one CLUT
     * painted everything in the font's colours. It also made one file the
     * canvas for three unrelated regions, which then had to be upscaled and
     * cut up more than once.
     *
     * Each page is now its own asset with its own palette and its own file.
     * This is the only one of the three that matches as a whole 256-row page
     * (verified 256/256 rows against three savestates); the other two are
     * partly overwritten in VRAM and so are anchored below.
     *
     * No palette here: this sheet's CLUT is not in the block loaded with it,
     * so a flat decode exports grey until a palette is captured live.
     *
     * Was tint -1 ("one true palette, just not yet known"), on the theory
     * that this page has exactly one colouring the way dialog_frame does.
     * The flat export disproved that once a palette WAS captured: CHEST,
     * ORDER, DECK, 1P/2P and the IN/OUT/LEFT/RIGHT arrow rows all came out
     * uniform grey -- the one colouring a single capture happened to see,
     * forced onto three unrelated menu screens (Chest/Order/Deck) that each
     * highlight their own furniture differently. Same shape as
     * ui/logo/konami: one sheet, several contexts, no way to tell them
     * apart except by capturing each draw's own CLUT. tint 0 (default) so
     * write_elements()/write_screens() do that, the same live per-region
     * capture already proven for duel_labels and the menu labels -- needs
     * the Chest, Order and Deck screens visited with HD textures on before
     * "Export stock textures" produces anything for it. */
    { "ui/panels",       0xB58000u,  0x8000u, 0x0000u,      0u,   1, 0, 256, 256, 4,   0, 0,
      0, 0, 0, 0, 0},

    /* The in-game TEXT fonts -- every glyph the game prints, both the small
     * 8x12 face dialogue uses and the larger one below it. Shares bytes with
     * the top of the sheet above, but it is a separate asset because it is a
     * separate THING in VRAM: the sheet's three 256-row bands land in three
     * adjacent texture pages, (640,0) (704,0) (768,0), and the game then
     * reuses the tail of this one for other art. Measured against three
     * savestates, rows 0..231 at (640,0) are byte-identical to the disc in all
     * of them while 232..254 differ in each -- so the 256-row band registers a
     * rectangle that never exists, and only an anchored sub-rectangle can
     * match.
     *
     * 168 rows, not 232: that is where the last glyph strip ends (content runs
     * 0..34, 36..117, 120..134, 136..151, 153..167; the rest is padding). It
     * covers every glyph while staying 64 rows clear of the region the game
     * overwrites, so a scene that reuses that tail differently cannot break
     * the match. Anchoring is exact -- one changed row and nothing draws --
     * so the margin is the point. */
    { "ui/font/font",      0xB48000u, 0x40000u, 0x0000u, 0x18800u,   1, 0, 256, 168, 4,  16, 0,
      640, 0, 64, 168, 1 },

    /* The DUEL UI -- terrain names, the field labels, the button and panel
     * furniture. It is the sheet's middle band, which lands in the texture
     * page at (704,0), and it needs two entries rather than one for the same
     * reason the font needed any: the game reuses parts of the page for
     * transient art, so the band never matches as a whole.
     *
     * Measured across three savestates (one taken mid-duel), the rows that are
     * byte-identical to the disc in ALL of them are local 32..127 and
     * 129..239. Row 128 is a divider line and rows 0..31 and 240..255 hold art
     * the game overwrites -- the mismatch count there drifts per scene
     * (216/214/213 rows), which is what a reused scratch region looks like.
     * The two runs are registered separately so each anchors only over rows
     * that are actually stable; a region covering the reused rows would draw
     * this sheet's art on top of whatever the duel put there.
     *
     * Offsets are the band's own: sheet row 288 is 0xB48000 + 288*128, row 385
     * likewise. No palette in the table -- the CLUT these are drawn with is
     * not stored beside them, so they export grey until the duel screen has
     * been on show once and the renderer has reported the real one
     * (texpack_observed_clut). Not tinted: unlike the glyphs these are
     * multi-coloured furniture drawn through one palette, so a replacement
     * supplies its own colour. */
    /* The monster-type icon strip -- rows 0..31 of this SAME sheet, the band
     * immediately above duel_labels. Confirmed in-game to be the small
     * per-card-type symbols (Dragon, Spellcaster, Fiend, ...), not the
     * guardian-star wheel a second, unrelated teatools tool (process-
     * guardianstars) claims for this exact byte range -- its own notes
     * describe reading WA 0xB50000 (128 bytes per row, CLUT at WA 0xB60500)
     * for an unrelated "who beats whom" matrix editor, and that claim does
     * not match what actually shows on screen. Decoding the bytes gives
     * sixteen crisp, correctly-shaped 16x16 icons laid out as a 16-wide
     * grid; only the RIGHT 8 are real (Deck-list-confirmed 2026-09-11: a
     * primitive samples texpage (704,0) 4bpp at word x=736+, exactly texel
     * column 128 -- the right half's own start). The left 8 are blank
     * filler, and this entry now excludes them entirely rather than just
     * noting it: w=128 (texel 128..255 of the disc row only), base shifted
     * 64 bytes in to match, row_stride carries the real 256-texel row
     * spacing so a strided read still lands correctly (see row_stride's own
     * comment in the header -- same idea as ui/duel_panels_lp).
     *
     * This split is not cosmetic, it is the actual fix: the blank left half
     * is PERMANENTLY not stock here -- checked live on a duel screen and
     * separately on this Deck-list screen, byte-identical both times, 655 of
     * 2048 bytes wrong in the left half and ZERO wrong in the right half
     * every single time. Something else genuinely lives at word x=704..736
     * always, not just during a duel as first assumed. The OLD entry's
     * anchor spanned the full 256-texel row, so that permanent left-half
     * mismatch failed the hash check every time and the whole strip -- real
     * icons included -- never validated, ever, on any screen tried. This
     * entry's anchor covers only the clean right half, so it validates
     * wherever the real icons are actually drawn (confirmed live on the Deck
     * list) regardless of whatever occupies the left half.
     *
     * No static palette (clut_entries 0): like duel_labels, each icon is
     * drawn through its own CLUT slot and recolours per context, so this
     * stays tinted rather than guessing one colour for all of them. */
    { "ui/icons/monster_types", 0xB50040u,  0x0800u, 0x0000u,      0u,   1, 0, 128,  32, 4,   0, 0,
      736, 0, 32, 32, 0, 0, 0, 0, 256},

    /* NOT opted out of tinting: measured against a live-state VRAM dump, this
     * exact pixel data is reused for at least two different visual contexts
     * (the duel HUD, and a menu dialog border/scrollbar sharing the sheet) --
     * each drawn through its own CLUT. A flat (opted-out) file can only ever
     * be correct for the palette it was captured under; tint mode takes
     * colour from the game's live CLUT every time, so it is right in both
     * places at once. Nothing has been painted here yet, so this costs
     * nothing to revert. */
    { "ui/duel_labels",         0xB51000u,  0x3000u, 0x0000u,  0xF9A0u,   1, 0, 256,  96, 4,  16, 0,
      704, 32, 64, 96, 0},
    /* The in-duel LP/COM/YOU HUD box is a single primitive sampling VRAM
     * (736,128)-(800,168) -- 40 rows starting at row 128, one row ABOVE this
     * entry's anchor (129..239). texpack_on_draw()'s containment check is
     * `y0 >= r->y`, so that whole 40-row primitive is rejected outright for
     * missing row 128, not just its top row -- this is the entire reason the
     * box never exports.
     *
     * TRIED extending anch_y back to 128 (2026-09-11) and REVERTED it: row
     * 128 is disc-contiguous with this sheet (duel_labels ends at exactly
     * 0xB54000, so 0xB54000 IS this sheet's own byte 0) but is NOT what's
     * live there during a duel. VRAM (736,128) itself decodes correctly as
     * "LP/COM(blue)/YOU(red)/LP" -- that part is genuinely this sheet.  But
     * x=704..736 of that SAME row is not: live-vs-disc diff (checked twice,
     * moments apart, mid-duel, identical both times -- not frame noise) shows
     * a small, stable, unrelated sprite sitting there instead of this sheet's
     * own stock pixels. Per the user's own check, that stray content belongs
     * to ui/icons/monster_types -- some duel-only draw reuses this VRAM
     * range as scratch for a type icon, colliding with exactly the row this
     * entry would need.
     *
     * That collision is not survivable by extending the anchor: register_one()
     * hashes a band at a row-stride of a->w (the FULL 256-texel sheet width,
     * see the `rowb` line above) -- there is no mechanism here for a hash
     * rectangle narrower than the sheet's own width, so row 128 can never be
     * included without requiring the whole 704..768 span (collision zone
     * included) to match disc stock. It never will, mid-duel, so a 112-row
     * anchor starting at 128 doesn't intermittently fail -- it PERMANENTLY
     * fails, taking the entire sheet down with it (confirmed: with anch_y=128
     * this asset went from working to "not live -- never recognised in VRAM"
     * even with the box visibly on screen). Capturing the box needs a hash
     * rectangle narrower than a->w -- see ui/duel_panels_lp below, which
     * registers the box's own clean column separately instead of trying to
     * fit it into this entry. */
    { "ui/duel_panels",         0xB54080u,  0x3780u, 0x0000u,  0xC920u,   1, 0, 256, 111, 4,  16, 0,
      704, 129, 64, 111, 0},

    /* The box's own column: VRAM word x=736..752 (texel 128..192 of the SAME
     * sheet above), rows 128..167 -- the part of row 128 that ISN'T the
     * monster_types collision (that lives at word x=704..736, texel 0..128,
     * see the comment above). Registered as its own narrow anchor so its
     * hash covers only this clean 64x40 texel column, not the polluted left
     * half of row 128 -- row_stride carries the sheet's REAL 256-texel row
     * spacing so reading this narrower crop still lands on the right bytes
     * of every row, the same "column out of a wider physical layout"
     * problem campaign_characters' bg_retile_cols solves for whole VRAM
     * pages, here at row instead of page granularity.
     *
     * base is duel_panels' own 0xB54000 (row 128, byte 0 of THIS sheet) plus
     * 64 bytes (128 texels * 4bpp/8) to reach texel column 128. clut_off is
     * recomputed for that shifted base so the absolute CLUT address stays
     * the fixed 0xB609A0 duel_panels itself uses -- same palette, same
     * sheet, just a different crop of it. Separate name (not folded into
     * duel_panels) because it is a genuinely separate anchor rectangle, the
     * same reason ui/logo/copyright is its own entry rather than a second
     * band bolted onto ui/logo/front. */
    { "ui/duel_panels_lp",      0xB54040u,  0x0500u, 0x0000u,  0xC960u,   1, 0,  64,  40, 4,  16, 0,
      736, 128, 16, 40, 0, 0, 0, 0, 256},

    /* Campaign character portraits. Every field here was wrong before, which
     * is why they exported as grey index maps: the base, the stride, the depth
     * and the palette. The right ones come from tools/teatools'
     * image-campaign-characters notes and were confirmed by decoding --
     * body 0x1DC0000 + 102400*(id-1), 128 bytes per row at 8bpp, and the
     * 256-entry CLUT at +0x18000. The old entry looked for a palette at
     * +0x18000 too, but from a base 0x19000 short, so it was reading a
     * neighbouring record's pixels and concluding there was no palette.
     *
     * The body is 0x18000 bytes = 768 rows across THREE 256-row texture
     * pages, streamed as three 128-wide VRAM columns (see is_campaign_char's
     * own comment for the memories-decomp and teatools cross-check). Whether
     * the third page carries real art varies BY CHARACTER, not a constant --
     * disc-checked: id 1 (Simon) and id 3 (Teana) have it all-zero, but id 2
     * (Jono), id 4 (Seto) and id 13 (Joey) do not. An earlier version of this
     * entry stopped at h=512 believing the third page was always blank,
     * which silently truncated every character that actually uses it. w/h
     * stay the raw 128-wide, 768-row page shape here (register_one bands it
     * automatically); psx_wa_catalog_disp_size()/is_campaign_char give the
     * untiled 384x256 canvas a replacement or export actually deals with.
     *
     * Stored as CHUNKS the game reassembles per character (a face, a pair of
     * eyes and a mouth sit apart from the body), so the export looks like a
     * sprite sheet rather than a portrait. That is the layout the game
     * samples, and therefore the one a replacement has to keep.
     *
     * A second CLUT for the STP=1 variant sits at +0x18200; only the first is
     * used here, since the export is a canvas and a replacement carries its
     * own colour. */
    { "campaign_characters/%03d",    0x1DC0000u, 0x19000u, 0x00000u, 0x18000u,  64, 1, 128, 768, 8, 256, 0 },

    /* Everything below came out of tools/teatools -- each tool's notes name a
     * body and a palette, and each entry here was confirmed by decoding it and
     * looking at the result before being added, the same bar the rest of this
     * table had to clear.
     *
     * Duelist portrait thumbnails: the SAME 48x48 / 64-colour / 2432-stride
     * shape as the icons above, at a different base. Two distinct sets exist
     * -- 40 framed icons at 0xF55000 and these 25 -- which is why both are
     * catalogued rather than one being folded into the other. */
    { "dialog_thumbnails/thumb_%03d",   0xF35000u,   2432u, 0x0000u,   0x900u,  25, 1,  48,  48, 8,  64, 0 },

    /* The card viewer's canvas: the gold card body, the spiral card back, the
     * MAGIC/EQUIP/TRAP/RITUAL labels and the stat digits, all in one block.
     * 928 rows are stored; only the first 768 are catalogued, because that is
     * three whole texture pages of real art and what follows decodes as noise
     * -- almost certainly a different palette's territory rather than more
     * picture. The mini-sleeve tool reads 0xB75800 with its own CLUT, but that
     * is a WINDOW ONTO THIS SAME BLOCK, not a separate asset, so it is
     * deliberately not a second entry: two assets over one set of bytes is
     * what forced the sheet split above. */
    { "ui/card_sleeves/canvas",     0xB63000u, 0x1D000u, 0x0000u, 0x21000u,   1, 0, 128, 768, 8, 256, 0 },

    /* Rows 768..928 of that same block: "other duel sprites" per
     * tools/teatools/image-cardsleeves (digits, MAGIC/EQUIP/TRAP/RITUAL
     * labels, icons) -- the very rows the comment above calls noise under
     * the canvas's own palette (row 8, 0x21000). Re-checked this session:
     * decoding these rows against six candidate CLUT-block rows (0x21000
     * itself, and block rows 0/9/10/13/14) all give the SAME correct
     * shapes -- a bordered badge/icon cluster top right, solid label bars,
     * a row of outline digits -- just recoloured differently by each
     * candidate, never landing on one palette that makes the whole picture
     * look right. That is what a live-recoloured sprite looks like under a
     * guessed static palette, the same signature duel_labels and
     * monster_types already have (see their comments above), and it lines
     * up with image-cardsleeves' own note that the ATK/DEF digits carry an
     * alternate colour selected by a card-record flag bit. So this is
     * catalogued the same way: no clut_entries (no static palette is
     * claimed), tint 0 + is_ui's default tinting lets write_elements /
     * write_screens dump each region in whatever colour actually drew it,
     * same mechanism as the sibling rows above. */
    { "ui/card_sleeves/duel_sprites", 0xB63000u, 0x1D000u, 0x18000u,     0u,   1, 0, 128, 160, 8,   0, 0 },

    /* Rows 928..1008: the baked 4bpp text placeholders -- "[Normal Magic
     * Card]", "[ Trap Card ]", "[Equip Magic Card]", "ATK", "DFD" -- five
     * 16-row strips, only the first 28 of each 128-byte disc row holding
     * real pixels (56 of 256 px at 4bpp; the rest of the row is unused).
     * Because psx_wa_catalog_decode_rows packs rows tight at w*bpp/8 with no
     * per-row gap, this is catalogued at the disc's REAL row width (w=256,
     * matching the physical 128-byte stride) rather than the used 56 px, so
     * the byte alignment stays honest -- most of each exported row is
     * transparent padding past x=56.
     *
     * image-cardsleeves documents the palette too: CLUT-block row 8 (the
     * same row canvas's own clut_off already points at), colours 240..255.
     * clut_off here is that row's start (0x21000) plus 240 entries
     * (240*2 = 0x1E0), i.e. 0x211E0. Verified by decoding: renders the five
     * labels above legibly, unlike every candidate tried for duel_sprites
     * above -- this one has a real, single, confirmed palette, so unlike
     * that entry it is tint = -1 (opted out of is_ui's live-capture
     * default) and exports through its own clut like dialog_frame/ui/panels
     * do, not as scattered per-draw captures. */
    { "ui/card_sleeves/text_placeholders", 0xB63000u, 0x1D000u, 0x1D000u, 0x211E0u, 1, 0, 256,  80, 4,  16, 0,
      0, 0, 0, 0, -1 },

    /* The duel playing field -- the grid the cards sit on. Seven copies, one
     * per terrain, at 0x75800 apart.
     *
     * Split into its three parts because they do NOT share a palette. The
     * record ends with five 16-colour CLUT slots at +0x7F00, and the parts
     * read slots 0, 2 and 4 -- decoding all 254 rows through slot 0 gave the
     * lower two thirds someone else's colours. Rows 0..103 are the top half,
     * 104..207 the bottom, 208..253 the middle strip.
     *
     * tint -1 set (2026-09-11, corrected from an earlier ref_whole-only
     * attempt): the split above already gives each of these three its own
     * single, correctly-matched palette slot, so -- unlike duel_labels,
     * genuinely reused across different screens under different CLUTs --
     * there is no "pixels times palette" ambiguity left to resolve live. A
     * terrain texture is drawn once, never recoloured for a second context,
     * the same shape as dialog_frame/ui/panels/ui/logo/back/
     * ui/card_sleeves/text_placeholders above, all tint -1 for the same
     * reason. That routes export_one() past the multi-region UI branch
     * (is_ui() && tint == 0) entirely and into the plain path, which writes
     * one real, directly-injectable "duel_fields/%d_top.png" etc. straight
     * from the disc palette -- immediately, not a "_reference.png" a pack
     * author has to rename, and not waiting on a live duel with that
     * terrain active to populate scattered per-draw captures. Verified by
     * decoding all three parts of records 0, 1, 3 and 6 (gold wasteland,
     * green forest, grey stone, blue-purple sea) and looking -- every one
     * coherent throughout, no mismatched patches. */
    { "duel_fields/%d_top",    0xBC8800u, 0x75800u, 0x0000u,  0x7F00u,   7, 0, 256, 104, 4,  16, 0,
      0, 0, 0, 0, -1 },
    { "duel_fields/%d_bottom", 0xBC8800u, 0x75800u, 0x3400u,  0x7F40u,   7, 0, 256, 104, 4,  16, 0,
      0, 0, 0, 0, -1 },
    { "duel_fields/%d_middle", 0xBC8800u, 0x75800u, 0x6800u,  0x7F80u,   7, 0, 256,  46, 4,  16, 0,
      0, 0, 0, 0, -1 },

    /* The FREE DUEL screen backdrop. The image runs exactly up to its palette:
     * 128 bytes x 512 rows is 0x10000, and the CLUT is at +0x10000. */
    { "free_duel/background",     0xF44000u, 0x11000u, 0x0000u, 0x10000u,   1, 0, 128, 512, 8, 256, 0 },

    /* Boot screens. The title logo is 8bpp with a 256-entry palette; the
     * Konami screen before it is 4bpp with 16. Both palettes live far from
     * their pixels, which is why the offsets look arbitrary -- they are the
     * ones that render the actual logos. */
    /* The title logo. Band 1 (image rows 256..511) matches as a whole page at
     * VRAM (704,256). Band 0 does not: it lands at (640,256) and the game
     * overwrites rows 208..239, so its 256-row hash never matched and the top
     * half of the sheet was never replaced. Measured against a title-screen
     * savestate: rows 0..207 are byte-identical to the disc, and the loader's
     * own chunk table only ever draws from rows 0..207 of it, so anchoring
     * exactly that range loses nothing. */
    { "ui/logo/front",         0xFD3800u, 0x18800u, 0x0000u, 0x18000u,   1, 0, 128, 512, 8, 256, 0,
      640, 256, 64, 208, 0, 0 },
    /* Genuinely single-purpose (the hieroglyph wallpaper, always drawn the
     * same way) with a real, fixed disc palette (clut_entries=16 above) --
     * exactly the "one true colouring, just not always found yet" shape
     * dialog_frame and ui/panels are, so tint -1 for the same reason: one
     * flat file, not the multi-region treatment genuinely multi-context UI
     * needs. Missed when that fix first went in; caught checking why this
     * one was still exporting fragmented. */
    { "ui/logo/back",       0xFE3800u,  0xA000u, 0x0000u,  0x8200u,   1, 0, 256, 256, 4,  16, 0,
      0, 0, 0, 0, -1 },

    /* Rows 208..239 of the same sheet as ui/logo/front above -- the copyright
     * line ("(c) 1996 KAZUKI TAKAHASHI"), confirmed by decoding exactly that
     * row range: content starts dead at row 208 and stops dead at row 240,
     * full 128-texel width both sides. This is the range the ui/logo/front
     * comment calls "the game overwrites" -- true, but only AFTER boot, and
     * the anchor re-validates every frame (rescan_anchored), so a scene that
     * has since reused this VRAM range just fails the hash check instead of
     * mis-drawing over it. Genuinely single-purpose (one boot screen, one
     * disc palette, never redrawn with another meaning), so tint -1: one
     * flat file, not the multi-region treatment ui/logo/front itself still
     * needs for its two DIFFERENT screens (the logo, PUSH START BUTTON)
     * sharing one upload. */
    { "ui/logo/copyright",  0xFDA000u,  0x6800u, 0x0000u, 0x11800u,   1, 0, 128,  32, 8, 256, 0,
      640, 464, 64, 32, -1, 0 },

    /* 256x256 4bpp, containing BOTH the KONAMI ribbon logo box AND the
     * separate "Konami Computer Entertainment Japan" text, OVERLAPPING in
     * the same stored bytes because the two are drawn at different times
     * from the same upload (the exact shape ui/logo/front's logo/PUSH START
     * split already has) -- confirmed by decoding the whole page through
     * this ONE clut and seeing both pictures at once, legibly, in their
     * real colours. That rules out multiple live palettes; it does NOT rule
     * out needing multi-region capture, since telling the two screens apart
     * still means knowing which rectangle got drawn where, not just what
     * colour it was. Left at tint 0 (default) for exactly that reason -- the
     * live capture this project already does for logo/PUSH START will do
     * the same job here once both screens have been visited with HD
     * textures on. The clut is new: previously undeclared (0 entries), so
     * every export before this was an uncoloured grey ramp. */
    { "ui/logo/konami",        0xFC2800u, 0x2A000u, 0x0000u,  0x10000u,   1, 0, 256, 256, 4,  16, 0,
      0, 0, 0, 0, 0},

    /* The main-menu text: SAVE / LOAD / TRADE / ... -- and the LOAD-style
     * highlight colouring that started this search. It is not in WA_MRG at
     * all; it lives in SU.MRG (disc LBA 954), the archive the decomp notes
     * call "the main-menu package", requested as one 0x73-sector async
     * transfer at boot.
     *
     * Found the same way as everything else that boots before the injector
     * is watching transfers: a title-screen VRAM dump, hashed and matched
     * BYTE-FOR-BYTE against a raw SU.MRG read, not guessed at. Three whole
     * 256-row 4bpp pages matched exactly, at SU.MRG offsets 0x000000,
     * 0x008000 and 0x020000, anchored at the VRAM locations the match
     * confirmed:
     *
     *   0x000000  (512,256)  SAVE / LOAD / TRADE / CAMPAIGN / NEW GAME /
     *                        FREE DUEL / LIBRARY
     *   0x008000  (576,256)  BUILD DECK / OPTION / PASSWORD
     *   0x020000  (896,  0)  REGULATION OF / 2P-DUEL RULES / CARD INDICATE /
     *                        DECK NUMBER / OPEN CARD
     *
     * (The gap between them, 0x010000..0x020000, is a duplicate of the boot
     * logo's own art -- also present byte-for-byte in WA_MRG's "ui/logo/back"
     * -- not more menu text, so it is not catalogued here.)
     *
     * No palette in the table: your screenshot shows "LOAD" picked out in
     * green with an orange border while the rest sit dim, which is a CLUT
     * swap per menu row, not one fixed colouring for the whole sheet -- the
     * same shape duel_labels turned out to have. Left untinted-by-table
     * (tint 0, the default) so is_ui()'s auto-tint applies and each row's
     * live palette is captured at draw time instead of guessed from a static
     * VRAM dump, which is what actually produced the scrambled "TV static"
     * renders while this was being tracked down. */
    /* Real disc palette found this pass, not guessed: memories-decomp's
     * MainMenu_LoadPackageStage (matched C) stages a dedicated palette block
     * at SU.MRG 0x30000, and modding-tutorial-evidence.md independently
     * decoded it as 8 CLUT rows (6 populated) at VRAM y=240-247, confirming
     * row 244 (SU.MRG+0x30800) is the background-tile's own palette.
     * Decoding this whole sheet's 4bpp indices through that SAME 16-entry
     * bank (offset 0 within the row) renders clean, legible SAVE/LOAD/...
     * text -- so the background tile and this sheet's DEFAULT (unselected)
     * colouring share one CLUT. This is only the default: the catalog
     * comment above already established a per-row CLUT swap for whichever
     * entry is highlighted, which stays a live-capture-only question (tint
     * 0, unchanged) -- this just gives export_one() a real disc colouring to
     * fall back to instead of grey when nothing has been captured yet. */
    { "ui/menu_labels_1",       0x000000u, 0x8000u, 0x0000u, 0x30800u,   1, 0, 256, 256, 4,  16, 0,
      512, 256, 64, 256, 0, 0, 1, 1 },
    /* clut_off is record-relative (added to this record's own base, 0x8000),
     * so it is 0x30800 - 0x8000 here -- the two menu_labels_2/3 entries do
     * NOT share menu_labels_1's literal clut_off; they resolve to the same
     * ABSOLUTE SU.MRG+0x30800 once added to their own differing base. */
    { "ui/menu_labels_2",       0x008000u, 0x8000u, 0x0000u, 0x28800u,   1, 0, 256, 256, 4,  16, 0,
      576, 256, 64, 256, 0, 0, 1, 1 },
    { "ui/menu_labels_3",       0x020000u, 0x8000u, 0x0000u, 0x10800u,   1, 0, 256, 256, 4,  16, 0,
      896,   0, 64, 256, 0, 0, 1, 1 },

    /* Everything below was found through memories-decomp (the project's
     * separate, from-scratch matching decompilation of SLUS_014.11 -- see
     * notes/mrg-files.md and notes/modding-tutorial-evidence.md there),
     * which independently recovered exact WA_MRG.MRG package/phase/palette
     * boundaries from matched loader C, cross-checked against community
     * hex-editor tutorials and confirmed by SHA-256 against the retail
     * archive. Every offset below was then decoded locally and looked at --
     * the same bar the rest of this table holds itself to -- before being
     * added; see tools/wa_assets.py-style verification, done ad hoc against
     * a local WA_MRG.MRG copy for this pass. */

    /* The password screen's own furniture: a fixed gold card-preview frame
     * (128x640 -- a card body atop a spiral card-back, exactly the shape
     * ui/card_sleeves/canvas has, just a separate physical copy for this
     * screen), its small selection-cursor sheet, and the stone/parchment
     * frame plus digit atlas and "[Normal Magic Card]"/"ATK"/"DFD" labels.
     * Decoded and inspected: all three are legible and use one fixed
     * colouring on this screen (the password UI does not appear to recolour
     * per card type the way the duel-side card display does), so tint -1
     * like dialog_frame -- one flat file each, not the multi-region
     * treatment genuinely multi-context UI needs.
     *
     * That held for card and cursor. It did not hold for frame: it is a
     * furniture sheet in the ui/panels sense, not a single picture -- the
     * digit atlas, the magnifier icon, the three card-type labels and the
     * ATK/DEF strip are separate elements the password UI very plausibly
     * recolours independently (a selected digit, a found-card highlight),
     * and one savestate's palette cannot tell "this element only ever has
     * one colour" from "I only ever saw it in this one state". Reclassified
     * to tint 0 for the same reason as ui/panels: capture each element's
     * own CLUT live instead of assuming the whole sheet shares one. */
    { "ui/password/card",    0xF97800u, 0x14000u, 0x0000u, 0x21000u,   1, 0, 128, 640, 8, 256, 0,
      0, 0, 0, 0, -1 },
    { "ui/password/cursor",  0xFAB800u,  0x4000u, 0x0000u,  0xD000u,   1, 0, 128, 128, 8, 256, 0,
      0, 0, 0, 0, -1 },
    { "ui/password/frame",   0xFAF800u,  0x8000u, 0x0000u,  0x8200u,   1, 0, 256, 256, 4,  16, 0,
      0, 0, 0, 0, 0 },

    /* The duel-results screen: "RESULTS OF DUEL" / TEC-POW-DUEL SKILL /
     * SPOILS / the S-A-B-C-D rank-grade letters on one sheet, and the
     * "YOU WIN!" / "...LOSE" banner text on a second. Both decode cleanly
     * through their first documented palette row, but memories-decomp's
     * loader trace shows several MORE 16-colour rows in the same uploaded
     * block -- one per rank grade, and separate win/lose (plus alternate
     * STP-bit) colourings for the banner -- so this is genuinely
     * multi-context CLUT swapping, not one fixed colouring. Left at tint 0
     * (the default) so live CLUT tracking supplies whichever row the game
     * actually drew, the same reason ui/duel_labels and the menu labels
     * above are untinted-by-table. */
    { "ui/results/labels",   0xED5800u,  0x8000u, 0x0000u, 0x10000u,   1, 0, 256, 256, 4,  16, 0 },
    { "ui/results/banner",   0xEDD800u,  0x8000u, 0x0000u,  0x8200u,   1, 0, 256, 256, 4,  16, 0 },

    /* The nine monster-attribute emblems (Light/Dark/Earth/Water/Fire/Wind/
     * Spell-Equip/Trap/"Light 2"), one row of 32x16 icons. memories-decomp's
     * tutorial cross-check found this exact 0x900-byte sequence duplicated
     * byte-for-byte at ten different disc locations (each duel terrain, plus
     * Library, Password and Build Deck's own packages) -- one registration
     * matches an upload from any of them, since matching is by content hash,
     * not disc position, so only the Normal-terrain copy is catalogued. */
    { "ui/icons/attributes", 0xB7F000u,  0x6000u, 0x0000u,  0x5E00u,   1, 0, 288,  16, 4,  16, 0 },

    /* The in-duel hand-card display frame -- the small per-card border drawn
     * under a monster/magic/trap card while it sits in your hand, one texel
     * page below the shared attribute-icon row on the same sheet. Same
     * duplicate-across-all-terrains shape as the attribute icons above (one
     * registration covers every terrain's copy). No palette recovered yet --
     * memories-decomp's loader trace did not pin one down for this specific
     * sub-image, unlike the neighbouring attribute icons -- so, like
     * ui/panels, it exports grey until the renderer reports the live one;
     * tint 0 because the duel-side card display very likely recolours this
     * per card type the same way ui/card_sleeves/canvas does. */
    { "ui/duel_hand/canvas", 0xB7B000u,  0x4000u, 0x0000u,      0u,   1, 0, 256, 128, 4,   0, 0 },

    /* The name-entry screen's own two furniture sheets: a parchment
     * background with the letter-grid's input-box outline, and a second
     * sheet of divider bars, a small icon square and a four-way movement
     * cursor. Both decode cleanly through the documented palette block.
     * Left untinted-by-table (tint 0) on the same reasoning as the other
     * screen sheets above -- nothing here is confirmed single-context. */
    { "ui/name_entry/background", 0xF6F800u, 0x18000u, 0x0000u,      0u,   1, 0, 128, 768, 8,   0, 0 },
    { "ui/name_entry/frame",      0xF87800u,  0x8000u, 0x0000u, 0x8000u,   1, 0, 256, 256, 4,  16, 0 },

    /* The Library screen's card-grid background and digit atlas (the numbers
     * under each grid slot). memories-decomp's "Library package phases"
     * (notes/overlays/runtime-loader.md) documents this as the package's
     * SECOND image phase, with a matching second palette phase right after
     * it -- confirmed by decoding both and looking. The package's FIRST
     * image phase is NOT catalogued here: byte-compared against a local
     * WA_MRG.MRG copy, it is bit-for-bit identical to ui/card_sleeves/canvas
     * above, so it already registers under that name (matching is by content
     * hash, not disc position -- the same reason ui/icons/attributes and
     * ui/duel_hand/canvas above only need one registration each despite
     * having several duplicate disc copies too). */
    { "ui/library/grid",     0xF08800u, 0x18000u, 0x0000u, 0x18000u,   1, 0, 128, 768, 8, 256, 0,
      0, 0, 0, 0, -1 },
};
#define ASSET_N ((int)(sizeof ASSETS / sizeof ASSETS[0]))

/* The largest PACKED image in the table: the 4bpp UI sheet, 256*768/2. */
/* The largest packed image in the table: family 2's first background layer is
 * 128x1024 at 8bpp. It was 0x18000 while the tallest page was 512 rows; a
 * taller entry silently fails to decode if this is not raised with it. */
#define WA_PACKED_MAX 0x20000

static int s_registered;
static int s_ready;

static int read_from(uint32_t lba_base, uint32_t off, uint8_t *out, uint32_t len)
{
    static uint8_t sec[SECTOR];
    while (len) {
        const uint32_t lba = lba_base + off / SECTOR;
        const uint32_t rem = off % SECTOR;
        uint32_t take = SECTOR - rem;
        if (take > len)
            take = len;
        if (!psx_mod_cd_read_stock_sector(lba, sec))
            return 0;
        memcpy(out, sec + rem, take);
        out += take;
        off += take;
        len -= take;
    }
    return 1;
}

int psx_wa_catalog_read(uint32_t off, uint8_t *out, uint32_t len)
{
    return read_from(WA_LBA, off, out, len);
}

/* Same, but relative to whichever archive `a` lives in -- WA_MRG.MRG for
 * every asset above, SU.MRG for the main-menu package. */
static int catalog_read_asset(const PsxWaAsset *a, uint32_t off,
                              uint8_t *out, uint32_t len)
{
    return read_from(a->mrg ? SU_LBA : WA_LBA, off, out, len);
}

/* Reads `rows` rows of a->w texels each, starting at `off`. Identical to one
 * big catalog_read_asset() call when a->row_stride is unset (every entry but
 * the ones that need it) -- rows genuinely ARE packed tight at a->w*bpp/8
 * there, so one contiguous read is both correct and cheaper. When
 * a->row_stride IS set, this entry's own w is narrower than the real
 * physical row it lives inside (see the field's comment in the header), so
 * each row is read separately at a->row_stride's spacing and only the first
 * a->w texels of it are kept -- the same crop-a-column-out-of-a-wider-layout
 * idea as the background tiling above, at row granularity. */
static int catalog_read_rows(const PsxWaAsset *a, uint32_t off,
                             uint8_t *out, int rows)
{
    const uint32_t rb = (uint32_t)a->w / (a->bpp == 4 ? 2u : 1u);
    if (!a->row_stride)
        return catalog_read_asset(a, off, out, (uint32_t)rows * rb);
    const uint32_t stride_b = (uint32_t)a->row_stride / (a->bpp == 4 ? 2u : 1u);
    for (int r = 0; r < rows; r++)
        if (!catalog_read_asset(a, off + (uint32_t)r * stride_b,
                                out + (uint32_t)r * rb, rb))
            return 0;
    return 1;
}

/* ---- background tiling ----------------------------------------------------
 *
 * A campaign background is stored as a 128x512 VRAM page cut into strips and
 * drawn as several quads that reassemble a 320x160 scene. The player wants to
 * paint the 320x160 scene; the game samples the 128x512 page. One band map,
 * two directions:
 *
 *   untile  tiled page  -> clean scene   (export: gives a paintable PNG)
 *   retile  clean scene -> tiled page    (import: packs an edited PNG back)
 *
 * Both scale by an integer N, so a 4x edit (1280x640) packs to a 4x page
 * (512x2048). The 96- and 16-row gaps in the page have no scene pixels and
 * are left transparent. Verified against the disc: applying untile to a real
 * record reassembles the Pharaoh's-palace scene cleanly. */
#define BG_TILE_W 128
#define BG_TILE_H 512
#define BG_TILE_MAX_H 1024      /* family 2 layer 1: four 256-row columns */
#define BG_DISP_W 320
#define BG_DISP_H 160

typedef struct { int dx, dy, sx, sy, w, h; } BgBand;  /* display <-> page, 1x */
static const BgBand BG_BANDS[] = {
    {   0, 0,   0,   0, 128, 160 },   /* left third   */
    { 128, 0,   0, 256, 128, 160 },   /* middle third */
    { 256, 0,   0, 416,  64,  80 },   /* right third, top half    */
    { 256,80,  64, 416,  64,  80 },   /* right third, bottom half */
};
#define BG_BAND_N ((int)(sizeof BG_BANDS / sizeof BG_BANDS[0]))

/* Family 2's FIRST layer is the odd one out: it is not a 320x160 scene at all
 * but a wide parallax strip whose size differs PER RECORD. The game uploads it
 * as 128-pixel columns taken from consecutive 256-row source pages, so a
 * 512-wide strip is four pages side by side and a 320-wide one is three (the
 * last only 64 wide). Decoding all thirteen as 320x160 like the other layers
 * is why they came out as noise.
 *
 * The sizes are not derivable from the record -- nothing in the data says how
 * wide the strip is -- so they are a table, one entry per record. */
static const struct { short w, h; } BG_MOV_L1[] = {
    { 512, 160 }, { 320, 256 }, { 512, 256 }, { 320, 256 }, { 512, 256 },
    { 512, 256 }, { 320, 256 }, { 512, 256 }, { 320, 256 }, { 344, 256 },
    { 512, 256 }, { 512, 256 }, { 512, 256 },
};
#define BG_MOV_L1_N ((int)(sizeof BG_MOV_L1 / sizeof BG_MOV_L1[0]))

/* Rows this layer's columns are lifted from: one page per column. */
#define BG_COL_W    128
#define BG_COL_PAGE 256

/* Every UI asset is tinted, whether or not its row says so.
 *
 * UI art is the console's CLUT-swapping at its most aggressive: one sheet of
 * furniture drawn through a dozen palettes, recoloured per screen and per card
 * type. There is no single palette to export it with -- picking one repainted
 * the whole chest screen in another sprite's colours -- and a flat replacement
 * would freeze whichever one was chosen. Tinting sidesteps both: the file
 * carries shape, the game keeps carrying colour.
 *
 * Deliberately NOT included: card art, portraits, characters and backgrounds.
 * Those are pictures a pack replaces with new artwork of its own, and tinting
 * would force the PS1's palette back onto them. */
static int is_ui(const PsxWaAsset *a)
{
    return strncmp(a->name_fmt, "ui/", 3) == 0 ||
           strncmp(a->name_fmt, "duel_fields/", 12) == 0 ||
           strcmp(a->name_fmt, "free_duel/background") == 0;
}

int psx_wa_catalog_ui(const PsxWaAsset *a)
{
    return a && is_ui(a);
}

static uint32_t record_base(const PsxWaAsset *a, int index);
static uint32_t packed_len(const PsxWaAsset *a);

/* The asset's raw INDICES, one byte per texel. This is what an index-map
 * replacement stores, and taking it straight from the disc is exact -- unlike
 * recovering it from a painted image, which cannot work: a palette is not
 * injective, so two indices that render alike under one CLUT (and differently
 * under another) are indistinguishable by colour. */
int psx_wa_catalog_indices(const PsxWaAsset *a, int index,
                           uint8_t *out, unsigned cap)
{
    static uint8_t pix[WA_PACKED_MAX];
    if (!a || !out || index < 0 || index >= a->count)
        return 0;
    const uint32_t npix = (uint32_t)(a->w * a->h);
    const uint32_t plen = packed_len(a);
    if (npix > cap || plen > sizeof pix ||
        !catalog_read_rows(a, record_base(a, index) + a->off, pix, a->h))
        return 0;
    for (uint32_t i = 0; i < npix; i++)
        out[i] = (a->bpp == 4)
               ? (uint8_t)((i & 1u) ? (pix[i >> 1] >> 4) : (pix[i >> 1] & 0xFu))
               : pix[i];
    return 1;
}

/* The asset's palette in INDEX ORDER, which is what an index map has to be
 * quantised against. Returns how many entries were written. */
int psx_wa_catalog_palette(const PsxWaAsset *a, int index,
                           uint16_t *out, int max_entries)
{
    if (!a || !out || a->clut_entries <= 0 || a->clut_entries > max_entries)
        return 0;
    static uint8_t buf[512];
    const uint32_t n = (uint32_t)a->clut_entries * 2u;
    if (n > sizeof buf ||
        !catalog_read_asset(a, record_base(a, index) + a->clut_off, buf, n))
        return 0;
    for (int i = 0; i < a->clut_entries; i++)
        out[i] = (uint16_t)(buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8));
    return a->clut_entries;
}

int psx_wa_catalog_tinted(const PsxWaAsset *a)
{
    if (!a)
        return 0;
    if (a->tint < 0)
        return 0;               /* opted out: this one is painted, not tinted */
    return a->tint > 0 || is_ui(a);
}

static int is_mov_l1(const PsxWaAsset *a)
{
    return strcmp(a->name_fmt, "backgrounds/mov/%02d_layer1") == 0;
}

/* families 1 (comp) and 2 (mov) both carry a third, 4bpp "light layer" in the
 * same record as their two 8bpp scene layers -- see is_background()'s own
 * comment for why it used to sit out of untiling entirely. */
static int is_layer3(const PsxWaAsset *a)
{
    return strcmp(a->name_fmt, "backgrounds/comp/%02d_layer3") == 0 ||
           strcmp(a->name_fmt, "backgrounds/mov/%02d_layer3") == 0;
}

/* Campaign dialogue portraits stream the SAME shape as mov's layer 1 --
 * three 128-wide, 256-tall VRAM pages, one after another in the disc record
 * -- confirmed two ways: memories-decomp's matched func_8003A01C (the disc
 * callback) programs the generic sector pump at w=0x40 words (128 texels at
 * 8bpp) x h=0x10 rows per chunk, the same "64x16-halfword-rect, wrap the
 * column every 256 rows" shape used everywhere else in this game; and
 * tools/teatools/image-campaign-characters' own reference dump
 * (`campaign_portraits.py cmd_dump`'s "raw" mode) reads and reassembles it
 * with the exact same three-loop, page*256+y, page*128+x math bg_untile_cols
 * already implements below -- verified by porting that exact loop in Python
 * against the real disc and getting a clean 384x256 canvas, not the
 * "half-visible, wrapped" mess the old w=128,h=512 (missing the third page
 * entirely) reading produced.
 *
 * The canvas is NOT one coherent photo for every character, and that is
 * correct, not a further bug: each of the 64 characters uses one of six
 * different "templates" (image-campaign-characters/scripts/templates.json)
 * that samples small, differently-shaped chunks out of this same 384x256
 * sheet for its dialogue animation (blinking, mouth shapes) -- a sprite
 * sheet, the same idea as this project's own UI multi-region assets, not a
 * single portrait. Getting the RAW byte layout right is the actual bar here,
 * matching what the game genuinely uploads to VRAM in one piece; per-chunk
 * decoding into per-template pictures is future work if ever wanted, not
 * something this fix claims to also do. */
static int is_campaign_char(const PsxWaAsset *a)
{
    return strcmp(a->name_fmt, "campaign_characters/%03d") == 0;
}

static void bg_copy(const uint8_t *src, int src_w,
                    uint8_t *dst, int dst_w,
                    int sx, int sy, int dx, int dy, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src + ((size_t)(sy + y) * src_w + sx) * 4u;
        uint8_t *dp = dst + ((size_t)(dy + y) * dst_w + dx) * 4u;
        memcpy(dp, sp, (size_t)w * 4u);
    }
}

/* tiled page (in) -> clean scene (out), at scale N = in_w / BG_TILE_W. */
static void bg_untile(const uint8_t *in, int in_w, int in_h,
                      uint8_t *out, int out_w, int out_h)
{
    const int n = in_w / BG_TILE_W;
    (void)in_h;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    for (int b = 0; b < BG_BAND_N; b++) {
        const BgBand *k = &BG_BANDS[b];
        bg_copy(in, in_w, out, out_w,
                k->sx * n, k->sy * n, k->dx * n, k->dy * n,
                k->w * n, k->h * n);
    }
}

/* clean scene (in) -> tiled page (out), at scale N = in_w / BG_DISP_W. The
 * injector calls this via the registered callback. */
/* Tiled columns <-> wide strip. Both directions derive the scale and the
 * canvas from the sizes they are handed: untile knows the input is 128*N wide,
 * retile knows the output is, so neither needs to be told which record it is
 * working on. */
static void bg_untile_cols(const uint8_t *in, int in_w, int in_h,
                           uint8_t *out, int out_w, int out_h)
{
    const int n = in_w / BG_COL_W;
    (void)in_h;
    if (n <= 0) return;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    const int cw = out_w / n, chh = out_h / n;
    for (int x = 0, j = 0; x < cw; x += BG_COL_W, j++) {
        const int w = (cw - x < BG_COL_W) ? cw - x : BG_COL_W;
        bg_copy(in, in_w, out, out_w,
                0, BG_COL_PAGE * j * n, x * n, 0, w * n, chh * n);
    }
}

static void bg_retile_cols(const uint8_t *in, int in_w, int in_h,
                           uint8_t *out, int out_w, int out_h)
{
    const int n = out_w / BG_COL_W;
    if (n <= 0) return;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    const int cw = in_w / n, chh = in_h / n;
    for (int x = 0, j = 0; x < cw; x += BG_COL_W, j++) {
        const int w = (cw - x < BG_COL_W) ? cw - x : BG_COL_W;
        bg_copy(in, in_w, out, out_w,
                x * n, 0, 0, BG_COL_PAGE * j * n, w * n, chh * n);
    }
}

static void bg_retile(const uint8_t *in, int in_w, int in_h,
                      uint8_t *out, int out_w, int out_h)
{
    const int n = in_w / BG_DISP_W;
    (void)in_h;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    for (int b = 0; b < BG_BAND_N; b++) {
        const BgBand *k = &BG_BANDS[b];
        bg_copy(in, in_w, out, out_w,
                k->dx * n, k->dy * n, k->sx * n, k->sy * n,
                k->w * n, k->h * n);
    }
}

/* Layer 3's OWN chunk layout -- not the 8bpp band map above, and not a
 * scaled variant of it either. Confirmed against tools/teatools/image-bgcomp
 * (the 320x160 "light layer" table, reused as-is by image-bgmov's own layer
 * 3): a 256x160 main scene, then two 64x80 corner pieces that share the SAME
 * 80 source rows at different columns and stack vertically on the canvas.
 * is_background() used to exclude this layer for exactly this reason --
 * "running it through the 8bpp band map would scramble it" -- and shipped it
 * as the raw 256x256 page instead: correct bytes, wrong shape, which is why
 * a light layer that should render as a round glow/halo came out scrambled.
 *
 * Fixed size, unlike BG_BANDS: layer 3 is always 320x160, it does not vary
 * per picture the way mov's layer 1 does, so untile takes no scale factor
 * (the source page is always exactly 256 wide). Retile does scale by N, the
 * same way bg_retile does, so an upscaled replacement still retiles. */
static const BgBand BG_L3_BANDS[] = {
    {   0,  0,   0,   0, 256, 160 },   /* main scene            */
    { 256,  0, 128, 160,  64,  80 },   /* right corner, top     */
    { 256, 80, 192, 160,  64,  80 },   /* right corner, bottom  */
};
#define BG_L3_BAND_N ((int)(sizeof BG_L3_BANDS / sizeof BG_L3_BANDS[0]))

static void bg_untile_l3(const uint8_t *in, int in_w, int in_h,
                         uint8_t *out, int out_w, int out_h)
{
    (void)in_h;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    for (int b = 0; b < BG_L3_BAND_N; b++) {
        const BgBand *k = &BG_L3_BANDS[b];
        bg_copy(in, in_w, out, out_w, k->sx, k->sy, k->dx, k->dy, k->w, k->h);
    }
}

static void bg_retile_l3(const uint8_t *in, int in_w, int in_h,
                         uint8_t *out, int out_w, int out_h)
{
    const int n = in_w / BG_DISP_W;
    (void)in_h;
    if (n <= 0) return;
    memset(out, 0, (size_t)out_w * (size_t)out_h * 4u);
    for (int b = 0; b < BG_L3_BAND_N; b++) {
        const BgBand *k = &BG_L3_BANDS[b];
        bg_copy(in, in_w, out, out_w,
                k->dx * n, k->dy * n, k->sx * n, k->sy * n,
                k->w * n, k->h * n);
    }
}

/* Is this catalog entry a tiled background? Keyed on the name prefix so the
 * table stays declarative. */
/* Whether this asset is one of the TILED background pages, and so needs
 * untiling for export: either the 8bpp 128-wide scene/sprite layers, or
 * layer 3's own 4bpp chunk layout (is_layer3(), dispatched separately below
 * -- see bg_untile_l3's header for why it used to be excluded here and
 * exported as a scrambled raw page instead). */
static int is_background(const PsxWaAsset *a)
{
    return strncmp(a->name_fmt, "backgrounds/", 12) == 0 &&
           ((a->bpp == 8 && a->w == 128) || is_layer3(a));
}

const PsxWaAsset *psx_wa_catalog_table(int *count)
{
    if (count) *count = ASSET_N;
    return ASSETS;
}

/* The single source both registration (below) and export
 * (psx_texture_export.c) read for a row's CURATED path -- "UI/Deck",
 * "Card assets/card artworks" -- so a pack author's actual folder tree
 * matches the Asset Manager's browsing tree instead of the disc-derived
 * family name ("ui", "cards/art") neither the manager nor a person picking
 * a screen out of a menu has any reason to know. Mirrors the Asset
 * Manager's own CAT_RULES table (psx_asset_manager.c) exactly -- that
 * window's browsing tree is built from the same (top, sub, name_fmt)
 * shape, so the two tables describe one agreement, not two independent
 * guesses that could drift. Keep them in sync by hand until one of the two
 * windows includes the other's header instead; duplicated here rather than
 * shared via a header because the Asset Manager is SDL/ImGui UI code this
 * file has no other reason to link against. */
typedef struct { const char *top, *sub, *name_fmt; } DisplayRule;
static const DisplayRule DISPLAY_RULES[] = {
    { "UI", "fonts",      "ui/font/font" },
    { "UI", "menu",       "ui/menu_labels_1" },
    { "UI", "menu",       "ui/menu_labels_2" },
    { "UI", "menu",       "ui/menu_labels_3" },
    { "UI", "duel HUD",   "ui/duel_labels" },
    { "UI", "duel HUD",   "ui/duel_panels" },
    { "UI", "duel HUD",   "ui/duel_panels_lp" },
    { "UI", "duel HUD",   "duel_fields/%d_top" },
    { "UI", "duel HUD",   "duel_fields/%d_bottom" },
    { "UI", "duel HUD",   "duel_fields/%d_middle" },
    { "UI", "duel HUD",   "ui/duel_hand/canvas" },
    { "UI", "icons",      "ui/icons/monster_types" },
    { "UI", "icons",      "ui/icons/attributes" },
    { "UI", "Logo",       "ui/logo/front" },
    { "UI", "Logo",       "ui/logo/copyright" },
    { "UI", "Logo",       "ui/logo/back" },
    { "UI", "Logo",       "ui/logo/konami" },
    { "UI", "Dialogue",   "ui/dialog_frame" },
    { "UI", "Deck",       "ui/panels" },
    { "UI", "Library",    "ui/library/grid" },
    { "UI", "Password",   "ui/password/card" },
    { "UI", "Password",   "ui/password/cursor" },
    { "UI", "Password",   "ui/password/frame" },
    { "UI", "Results",    "ui/results/labels" },
    { "UI", "Results",    "ui/results/banner" },
    { "UI", "Name Entry", "ui/name_entry/background" },
    { "UI", "Name Entry", "ui/name_entry/frame" },

    { "Card assets", "card artworks",   "cards/art/%03d" },
    { "Card assets", "card thumbnails", "cards/mini/%03d" },
    { "Card assets", "card thumbnails", "cards/duel_thumb/%03d" },
    { "Card assets", "card Titles",     "cards/titles/%03d" },
    { "Card assets", "card Frames",     "ui/card_sleeves/canvas" },
    { "Card assets", "card Frames",     "ui/card_sleeves/duel_sprites" },
    { "Card assets", "card Frames",     "ui/card_sleeves/text_placeholders" },

    { "Campaign", "Static Backgrounds", "backgrounds/static/%02d" },
    { "Campaign", "characters",         "campaign_characters/%03d" },
    { "Campaign", "cutscene portraits", "dialog_thumbnails/thumb_%03d" },
    /* The interleaved layer families -- the Asset Manager's build_catalog()
     * walks these through expand_interleaved_bg() rather than a simple one
     * name_fmt per branch, since it shows 00_layer1, 00_layer2, 00_layer3,
     * 01_layer1, ... in on-screen order rather than all of one layer before
     * the next. That interleaving is a BROWSING-ORDER concern, not a path
     * one: for where a file lands, each layer is still just its own leaf
     * under one shared category, exactly like any other three-row family
     * above, so it needs no special handling here -- only omitting these
     * rows entirely (the earlier mistake) left them on the raw path. */
    { "Campaign", "Compounded Backgrounds", "backgrounds/comp/%02d_layer1" },
    { "Campaign", "Compounded Backgrounds", "backgrounds/comp/%02d_layer2" },
    { "Campaign", "Compounded Backgrounds", "backgrounds/comp/%02d_layer3" },
    { "Campaign", "cutscenes Backgrounds",  "backgrounds/mov/%02d_layer1" },
    { "Campaign", "cutscenes Backgrounds",  "backgrounds/mov/%02d_layer2" },
    { "Campaign", "cutscenes Backgrounds",  "backgrounds/mov/%02d_layer3" },

    { "Free duel", "portraits",  "free_duel/portrait_%03d" },
    { "Free duel", "background", "free_duel/background" },
};
#define DISPLAY_RULE_N ((int)(sizeof DISPLAY_RULES / sizeof DISPLAY_RULES[0]))

int psx_wa_catalog_display_path(const PsxWaAsset *a, int index,
                                char *out, unsigned cap)
{
    if (!a || !out || !cap)
        return 0;
    const DisplayRule *rule = NULL;
    for (int i = 0; i < DISPLAY_RULE_N; i++)
        if (!strcmp(DISPLAY_RULES[i].name_fmt, a->name_fmt)) {
            rule = &DISPLAY_RULES[i];
            break;
        }
    if (!rule)
        return 0;

    /* Same leaf a raw export would use -- only the family prefix changes,
     * not how an individual row's own id gets formatted. */
    const char *slash = strrchr(a->name_fmt, '/');
    const char *leaf_fmt = slash ? slash + 1 : a->name_fmt;
    char leaf[64];
    if (strchr(leaf_fmt, '%'))
        snprintf(leaf, sizeof leaf, leaf_fmt, a->first + index);
    else
        snprintf(leaf, sizeof leaf, "%s", leaf_fmt);

    return (unsigned)snprintf(out, cap, "%s/%s/%s", rule->top, rule->sub, leaf) < cap;
}

/* Byte offset of a record. The backgrounds use the game's BCD index (10
 * becomes 0x10) -- everything else is a plain multiply. */
static uint32_t record_base(const PsxWaAsset *a, int index)
{
    uint32_t i = (uint32_t)index;
    if (a->bcd)
        i = ((i / 10u) << 4) | (i % 10u);
    return a->base + i * a->stride;
}

static uint32_t packed_len(const PsxWaAsset *a)
{
    const uint32_t n = (uint32_t)(a->w * a->h);
    return a->bpp == 4 ? n / 2u : n;
}

static int is_background(const PsxWaAsset *a);
static void bg_untile(const uint8_t *in, int in_w, int in_h,
                      uint8_t *out, int out_w, int out_h);

int psx_wa_catalog_decode(const PsxWaAsset *a, int index, uint8_t *out)
{
    return psx_wa_catalog_decode_pal(a, index, out, NULL, 0);
}

int psx_wa_catalog_decode_pal(const PsxWaAsset *a, int index, uint8_t *out,
                              const uint16_t *pal, int pal_n)
{
    if (!a || !psx_wa_catalog_decode_rows(a, index, out, 0, a->h, pal, pal_n))
        return 0;

    /* Backgrounds decode as the tiled page above; the exporter wants the clean
     * scene, so untile in place through a scratch. `out` was sized by the
     * caller for the DISPLAY dimensions (see psx_wa_catalog_disp_size), which
     * are smaller than the page, so this always fits. */
    if (is_background(a) || is_campaign_char(a)) {
        /* Sized for the TALLEST tiled page in the table -- family 2's first
         * layer is 1024 rows, four 256-row columns, not the 512 the other
         * layers use. */
        static uint8_t scratch[BG_TILE_W * BG_TILE_MAX_H * 4];
        if ((size_t)(a->w * a->h) * 4u > sizeof scratch)
            return 0;
        int dw, dh;
        psx_wa_catalog_disp_size(a, index, &dw, &dh);
        memcpy(scratch, out, (size_t)(a->w * a->h) * 4u);
        if (is_mov_l1(a) || is_campaign_char(a))
            bg_untile_cols(scratch, a->w, a->h, out, dw, dh);
        else if (is_layer3(a))
            bg_untile_l3(scratch, a->w, a->h, out, dw, dh);
        else
            bg_untile(scratch, a->w, a->h, out, dw, dh);
    }
    return 1;
}

int psx_wa_catalog_decode_rows(const PsxWaAsset *a, int index, uint8_t *out,
                               int row0, int nrows,
                               const uint16_t *pal, int pal_n)
{
    static uint8_t pix[WA_PACKED_MAX];
    static uint8_t clut[512];
    if (!a || !out || index < 0 || index >= a->count)
        return 0;
    if (row0 < 0 || nrows <= 0 || row0 + nrows > a->h)
        return 0;

    /* A caller-supplied palette stands in for one the table could not find.
     * It is copied into the same buffer the disc path fills so the decode
     * below has exactly one case to handle.
     *
     * `have_caller_pal` is tracked EXPLICITLY rather than inferred from
     * `entries == a->clut_entries`: a 4bpp caller palette is always 16
     * entries, and so is a->clut_entries whenever the table has a real one --
     * so that comparison was true whenever BOTH existed, not just when the
     * caller had none, and it made the disc read below overwrite the
     * caller's palette with the table's every single time. That silently
     * discarded every region-specific, live-observed palette this session
     * ever captured -- duel_labels exported through its own static "16
     * greys" table entry no matter what the game actually drew it with,
     * which is why a hazard-stripe border that is genuinely yellow at draw
     * time came out as a grey/navy ramp on every single export. */
    int entries = a->clut_entries;
    int have_caller_pal = 0;
    if (pal && pal_n > 0 && (unsigned)pal_n * 2u <= sizeof clut) {
        for (int i = 0; i < pal_n; i++) {
            clut[i * 2]     = (uint8_t)(pal[i] & 0xFFu);
            clut[i * 2 + 1] = (uint8_t)(pal[i] >> 8);
        }
        entries = pal_n;
        have_caller_pal = 1;
    }

    const uint32_t plen = packed_len(a);
    const uint32_t rec = record_base(a, index);
    if (plen > sizeof pix)
        return 0;
    if (!catalog_read_rows(a, rec + a->off, pix, a->h))
        return 0;
    if (!have_caller_pal && a->clut_entries &&
        !catalog_read_asset(a, rec + a->clut_off, clut,
                             (uint32_t)a->clut_entries * 2u))
        return 0;

    const uint32_t first = (uint32_t)row0 * (uint32_t)a->w;
    const uint32_t npix   = first + (uint32_t)nrows * (uint32_t)a->w;
    for (uint32_t i = first; i < npix; i++) {
        unsigned idx;
        if (a->bpp == 4) {
            const uint8_t b = pix[i >> 1];
            idx = (i & 1u) ? (b >> 4) : (b & 0xFu);   /* low nibble first */
        } else {
            idx = pix[i];
        }

        uint8_t *q = out + i * 4u;
        if (!entries) {
            /* No palette recovered: index as a grey ramp, index 0 the hole.
             * Structure without invented colour. */
            const uint8_t g = (uint8_t)(idx * 17u);
            q[0] = q[1] = q[2] = g;
            q[3] = idx ? 255 : 0;
            continue;
        }
        if ((int)idx >= entries) {              /* never seen; refuse to guess */
            q[0] = q[1] = q[2] = q[3] = 0;
            continue;
        }
        const unsigned v = (unsigned)clut[idx * 2]
                         | ((unsigned)clut[idx * 2 + 1] << 8);
        if (v == 0) {                            /* 0x0000 is transparent */
            q[0] = q[1] = q[2] = q[3] = 0;
            continue;
        }
        q[0] = (uint8_t)(((v      ) & 31) * 255 / 31);
        q[1] = (uint8_t)(((v >>  5) & 31) * 255 / 31);
        q[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        q[3] = 255;
    }

    return 1;
}

/* The size the exporter writes and the injector validates against: the display
 * scene for backgrounds, the sampled size for everything else. */
void psx_wa_catalog_disp_size(const PsxWaAsset *a, int index, int *w, int *h)
{
    if (is_mov_l1(a) && index >= 0 && index < BG_MOV_L1_N) {
        *w = BG_MOV_L1[index].w;
        *h = BG_MOV_L1[index].h;
    } else if (is_campaign_char(a)) {
        /* Three 128-wide columns side by side, not BG_DISP_W/H -- that pair
         * is backgrounds/comp+mov's own fixed 320x160 scene size, a
         * different family with a different real canvas. */
        *w = 384; *h = 256;
    } else if (is_background(a)) {
        *w = BG_DISP_W; *h = BG_DISP_H;
    } else {
        *w = a->w;      *h = a->h;
    }
}

/* A PS1 texture page is at most 256 rows, so the game uploads a taller image
 * as several pages -- a 512-row background as two, the 768-row UI sheet as
 * three -- and each arrives as its own transfer with its own bytes. Matching
 * therefore has to be per page: one registration per 256-row band, all sharing
 * the single file the player edits.
 *
 * This is what the runtime probe finally showed. Characters (exactly 256 rows)
 * matched from the start precisely because they are one page; everything
 * taller silently never did. */
#define PAGE_ROWS 256

/* Art whose palette this table cannot supply is the art whose palettes have to
 * be watched for at draw time -- and it is the only art worth spending the
 * shared pool on. Everything else already has a correct palette here. */
static int needs_clut_tracking(const PsxWaAsset *a)
{
    return a && (a->clut_entries == 0 || is_ui(a));
}

static void register_one(const PsxWaAsset *a, int index)
{
    static uint8_t buf[WA_PACKED_MAX];
    const uint32_t rec = record_base(a, index) + a->off;

    int dw, dh;
    psx_wa_catalog_disp_size(a, index, &dw, &dh);
    const int tiled = is_background(a) || is_campaign_char(a);
    const TexPackRetile retile = !tiled ? 0
                               : is_mov_l1(a) || is_campaign_char(a) ? bg_retile_cols
                               : is_layer3(a) ? bg_retile_l3
                               : bg_retile;

    const int bands = (a->h + PAGE_ROWS - 1) / PAGE_ROWS;
    const uint32_t rowb = (uint32_t)a->w / (a->bpp == 4 ? 2u : 1u);

    /* The curated path when this row has one (see
     * psx_wa_catalog_display_path()'s header comment), so the name registered
     * here -- the exact string texpack matches hashes and live draws against,
     * and export_one() derives its file path from -- already IS the path a
     * pack author's folder tree and the Asset Manager's own browsing tree
     * agree on. Falls back to the raw disc-family name for anything not
     * listed there yet; that is still a real, working registration, just
     * under the old family grouping until it gets a curated home too. */
    char name[96];
    if (!psx_wa_catalog_display_path(a, index, name, sizeof name))
        snprintf(name, sizeof name, a->name_fmt, a->first + index);

    for (int b = 0; b < bands; b++) {
        const int row0 = b * PAGE_ROWS;
        int rows = a->h - row0;
        if (rows > PAGE_ROWS) rows = PAGE_ROWS;

        /* An ANCHORED band is registered over exactly the rows its anchor
         * rectangle covers, not the full 256. The hash has to describe the
         * same bytes the rectangle does or it can never match -- and the whole
         * reason a band is anchored is that the game overwrites the rest of
         * its page. */
        if (a->anch_w && b == a->anch_band && a->anch_h > 0 &&
            a->anch_h <= rows)
            rows = a->anch_h;

        const uint32_t bb = (uint32_t)rows * rowb;
        if (bb > sizeof buf)
            return;
        /* row0's own skip has to advance by the REAL row spacing (the
         * stride), not by rowb (this entry's own, possibly-cropped, width)
         * -- they only coincide when row_stride is unset. */
        const uint32_t row0_stride_b = a->row_stride
            ? (uint32_t)a->row_stride / (a->bpp == 4 ? 2u : 1u) : rowb;
        if (!catalog_read_rows(a, rec + (uint32_t)row0 * row0_stride_b,
                               buf, rows))
            return;
        /* Skip blank bands: uniform content is not identifiable, and
         * registering it makes every blank VRAM page match it. */
        int flat = 1;
        for (uint32_t q = 1; q < bb; q++)
            if (buf[q] != buf[0]) { flat = 0; break; }
        if (flat)
            continue;
        /* Hash covers exactly the bytes of THIS page -- what one upload
         * carries. The whole-image hash matched nothing, which is why
         * backgrounds and the UI sheet never reinjected. */
        /* Anchored art is one band by construction and is found by position,
         * so it never goes through the page registration below. */
        /* A single-band anchored asset is standalone; a band of a taller one
         * registers as a page and has the anchor attached afterwards, so it
         * still maps into the same file as its siblings. */
        if (a->anch_w && bands == 1) {
            if (texpack_register_asset_anchored(name, texpack_hash(buf, bb),
                                                a->w, rows,
                                                a->anch_x, a->anch_y,
                                                a->anch_w, a->anch_h)) {
                if (psx_wa_catalog_tinted(a))
                    (void)texpack_set_tinted(name, 1);
                if (needs_clut_tracking(a))
                    (void)texpack_set_track_cluts(name, 1);
                if (is_ui(a) && !psx_wa_catalog_tinted(a))
                    (void)texpack_set_no_plain_fallback(name, 1);
                s_registered++;
            }
            continue;
        }
        if (texpack_register_asset_page(name, texpack_hash(buf, bb),
                                        a->w, rows,          /* this page   */
                                        dw, dh,              /* file on disk*/
                                        a->w, a->h,          /* whole image */
                                        0, row0,             /* page origin */
                                        retile)) {
            if (a->anch_w && b == a->anch_band)
                (void)texpack_set_anchor(name, row0, a->anch_x, a->anch_y,
                                         a->anch_w, a->anch_h);
            /* Tinting is a property of the NAME, so it is applied here too --
             * it used to be set only on the anchored path, which silently did
             * nothing for every UI asset that is not anchored. */
            if (psx_wa_catalog_tinted(a))
                (void)texpack_set_tinted(name, 1);
            if (needs_clut_tracking(a))
                (void)texpack_set_track_cluts(name, 1);
            if (is_ui(a) && !psx_wa_catalog_tinted(a))
                (void)texpack_set_no_plain_fallback(name, 1);
            s_registered++;
        }
    }
}

/* Runs once, after the guest is up so the disc is readable. Registering all
 * 722 cards is ~3600 sector reads; it happens on the start hook rather than
 * per frame so the cost lands once, before anything is drawn. */
static void build_catalog(void)
{
    if (s_ready)
        return;
    s_ready = 1;

    texpack_init();

    /* One pack directory means that pack is the one you meant. Selecting a
     * pack is not yet a menu option, and defaulting to "none" would make a
     * pack the player deliberately installed do nothing at all with no way to
     * say so. When several exist the first by name wins, which is at least
     * predictable; a proper selector belongs with the drawing path. */
    if (texpack_active_pack() < 0 && texpack_pack_count() > 0)
        texpack_set_active_pack(0);

    for (int t = 0; t < ASSET_N; t++)
        for (int i = 0; i < ASSETS[t].count; i++)
            register_one(&ASSETS[t], i);
}

int psx_wa_catalog_state_json(char *out, unsigned cap)
{
    return (unsigned)snprintf(out, cap, "\"ready\":%d,\"registered\":%d",
                              s_ready, s_registered) < cap;
}

static const char *const ONOFF[] = { "Off", "On" };

/* Which card the deferred rebuild has reached; 0 when idle. */
static int s_reload_id;

/*
 * The menu callback does nothing but flip a flag and arm the rebuild.
 *
 * It used to reload all 722 packs inline, which crashed on the way OUT of HD
 * mode -- reloading 722 cards' ini/effects/deck state synchronously from a
 * menu callback was the bug regardless of what catalog_tick()'s rebuild
 * comment above says the rebuild is FOR (that part is now stale -- see
 * there); reading all that state back in still costs real disc and guest
 * work per card and still cannot safely happen inline from a callback.
 *
 * Every other option in this game sets a variable and returns (see
 * card_name_color_enabled_changed). The work belongs on the emulation thread,
 * a few cards per frame, which is also what keeps the toggle from freezing the
 * picture for a second and a half.
 */
static void hd_textures_changed(int value)
{
    texpack_set_enabled(value);

    /* Turning it back on re-reads the pack's files, so toggling off and on is
     * how you see art you just edited without restarting the game. Decoded
     * pictures are cached for the life of the process otherwise, and a file
     * changed on disk would never be read a second time. */
    if (value)
        texpack_request_reload();

    if (s_ready)
        s_reload_id = 1;
}

/* Re-read the pack without touching the toggle. Toggling off and on also
 * works, but it hands card art back to the card manager and then takes it
 * again -- a whole 722-card rebuild -- for a change this does directly. */
static void reload_pack_activate(void)
{
    texpack_request_reload();
}

/*
 * Rebuild the disc-side card packs after the HD toggle moved.
 *
 * Stale as of 2026-09-13: build_disc_side() (psx_card_packs.c) no longer
 * calls texpack_enabled() at all -- there is no more quantized disc-side
 * fallback for it to choose between, only "stock unless a shared file
 * exists", which does not change when the toggle does. texpack_enabled() is
 * consulted directly inside texture_pack.c's own resolve path instead (what
 * the raw VRAM injector actually substitutes at draw time), which is where
 * turning HD off or on takes effect now.
 *
 * This sweep is therefore not doing anything the toggle still needs -- kept
 * for now rather than removed mid-review; see the flagged follow-up to
 * confirm and drop it. */
static void catalog_tick(void)
{
    if (!psx_mod_game_started())
        return;

    /* The text font is written to VRAM during boot and never transferred
     * again, so there is no upload to catch it by. Look for it where it
     * lives instead; this is a no-op once it has been found. */
    texpack_resolve_anchors();

    /* Deferred from the menu callback: reloading frees decoded art and walks
     * the pack directory, which must not happen underneath a draw. */
    texpack_reload_if_pending();

    if (!s_reload_id)
        return;

    for (int n = 0; n < 8 && s_reload_id <= CARD_COUNT; n++)
        psx_card_packs_reload(s_reload_id++);

    if (s_reload_id > CARD_COUNT)
        s_reload_id = 0;
}

PSX_MOD_CONSTRUCTOR(psx_wa_catalog_install)
{
    /* On by default: a pack only exists because the player put one there.
     * The toggle is for comparing against the original, and for turning the
     * whole thing off without deleting the pack. */
    (void)psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "HD textures",
        "Draw HD replacements from the active texture pack",
        ONOFF, 2, "hd_textures", 1, hd_textures_changed);
    (void)psx_video_menu_add_action(
        PSX_VM_MENU_MODS, "Reload texture pack",
        "Re-read the pack's files so edited art appears without restarting",
        reload_pack_activate);
    (void)psx_game_add_start_hook(build_catalog);
    (void)psx_game_add_frame_hook(catalog_tick);
}
