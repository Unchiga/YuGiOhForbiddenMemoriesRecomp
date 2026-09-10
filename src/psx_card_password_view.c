/* Display the effective card password on the game's common card-detail view.
 *
 * Password editing was already complete in the Cards page and card-pack
 * backend. The missing half was presentation: outside the Password screen a
 * full card view showed art, stats, stars, type and description, but not the
 * password. Library, deck building, the chest and duel inspection all use the
 * same card-field text command and detail-open state, so observe those shared
 * paths instead of identifying individual menus or maintaining per-screen
 * drawing patches.
 *
 * func_80037DA4 reads a mode byte from the active text cursor. Mode 0x40 is
 * the selected card's description; it is therefore a precise witness that a
 * FULL detail widget is being drawn (unlike mode 0x20, the name, which also
 * appears under grids and the duel hand). See psx_card_name_color.c for the
 * decoded command and cursor-stack format. The widget's active allocation at
 * +0x28 remains live for the static Library detail view and becomes null when
 * it is destroyed. Deck/chest and duel use the established inspector flag at
 * 0x8009B140. Their card-flip state at 0x8009B248 reaches 0xA0 only once the
 * front has settled, and leaves 0xA0 on the first exit-animation frame. That
 * gives the footer the same reveal/lifetime as the card instead of flashing
 * over the list or floating beside a card back. Circle's new-press bit is an
 * immediate additional close edge for the non-animated Library view.
 *
 * The eight white digits sit at the bottom-right of the description panel.
 * The common animated card and the shorter static Library card have different
 * panel bottoms, so their Y origins differ. Both positions are below the
 * seventh description baseline and just inside the panel's lower gray frame.
 * A compact, transparent 3x5 digit face needs no label or background badge and
 * keeps even a full seven-line description reachable and unobscured.
 * It stays in 320x240 guest coordinates through
 * psx_guest_overlay. No guest RAM, VRAM, save data or RNG state is changed.
 */

#include "psx_card_password_view.h"

#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_card_packs.h"
#include "psx_game_hooks.h"
#include "psx_guest_overlay.h"
#include "psx_ygo_netplay.h"

#define TEXT_FN          0x80037DA4u
#define TEXT_CALLER      0x80038B98u
#define SELECTED_CARD    0x8009B338u
#define CARD_DETAIL_DUEL 0x8009B140u
#define CARD_FLIP_STATE  0x8009B248u
#define GAME_MODE        0x8009B26Cu
#define PAD_NEW          0x8009B394u
#define TEXT_STACK_OFF   0x58u
#define TEXT_ACTIVE_OFF  0x28u
#define TEXT_SLOTS       12
#define MODE_SET_COLOR   0x10u
#define MODE_DESCRIPTION 0x40u
#define MODE_DUEL        0xC3u
#define MODE_LIBRARY     0xC4u
#define MODE_DECK        0xC7u
#define FLIP_READY       0xA0u
#define PAD_CIRCLE       0x0020u
#define CARD_MAX         722

#define CANVAS_W 31
#define CANVAS_H 5
#define ORIGIN_X 288
#define ORIGIN_Y_LIBRARY 183
#define ORIGIN_Y_ANIMATED 203
#define COL_VALUE 0xFFFFFFFFu

static uint32_t s_px[CANVAS_W * CANVAS_H];
static int s_w;
static int s_active;
static int s_visible;
static int s_present_hold;
static int s_card;
static int s_drawn_card;
static uint32_t s_text_obj;
static unsigned s_drawn_generation = (unsigned)-1;
static char s_password[9];
static unsigned s_detail_hits;
static unsigned s_image_calls;
static unsigned s_place_calls;
static int s_registered;
static int s_placement[10];

static uint32_t text_cursor(uint32_t obj)
{
    const int slot = (int8_t)psx_mod_read_byte(obj + TEXT_STACK_OFF);
    if (slot < 0 || slot >= TEXT_SLOTS) return 0;
    return psx_mod_read_word(obj + (uint32_t)slot * 4u);
}

static uint32_t text_active(void)
{
    const uint32_t phys = s_text_obj & 0x1FFFFFFFu;
    if (!s_text_obj || phys > 0x200000u - TEXT_ACTIVE_OFF - 4u) return 0;
    return psx_mod_read_word(s_text_obj + TEXT_ACTIVE_OFF);
}

static int effective_password(int card, char out[9])
{
    PsxCardPack edit;
    PsxCardStock stock;
    out[0] = 0;
    if (card < 1 || card > CARD_MAX) return 0;
    if (psx_card_packs_get(card, &edit) && edit.password[0]) {
        memcpy(out, edit.password, 9);
        return 1;
    }
    if (!psx_card_packs_stock(card, &stock) || !stock.password[0]) return 0;
    memcpy(out, stock.password, 9);
    return 1;
}

/* Compact, unambiguous 3x5 digits. Reducing the game's one-pixel strokes to
 * four columns made 0/1/7 visually collapse; this tiny face is both smaller
 * and easier to read at every whole-number guest scale. */
