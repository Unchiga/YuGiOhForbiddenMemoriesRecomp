/* Completion feedback on the FREE DUEL opponent grid.
 *
 * The fraction is deliberately OWNED / OBTAINABLE, not a history of which
 * opponent awarded which cards: Forbidden Memories keeps no acquisition
 * provenance. "Obtainable" is the unique union of every non-zero card in the
 * selected CPU's three effective rank bands (S/A POW, B/C/D, S/A TEC), after
 * Drop Missing Cards and Drop Table Manager edits. "Owned" intersects that
 * union with the live deck + trunk snapshot. Thus five owned out of twenty
 * possible correctly reads 5/20 even if twenty distinct cards have previously
 * appeared in duel results.
 *
 * An empty effective union is 0/0 and is NEVER complete: vacuous truth must
 * not award a completion frame.
 *
 * The project owner's eight authored 48x48 frames sit over every completed
 * visible CPU portrait. The grid itself is five columns by three visible
 * rows; cell 0 is Build Deck and cells 1..39 map directly to drop-db duelists
 * 0..38. We follow the selected global row to mirror the stock scroll window.
 * Everything is present-only and disabled for netplay: no local inventory or
 * per-machine drop edit can leak into a shared session's presentation.
 */

#include "psx_free_duel_completion.h"

#include <stdio.h>
#include <string.h>

#include "host_osd.h"
#include "mod_plugins.h"
#include "psx_card_inventory.h"
#include "psx_cpu_data.h"
#include "psx_debug_commands.h"
#include "psx_drop_db.h"
#include "psx_drop_edits.h"
#include "psx_drop_missing.h"
#include "psx_free_duel_completion_art.h"
#include "psx_fusion_font.h"
#include "psx_game_hooks.h"
#include "psx_guest_overlay.h"
#include "psx_video_menu.h"
#include "psx_ygo_netplay.h"

#define MODE_ADDR       0x8009B26Cu
#define FREE_DUEL_MODE  0xC6u
#define CURSOR_COL_ADDR 0x8009B36Cu
#define CURSOR_ROW_ADDR 0x8009B36Du
#define SCROLL_Y_ADDR   0x8009B148u
#define AVAIL_ADDR      0x80169030u

#define SCREEN_W 320
#define SCREEN_H 240
#define GRID_COLS 5
#define GRID_ROWS 8
#define VIEW_ROWS 3
#define BORDER_W 48
#define BORDER_H 48

/* Measured against the stock 320x240 grid. The portrait itself starts at
 * (24,45) and is 38x38; the supplied 48x48 frame is centred around it. */
#define BORDER_X0 19
#define BORDER_Y0 40
#define BORDER_DY 52

#define TEXT_RIGHT 297
#define TEXT_Y 17

static uint32_t s_canvas[SCREEN_W * SCREEN_H];
static uint32_t s_border[BORDER_W * BORDER_H];
static uint16_t s_owned[PSX_DROP_DB_DUELISTS];
static uint16_t s_obtainable[PSX_DROP_DB_DUELISTS];
static uint8_t s_complete[PSX_DROP_DB_DUELISTS];
static PsxCardInventorySnapshot s_inventory;
static int s_inventory_ready;
static int s_counts_ready;
static int s_screen;
static int s_visible;
static int s_selected = -1;
static int s_cursor_col = -1, s_cursor_row = -1;
static int s_top_row;
static int s_scroll_y;
static int s_frame;
static int s_anim_clock;
static int s_present_hold;
static int s_dirty = 1;
static int s_completed_total;
static int s_border_count;
static uint64_t s_border_mask;
static int s_last_border_x = -1, s_last_border_y = -1;
static int s_placement[10];
static unsigned s_edit_gen;
static unsigned s_cpu_gen;
static int s_missing_on = -1;
static int s_enabled;
static int s_menu_row = -1;

static int border_x(int col)
{
    /* The real horizontal pitch is 56.25 pixels. */
    return BORDER_X0 + (col * 225 + 2) / 4;
}

