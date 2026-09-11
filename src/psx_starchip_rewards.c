/* Ordered, conditional starchip rewards at the duel-results boundary.
 *
 * Stock computes RESULT[56] = rank letter (D..S), RESULT[57] = TEC/POW and
 * RESULT[58] = letter + 1. A won duel adds that one-byte 1..5 amount to the
 * live save at 0x801D07E0, then clamps the 32-bit total to 999999. The same
 * byte controls a loop that draws one star icon per chip on the summary page;
 * widening only the value would therefore overflow both storage and layout.
 *
 * The results-state function at 0x800218F0 runs once per displayed frame. Its
 * first entry is before stock calculates/applies the reward; the next entry is
 * after. We snapshot the exact pre-award total on the first entry, select the
 * first matching authored rule on the second, and replace the total with a
 * saturating pre_total + configured_amount. With no match, nothing is written
 * or drawn and the disc path is bit-for-bit untouched.
 *
 * A matched rule gets a present-only compact `star x amount` row. This covers
 * the stock one-to-five stars without touching guest VRAM and safely fits all
 * six digits permitted by the real save cap. It is shown only on results page
 * zero, and netplay disables both mutation and presentation while preserving
 * the persisted offline rules.
 */

#include "psx_starchip_rewards.h"

#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_card_inventory.h"
#include "psx_cpu_data.h"
#include "psx_drop_edits.h"
#include "psx_game_hooks.h"
#include "psx_rank_sprites.h"
#include "psx_shop_skin.h"
#include "psx_ygo_netplay.h"

#define RESULTS_FN       0x800218F0u
#define GP_ADDR          0x8009AF08u
#define RESULT_PTR       (GP_ADDR + 736u)
#define RESULT_PAGE_OFF  55u
#define RESULT_LETTER    56u
#define RESULT_POW       57u
#define RESULT_CHIPS     58u
#define RESULT_TAG0      52u
#define RESULT_TAG2      54u
#define OPPONENT_ID      0x8009B361u
#define FREE_DUEL_FLAGS  0x8009B365u
#define FREE_DUEL_BIT    0x80u
#define RANK_BLOCK       0x800E9FF0u
#define RANK_STRIDE      0x20u
#define RANK_BONUS       0u
#define RANK_LP          0x14u
#define CHIPS_ADDR       0x801D07E0u

#define ORIGIN_X 136
#define ORIGIN_Y 184
#define CANVAS_W 100
#define CANVAS_H 16

static uint32_t s_canvas[CANVAS_W * CANVAS_H];
static int s_registered;
static int s_hooked_this_frame;
static int s_absent_frames;
static int s_results_active;
static int s_result_calls;
static uint32_t s_before;
static int s_decided;
static int s_matched;
static int s_rule = -1;
static int s_mode = -1;
static int s_opponent = -1;
static int s_outcome = -1;
static int s_rank = -1;
static uint32_t s_amount;
static uint32_t s_after;
static uint32_t s_lost_to_cap;
static unsigned s_applies;
static unsigned s_stock_fallbacks;
static int s_present_hold;
static int s_placement[10];
static uint32_t s_state_mem;

#define CHIP_STATE_MAGIC   0x43534850u /* CSHP */
#define CHIP_STATE_VERSION 1u
#define CHIP_STATE_WORDS   24u

static void blit(const PsxSprite *sprite, int dx, int dy)
{
    if (!sprite || !sprite->px) return;
    for (int y = 0; y < sprite->h; y++) {
        if (dy + y < 0 || dy + y >= CANVAS_H) continue;
        for (int x = 0; x < sprite->w; x++) {
            if (dx + x < 0 || dx + x >= CANVAS_W) continue;
            const uint32_t p = sprite->px[y * sprite->w + x];
            if (p >> 24) s_canvas[(dy + y) * CANVAS_W + dx + x] = p;
        }
    }
}

static void redraw(void)
{
    /* Opaque enough to hide every stock star, transparent enough to retain
     * the summary art as context. */
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) s_canvas[i] = 0xD0000000u;
    blit(&psx_spr_shop_star, 0, 0);
    /* A compact white x between the starchip and the amount. */
    for (int i = 0; i < 5; i++) {
        s_canvas[(5 + i) * CANVAS_W + 20 + i] = 0xFFFFFFFFu;
        s_canvas[(5 + i) * CANVAS_W + 24 - i] = 0xFFFFFFFFu;
    }
    char digits[7];
    snprintf(digits, sizeof digits, "%u", (unsigned)s_amount);
    int x = 29;
    for (const char *p = digits; *p; p++, x += 8)
        blit(&psx_spr_digit[*p - '0'], x, 4);
    s_present_hold = 3;
}

