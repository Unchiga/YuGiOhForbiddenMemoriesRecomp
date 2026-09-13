/* psx_asset_manager.c -- see psx_asset_manager.h.
 *
 * A reference window beside the game, same family as the Card/Dialogue/
 * Fusion managers: browse what psx_wa_catalog.c's ONE TABLE knows how to
 * export, compare the stock picture against whatever the active HD texture
 * pack currently has for it, and upload a replacement without leaving the
 * game or hand-typing a folder path.
 *
 * CATEGORIES AND ASSETS, NOT ONE FLAT LIST
 * -----------------------------------------
 * The catalog table already groups related art into "families" (a name_fmt
 * like "cards/art/%03d"); this window turns that into the two-level browser
 * the texture-pack folder itself already is. A category is everything in a
 * name_fmt up to its LAST '/' ("cards/art", "ui/font", "backgrounds/comp");
 * an asset is one instance of it ("001", "font", "01_layer1"). Two different
 * table rows land in the same category when they share that prefix (the
 * three duel_fields rows, the three background layers), which is exactly
 * how the exporter already piles them into one folder.
 *
 * STOCK VS PACK, NOT STOCK VS "REPLACED"
 * -----------------------------------------
 * The stock side always decodes fresh from the disc via psx_wa_catalog_
 * decode() -- the same call the exporter makes, so what this window shows
 * is exactly what "Export stock textures" would write. The pack side reads
 * whatever PNG currently sits at the path the exporter and the injector both
 * use, at ITS OWN resolution (no forced resize): psx_ui_blit_scaled already
 * scales an arbitrary source into a fixed box, so a 4x painted replacement
 * shows the upscale it actually is instead of being squashed to stock size.
 *
 * MULTI-REGION UI ART HAS NO SINGLE "PACK FILE" TO SHOW
 * -------------------------------------------------------
 * Most families export as one flat PNG per asset and this window treats them
 * that way: pick one, see two pictures, click Upload to replace the second.
 * But a UI sheet drawn through more than one palette (psx_wa_catalog_ui(a) &&
 * a->tint != 1 -- duel_labels, duel_panels, the monster-type icons, the menu labels,
 * panels, dialog_frame, card_sleeves/canvas) exports as MANY small files
 * named by a content hash, one per (rectangle, palette) pair the game was
 * actually seen drawing -- see psx_texture_export.c's write_elements/
 * write_screens. There is no single path to preview or replace for these;
 * pretending otherwise would either show the wrong thing or invite an
 * upload that the injector would never look for. This window instead lists
 * whatever region files already exist for it and says so plainly.
 *
 * Same skeleton as psx_dialogue_manager.c: created on open and destroyed on
 * close, the F10 menu's toolkit and palette, sizes in design units (one
 * 480th of the window height), the software renderer on Windows and the
 * accelerated one elsewhere with the game's GL context put back after every
 * renderer call (PSX_TOOL_RENDERER overrides).
 */

#include "psx_asset_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define AM_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define AM_MKDIR(p) mkdir((p), 0755)
#endif

#include "psx_tool_window.h"
#include "psx_sdl.h"

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_texture_export.h"
#include "psx_wa_catalog.h"
#include "texture_pack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../psxrecomp/runtime/third_party/stb_image.h"

/* --- look, matched to the other tool windows ------------------------------ */

#define WIN_W  1400
#define WIN_H   800

#define COL_BG        0xFF0F1219u
#define COL_BAR       0xFF141826u
#define COL_PANEL     0xFF1E2233u
#define COL_WELL      0xFF14171Fu   /* preview boxes: darker, so alpha reads */
#define COL_TEXT      0xFFC9CFDDu
#define COL_DIM       0xFF7C8598u
#define COL_ACCENT    0xFF7FA6FFu
#define COL_SEL_BG    0xFF2C3B60u
#define COL_HOVER     0x14FFFFFFu
#define COL_EDIT_BG   0xFF0E1119u
#define COL_BTN       0xFF2A3147u
#define COL_BTN_ON    0xFF3D5A9Cu
#define COL_BTN_OFF   0xFF1C2030u   /* disabled (no folder chosen, etc.) */
#define COL_TRACK     0x40FFFFFFu
#define COL_THUMB     0xFF7C8598u
#define COL_WARN      0xFFE8C36Au
#define COL_HAVE      0xFF8BD48Bu   /* pack has this asset */
#define COL_MISS      0xFF7C8598u   /* pack does not */

#define U_BAR_H     26.0f
#define U_GAP        6.0f
#define U_PAD       10.0f
#define U_ROW_H     16.0f
#define U_HDR_H     16.0f
#define U_BTN_H     17.0f
#define U_FOOT_H    14.0f
#define U_SB_W       4.0f
#define U_R_PANEL    9.0f
#define U_R_BOX      5.0f
#define U_FS_TITLE  11.0f
#define U_FS_BODY    9.5f
#define U_FS_SMALL   8.5f
#define U_CAT_W    150.0f
#define U_ASSET_W  170.0f
#define U_PREVIEW_H 260.0f

#define S_ELLIP  "\xE2\x80\xA6"

static SDL_Window   *s_win;
static uint32_t     *s_px;
static int           s_w, s_h;
static int           s_dirty = 1;
static PsxUiCanvas   s_cv;
static float         s_u = 1.0f;