static void seed_tier(int duelist, int tier, uint16_t *weights)
{
    memset(weights, 0, PSX_DROP_DB_CARDS * sizeof *weights);
    const PsxDropDbDuelist *db = &PSX_DROP_DB[duelist];
    for (int i = 0; i < db->count[tier]; i++) {
        const PsxDropWeight *e = &db->tier[tier][i];
        if (e->card >= 1 && e->card <= PSX_DROP_DB_CARDS)
            weights[e->card - 1] = e->weight;
    }
    if (psx_drop_missing_enabled())
        (void)psx_drop_missing_transform(duelist, tier, weights);
    /* Negative returns leave the input intact, including -5 for an editor's
     * pending empty replacement. That state is not safe to apply to a duel,
     * so the live table (and this completion view) retains stock/mod weights. */
    (void)psx_drop_edits_apply(duelist, tier, weights);
}

static void rebuild_counts(const PsxCardInventorySnapshot *inv)
{
    uint16_t weights[PSX_DROP_DB_CARDS];
    uint8_t union_cards[PSX_DROP_DB_CARDS];
    s_completed_total = 0;
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++) {
        memset(union_cards, 0, sizeof union_cards);
        for (int tier = 0; tier < PSX_DROP_DB_TIERS; tier++) {
            seed_tier(d, tier, weights);
            for (int c = 0; c < PSX_DROP_DB_CARDS; c++)
                if (weights[c]) union_cards[c] = 1;
        }
        int owned = 0, obtainable = 0;
        for (int c = 0; c < PSX_DROP_DB_CARDS; c++) {
            if (!union_cards[c]) continue;
            obtainable++;
            if (inv->deck[c] || inv->trunk[c]) owned++;
        }
        s_owned[d] = (uint16_t)owned;
        s_obtainable[d] = (uint16_t)obtainable;
        s_complete[d] = obtainable > 0 && owned == obtainable;
        s_completed_total += s_complete[d];
    }
    s_counts_ready = 1;
    s_dirty = 1;
    s_present_hold = 3;
}

static int glyph_bounds(int cell, int *lo, int *hi)
{
    if (!psx_fusion_font.px || cell < 0 ||
        cell >= PSX_FUSION_FONT_CELLS) return 0;
    const uint8_t *g = psx_fusion_font.px +
        (size_t)cell * (size_t)psx_fusion_font.w * (size_t)psx_fusion_font.h;
    int l = psx_fusion_font.w, h = -1;
    for (int y = 0; y < psx_fusion_font.h; y++)
        for (int x = 0; x < psx_fusion_font.w; x++)
            if (g[y * psx_fusion_font.w + x]) {
                if (x < l) l = x;
                if (x > h) h = x;
            }
    if (h < 0) return 0;
    if (lo) *lo = l;
    if (hi) *hi = h;
    return h - l + 1;
}

static int text_width(const char *text)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == ' ') { w += 4; continue; }
        const int cell = psx_fusion_font_cell(*p);
        const int gw = glyph_bounds(cell, NULL, NULL);
        w += (gw ? gw : 5) + 1;
    }
    return w ? w - 1 : 0;
}

static void put_pixel(int x, int y, uint32_t argb)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    s_canvas[y * SCREEN_W + x] = argb;
}

static int put_glyph(int cell, int x, int y)
{
    int lo = 0, hi = -1;
    const int width = glyph_bounds(cell, &lo, &hi);
    if (!width) return 0;
    const uint8_t *g = psx_fusion_font.px +
        (size_t)cell * (size_t)psx_fusion_font.w * (size_t)psx_fusion_font.h;
    for (int row = 0; row < psx_fusion_font.h; row++)
        for (int col = lo; col <= hi; col++) {
            const uint8_t v = g[row * psx_fusion_font.w + col];
            if (!v) continue;
            const uint32_t l = (uint32_t)v * 17u;
            /* Warm gold-white core, while the atlas's low values retain the
             * same dark outline as the game's own title text. */
            put_pixel(x + col - lo, y + row,
                      0xFF000000u | (l << 16) | ((l * 230u / 255u) << 8) |
                      (l * 150u / 255u));
        }
    return width;
}