static int classify_outcome(void)
{
    const int bonus = (int8_t)psx_mod_read_byte(RANK_BLOCK + RANK_BONUS);
    if (bonus > 0) return PSX_STARCHIP_OUTCOME_WIN;
    if (bonus < 0) return PSX_STARCHIP_OUTCOME_LOSS;
    const unsigned you = psx_mod_read_half(RANK_BLOCK + RANK_LP);
    const unsigned opp = psx_mod_read_half(RANK_BLOCK + RANK_STRIDE + RANK_LP);
    if (!opp && you) return PSX_STARCHIP_OUTCOME_WIN;
    if (!you && opp) return PSX_STARCHIP_OUTCOME_LOSS;
    return -1;
}

static void clear_decision(void)
{
    s_decided = 0;
    s_matched = 0;
    s_rule = -1;
    s_mode = -1;
    s_opponent = -1;
    s_outcome = -1;
    s_rank = -1;
    s_amount = 0;
    s_after = 0;
    s_lost_to_cap = 0;
}

static void reset_result(void)
{
    s_results_active = 0;
    s_result_calls = 0;
    s_before = 0;
    clear_decision();
    s_present_hold = 3;
}

static void on_results(CPUState *cpu, uint32_t address)
{
    (void)cpu;
    if (address != RESULTS_FN || psx_ygo_netplay_session()) return;
    s_hooked_this_frame = 1;
    if (!s_results_active) {
        s_results_active = 1;
        s_result_calls = 0;
        clear_decision();
        s_before = psx_mod_read_word(CHIPS_ADDR);
    }
    s_result_calls++;
    if (s_decided || s_result_calls < 2) return;

    const uint32_t result = psx_mod_read_word(RESULT_PTR);
    if (!result || psx_mod_read_byte(result + RESULT_TAG0) != 68u ||
        psx_mod_read_byte(result + RESULT_TAG2) != 69u) return;
    const int letter = (int)psx_mod_read_byte(result + RESULT_LETTER);
    const int pow = (int)psx_mod_read_byte(result + RESULT_POW);
    if (letter < 0 || letter > 4 || pow < 0 || pow > 1) return;

    s_mode = (psx_mod_read_byte(FREE_DUEL_FLAGS) & FREE_DUEL_BIT)
                 ? PSX_STARCHIP_MODE_FREE_DUEL
                 : PSX_STARCHIP_MODE_CAMPAIGN;
    s_opponent = (int)psx_mod_read_byte(OPPONENT_ID) - 1;
    s_outcome = classify_outcome();
    s_rank = letter + (pow ? 5 : 0);
    s_decided = 1;
    if (s_opponent < 0 || s_opponent >= PSX_DROP_DB_DUELISTS ||
        s_outcome < 0 || !psx_drop_edits_starchip_select(
            s_mode, s_opponent, s_outcome, s_rank, &s_amount, &s_rule)) {
        s_after = psx_mod_read_word(CHIPS_ADDR);
        s_stock_fallbacks++;
        return;
    }

    const uint64_t sum = (uint64_t)s_before + s_amount;
    s_after = sum > PSX_CARD_STARCHIP_CAP ? PSX_CARD_STARCHIP_CAP
                                          : (uint32_t)sum;
    s_lost_to_cap = sum > PSX_CARD_STARCHIP_CAP
                        ? (uint32_t)(sum - PSX_CARD_STARCHIP_CAP) : 0u;
    psx_mod_write_word(CHIPS_ADDR, s_after);
    s_matched = 1;
    s_applies++;
    redraw();
}

static void state_put(unsigned *at, uint32_t value)
{
    if (s_state_mem && *at < CHIP_STATE_WORDS)
        psx_mod_write_word(s_state_mem + 4u * (*at)++, value);
}

static uint32_t state_get(unsigned *at)
{
    if (!s_state_mem || *at >= CHIP_STATE_WORDS) return 0;
    return psx_mod_read_word(s_state_mem + 4u * (*at)++);
}

/* Results can be saved after stock has already added its 1-5 chips. Mirroring
 * the host decision into guest-backed state prevents a reload from treating
 * that post-award total as a new pre-award value and adding the override a
 * second time. Old states have no magic and safely start with no decision. */
static void state_before_save(void)
{
    unsigned at = 0;
    if (!s_state_mem) return;
    for (unsigned i = 0; i < CHIP_STATE_WORDS; i++)
        psx_mod_write_word(s_state_mem + 4u * i, 0);
    state_put(&at, CHIP_STATE_MAGIC);
    state_put(&at, CHIP_STATE_VERSION);
    state_put(&at, (uint32_t)s_results_active);
    state_put(&at, (uint32_t)s_result_calls);
    state_put(&at, s_before);
    state_put(&at, (uint32_t)s_decided);
    state_put(&at, (uint32_t)s_matched);
    state_put(&at, (uint32_t)s_rule);
    state_put(&at, (uint32_t)s_mode);
    state_put(&at, (uint32_t)s_opponent);
    state_put(&at, (uint32_t)s_outcome);
    state_put(&at, (uint32_t)s_rank);
    state_put(&at, s_amount);
    state_put(&at, s_after);
    state_put(&at, s_lost_to_cap);
    state_put(&at, (uint32_t)s_present_hold);
}