static int px(float u) { return (int)(u * s_u + 0.5f); }
static const PsxUiFace *face_title(void) { return psx_ui_font_face(U_FS_TITLE * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_body(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_REGULAR); }
static const PsxUiFace *face_bold(void)  { return psx_ui_font_face(U_FS_BODY * s_u, PSX_UI_FONT_SEMIBOLD); }
static const PsxUiFace *face_small(void) { return psx_ui_font_face(U_FS_SMALL * s_u, PSX_UI_FONT_REGULAR); }
static int tw(const PsxUiFace *f, const char *s) { return f ? psx_ui_font_text_w(f, s) : 0; }

typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
static void text_in(const Rect *r, int inset, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text_clip(&s_cv, r->x + inset, psx_ui_baseline_in(r->y, r->h, f), s, col, f, r->w - inset * 2);
}
static void text_centered(const Rect *r, const char *s, uint32_t col, const PsxUiFace *f)
{
    psx_ui_text(&s_cv, r->x + (r->w - tw(f, s)) / 2, psx_ui_baseline_in(r->y, r->h, f), s, col, f);
}

/* --- the catalog, flattened into categories and assets --------------------
 *
 * Built once and kept for the process's life: the table this reads
 * (psx_wa_catalog_table) is a compile-time constant, so there is nothing to
 * refresh it against. */
#define CAT_MAX    64
#define ASSET_MAX  3200   /* >= the ~2625 individual assets this title's
                           * table currently expands to; see catalog_tick's
                           * "registered" count for the live figure. */

typedef struct {
    char name[64];
    int  count;             /* for the "(N)" badge only -- see AssetEntry.cat
                             * for why membership is no longer a contiguous
                             * range into s_assets[] */
} Cat;

typedef struct {
    char name[48];          /* leaf: "001", "duel_labels", "01_layer1" */
    int  cat;               /* which s_cats[] entry this belongs to. NOT
                             * necessarily contiguous with its neighbours:
                             * expand_interleaved_bg() runs its own pass AFTER
                             * the main CAT_RULES loop has already interleaved
                             * every other category, and can add more members
                             * to a category that loop already created, with
                             * everything from OTHER categories sitting
                             * between them in s_assets[] by then. A category
                             * used to be identified by a single (start,
                             * count) range on the assumption that everything
                             * in it landed back-to-back; that assumption
                             * breaks the moment a second pass can add to an
                             * already-populated category. Filtering by this
                             * field instead of a range is what actually
                             * answers "is this asset in category X", however
                             * many separate passes contributed to it. */
    int  row;               /* index into the psx_wa_catalog_table() array */
    int  index;             /* 0-based within that row, for the decode calls */
    int  multi_region;      /* 1: exports as many hash-named region files,
                             * not one PNG -- see the file header. */
} AssetEntry;

static Cat        s_cats[CAT_MAX];
static int        s_cat_n;
static AssetEntry s_assets[ASSET_MAX];
static int        s_asset_n;
static int        s_catalog_built;

/* "cards/art/%03d" -> cat "cards/art", leaf_fmt "%03d". Splits at the LAST
 * '/' so a family with its own subdirectory ("ui/font/font") still nests
 * the way its files actually sit on disk. */
static void split_family(const char *name_fmt, char *cat, size_t catcap,
                         char *leaf_fmt, size_t leafcap)
{
    const char *slash = strrchr(name_fmt, '/');
    if (!slash) { snprintf(cat, catcap, "%s", ""); snprintf(leaf_fmt, leafcap, "%s", name_fmt); return; }
    const size_t n = (size_t)(slash - name_fmt);
    snprintf(cat, catcap, "%.*s", (int)(n < catcap - 1 ? n : catcap - 1), name_fmt);
    snprintf(leaf_fmt, leafcap, "%s", slash + 1);
}

/* A leaf format may have no '%' at all (every count==1 asset without a
 * numeric id, e.g. "duel_labels") -- snprintf with an int argument and no
 * specifier to consume it is well-defined but warns on some compilers, so
 * this only passes the id through when the format actually wants it. */
static void format_leaf(const char *fmt, int id, char *out, size_t cap)
{
    if (strchr(fmt, '%')) snprintf(out, cap, fmt, id);
    else snprintf(out, cap, "%s", fmt);
}

static int find_or_add_cat(const char *name)
{
    for (int i = 0; i < s_cat_n; i++)
        if (!strcmp(s_cats[i].name, name)) return i;
    if (s_cat_n >= CAT_MAX) return -1;
    snprintf(s_cats[s_cat_n].name, sizeof s_cats[s_cat_n].name, "%s", name);
    s_cats[s_cat_n].count = 0;
    return s_cat_n++;
}

/* The path this asset's file actually lives at, root-relative and without
 * extension -- "UI/Deck/panels" -- exactly what register_one() and
 * export_one() (psx_wa_catalog.c / psx_texture_export.c) now both derive
 * through psx_wa_catalog_display_path(), so this window's browsing tree and
 * the on-disk folder tree are the SAME agreement instead of two hand-kept
 * ones that used to intentionally differ (curated display vs. disc-family
 * path). Falls back to the raw disc-family path for a row not yet listed in
 * that table, matching the same fallback registration/export use.
 *
 */
static void asset_rel_path(const AssetEntry *e, char *out, size_t cap)
{
    int rows = 0;
    const PsxWaAsset *tbl = psx_wa_catalog_table(&rows);
    if (e->row < 0 || e->row >= rows) { out[0] = 0; return; }
    const PsxWaAsset *a = &tbl[e->row];
    if (!psx_wa_catalog_display_path(a, e->index, out, (unsigned)cap))
        snprintf(out, cap, a->name_fmt, a->first + e->index);
}

/* How many draws of a multi-region asset the running game has actually
 * shown this session -- exactly the question "will Export stock textures
 * write anything for this yet", since write_elements/write_screens
 * (psx_texture_export.c) read from this same live table and write nothing
 * when it is empty. 0 for anything that is not multi-region: a flat asset
 * either has a disc palette or falls back to texpack_observed_clut() on its
 * own, so "seen" is not the gate for it the way it is here.
 *
 * The name has to be reconstructed exactly as register_one()
 * (psx_wa_catalog.c) built it -- asset_rel_path()'s curated path, now that
 * registration itself derives its name the same way -- because that string,
 * not a display label, is texpack's registration key. */
static int asset_live_seen_count(const AssetEntry *e)
{
    if (!e->multi_region)
        return 0;
    char name[128];
    asset_rel_path(e, name, sizeof name);
    return texpack_clut_rect_count(name);
}

/* --- the curated browsing tree ---------------------------------------------
 *
 * The catalog's own family names are the right thing for a FILE PATH and the
 * wrong thing for a MENU: "cards/titles" means nothing to a pack author, and
 * a mechanically-derived tree scatters art someone thinks of as one screen
 * (SAVE/LOAD/TRADE, BUILD DECK/OPTION/PASSWORD, the CAUTION box) across
 * whatever family happened to catalogue it first. This table is hand-sorted
 * by what a player recognises on screen instead.
 *
 * `name_fmt` matches a row's OWN name_fmt exactly -- the same string the
 * ASSETS[] table in psx_wa_catalog.c uses -- so a row COULD be listed under
 * more than one branch if that ever reads better than one home each (none
 * currently are): each listing just re-decodes and re-paths the same row,
 * never a second copy of the asset. `name_fmt` NULL means the branch exists
 * with nothing catalogued under it
 * yet -- shown with a (0) count rather than silently missing, so the menu
 * this project is building toward is visible before every screen behind it
 * has been individually catalogued (see the SU.MRG main-menu work). */
typedef struct { const char *top, *sub, *name_fmt; } CatRule;
static const CatRule CAT_RULES[] = {
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
    /* Option / Trade: each a specific screen the SU.MRG main-menu
     * investigation is still splitting out of the shared menu-label pages
     * above (see ui/menu_labels_1..3's own header comment in
     * psx_wa_catalog.c) -- placeholders until that finishes rather than an
     * incorrect guess at which pixels belong to which screen. Deck's own
     * furniture (CHEST/ORDER/DECK) IS already catalogued, at ui/panels. */
    { "UI", "Deck",       "ui/panels" },
    { "UI", "Option",     NULL },
    { "UI", "Trade",      NULL },
    /* Library's own card-grid/digit sheet -- a WA_MRG.MRG package entirely
     * separate from the SU.MRG menu-label pages above. Its OTHER sheet, the
     * card-viewer frame, is not a second row here: it is the exact same
     * bytes as ui/card_sleeves/canvas (see the provenance comment on this
     * row in psx_wa_catalog.c), so it already appears under Card assets. */
    { "UI", "Library",    "ui/library/grid" },
    /* Password's own screen furniture -- a WA_MRG.MRG package entirely
     * separate from the SU.MRG menu-label pages above, so it did not need
     * to wait on that investigation; found via memories-decomp (see the
     * provenance comment on these rows in psx_wa_catalog.c). */
    { "UI", "Password",   "ui/password/card" },
    { "UI", "Password",   "ui/password/cursor" },
    { "UI", "Password",   "ui/password/frame" },
    { "UI", "Results",    "ui/results/labels" },
    { "UI", "Results",    "ui/results/banner" },
    { "UI", "Name Entry", "ui/name_entry/background" },
    { "UI", "Name Entry", "ui/name_entry/frame" },

    { "Card assets", "card artworks",   "cards/art/%03d" },
    { "Card assets", "card thumbnails", "cards/mini/%03d" },
    { "Card assets", "card Titles",     "cards/titles/%03d" },
    { "Card assets", "card Frames",     "ui/card_sleeves/canvas" },
    { "Card assets", "card Frames",     "ui/card_sleeves/duel_sprites" },
    { "Card assets", "card Frames",     "ui/card_sleeves/text_placeholders" },

    { "Campaign", "Static Backgrounds",     "backgrounds/static/%02d" },
    /* NOT listed here: the comp/mov layers interleave (00_layer1, 00_layer2,
     * 00_layer3, 01_layer1, ...) rather than running all of one layer before
     * the next, which a flat name_fmt-per-rule rule can't express -- see
     * expand_interleaved_bg, called separately in build_catalog(). */
    { "Campaign", "characters",             "campaign_characters/%03d" },
    { "Campaign", "cutscene portraits",     "dialog_thumbnails/thumb_%03d" },

    { "Free duel", "portraits",  "free_duel/portrait_%03d" },
    { "Free duel", "background", "free_duel/background" },
    /* No free-duel-specific UI art is catalogued separately yet -- the
     * screen reuses the shared UI sheet above (UI > menu, UI > duel HUD). */
    { "Free duel", "UI",         NULL },
};
#define CAT_RULE_N ((int)(sizeof CAT_RULES / sizeof CAT_RULES[0]))

static int find_row_by_name_fmt(const PsxWaAsset *tbl, int rows, const char *name_fmt)
{
    for (int t = 0; t < rows; t++)
        if (!strcmp(tbl[t].name_fmt, name_fmt)) return t;
    return -1;
}

/* Appends one (row, index) instance to category ci, formatting its leaf name
 * from that row's own name_fmt. Shared by the plain per-rule expansion below
 * and by expand_interleaved_bg's per-index interleave. */
static void append_asset(int ci, const PsxWaAsset *tbl, int t, int index)
{
    if (ci < 0 || s_asset_n >= ASSET_MAX) return;
    const PsxWaAsset *a = &tbl[t];
    char cat_unused[64], leaf_fmt[48];
    split_family(a->name_fmt, cat_unused, sizeof cat_unused, leaf_fmt, sizeof leaf_fmt);
    AssetEntry *e = &s_assets[s_asset_n];
    format_leaf(leaf_fmt, a->first + index, e->name, sizeof e->name);
    e->cat = ci; e->row = t; e->index = index;
    /* tint == 0 is the "genuinely drawn through more than one palette"
     * case export_one() (psx_texture_export.c) treats as multi-region --
     * this must agree with that exact condition or an asset can export as
     * one flat file while this window still shows it as region fragments
     * (or vice versa). tint == -1 (dialog_frame, panels, back, copyright)
     * has one real fixed palette and is a flat file; tint == 1 is the
     * always-tinted mask case, also flat. */
    e->multi_region = psx_wa_catalog_ui(a) && a->tint == 0;
    s_cats[ci].count++;
    s_asset_n++;
}

/* backgrounds/comp and backgrounds/mov each store their three layers as
 * separate table rows (a layer is its own palette, its own bpp even), but a
 * pack author thinks of them as one background with three parts -- 00_layer1,
 * 00_layer2, 00_layer3, then 01's three, not all 42 of layer1 before layer2
 * starts. Interleaving by index is what a plain per-rule CAT_RULES entry
 * cannot express, so this runs it directly. */
static void expand_interleaved_bg(const PsxWaAsset *tbl, int rows,
                                  const char *top, const char *sub,
                                  const char *fmt1, const char *fmt2, const char *fmt3)
{
    char label[80];
    snprintf(label, sizeof label, "%s / %s", top, sub);
    const int ci = find_or_add_cat(label);
    const int t1 = find_row_by_name_fmt(tbl, rows, fmt1);
    const int t2 = find_row_by_name_fmt(tbl, rows, fmt2);
    const int t3 = find_row_by_name_fmt(tbl, rows, fmt3);
    if (ci < 0 || t1 < 0 || t2 < 0 || t3 < 0) return;
    const int n = tbl[t1].count;
    for (int i = 0; i < n && s_asset_n < ASSET_MAX; i++) {
        append_asset(ci, tbl, t1, i);
        append_asset(ci, tbl, t2, i);
        append_asset(ci, tbl, t3, i);
    }
}

static void build_catalog(void)
{
    if (s_catalog_built) return;
    s_catalog_built = 1;
    int rows = 0;
    const PsxWaAsset *tbl = psx_wa_catalog_table(&rows);
    for (int r = 0; r < CAT_RULE_N; r++) {
        const CatRule *rule = &CAT_RULES[r];
        char label[80];
        snprintf(label, sizeof label, "%s / %s", rule->top, rule->sub);
        const int ci = find_or_add_cat(label);
        if (ci < 0 || !rule->name_fmt) continue;   /* NULL: placeholder branch */
        const int t = find_row_by_name_fmt(tbl, rows, rule->name_fmt);
        if (t < 0) continue;                       /* should not happen; stay safe */
        for (int i = 0; i < tbl[t].count && s_asset_n < ASSET_MAX; i++)
            append_asset(ci, tbl, t, i);
    }
    expand_interleaved_bg(tbl, rows, "Campaign", "Compounded Backgrounds",
                         "backgrounds/comp/%02d_layer1", "backgrounds/comp/%02d_layer2", "backgrounds/comp/%02d_layer3");
    expand_interleaved_bg(tbl, rows, "Campaign", "cutscenes Backgrounds",
                         "backgrounds/mov/%02d_layer1", "backgrounds/mov/%02d_layer2", "backgrounds/mov/%02d_layer3");
}

/* --- state ----------------------------------------------------------------- */

static int  s_open_req;
static int  s_cat_sel = -1, s_asset_sel = -1;
static int  s_cat_scroll, s_asset_scroll;
static int  s_cat_hover = -1, s_asset_hover = -1;
static char s_search[48];
static int  s_caret_on = 1;
static char s_msg[256];
static Uint32 s_msg_until;

static char s_pack_root[1024];     /* "" until chosen or an active pack exists */

/* the two preview images for the current selection */
#define PIXELS_MAX (256 * 768)     /* the largest whole image the table holds */
static uint32_t s_stock_argb[PIXELS_MAX];
static int      s_stock_ok, s_stock_w, s_stock_h;
static const uint32_t *stock_ptr(void) { return s_stock_argb; }
static uint32_t *s_pack_argb;       /* malloc'd: an uploaded pack file may be
                                     * far larger than PIXELS_MAX (a 4x paint) */
static int      s_pack_ok, s_pack_w, s_pack_h;
static int      s_preview_asset = -1;   /* which s_assets[] index they are for */
static char     s_preview_root[1024];   /* which pack root they were read from */

/* multi-region: filenames already exported for the current selection.
 *
 * Was 64. A long session on a busy multi-context sheet (duel_labels: every
 * distinct rect+palette combination actually drawn gets its own file) grows
 * past that easily -- caught duel_labels at 71 files, with readdir()'s
 * unsorted order putting one of them at position 67. scan_region_files()
 * below stops the moment it hits the cap, so anything past it is silently
 * missing from the Pack preview -- the file is fine on disk, it was just
 * never looked at. Raised well past the one count observed rather than to
 * it exactly, since the next long session hits whatever number is chosen
 * eventually too. 4096 entries costs 4096*64 = 256 KB static, trivial. */
#define REGION_FILES_MAX 4096
static char s_region_files[REGION_FILES_MAX][64];
static int  s_region_n;

/* multi-region: the region files re-assembled into one picture, the same
 * way write_screens() (psx_texture_export.c) lays them out for a human to
 * look at -- every region filename already carries its own destination
 * rect ("x_y_WxH@hash.png", see write_elements), so this just blits each
 * decoded region into a canvas sized to the asset's own declared w/h at
 * that rect instead of leaving the pack side of the window blank. Not a
 * new export format: still reads exactly the files write_elements wrote. */
static uint32_t *s_pack_region_argb;
static int       s_pack_region_ok, s_pack_region_w, s_pack_region_h;

enum { BTN_FOLDER = 0, BTN_UPLOAD, BTN_OPEN_FOLDER, BTN_EXPORT, BTN_EXPORT_ALL, BTN_COUNT };
static const char *const BTN_LABEL[BTN_COUNT] = {
    "Choose pack folder" S_ELLIP, "Upload" S_ELLIP, "Open folder",
    "Export" S_ELLIP, "Export all" S_ELLIP
};

/* the file/folder dialog's answer, consumed on the emulation thread */
static char s_pick_path[1200];
static int  s_pick_kind;   /* 1 folder chosen, 2 file to upload, 3 export-one
                             * destination, 4 export-all destination */

/* Export All walks s_assets[] a handful at a time from tick() (see the
 * background exporter's own "a few per frame" reasoning in
 * psx_texture_export.c's header -- doing ~2700 disc decodes in one button
 * click would freeze the window for the same reason doing all of them in one
 * frame would freeze the game) rather than in one shot, so the window keeps
 * redrawing and the footer message can show live progress. */
static char s_export_all_root[1200];
static int  s_export_all_active;
static int  s_export_all_idx;
static int  s_export_all_written;
static int  s_export_all_failed;
#define EXPORT_ALL_PER_TICK 8

typedef struct {
    Rect bar, btn[BTN_COUNT], search;
    Rect cats, assets, detail;
    Rect prev_stock, prev_pack;
    Rect region_list;
    int  row_h;
} Layout;
static Layout s_L;

static void say(const char *m)
{
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = SDL_GetTicks() + 6000u;
    s_dirty = 1;
}

/* --- reading pictures ------------------------------------------------------ */

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = sz;
    return buf;
}