static void put_text_right(const char *text, int right, int y)
{
    int x = right - text_width(text);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == ' ') { x += 4; continue; }
        const int width = put_glyph(psx_fusion_font_cell(*p), x, y);
        x += (width ? width : 5) + 1;
    }
}

static void blit_border(int x, int y)
{
    for (int row = 0; row < BORDER_H; row++)
        for (int col = 0; col < BORDER_W; col++) {
            const uint32_t p = s_border[row * BORDER_W + col];
            const int py = y + row;
            /* Match the stock grid's clipping window while it scrolls. */
            if ((p >> 24) && py >= BORDER_Y0 && py < 188)
                put_pixel(x + col, py, p);
        }
}

/* The loaded Free Duel overlay rewrites this 40-byte table as its current
 * display/occupancy source. Gate on what the stock screen can draw now; do not
 * interpret it as durable campaign-unlock state. */
static int cell_present(int cell)
{
    return cell >= 1 && cell <= PSX_DROP_DB_DUELISTS &&
           psx_mod_read_byte(AVAIL_ADDR + (uint32_t)cell) != 0u;
}

static void redraw(void)
{
    memset(s_canvas, 0, sizeof s_canvas);
    s_border_count = 0;
    s_border_mask = 0;
    s_last_border_x = s_last_border_y = -1;
    if (!s_visible || !s_counts_ready) { s_dirty = 0; return; }

    if (s_selected >= 0 && s_selected < PSX_DROP_DB_DUELISTS) {
        char fraction[16];
        snprintf(fraction, sizeof fraction, "%u/%u",
                 (unsigned)s_owned[s_selected],
                 (unsigned)s_obtainable[s_selected]);
        put_text_right(fraction, TEXT_RIGHT, TEXT_Y);
    }

    psx_free_duel_completion_art_frame(s_frame, s_border);
    for (int global_row = 0; global_row < GRID_ROWS; global_row++) {
        const int y = BORDER_Y0 + global_row * BORDER_DY - s_scroll_y;
        if (y >= 188 || y + BORDER_H <= BORDER_Y0) continue;
        for (int col = 0; col < GRID_COLS; col++) {
            const int cell = global_row * GRID_COLS + col;
            const int duelist = cell - 1;
            if (duelist < 0 || duelist >= PSX_DROP_DB_DUELISTS ||
                !s_complete[duelist] || !cell_present(cell)) continue;
            const int x = border_x(col);
            blit_border(x, y);
            s_border_mask |= UINT64_C(1) << cell;
            s_last_border_x = x;
            s_last_border_y = y;
            s_border_count++;
        }
    }
    s_dirty = 0;
}