static void state_after_load(void)
{
    unsigned at = 0;
    const uint32_t magic = state_get(&at);
    const uint32_t version = state_get(&at);
    reset_result();
    if (magic != CHIP_STATE_MAGIC || version != CHIP_STATE_VERSION) return;
    s_results_active = (int)state_get(&at);
    s_result_calls = (int)state_get(&at);
    s_before = state_get(&at);
    s_decided = (int)state_get(&at);
    s_matched = (int)state_get(&at);
    s_rule = (int)state_get(&at);
    s_mode = (int)state_get(&at);
    s_opponent = (int)state_get(&at);
    s_outcome = (int)state_get(&at);
    s_rank = (int)state_get(&at);
    s_amount = state_get(&at);
    s_after = state_get(&at);
    s_lost_to_cap = state_get(&at);
    s_present_hold = (int)state_get(&at);
    s_hooked_this_frame = 0;
    s_absent_frames = 0;
}

static void tick(void)
{
    if (s_present_hold > 0) s_present_hold--;
    if (psx_ygo_netplay_session()) {
        if (s_results_active || s_matched) reset_result();
        s_hooked_this_frame = 0;
        s_absent_frames = 0;
        return;
    }
    if (s_hooked_this_frame) s_absent_frames = 0;
    else if (s_results_active && ++s_absent_frames > 3) reset_result();
    s_hooked_this_frame = 0;
}

void psx_starchip_rewards_init(void)
{
    if (s_registered) return;
    s_registered = 1;
    s_state_mem = psx_mod_alloc_guest_memory(CHIP_STATE_WORDS * 4u, 4u);
    (void)psx_mod_register_state_plugin(
        "starchip_rewards_state", state_before_save, state_after_load);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.starchip_rewards", RESULTS_FN, on_results);
    (void)psx_game_add_frame_hook(tick);
}

int psx_starchip_rewards_image(const uint32_t **pixels, int *w, int *h)
{
    const uint32_t result = psx_mod_read_word(RESULT_PTR);
    if (!s_matched || !s_results_active || !result ||
        psx_mod_read_byte(result + RESULT_PAGE_OFF) != 0u ||
        psx_ygo_netplay_session()) return 0;
    if (pixels) *pixels = s_canvas;
    if (w) *w = CANVAS_W;
    if (h) *h = CANVAS_H;
    return 1;
}

void psx_starchip_rewards_origin(int *x, int *y)
{
    if (x) *x = ORIGIN_X;
    if (y) *y = ORIGIN_Y;
}

int psx_starchip_rewards_needs_present(void)
{
    return s_present_hold > 0;
}

void psx_starchip_rewards_placed(const int *placement)
{
    if (placement) memcpy(s_placement, placement, sizeof s_placement);
}

int psx_starchip_rewards_state_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    const uint32_t result = psx_mod_read_word(RESULT_PTR);
    const int page = result ? (int)psx_mod_read_byte(result + RESULT_PAGE_OFF) : -1;
    char opponent_name[PSX_CPU_NAME_MAX * 2 + 8];
    if (s_opponent >= 0 && s_opponent < PSX_DROP_DB_DUELISTS)
        (void)psx_cpu_display_name_json(s_opponent, opponent_name, sizeof opponent_name);
    else snprintf(opponent_name, sizeof opponent_name, "-");
    const int n = snprintf(out, cap,
        "\"configured_rules\":%d,\"active\":%d,\"calls\":%d,"
        "\"decided\":%d,\"matched\":%d,\"rule\":%d,"
        "\"mode\":%d,\"opponent\":%d,\"opponent_name\":\"%s\",\"outcome\":%d,\"rank\":%d,"
        "\"amount\":%u,\"before\":%u,\"after\":%u,\"cap_loss\":%u,"
        "\"page\":%d,\"visible\":%d,\"applies\":%u,"
        "\"stock_fallbacks\":%u,\"placement\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
        psx_drop_edits_starchip_count(), s_results_active, s_result_calls,
        s_decided, s_matched, s_rule, s_mode, s_opponent,
        opponent_name,
        s_outcome, s_rank,
        (unsigned)s_amount, (unsigned)s_before, (unsigned)s_after,
        (unsigned)s_lost_to_cap, page,
        s_matched && s_results_active && page == 0, s_applies,
        s_stock_fallbacks,
        s_placement[0], s_placement[1], s_placement[2], s_placement[3],
        s_placement[4], s_placement[5], s_placement[6], s_placement[7],
        s_placement[8], s_placement[9]);
    return n > 0 && n < (int)cap;
}