static void mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        AM_MKDIR(path);
        *p = '/';
    }
    AM_MKDIR(path);
}

/* <root>/<rel>.png, the same path the exporter writes and the injector reads
 * -- the one fixed point both this window and the running game agree on.
 * `rel` is asset_rel_path()'s result: the curated path when this asset has
 * one, the raw disc-family path otherwise -- either way, exactly what
 * register_one()/export_one() derived the same name from. */
static void asset_path(const char *root, const char *rel,
                       char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.png", root, rel);
}

/* <root>/<rel>/ -- where a multi-region asset's per-rectangle files land
 * (see psx_texture_export.c's write_elements/write_screens). */
static void asset_dir(const char *root, const char *rel,
                      char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", root, rel);
}

/* Native resolution, no forced resize: psx_ui_blit_scaled fits whatever this
 * returns into the preview box, so a painted 4x replacement shows the
 * upscale it actually is. */
static int load_png_native(const char *path, uint32_t **out_argb, int *w, int *h)
{
    long sz;
    unsigned char *file = read_file(path, &sz);
    if (!file) return 0;
    int comp;
    unsigned char *img = stbi_load_from_memory(file, (int)sz, w, h, &comp, 4);
    free(file);
    if (!img) return 0;
    const size_t n = (size_t)(*w) * (size_t)(*h);
    uint32_t *argb = (uint32_t *)malloc(n * 4u);
    if (!argb) { stbi_image_free(img); return 0; }
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = img + i * 4u;
        argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    stbi_image_free(img);
    *out_argb = argb;
    return 1;
}

/* Filenames already exported for a multi-region asset -- no path, just what
 * a listing of its subfolder finds, so the window can say "3 regions
 * exported" without pretending there is one file to preview or replace.
 *
 * Skips names starting with '_' -- "_reference.png" (psx_texture_export.c)
 * is deliberately named so the RUNTIME's exact-match replacement lookup
 * never picks it up; this scan didn't know that convention and counted it
 * as a captured region anyway, so an asset with zero real captures but a
 * written reference showed "1 region file exported" with nothing to show
 * for it (compose_region_files()'s sscanf silently skips the unparseable
 * name, so the count and the composed preview disagreed -- caught on
 * duel_fields/0_top, whose Pack panel was blank under that misleading
 * count). Any future underscore-prefixed helper file gets the same pass. */