static void tick(void)
{
    if (s_present_hold > 0) s_present_hold--;
    const int netplay = psx_ygo_netplay_session();
    const int screen = psx_mod_game_started() && !netplay &&
        psx_mod_read_byte(MODE_ADDR) == FREE_DUEL_MODE;
    /* 0x8009B32E is the last VALID highlighted duelist. The stock overlay
     * intentionally leaves it unchanged while the cursor traverses an empty
     * cell, so it cannot drive either the fraction or the scroll window.
     * B36C/D are the pending cursor coordinates and change as soon as input is
     * accepted (B366/7 lag until the eight-frame cursor tween completes).
     * B148 is the stock object's exact pixel scroll, including the tween. */
    const int cursor_col = screen
        ? (int)(int8_t)psx_mod_read_byte(CURSOR_COL_ADDR) : -1;
    const int cursor_row = screen
        ? (int)(int8_t)psx_mod_read_byte(CURSOR_ROW_ADDR) : -1;
    const int cursor_valid = cursor_col >= 0 && cursor_col < GRID_COLS &&
                             cursor_row >= 0 && cursor_row < GRID_ROWS;
    const int cell = cursor_valid ? cursor_row * GRID_COLS + cursor_col : -1;
    const int selected = cursor_valid && cell_present(cell) ? cell - 1 : -1;
    int next_scroll = screen
        ? (int)(int16_t)(psx_mod_read_byte(SCROLL_Y_ADDR) |
              ((uint16_t)psx_mod_read_byte(SCROLL_Y_ADDR + 1u) << 8)) : 0;
    if (next_scroll < 0) next_scroll = 0;
    if (next_scroll > (GRID_ROWS - VIEW_ROWS) * BORDER_DY)
        next_scroll = (GRID_ROWS - VIEW_ROWS) * BORDER_DY;
    const int next_top = next_scroll / BORDER_DY;

    s_cursor_col = cursor_col;
    s_cursor_row = cursor_row;

    if (screen != s_screen) {
        s_screen = screen;
        s_visible = s_enabled && screen;
        s_top_row = screen ? next_top : 0;
        s_scroll_y = screen ? next_scroll : 0;
        if (!screen) s_selected = -1;
        s_counts_ready = 0;
        s_dirty = 1;
        s_present_hold = 3;
    }
    if (!screen || !s_enabled) return;

    if (selected != s_selected) {
        s_selected = selected;
        s_dirty = 1;
        s_present_hold = 3;
    }
    if (next_scroll != s_scroll_y) {
        s_scroll_y = next_scroll;
        s_top_row = next_top;
        s_dirty = 1;
    }

    const unsigned edit_gen = psx_drop_edits_generation();
    const unsigned cpu_gen = psx_cpu_generation();
    const int missing_on = psx_drop_missing_enabled();
    PsxCardInventorySnapshot now;
    const int inventory_ready = psx_card_inventory_snapshot(&now);
    const int inventory_changed = inventory_ready != s_inventory_ready ||
        (inventory_ready && memcmp(&now, &s_inventory, sizeof now) != 0);
    if (inventory_changed || edit_gen != s_edit_gen ||
        missing_on != s_missing_on || !s_counts_ready) {
        s_inventory_ready = inventory_ready;
        if (inventory_ready) {
            s_inventory = now;
            if (missing_on) psx_drop_missing_ensure_loaded();
            rebuild_counts(&s_inventory);
        } else {
            s_counts_ready = 0;
            s_dirty = 1;
        }
        s_edit_gen = edit_gen;
        s_missing_on = missing_on;
    }
    /* Renames do not change the numeric overlay, but they do change the
     * selected display name reported by the debug surface immediately. Keep
     * its generation observable and force a fresh composite for parity with
     * every other CPU-aware UI. */
    if (cpu_gen != s_cpu_gen) {
        s_cpu_gen = cpu_gen;
        s_dirty = 1;
        s_present_hold = 3;
    }

    if (s_counts_ready && s_completed_total > 0 && ++s_anim_clock >= 6) {
        s_anim_clock = 0;
        s_frame = (s_frame + 1) % PSX_FD_COMPLETION_FRAME_COUNT;
        s_dirty = 1;
        s_present_hold = 2;
    }
}

int psx_free_duel_completion_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_enabled || !s_visible || psx_ygo_netplay_session()) return 0;
    if (s_dirty) redraw();
    if (pixels) *pixels = s_canvas;
    if (w) *w = SCREEN_W;
    if (h) *h = SCREEN_H;
    return s_counts_ready;
}

void psx_free_duel_completion_origin(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
}

int psx_free_duel_completion_needs_present(void)
{
    /* An Off transition still needs a present so the compositor removes the
     * last ratio/frame instead of leaving it stranded until unrelated video
     * activity happens. */
    return s_present_hold > 0;
}

void psx_free_duel_completion_placed(const int *placement)
{
    if (placement) memcpy(s_placement, placement, sizeof s_placement);
}