static const uint8_t k_digit_rows[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
    { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
    { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
    { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }
};

static void put_password(const char value[9])
{
    int x0 = 0;
    for (int i = 0; i < 8; i++, x0 += 4) {
        const int digit = value[i] >= '0' && value[i] <= '9'
            ? value[i] - '0' : -1;
        for (int y = 0; y < 5; y++) {
            const uint8_t bits = digit >= 0 ? k_digit_rows[digit][y]
                                            : (y == 2 ? 7u : 0u);
            for (int x = 0; x < 3; x++)
                if (bits & (uint8_t)(4u >> x))
                    s_px[y * s_w + x0 + x] = COL_VALUE;
        }
    }
}

static void redraw(void)
{
    char password[9];
    char value[9] = "--------";
    if (effective_password(s_card, password)) memcpy(value, password, 9);
    memcpy(s_password, value, 9);
    s_w = CANVAS_W;
    memset(s_px, 0, sizeof s_px);
    put_password(value);
    s_drawn_card = s_card;
    s_drawn_generation = psx_card_packs_generation();
}

static void text_hook(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != TEXT_FN || cpu->gpr[31] != TEXT_CALLER ||
        psx_ygo_netplay_session()) return;
    const uint32_t cursor = text_cursor(cpu->gpr[4]);
    if (!cursor) return;
    const unsigned mode = psx_mod_read_byte(cursor);
    if (mode & MODE_SET_COLOR) return;
    if (mode & MODE_DESCRIPTION) {
        const int card = (int)psx_mod_read_half(SELECTED_CARD);
        if (card < 1 || card > CARD_MAX) return;
        if (!s_active || card != s_card) s_present_hold = 3;
        s_card = card;
        s_text_obj = cpu->gpr[4];
        s_active = 1;
        s_visible = 0;
        s_detail_hits++;
    }
}

static int detail_alive(unsigned mode)
{
    if (!s_active || (psx_mod_read_half(PAD_NEW) & PAD_CIRCLE)) return 0;
    if (mode == MODE_DUEL || mode == MODE_DECK)
        return psx_mod_read_byte(CARD_DETAIL_DUEL) != 0u;
    if (mode == MODE_LIBRARY) return text_active() != 0u;
    return 0;
}

static int display_allowed(void)
{
    const unsigned mode = psx_mod_read_byte(GAME_MODE);
    if (!detail_alive(mode)) return 0;
    return (mode != MODE_DUEL && mode != MODE_DECK) ||
           psx_mod_read_byte(CARD_FLIP_STATE) == FLIP_READY;
}

static void tick(void)
{
    if (s_present_hold > 0) s_present_hold--;
    if (psx_ygo_netplay_session()) {
        if (s_visible) s_present_hold = 2;
        s_active = 0;
        s_visible = 0;
        return;
    }
    const unsigned mode = psx_mod_read_byte(GAME_MODE);
    const unsigned pad_new = psx_mod_read_half(PAD_NEW);
    if (s_active && (pad_new & PAD_CIRCLE)) s_active = 0;

    const int alive = detail_alive(mode);
    const int visible = display_allowed();
    if (visible != s_visible) {
        s_visible = visible;
        s_present_hold = 3;
    }
    if (!alive) s_active = 0;
    if (s_visible && s_drawn_generation != psx_card_packs_generation())
        s_present_hold = 3;
}

int psx_card_password_view_image(const uint32_t **px, int *w, int *h)
{
    s_image_calls++;
    /* The frame hook can run just before the guest consumes an exit press.
     * Recheck the live animation/pad state in the compositor so the footer is
     * absent from that very first outgoing frame too. */
    if (!s_visible || !display_allowed() || s_card < 1 || s_card > CARD_MAX)
        return 0;
    if (s_drawn_card != s_card || s_drawn_generation != psx_card_packs_generation()) redraw();
    *px = s_px; *w = s_w; *h = CANVAS_H;
    return s_w > 0;
}

void psx_card_password_view_placed(const int *placement)
{
    if (!placement) return;
    memcpy(s_placement, placement, sizeof s_placement);
    s_place_calls++;
}

void psx_card_password_view_registered(int ok) { s_registered = ok != 0; }

void psx_card_password_view_origin(int *x, int *y)
{
    *x = ORIGIN_X;
    *y = psx_mod_read_byte(GAME_MODE) == MODE_LIBRARY
        ? ORIGIN_Y_LIBRARY : ORIGIN_Y_ANIMATED;
}

int psx_card_password_view_needs_present(void)
{
    return s_present_hold > 0;
}

int psx_card_password_view_state_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    const int visible = s_visible && display_allowed();
    if (visible && (s_drawn_card != s_card ||
                    s_drawn_generation != psx_card_packs_generation())) redraw();
    int origin_x = 0, origin_y = 0;
    psx_card_password_view_origin(&origin_x, &origin_y);
    const int n = snprintf(out, cap,
        "\"active\":%d,\"visible\":%d,\"card\":%d,\"password\":\"%s\","
        "\"detail_hits\":%u,\"registered\":%d,\"overlay_count\":%d,"
        "\"mode\":%u,\"duel_flag\":%u,\"flip_state\":%u,"
        "\"pad_new\":%u,\"text_obj\":\"0x%08X\",\"text_active\":\"0x%08X\","
        "\"image_calls\":%u,\"place_calls\":%u,"
        "\"origin\":[%d,%d],\"size\":[%d,%d],"
        "\"placement\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
        s_active, visible, s_card, s_password, s_detail_hits, s_registered,
        psx_guest_overlay_count(), (unsigned)psx_mod_read_byte(GAME_MODE),
        (unsigned)psx_mod_read_byte(CARD_DETAIL_DUEL),
        (unsigned)psx_mod_read_byte(CARD_FLIP_STATE),
        (unsigned)psx_mod_read_half(PAD_NEW), s_text_obj, text_active(), s_image_calls,
        s_place_calls, origin_x, origin_y, s_w,
        CANVAS_H, s_placement[0], s_placement[1], s_placement[2],
        s_placement[3], s_placement[4], s_placement[5], s_placement[6],
        s_placement[7], s_placement[8], s_placement[9]);
    return n >= 0 && (unsigned)n < cap;
}

void psx_card_password_view_init(void)
{
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_password_view.text", TEXT_FN, text_hook);
    (void)psx_game_add_frame_hook(tick);
}