static void scan_region_files(const char *dir)
{
    s_region_n = 0;
#if defined(_WIN32)
    char glob[1200];
    snprintf(glob, sizeof glob, "%s\\*.png", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (s_region_n >= REGION_FILES_MAX) break;
        if (fd.cFileName[0] == '_') continue;
        snprintf(s_region_files[s_region_n++], sizeof s_region_files[0], "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && s_region_n < REGION_FILES_MAX) {
        if (de->d_name[0] == '_') continue;
        const char *ext = strrchr(de->d_name, '.');
        if (!ext || strcmp(ext, ".png") != 0) continue;
        snprintf(s_region_files[s_region_n++], sizeof s_region_files[0], "%s", de->d_name);
    }
    closedir(dp);
#endif
}

/* Reassembles this multi-region asset's exported files into one picture at
 * canvas_w x canvas_h -- the asset's own declared display size, the same
 * canvas write_screens() (psx_texture_export.c) lays these exact rectangles
 * into for its own "screens" reference output. Each region filename
 * already carries its destination rect ("x_y_WxH@hash.png", see
 * write_elements), so no new bookkeeping is needed to place it.
 *
 * Two regions can legitimately share one rectangle -- the same wordmark
 * exported once per palette it is actually drawn through (a Konami-style
 * black/white wash) -- and here scan order decides which is on top. That is
 * fine for a review picture; it is not the injector's own per-draw CLUT
 * match and is not meant to stand in for it. */
static void compose_region_files(const char *dir, int canvas_w, int canvas_h)
{
    free(s_pack_region_argb); s_pack_region_argb = NULL;
    s_pack_region_ok = 0; s_pack_region_w = s_pack_region_h = 0;
    if (canvas_w <= 0 || canvas_h <= 0 || (size_t)canvas_w * (size_t)canvas_h > PIXELS_MAX)
        return;
    uint32_t *canvas = (uint32_t *)calloc((size_t)canvas_w * (size_t)canvas_h, 4u);
    if (!canvas) return;

    int placed = 0;
    for (int i = 0; i < s_region_n; i++) {
        int x0, y0, w, h;
        if (sscanf(s_region_files[i], "%d_%d_%dx%d@", &x0, &y0, &w, &h) != 4)
            continue;
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", dir, s_region_files[i]);
        uint32_t *argb = NULL; int rw, rh;
        if (!load_png_native(path, &argb, &rw, &rh)) continue;
        for (int y = 0; y < rh; y++) {
            const int cy = y0 + y;
            if (cy < 0 || cy >= canvas_h) continue;
            for (int x = 0; x < rw; x++) {
                const int cx = x0 + x;
                if (cx < 0 || cx >= canvas_w) continue;
                canvas[(size_t)cy * canvas_w + cx] = argb[(size_t)y * rw + x];
            }
        }
        free(argb);
        placed++;
    }

    if (placed) {
        s_pack_region_argb = canvas;
        s_pack_region_w = canvas_w; s_pack_region_h = canvas_h;
        s_pack_region_ok = 1;
    } else {
        free(canvas);
    }
}

static void free_pack_preview(void)
{
    free(s_pack_argb); s_pack_argb = NULL;
    s_pack_ok = 0; s_pack_w = s_pack_h = 0;
    free(s_pack_region_argb); s_pack_region_argb = NULL;
    s_pack_region_ok = 0; s_pack_region_w = s_pack_region_h = 0;
}

/* Re-decode the stock side and re-read the pack side for whichever asset is
 * selected. Cheap enough to call on every selection change and every folder
 * change: the stock decode is one disc-format image, and the pack read is
 * one small PNG file -- neither is a per-frame cost, this only runs on
 * clicks. */
static void refresh_preview(void)
{
    if (s_asset_sel == s_preview_asset && !strcmp(s_pack_root, s_preview_root))
        return;
    s_preview_asset = s_asset_sel;
    snprintf(s_preview_root, sizeof s_preview_root, "%s", s_pack_root);
    free_pack_preview();
    s_stock_ok = 0;
    s_region_n = 0;
    if (s_asset_sel < 0) return;

    const AssetEntry *e = &s_assets[s_asset_sel];
    int rows = 0;
    const PsxWaAsset *tbl = psx_wa_catalog_table(&rows);
    const PsxWaAsset *a = &tbl[e->row];
    psx_wa_catalog_disp_size(a, e->index, &s_stock_w, &s_stock_h);
    if ((size_t)s_stock_w * (size_t)s_stock_h <= PIXELS_MAX) {
        static uint8_t rgba[PIXELS_MAX * 4];
        if (psx_wa_catalog_decode(a, e->index, rgba)) {
            const size_t n = (size_t)s_stock_w * (size_t)s_stock_h;
            for (size_t i = 0; i < n; i++) {
                const uint8_t *p = rgba + i * 4u;
                s_stock_argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            }
            s_stock_ok = 1;
        }
    }

    if (!s_pack_root[0]) return;
    char rel[128]; asset_rel_path(e, rel, sizeof rel);
    if (e->multi_region) {
        char dir[1200];
        asset_dir(s_pack_root, rel, dir, sizeof dir);
        scan_region_files(dir);
        /* Same canvas the stock decode above just used -- the exported
         * regions are destination rects within that same declared size. */
        compose_region_files(dir, s_stock_w, s_stock_h);
    } else {
        char path[1200];
        asset_path(s_pack_root, rel, path, sizeof path);
        s_pack_ok = load_png_native(path, &s_pack_argb, &s_pack_w, &s_pack_h);
    }
}

/* --- layout ---------------------------------------------------------------- */

static void layout_compute(void)
{
    Layout *L = &s_L;
    const int gap = px(U_GAP), pad = px(U_PAD);
    L->row_h = px(U_ROW_H);
    L->bar = (Rect){ 0, 0, s_w, px(U_BAR_H) };

    int x = s_w - pad;
    const PsxUiFace *fb = face_body();
    for (int b = BTN_COUNT - 1; b >= 0; b--) {
        const int w = tw(fb, BTN_LABEL[b]) + px(16.0f);
        x -= w;
        L->btn[b] = (Rect){ x, (L->bar.h - px(U_BTN_H)) / 2, w, px(U_BTN_H) };
        x -= gap;
    }
    const int sw = px(160.0f);
    L->search = (Rect){ x - sw - gap, (L->bar.h - px(U_BTN_H)) / 2, sw, px(U_BTN_H) };

    const int body_y = L->bar.h + gap, body_h = s_h - body_y - pad;
    const int cat_w = px(U_CAT_W), asset_w = px(U_ASSET_W);
    L->cats   = (Rect){ pad, body_y, cat_w, body_h };
    L->assets = (Rect){ L->cats.x + cat_w + gap, body_y, asset_w, body_h };
    L->detail = (Rect){ L->assets.x + asset_w + gap, body_y,
                        s_w - pad - (L->assets.x + asset_w + gap), body_h };

    const int prev_h = px(U_PREVIEW_H);
    const int half = (L->detail.w - gap) / 2;
    L->prev_stock = (Rect){ L->detail.x, L->detail.y + px(U_HDR_H), half, prev_h };
    L->prev_pack  = (Rect){ L->detail.x + half + gap, L->detail.y + px(U_HDR_H), half, prev_h };
    L->region_list = (Rect){ L->detail.x, L->prev_stock.y + prev_h + gap + px(U_HDR_H),
                             L->detail.w, L->detail.h - (px(U_HDR_H) + prev_h + gap + px(U_HDR_H)) - px(U_FOOT_H) - gap };
}

static int cat_rows(void) { const int n = s_L.cats.h / (s_L.row_h > 0 ? s_L.row_h : 1); return n > 0 ? n : 1; }
static int asset_rows(void) { const int n = s_L.assets.h / (s_L.row_h > 0 ? s_L.row_h : 1); return n > 0 ? n : 1; }

static int cat_at(int x, int y)
{
    if (!in_rect(&s_L.cats, x, y)) return -1;
    const int hdr = px(U_HDR_H);
    if (y < s_L.cats.y + hdr) return -1;
    const int r = s_cat_scroll + (y - (s_L.cats.y + hdr)) / s_L.row_h;
    return r < s_cat_n ? r : -1;
}

static int filtered_asset_row(int i)
{
    /* Maps a VISIBLE row to an asset index within the selected category,
     * honouring the search box. Scans every asset checking e->cat rather
     * than a (start, count) range -- see AssetEntry.cat for why a range
     * stopped being able to answer "is this in category X" once a second
     * pass (expand_interleaved_bg) could add to a category an earlier pass
     * already created. Linear, but ASSET_MAX tops out in the low thousands
     * and this only runs on input, not per frame. */
    if (s_cat_sel < 0) return -1;
    int seen = 0;
    for (int k = 0; k < s_asset_n; k++) {
        const AssetEntry *e = &s_assets[k];
        if (e->cat != s_cat_sel) continue;
        if (s_search[0]) {
            char lo[48], needle[48], *p;
            snprintf(lo, sizeof lo, "%s", e->name);
            snprintf(needle, sizeof needle, "%s", s_search);
            for (p = lo; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            for (p = needle; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            if (!strstr(lo, needle)) continue;
        }
        if (seen == i) return k;
        seen++;
    }
    return -1;
}

static int filtered_asset_count(void)
{
    int n = 0;
    while (filtered_asset_row(n) >= 0) n++;
    return n;
}

static int asset_at(int x, int y)
{
    if (!in_rect(&s_L.assets, x, y)) return -1;
    const int hdr = px(U_HDR_H);
    if (y < s_L.assets.y + hdr) return -1;
    const int r = s_asset_scroll + (y - (s_L.assets.y + hdr)) / s_L.row_h;
    return filtered_asset_row(r);
}

static int button_at(int x, int y)
{
    for (int b = 0; b < BTN_COUNT; b++) if (in_rect(&s_L.btn[b], x, y)) return b;
    return -1;
}

static void set_cat_scroll(int v)
{
    const int max = s_cat_n - cat_rows();
    if (v > max) v = max > 0 ? max : 0;
    if (v < 0) v = 0;
    if (v != s_cat_scroll) { s_cat_scroll = v; s_dirty = 1; }
}
static void set_asset_scroll(int v)
{
    const int max = filtered_asset_count() - asset_rows();
    if (v > max) v = max > 0 ? max : 0;
    if (v < 0) v = 0;
    if (v != s_asset_scroll) { s_asset_scroll = v; s_dirty = 1; }
}

/* --- drawing ---------------------------------------------------------------- */

static void draw_button(const Rect *r, const char *label, int hover, int enabled)
{
    psx_ui_round_rect(&s_cv, r->x, r->y, r->w, r->h, U_R_BOX * s_u,
                      !enabled ? COL_BTN_OFF : (hover ? COL_BTN_ON : COL_BTN));
    text_centered(r, label, enabled ? COL_TEXT : COL_DIM, face_body());
}

/* set by on_event/tick before draw(), read by draw_bar's hover highlight --
 * a plain global instead of threading one more parameter through, since
 * nothing else needs it. */
static int s_hover_btn_cache = -1;

static void draw_bar(void)
{
    const Layout *L = &s_L;
    psx_ui_fill(&s_cv, L->bar.x, L->bar.y, L->bar.w, L->bar.h, COL_BAR);
    char title[256];
    snprintf(title, sizeof title, "Textures   %s", s_pack_root[0] ? s_pack_root : "no pack folder chosen yet");
    Rect t = { px(U_PAD), L->bar.y, L->search.x - px(U_PAD) - px(U_GAP), L->bar.h };
    text_in(&t, 0, title, s_pack_root[0] ? COL_TEXT : COL_WARN, face_title());

    psx_ui_round_rect(&s_cv, L->search.x, L->search.y, L->search.w, L->search.h, U_R_BOX * s_u, COL_EDIT_BG);
    if (s_search[0]) {
        char shown[64]; snprintf(shown, sizeof shown, "%s%s", s_search, s_caret_on ? "|" : "");
        text_in(&L->search, px(5.0f), shown, COL_TEXT, face_body());
    } else text_in(&L->search, px(5.0f), "Filter assets", COL_DIM, face_body());

    const int has_asset = s_asset_sel >= 0 && !s_assets[s_asset_sel].multi_region;
    for (int b = 0; b < BTN_COUNT; b++) {
        int enabled;
        if (b == BTN_FOLDER) enabled = 1;
        else if (b == BTN_UPLOAD) enabled = s_pack_root[0] && has_asset;
        else if (b == BTN_OPEN_FOLDER) enabled = s_pack_root[0] && s_asset_sel >= 0;
        else if (b == BTN_EXPORT) enabled = s_asset_sel >= 0 && !s_export_all_active;
        else /* BTN_EXPORT_ALL */ enabled = !s_export_all_active;
        draw_button(&L->btn[b], BTN_LABEL[b], b == s_hover_btn_cache && enabled, enabled);
    }
}

static void draw_cats(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->cats.x, L->cats.y, L->cats.w, L->cats.h, U_R_PANEL * s_u, COL_PANEL);
    Rect hdr = { L->cats.x, L->cats.y, L->cats.w, px(U_HDR_H) };
    text_in(&hdr, px(6.0f), "Categories", COL_DIM, face_small());
    const int rows_y = L->cats.y + px(U_HDR_H);
    const int nrows = cat_rows();
    for (int i = 0; i < nrows && s_cat_scroll + i < s_cat_n; i++) {
        const int row = s_cat_scroll + i;
        const int y = rows_y + i * L->row_h;
        if (row == s_cat_sel) psx_ui_round_rect(&s_cv, L->cats.x, y, L->cats.w, L->row_h, U_R_BOX * s_u, COL_SEL_BG);
        else if (row == s_cat_hover) psx_ui_round_rect(&s_cv, L->cats.x, y, L->cats.w, L->row_h, U_R_BOX * s_u, COL_HOVER);
        Rect r = { L->cats.x, y, L->cats.w, L->row_h };
        char label[96];
        snprintf(label, sizeof label, "%s (%d)", s_cats[row].name, s_cats[row].count);
        text_in(&r, px(6.0f), label, row == s_cat_sel ? COL_TEXT : COL_DIM, face_body());
    }
}

static void draw_assets(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->assets.x, L->assets.y, L->assets.w, L->assets.h, U_R_PANEL * s_u, COL_PANEL);
    Rect hdr = { L->assets.x, L->assets.y, L->assets.w, px(U_HDR_H) };
    if (s_cat_sel < 0) { text_in(&hdr, px(6.0f), "Pick a category", COL_DIM, face_small()); return; }
    char label[64];
    snprintf(label, sizeof label, "%s", s_cats[s_cat_sel].name);
    text_in(&hdr, px(6.0f), label, COL_DIM, face_small());
    const int rows_y = L->assets.y + px(U_HDR_H);
    const int nrows = asset_rows();
    for (int i = 0; i < nrows; i++) {
        const int ai = filtered_asset_row(s_asset_scroll + i);
        if (ai < 0) break;
        const int y = rows_y + i * L->row_h;
        if (ai == s_asset_sel) psx_ui_round_rect(&s_cv, L->assets.x, y, L->assets.w, L->row_h, U_R_BOX * s_u, COL_SEL_BG);
        else if (s_asset_scroll + i == s_asset_hover) psx_ui_round_rect(&s_cv, L->assets.x, y, L->assets.w, L->row_h, U_R_BOX * s_u, COL_HOVER);
        Rect r = { L->assets.x, y, L->assets.w - px(14.0f), L->row_h };
        text_in(&r, px(6.0f), s_assets[ai].name, ai == s_asset_sel ? COL_TEXT : COL_TEXT, face_body());
        /* a small dot: does the pack already have this one? Cheap enough to
         * stat() per visible row (a couple dozen), not per asset. Three
         * states for a multi-region asset, since "the pack has it" and "the
         * exporter has anything to write" are different questions for one of
         * these -- see asset_live_seen_count(): amber means Export stock
         * textures will actually produce something right now, grey-hollow
         * means the screen that draws it still needs a visit first. */
        char path[1200], rel[128];
        struct stat st;
        int have;
        asset_rel_path(&s_assets[ai], rel, sizeof rel);
        if (s_assets[ai].multi_region) {
            char dir[1200];
            asset_dir(s_pack_root[0] ? s_pack_root : ".", rel, dir, sizeof dir);
            have = s_pack_root[0] && stat(dir, &st) == 0;
        } else {
            asset_path(s_pack_root[0] ? s_pack_root : ".", rel, path, sizeof path);
            have = s_pack_root[0] && stat(path, &st) == 0;
        }
        uint32_t dot_col = have ? COL_HAVE : COL_MISS;
        int ready = have;
        if (!have && s_assets[ai].multi_region && asset_live_seen_count(&s_assets[ai]) > 0)
            { dot_col = COL_WARN; ready = 1; }
        Rect dot = { L->assets.x + L->assets.w - px(12.0f), y, px(8.0f), L->row_h };
        text_centered(&dot, ready ? "\xE2\x97\x8F" : "\xE2\x97\x8B", dot_col, face_small());
    }
    if (filtered_asset_count() > nrows) {
        Rect sb = { L->assets.x + L->assets.w - px(U_SB_W), rows_y, px(U_SB_W), L->assets.h - px(U_HDR_H) };
        psx_ui_round_rect(&s_cv, sb.x, sb.y, sb.w, sb.h, U_SB_W * s_u, COL_TRACK);
        const int total = filtered_asset_count();
        const int th = sb.h * nrows / total;
        const int ty = sb.y + (sb.h - th) * s_asset_scroll / (total - nrows);
        psx_ui_round_rect(&s_cv, sb.x, ty, sb.w, th > px(8.0f) ? th : px(8.0f), U_SB_W * s_u, COL_THUMB);
    }
}

/* Fit src into box, preserving aspect ratio, centred -- a 320x160 background
 * and a 12x16 icon both land readably instead of one being squashed and the
 * other lost in a corner. */
static void fit_rect(const Rect *box, int sw, int sh, Rect *out)
{
    if (sw <= 0 || sh <= 0) { *out = *box; return; }
    const float sx = (float)box->w / (float)sw, sy = (float)box->h / (float)sh;
    const float s = sx < sy ? sx : sy;
    out->w = (int)(sw * s); out->h = (int)(sh * s);
    out->x = box->x + (box->w - out->w) / 2;
    out->y = box->y + (box->h - out->h) / 2;
}

/* Empty-state message in a caller-chosen colour -- used to make "ready to
 * export" (COL_WARN) visually distinct from "nothing here yet" (COL_DIM)
 * for a multi-region asset's pack side, see draw_detail(). */
static void draw_preview_box_col(const Rect *box, const char *caption,
                                 int ok, const uint32_t *argb, int sw, int sh,
                                 const char *empty_msg, uint32_t empty_col)
{
    psx_ui_round_rect(&s_cv, box->x, box->y - px(U_HDR_H), box->w, box->h + px(U_HDR_H), U_R_BOX * s_u, COL_WELL);
    Rect cap = { box->x, box->y - px(U_HDR_H), box->w, px(U_HDR_H) };
    text_in(&cap, px(4.0f), caption, COL_DIM, face_small());
    if (ok && argb) {
        Rect fit; fit_rect(box, sw, sh, &fit);
        psx_ui_blit_scaled(&s_cv, fit.x, fit.y, fit.w, fit.h, U_R_BOX * s_u, argb, sw, sh);
        char dims[32]; snprintf(dims, sizeof dims, "%dx%d", sw, sh);
        Rect d = { box->x + px(4.0f), box->y + box->h - px(12.0f), box->w - px(8.0f), px(11.0f) };
        text_in(&d, 0, dims, COL_DIM, face_small());
    } else {
        Rect m = { box->x, box->y + box->h / 2 - px(6.0f), box->w, px(12.0f) };
        text_centered(&m, empty_msg, empty_col, face_small());
    }
}

static void draw_preview_box(const Rect *box, const char *caption,
                             int ok, const uint32_t *argb, int sw, int sh,
                             const char *empty_msg)
{
    draw_preview_box_col(box, caption, ok, argb, sw, sh, empty_msg, COL_DIM);
}

static void draw_detail(void)
{
    const Layout *L = &s_L;
    psx_ui_round_rect(&s_cv, L->detail.x, L->detail.y, L->detail.w, L->detail.h, U_R_PANEL * s_u, COL_PANEL);
    if (s_asset_sel < 0) {
        Rect t = { L->detail.x, L->detail.y, L->detail.w, px(U_HDR_H) };
        text_in(&t, px(6.0f), "Select an asset to compare it against the pack", COL_DIM, face_body());
        return;
    }
    const AssetEntry *e = &s_assets[s_asset_sel];
    Rect t = { L->detail.x, L->detail.y, L->detail.w, px(U_HDR_H) };
    char head[96];
    snprintf(head, sizeof head, "%s / %s%s", s_cats[s_cat_sel].name, e->name,
            e->multi_region ? "  (multi-region UI art)" : "");
    text_in(&t, px(6.0f), head, COL_TEXT, face_bold());

    draw_preview_box(&L->prev_stock, "Stock (from the disc)",
                     s_stock_ok, stock_ptr(), s_stock_w, s_stock_h,
                     "could not decode");

    if (e->multi_region) {
        /* "Has the game drawn this yet" and "does the pack have files for
         * it" are different questions for a multi-region asset -- the first
         * is what Export stock textures actually reads from (see
         * asset_live_seen_count()), the second is what a previous export
         * left on disk. Both get their own message so the empty case says
         * which one to go do, instead of one flat "nothing here". */
        const int seen = asset_live_seen_count(e);
        char msg[80];
        uint32_t msg_col = COL_DIM;
        if (s_region_n > 0) {
            snprintf(msg, sizeof msg, "%d region file%s exported", s_region_n, s_region_n == 1 ? "" : "s");
        } else if (!s_pack_root[0]) {
            snprintf(msg, sizeof msg, "choose a pack folder first");
        } else if (seen > 0) {
            snprintf(msg, sizeof msg, "%d draw%s seen \xE2\x80\x94 ready: Mods > Export stock textures",
                    seen, seen == 1 ? "" : "s");
            msg_col = COL_WARN;
        } else {
            snprintf(msg, sizeof msg, "not seen yet \xE2\x80\x94 visit the screen that draws this");
        }
        /* Composited from the same per-region files below, laid out at
         * their own destination rects -- how the asset actually looks
         * assembled, not a promise that it is one file underneath. */
        draw_preview_box_col(&L->prev_pack, "Pack (assembled from regions)",
                        s_pack_region_ok, s_pack_region_argb, s_pack_region_w, s_pack_region_h,
                        msg, msg_col);
        Rect rh = { L->region_list.x, L->region_list.y - px(U_HDR_H), L->region_list.w, px(U_HDR_H) };
        text_in(&rh, px(6.0f), "This asset is drawn through more than one palette, so the game and the "
                              "exporter capture it as several small files, assembled above for review -- "
                              "editing means replacing the region file(s) below, not one whole picture. "
                              "Visit the screen that draws it, then Mods > Export stock textures; the "
                              "files appear once it has been.", COL_DIM, face_small());
        for (int i = 0; i < s_region_n; i++) {
            Rect r = { L->region_list.x, L->region_list.y + i * L->row_h, L->region_list.w, L->row_h };
            if (r.y + L->row_h > L->region_list.y + L->region_list.h) break;
            text_in(&r, px(6.0f), s_region_files[i], COL_DIM, face_small());
        }
    } else {
        draw_preview_box(&L->prev_pack, "Pack (replaces this)", s_pack_ok, s_pack_argb, s_pack_w, s_pack_h,
                        s_pack_root[0] ? "not in the pack yet -- Upload one" : "choose a pack folder first");
    }
}

static void draw_footer(void)
{
    const Layout *L = &s_L;
    const char *m = s_msg[0] ? s_msg :
        "Click a category, then an asset. Green \xE2\x97\x8F: the pack has it. Amber \xE2\x97\x8F: seen in-game, ready to export. Upload replaces the file directly.";
    Rect r = { px(U_PAD), s_h - px(U_FOOT_H), s_w - px(U_PAD) * 2, px(U_FOOT_H) };
    text_in(&r, 0, m, s_msg[0] ? COL_WARN : COL_DIM, face_small());
}

static void draw(void)
{
    s_cv.px = s_px; s_cv.w = s_w; s_cv.h = s_h;
    layout_compute();
    psx_ui_fill(&s_cv, 0, 0, s_w, s_h, COL_BG);
    draw_bar();
    draw_cats();
    draw_assets();
    draw_detail();
    draw_footer();
}

/* --- actions ---------------------------------------------------------------- */

#if defined(PSX_SDL3)
static void SDLCALL pick_cb(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    if (!filelist || !filelist[0]) return;
    snprintf(s_pick_path, sizeof s_pick_path, "%s", filelist[0]);
    s_pick_kind = (int)(intptr_t)userdata;
}
#endif

static void do_choose_folder(void)
{
#if defined(PSX_SDL3)
    const char *start = s_pack_root[0] ? s_pack_root : psx_mod_player_data_dir();
    SDL_ShowOpenFolderDialog(pick_cb, (void *)(intptr_t)1, s_win, start, false);
#else
    say("No folder dialog in this build: the active texture pack is used automatically");
#endif
}

/* Writes an uploaded replacement into the pack, correcting it first when the
 * target asset is TINTED (currently just the font -- see psx_wa_catalog_tinted
 * and export_one()'s own tinted branch in psx_texture_export.c). Tinted art is
 * a white mask on transparency that the shader recolours per draw; an
 * upscaler that flattens the file onto a background destroys that alpha and
 * produces something that looks right in a viewer but draws as solid dark
 * blocks in-game. This is exactly the transform tools/font_mask.py applies by
 * hand (see its own header for the mechanism) -- luminance becomes coverage,
 * RGB is forced white, and an input that still carries real (non-opaque)
 * alpha at a pixel is left alone there, so running this on an already-correct
 * mask is a no-op. Every OTHER asset is written byte-for-byte as uploaded:
 * for ordinary art a dark pixel means dark, not absent, and "correcting" it
 * the same way would delete every shadow in it -- font_mask.py's own SCOPE
 * note says exactly this. Returns 1 on success. */
/* 1: written. 0: could not read/decode the source file. -1: decoded (or read)
 * fine but the write into the pack folder failed. Kept distinct so the
 * caller can report the same two messages this always gave, tinted or not. */
static int save_upload(const char *src_path, const char *dst_path, int tinted)
{
    if (!tinted) {
        long sz;
        unsigned char *bytes = read_file(src_path, &sz);
        if (!bytes) return 0;
        FILE *f = fopen(dst_path, "wb");
        const int ok = f && fwrite(bytes, 1, (size_t)sz, f) == (size_t)sz;
        if (f) fclose(f);
        free(bytes);
        return ok ? 1 : -1;
    }

    long sz;
    unsigned char *file = read_file(src_path, &sz);
    if (!file) return 0;
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 4);
    free(file);
    if (!img) return 0;

    const size_t n = (size_t)w * (size_t)h;
    uint8_t *mask = (uint8_t *)malloc(n * 4u);
    if (!mask) { stbi_image_free(img); return -1; }
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = img + i * 4u;
        const int lum = ((int)p[0] * 299 + (int)p[1] * 587 + (int)p[2] * 114) / 1000;
        const int a = p[3];
        mask[i * 4 + 0] = mask[i * 4 + 1] = mask[i * 4 + 2] = 255;
        mask[i * 4 + 3] = (uint8_t)(a == 255 ? lum : (a < lum ? a : lum));
    }
    stbi_image_free(img);
    const int ok = psx_texture_export_write_png(dst_path, mask, w, h);
    free(mask);
    return ok ? 1 : -1;
}