int psx_free_duel_completion_state_json(char *out, unsigned cap)
{
    if (!out || cap < 512u) return 0;
    char name[PSX_CPU_NAME_MAX * 2 + 1];
    if (s_selected >= 0)
        (void)psx_cpu_display_name_json(s_selected, name, sizeof name);
    else
        name[0] = 0;
    const int owned = s_selected >= 0 && s_counts_ready
                          ? s_owned[s_selected] : -1;
    const int obtainable = s_selected >= 0 && s_counts_ready
                               ? s_obtainable[s_selected] : -1;
    const int complete = s_selected >= 0 && s_counts_ready
                             ? s_complete[s_selected] : 0;
    const int n = snprintf(out, cap,
        "\"enabled\":%d,\"screen\":%d,\"visible\":%d,\"netplay\":%d,"
        "\"selected\":%d,\"name\":\"%s\",\"owned\":%d,"
        "\"obtainable\":%d,\"complete\":%d,\"empty_is_complete\":false,"
        "\"completed_opponents\":%d,\"cursor\":[%d,%d],"
        "\"top_row\":%d,\"scroll_y\":%d,\"frame\":%d,"
        "\"borders\":%d,\"border_mask\":\"0x%010llX\",\"last_border\":[%d,%d],"
        "\"inventory_ready\":%d,\"drop_missing\":%d,"
        "\"edit_gen\":%u,\"cpu_gen\":%u,"
        "\"placement\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
        s_enabled, s_screen, s_enabled && s_visible && !psx_ygo_netplay_session(),
        psx_ygo_netplay_session(), s_selected, name, owned, obtainable,
        complete, s_completed_total, s_cursor_col, s_cursor_row,
        s_top_row, s_scroll_y, s_frame, s_border_count,
        (unsigned long long)((s_enabled && s_visible &&
                              !psx_ygo_netplay_session()) ? s_border_mask : 0),
        s_last_border_x, s_last_border_y, s_inventory_ready, s_missing_on,
        s_edit_gen, s_cpu_gen,
        s_placement[0], s_placement[1], s_placement[2], s_placement[3],
        s_placement[4], s_placement[5], s_placement[6], s_placement[7],
        s_placement[8], s_placement[9]);
    return n > 0 && n < (int)cap;
}

static void handle_debug(int id, const char *json)
{
    const int enabled = json_get_int(json, "enabled", -1);
    if ((enabled == 0 || enabled == 1) && s_menu_row >= 0 &&
        psx_video_menu_get_row(s_menu_row) != enabled) {
        psx_video_menu_set_row(s_menu_row, enabled);
        psx_video_menu_note_change();
    }
    char body[1024];
    if (!psx_free_duel_completion_state_json(body, sizeof body)) {
        send_err(id, "free duel completion state unavailable");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

static void enabled_changed(int value)
{
    s_enabled = value ? 1 : 0;
    s_visible = s_enabled && s_screen;
    s_counts_ready = 0;
    if (s_enabled) {
        s_dirty = 1;
    } else {
        memset(s_canvas, 0, sizeof s_canvas);
        s_border_count = 0;
        s_last_border_x = s_last_border_y = -1;
        s_dirty = 0;
    }
    s_present_hold = 3;
    if (psx_video_menu_is_restoring()) return;
    host_osd_push(s_enabled ? "Free Duel progress: on"
                            : "Free Duel progress: off", 1000);
}

static void register_menu(void)
{
    static const char *const ONOFF[] = { "Off", "On" };
    static const char *const HINTS[] = {
        "Use the stock Free Duel portraits without collection progress",
        "Show owned/drop totals and glow completed opponents",
    };
    s_menu_row = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "Free Duel progress", HINTS[0],
        ONOFF, 2, "free_duel_progress", 0, enabled_changed);
    psx_video_menu_set_row_hints(s_menu_row, HINTS);
}

PSX_MOD_CONSTRUCTOR(psx_free_duel_completion_install)
{
    register_menu();
    PsxGuestOverlay overlay = {
        psx_free_duel_completion_image,
        psx_free_duel_completion_origin,
        NULL,
        psx_free_duel_completion_needs_present,
        -1,
        psx_free_duel_completion_placed,
    };
    (void)psx_guest_overlay_register(&overlay);
    (void)psx_game_add_frame_hook(tick);
    (void)psx_debug_add_command("free_duel_completion", handle_debug);
}