static void do_upload(void)
{
    if (s_asset_sel < 0 || s_assets[s_asset_sel].multi_region || !s_pack_root[0]) return;
#if defined(PSX_SDL3)
    static const SDL_DialogFileFilter filters[] = { { "PNG images", "png" } };
    SDL_ShowOpenFileDialog(pick_cb, (void *)(intptr_t)2, s_win, filters, 1, psx_mod_player_data_dir(), false);
#else
    say("No file dialog in this build: copy the PNG into the pack folder by hand");
#endif
}

/* Opens the REAL on-disk folder for the selected ASSET, not the curated
 * category it is browsed under -- a category like "UI > duel HUD" can span
 * more than one real directory (ui/, duel_fields/), so "the category's
 * folder" has no single answer once the browsing tree stopped mirroring the
 * disk layout. The asset's own folder always does. */
static void do_open_folder(void)
{
    if (s_asset_sel < 0 || !s_pack_root[0]) return;
    char rel[128], dir[1200];
    asset_rel_path(&s_assets[s_asset_sel], rel, sizeof rel);
    char *slash = strrchr(rel, '/');
    if (slash) *slash = 0;   /* rel's own containing folder, not its leaf */
    snprintf(dir, sizeof dir, "%s/%s", s_pack_root, rel);
    mkdir_p(dir);
#if defined(PSX_SDL3)
    SDL_OpenURL(dir);
#endif
}

/* Exports ONE catalog-driven asset to dest_root via psx_texture_export_one()
 * (psx_texture_export.c) -- the SAME logic the old automatic "Mods > Export
 * stock textures" walker used, just called on demand with a caller-chosen
 * destination instead of a background frame-hook always targeting the
 * active pack. That single function already handles every asset shape
 * correctly (multi-region live-captured elements for genuinely multi-palette
 * UI sheets, tinted white masks, plain disc decode, reference-PNG
 * fallbacks) -- reimplementing any of that here would either drift from it
 * or leave multi-region sheets (duel_labels and the like) permanently
 * unexportable, since this window has no other path to their live-captured
 * per-region data. */
static int export_stock_png(const AssetEntry *e, const char *dest_root)
{
    int rows = 0;
    const PsxWaAsset *tbl = psx_wa_catalog_table(&rows);
    if (e->row < 0 || e->row >= rows) return 0;
    return psx_texture_export_one(&tbl[e->row], e->index, dest_root);
}

static void do_export_one(void)
{
    if (s_asset_sel < 0 || s_export_all_active) return;
#if defined(PSX_SDL3)
    SDL_ShowOpenFolderDialog(pick_cb, (void *)(intptr_t)3, s_win, psx_mod_player_data_dir(), false);
#else
    say("No folder dialog in this build");
#endif
}

static void do_export_all(void)
{
    if (s_export_all_active) return;
#if defined(PSX_SDL3)
    SDL_ShowOpenFolderDialog(pick_cb, (void *)(intptr_t)4, s_win, psx_mod_player_data_dir(), false);
#else
    say("No folder dialog in this build");
#endif
}

static void run_button(int b)
{
    if (b == BTN_FOLDER) do_choose_folder();
    else if (b == BTN_UPLOAD) do_upload();
    else if (b == BTN_OPEN_FOLDER) do_open_folder();
    else if (b == BTN_EXPORT) do_export_one();
    else if (b == BTN_EXPORT_ALL) do_export_all();
}

static void select_cat(int i)
{
    if (i < 0 || i >= s_cat_n || i == s_cat_sel) return;
    s_cat_sel = i; s_asset_sel = -1; s_asset_scroll = 0; s_search[0] = 0;
    s_dirty = 1;
}
static void select_asset(int i)
{
    if (i < 0) return;
    s_asset_sel = i;
    refresh_preview();
    s_dirty = 1;
}

static void click(int x, int y)
{
    const int b = button_at(x, y);
    if (b >= 0) { run_button(b); return; }
    const int c = cat_at(x, y);
    if (c >= 0) { select_cat(c); return; }
    const int a = asset_at(x, y);
    if (a >= 0) { select_asset(a); return; }
}

/* --- window lifecycle, event pump: identical shape to the sibling tool
 * windows (psx_dialogue_manager.c, psx_card_manager.c) -- see there for why
 * each piece is the way it is (renderer fallback, GL context save/restore,
 * canvas-size-follows-window). Not re-explained here. */

static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static SDL_Window   *s_gl_win;
static SDL_GLContext s_gl_ctx;
static int           s_ren_software;
static int           s_present_fail;

static void gl_capture(void) { s_gl_win = SDL_GL_GetCurrentWindow(); s_gl_ctx = SDL_GL_GetCurrentContext(); }
static void gl_restore(void)
{
    if (s_ren_software) return;
    if (s_gl_ctx && s_gl_win && SDL_GL_GetCurrentContext() != s_gl_ctx) SDL_GL_MakeCurrent(s_gl_win, s_gl_ctx);
}

static int ensure_canvas(int w, int h)
{
    if (w == s_w && h == s_h && s_px && s_tex) return 1;
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    free(s_px);
    s_px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!s_px) { s_w = s_h = 0; gl_restore(); return 0; }
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    gl_restore();
    if (!s_tex) { free(s_px); s_px = NULL; s_w = s_h = 0; return 0; }
    s_w = w; s_h = h;
    s_u = (float)h / 480.0f;
    if (s_u < 1.0f) s_u = 1.0f;
    if (s_u > 8.0f) s_u = 8.0f;
    s_dirty = 1;
    return 1;
}

static void present_canvas(void)
{
    if (!s_ren || !s_tex) return;
    const int ok = psx_tool_present(s_ren, s_tex, s_px, s_w, s_h, "Textures");
    gl_restore();
    if (ok) { s_present_fail = 0; return; }
    if (++s_present_fail < 3) { s_dirty = 1; return; }
    psx_tool_log("Textures: switching to the %s renderer after %d failed presents", s_ren_software ? "accelerated" : "software", s_present_fail);
    gl_capture();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    SDL_DestroyRenderer(s_ren);
    s_ren = psx_tool_renderer_create(s_win, "Textures", s_ren_software ? 1 : 0, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) { psx_asset_manager_close(); return; }
    const int w = s_w, h = s_h; s_w = s_h = 0;
    if (!ensure_canvas(w, h)) { psx_asset_manager_close(); return; }
    s_dirty = 1;
}

void psx_asset_manager_open(void)
{
    if (s_win) { SDL_RaiseWindow(s_win); return; }
    build_catalog();
    /* Always the active pack's own folder -- never left empty waiting for a
     * pack to happen to already be active, which is what let this window and
     * the folder card art/CPU portraits actually read from drift apart. Also
     * arms it as the runtime's ACTUAL active pack (texture_pack.c no longer
     * does this by itself at boot -- see discover_packs()'s comment on why),
     * so opening this window is what turns raw VRAM replacement on for it,
     * not something every launch pays a directory scan for regardless of
     * whether this window is ever opened. */
    if (!s_pack_root[0]) {
        texpack_active_dir(s_pack_root, (unsigned)sizeof s_pack_root);
        texpack_set_active_dir(s_pack_root);
    }
    s_win = psx_fm_editor_acquire(PSX_FM_PAGE_TEXTURES, WIN_W, WIN_H);
    if (!s_win) { host_osd_push("Textures: no window", 2000); return; }
    gl_capture();
    s_ren = psx_tool_renderer_create(s_win, "Textures", -1, &s_ren_software);
    gl_restore();
    s_present_fail = 0;
    if (!s_ren) {
        psx_fm_editor_release(PSX_FM_PAGE_TEXTURES); s_win = NULL;
        host_osd_push("Textures: no renderer", 2000);
        return;
    }
    if (!ensure_canvas(WIN_W, WIN_H)) { psx_asset_manager_close(); return; }
    layout_compute();
    SDL_StartTextInput(s_win);
}

void psx_asset_manager_close(void)
{
    const int preserve = psx_fm_editor_is_switching();
    if (s_tex) { SDL_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { psx_fm_editor_release(PSX_FM_PAGE_TEXTURES); s_win = NULL; }
    gl_restore();
    s_ren_software = 0;
    free(s_px); s_px = NULL;
    s_w = s_h = 0;
    if (preserve) return;
    s_cat_hover = s_asset_hover = -1;
    free_pack_preview();
    s_preview_asset = -1;
}

int psx_asset_manager_is_open(void) { return s_win != NULL; }

static int on_event(const void *evp)
{
    const SDL_Event *raw = (const SDL_Event *)evp;
    SDL_Event adjusted;
    if (!s_win) return 0;
    if (psx_fm_editor_filter_event(PSX_FM_PAGE_TEXTURES, raw, &adjusted)) return 1;
    const SDL_Event *ev = &adjusted;
    const Uint32 id = SDL_GetWindowID(s_win);
    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN: {
        if (ev->button.windowID != id) return 0;
        if (ev->button.button != SDL_BUTTON_LEFT) return 1;
        layout_compute();
        click((int)ev->button.x, (int)ev->button.y);
        return 1;
    }
    case SDL_MOUSEBUTTONUP:
        return ev->button.windowID == id;
    case SDL_MOUSEMOTION: {
        if (ev->motion.windowID != id) return 0;
        layout_compute();
        const int c = cat_at((int)ev->motion.x, (int)ev->motion.y);
        const int a = asset_at((int)ev->motion.x, (int)ev->motion.y);
        const int b = button_at((int)ev->motion.x, (int)ev->motion.y);
        /* s_asset_hover, like draw_assets()'s hover check, is a filtered
         * VISIBLE-ROW rank (what filtered_asset_row's own index means),
         * not an absolute s_assets[] index -- so translate asset_at()'s
         * absolute index back to that rank the same way filtered_asset_row
         * produces it, rather than assuming a category is a contiguous
         * range (it isn't; see AssetEntry.cat). */
        int ar = -1;
        if (a >= 0 && s_cat_sel >= 0) {
            for (int k = 0; ; k++) {
                const int idx = filtered_asset_row(k);
                if (idx < 0) break;
                if (idx == a) { ar = k; break; }
            }
        }
        if (c != s_cat_hover || ar != s_asset_hover || b != s_hover_btn_cache) {
            s_cat_hover = c; s_asset_hover = ar; s_hover_btn_cache = b; s_dirty = 1;
        }
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        if (ev->wheel.windowID != id) return 0;
        int mx, my;
#if defined(PSX_SDL3)
        mx = (int)ev->wheel.mouse_x; my = (int)ev->wheel.mouse_y;
#else
        SDL_GetMouseState(&mx, &my);
#endif
        layout_compute();
        if (in_rect(&s_L.cats, mx, my)) set_cat_scroll(s_cat_scroll + (ev->wheel.y > 0 ? -3 : 3));
        else set_asset_scroll(s_asset_scroll + (ev->wheel.y > 0 ? -3 : 3));
        return 1;
    }
    case SDL_KEYDOWN: {
        if (ev->key.windowID != id) return 0;
#if defined(PSX_SDL3)
        const int key = (int)ev->key.key;
#else
        const int key = (int)ev->key.keysym.sym;
#endif
        if (key == SDLK_ESCAPE) {
            if (s_search[0]) { s_search[0] = 0; s_asset_scroll = 0; }
            else psx_asset_manager_close();
        } else if (key == SDLK_BACKSPACE) {
            const size_t n = strlen(s_search);
            if (n) { s_search[n - 1] = 0; s_asset_scroll = 0; }
        } else if (key == SDLK_UP) set_asset_scroll(s_asset_scroll - 1);
        else if (key == SDLK_DOWN) set_asset_scroll(s_asset_scroll + 1);
        s_dirty = 1;
        return 1;
    }
    case SDL_TEXTINPUT:
        if (ev->text.windowID != id) return 0;
        for (const char *p = ev->text.text; *p; p++) {
            if ((unsigned char)*p >= 32u && (unsigned char)*p < 127u) {
                const size_t n = strlen(s_search);
                if (n + 1 < sizeof s_search) { s_search[n] = *p; s_search[n + 1] = 0; }
            }
        }
        s_asset_scroll = 0;
        s_dirty = 1;
        return 1;
    case SDL_WINDOWEVENT_CLOSE:
        if (ev->window.windowID != id) return 0;
        psx_asset_manager_close();
        return 1;
    case SDL_WINDOWEVENT_EXPOSED:
    case SDL_WINDOWEVENT_RESIZED:
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (ev->window.windowID != id) return 0;
        s_dirty = 1;
        return 1;
    default:
        break;
    }
    return 0;
}

static void tick(void)
{
    const int req = s_open_req;
    if (req) {
        s_open_req = 0;
        if (req > 0) psx_asset_manager_open(); else psx_asset_manager_close();
    }
    if (s_pick_kind) {
        const int kind = s_pick_kind; s_pick_kind = 0;
        if (kind == 1) {
            snprintf(s_pack_root, sizeof s_pack_root, "%s", s_pick_path);
            /* Make it the ACTIVE pack, not just what this window happens to
             * be looking at: the raw VRAM injector, the exporter, card art
             * and CPU portraits all resolve "the pack folder" through this
             * same call, so picking a folder here is what makes it real
             * everywhere else too. */
            texpack_set_active_dir(s_pack_root);
            char msg[1200]; snprintf(msg, sizeof msg, "Pack folder: %s", s_pack_root);
            say(msg);
        } else if (kind == 2 && s_asset_sel >= 0 && !s_assets[s_asset_sel].multi_region) {
            AssetEntry *e = &s_assets[s_asset_sel];
            int rows = 0;
            const PsxWaAsset *tbl = psx_wa_catalog_table(&rows);
            const int tinted = psx_wa_catalog_tinted(&tbl[e->row]);

            char path[1200], rel[128];
            asset_rel_path(e, rel, sizeof rel);
            asset_path(s_pack_root, rel, path, sizeof path);
            char dir[1200]; snprintf(dir, sizeof dir, "%s", path);
            char *slash = strrchr(dir, '/'); if (slash) *slash = 0;
            mkdir_p(dir);

            const int r = save_upload(s_pick_path, path, tinted);
            if (r > 0) {
                char msg[1300];
                snprintf(msg, sizeof msg, "Saved to %s%s", path,
                         tinted ? " (alpha rebuilt from luminance)" : "");
                say(msg);
                s_preview_asset = -1;   /* force a reread */
                refresh_preview();
            } else if (r < 0) {
                say("Could not write into the pack folder");
            } else {
                say(tinted ? "Could not read that file as an image"
                           : "Could not read that file");
            }
        } else if (kind == 3 && s_asset_sel >= 0) {
            char path[1200], rel[128];
            asset_rel_path(&s_assets[s_asset_sel], rel, sizeof rel);
            asset_path(s_pick_path, rel, path, sizeof path);
            char msg[1300];
            if (export_stock_png(&s_assets[s_asset_sel], s_pick_path))
                snprintf(msg, sizeof msg, "Exported stock image to %s", path);
            else
                snprintf(msg, sizeof msg, "Could not export -- no disc image for this asset");
            say(msg);
        } else if (kind == 4) {
            snprintf(s_export_all_root, sizeof s_export_all_root, "%s", s_pick_path);
            s_export_all_active = 1;
            s_export_all_idx = 0;
            s_export_all_written = 0;
            s_export_all_failed = 0;
        }
    }
    /* Export All: a handful of stock decodes per tick, not all ~2700 in one
     * button click -- see EXPORT_ALL_PER_TICK's own declaration for why. */
    if (s_export_all_active) {
        int done = 0;
        while (s_export_all_active && done < EXPORT_ALL_PER_TICK) {
            if (s_export_all_idx >= s_asset_n) {
                s_export_all_active = 0;
                char msg[256];
                snprintf(msg, sizeof msg, "Export all done: %d written, %d skipped, into %s",
                         s_export_all_written, s_export_all_failed, s_export_all_root);
                say(msg);
                break;
            }
            const AssetEntry *e = &s_assets[s_export_all_idx++];
            if (export_stock_png(e, s_export_all_root)) s_export_all_written++;
            else s_export_all_failed++;
            done++;
        }
        if (s_export_all_active) {
            char msg[256];
            snprintf(msg, sizeof msg, "Exporting all... %d/%d (%d written)",
                     s_export_all_idx, s_asset_n, s_export_all_written);
            say(msg);
        }
        s_dirty = 1;
    }
    if (!s_win) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    h = psx_fm_editor_content_height(h);
    if (w > 0 && h > 0 && (w != s_w || h != s_h)) { if (!ensure_canvas(w, h)) { psx_asset_manager_close(); return; } }
    {
        const int on = ((SDL_GetTicks() / 530u) & 1u) == 0u;
        if (on != s_caret_on) { s_caret_on = on; if (s_search[0]) s_dirty = 1; }
    }
    if (s_msg[0] && SDL_GetTicks() >= s_msg_until) { s_msg[0] = 0; s_dirty = 1; }
    if (s_dirty) { draw(); s_dirty = 0; present_canvas(); }
}

/* --- the debug side ---------------------------------------------------------
 *
 * No View-menu row of its own: this page is reached through the single "FM
 * Editor" row (psx_tool_window.c) and its tab strip, same as Cards, Drop
 * Tables, Fusions, Dialogue and CPU -- a second, separate "Asset manager" row
 * used to open a plain duplicate of this same window and had no reason to. */

void psx_asset_manager_request_open(int open) { s_open_req = open ? 1 : -1; }

static int inject_button(int x, int y, int button, int down)
{
    SDL_Event ev;
    if (!s_win) return 0;
    SDL_zero(ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = SDL_GetWindowID(s_win);
    ev.button.button = (Uint8)button;
#if defined(PSX_SDL3)
    ev.button.down = down ? true : false;
#else
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
    ev.button.clicks = 1;
    ev.button.x = x; ev.button.y = y;
    return SDL_PushEvent(&ev) == 1;
}

int psx_asset_manager_click(int x, int y, int button)
{
    if (!s_win) return 0;
    if (button <= 0) button = SDL_BUTTON_LEFT;
    if (!inject_button(x, y, button, 1)) return 0;
    return inject_button(x, y, button, 0);
}

int psx_asset_manager_shot(const char *path)
{
    if (!s_win || !s_px || !path) return 0;
    if (s_dirty) { draw(); s_dirty = 0; }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
    for (int i = 0; i < s_w * s_h; i++) {
        const uint32_t c = s_px[i];
        const unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

int psx_asset_manager_state_json(char *out, unsigned cap)
{
    build_catalog();
    return (unsigned)snprintf(out, cap,
        "\"open\":%d,\"categories\":%d,\"assets\":%d,\"cat_sel\":%d,\"asset_sel\":%d,"
        "\"pack_root\":\"%s\",\"canvas\":[%d,%d],\"unit\":%.3f",
        s_win != NULL, s_cat_n, s_asset_n, s_cat_sel, s_asset_sel, s_pack_root, s_w, s_h, s_u) < cap;
}

PSX_MOD_CONSTRUCTOR(psx_asset_manager_install)
{
    (void)psx_game_add_frame_hook(tick);
    (void)psx_game_add_event_hook(on_event);
}
