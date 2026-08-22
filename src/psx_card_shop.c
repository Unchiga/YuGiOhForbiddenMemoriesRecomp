/* CARD SHOP — a native row on the campaign shopkeeper's menu that opens a
 * pack shop, drawn with the game's own text engine wherever the game can be
 * made to do it, and with its own baked font where it cannot.
 *
 * === How the native row works (all measured live, 2026-08-21) ===
 *
 * The shopkeeper menu's four labels are ONE glyph stream — string id 17 in a
 * u16 offset table at 0x801C0000, streams based at 0x801B0000, letters in the
 * game's frequency-ordered alphabet (00=space 01=e 02=t ... 1D=S 18=A 39=V
 * 25=E). The stream's 3-byte FB header carries the ROW COUNT in byte1's low
 * three bits (the menu-open code at 0x80038CA0 does `andi v0, a1, 7` on it
 * and stores the count byte the cursor driver clamps against).
 *
 * So: compose a FOUR-line replacement (SAVE / CARD SHOP / BUILD DECK /
 * RETURN TO TITLE), stash it in the zero arena at the top of the bank
 * (0x801BFF80 — 1.2 KB of unused zeros, offset still reaches the u16 table),
 * and repoint table entry 17. The game's next natural menu open draws OUR
 * labels in its own font with its own highlight. Count=5 in the header
 * STALLS the decoder (a four-line limit in this window style — measured
 * twice, locks the menu), so the fifth row, LEAVE SHOP, is our overlay band
 * under the wedge, and the COUNT BYTE is bumped 4->5 separately so the
 * game's own cursor driver runs all five positions.
 *
 * The insert shifts dispatch: the game maps cursor 0..3 to
 * SAVE/BUILD/RETURN/LEAVE. On an X press we therefore rewrite the cursor
 * DOWN by one for rows 2..4 before the game's tick reads it (our frame hook
 * demonstrably wins that race — it is the same trick that eats presses), and
 * row 1 (CARD SHOP) is eaten and opens our panel. Row 0 passes untouched.
 *
 * NEVER touch widget state (latch bits, cursors, pending flags): forcing a
 * mid-screen re-decode wedged the menu twice during bring-up. The stream and
 * table are asserted from the tick; only a natural menu open consumes them.
 *
 * Everything binds to reality through the widget's own cursor: the remap,
 * the extra row and the count bump apply ONLY while the menu widget is
 * actually displaying OUR stream (its cursor points into our arena). If a
 * savestate restored the stock table mid-screen, the menu shows stock labels
 * and this module behaves as if absent until the next natural open. */

#include "psx_card_shop.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_drop_db.h"
#include "psx_fusion_font.h"
#include "psx_video_menu.h"

/* ---- measured addresses -------------------------------------------------- */
#define SHOP_STATE_ADDR   0x8009B23Au   /* campaign overlay state: 0xE00D   */
#define SHOP_MENUFLAG     0x8009B350u   /* gp+1096: 1 while a menu is open  */
#define SHOP_SIG_A        0x8009B338u   /* == 0x08 on the shopkeeper menu   */
#define SHOP_SIG_B        0x8009B33Au   /* == 0xFF on the shopkeeper menu   */
#define SHOP_SIG_C        0x8009B344u   /* == 0x20 on the shopkeeper menu   */
#define SHOP_COUNT_ADDR   0x8009B345u   /* menu row count (gp+1085)         */
#define SHOP_CURSOR_ADDR  0x8009B34Du   /* menu cursor    (gp+1093)         */
#define SHOP_PAD_NEW_ADDR 0x8009B394u   /* new-press mask, byte-swapped     */
#define SHOP_WIDGET1      0x800EB224u   /* the campaign menu text widget    */
#define SHOP_LBL_TABLE    0x801C0000u   /* u16 stream offsets, id 17 = menu */
#define SHOP_LBL_BASE     0x801B0000u
#define SHOP_ARENA_OFF    0xFF80u       /* our stream in the bank's zeros   */
#define SHOP_CHIPS_ADDR   0x801D07E0u
#define SHOP_SAVE_LIVE    0x801D0200u
#define SHOP_SAVE_MIRROR  0x801D3200u
#define SHOP_TRUNK_OFF    0x50u

/* Byte-swapped new-press bits (raw pad halfword swapped, see card_drops). */
#define SHOP_NP_UP      0x1000u
#define SHOP_NP_DOWN    0x4000u
#define SHOP_NP_LEFT    0x8000u
#define SHOP_NP_RIGHT   0x2000u
#define SHOP_NP_CROSS   0x0040u
#define SHOP_NP_CIRCLE  0x0020u

#define SHOP_ROW  1                     /* CARD SHOP's cursor index */
#define SHOP_ROWS 5                     /* rows the cursor can reach */

/* The composed menu stream. Header FB / count|style / metric copied from
 * stock (count stays 4 — five stalls the decoder); per-line F8 02 xx is the
 * line's x offset, FE is the line break, FB 80 the stock trailer. Letters
 * are the game's own codes (see the alphabet in the header comment). */
/* THE menu stream: five lines, count=5 in the header (byte1 & 7). The
 * window-kind byte 0x4D dispatches the same no-op stub as stock 0x4C (both
 * measured `jr ra`), so the only thing that ever blocked five rows was the
 * shopkeeper screen's hardcoded window HEIGHT literal — patched below. */
static const uint8_t k_menu_stream[] = {
    /* Header: FB / kind|count / flags|enable-mask.
     * byte1 0x4D: window kind 0x4D (a no-op stub like stock 0x4C) with the
     *   row COUNT 5 in the low three bits.
     * byte2 0x9F: bit7 = synchronous feed (as stock), low bits 0x1F = the
     *   ENABLED-ROWS bitmask, all five rows. (Leaving a row out of the mask
     *   makes the cursor SKIP it entirely - measured - so denial cannot be
     *   used to guard CARD SHOP; the sticky remap below does that instead.) */
    0xFB, 0x4D, 0x9F,
    0xF8, 0x02, 0x2C,  0x1D, 0x18, 0x39, 0x25,                   0xFE, /* SAVE */
    0xF8, 0x02, 0x18,  0x2B, 0x18, 0x26, 0x20, 0x00,
                       0x1D, 0x23, 0x21, 0x2F,                   0xFE, /* CARD SHOP */
    0xF8, 0x02, 0x14,  0x2D, 0x35, 0x1A, 0x2A, 0x20, 0x00,
                       0x20, 0x25, 0x2B, 0x33,                   0xFE, /* BUILD DECK */
    0x26, 0x25, 0x1C, 0x35, 0x26, 0x2C, 0x00,
    0x1C, 0x21, 0x00,  0x1C, 0x1A, 0x1C, 0x2A, 0x25,             0xFE, /* RETURN TO TITLE */
    0xF8, 0x02, 0x14,  0x2A, 0x25, 0x18, 0x39, 0x25, 0x00,
                       0x1D, 0x23, 0x21, 0x2F,                   0xFE, /* LEAVE SHOP */
    0xFB, 0x80, 0x00, 0x00, 0x00, 0x00,
};

/* The shopkeeper screen opens its menu window with hardcoded geometry
 * (id 17, x=-144, y=56, w=120, h=48 -- literals at 0x8002F008..0x8002F020).
 * h=48 is exactly four 12px rows: a five-line stream fed into it leaves the
 * window-grow feeder waiting forever for a row that cannot fit, which
 * presents as a locked, invisible menu. One patched immediate makes the
 * window five rows tall; the feeder then feeds all five and terminates on
 * the stream trailer as normal. Scoped to this one screen's open call. */
#define SHOP_H_PATCH_ADDR 0x8002F020u
#define SHOP_H_STOCK      0x24020030u   /* addiu v0, zero, 48 */
#define SHOP_H_FIVE       0x2402003Cu   /* addiu v0, zero, 60 */

/* The screen's menu dispatch (0x8002F35C..) is a compare chain on the cursor
 * sampled AT THE ACCEPT INSTANT - measured: no host-side rewrite can land
 * before it. So the chain itself is shifted for the inserted row: BUILD DECK
 * fires on 2, RETURN TO TITLE on 3, LEAVE SHOP on 4, and index 1 (CARD SHOP)
 * falls through to the chain's own default, which simply ends the dialog.
 * The module opens its panel on that same press; the game contributes a
 * cleanly closed menu underneath. Three immediates, same screen, same
 * mechanism as the height patch. */
#define SHOP_D1_ADDR 0x8002F360u
#define SHOP_D1_STOCK 0x24020001u   /* addiu v0, zero, 1 */
#define SHOP_D1_OURS  0x24020002u
#define SHOP_D2_ADDR 0x8002F384u
#define SHOP_D2_STOCK 0x24020002u
#define SHOP_D2_OURS  0x24020003u
#define SHOP_D3_ADDR 0x8002F390u
#define SHOP_D3_STOCK 0x24020003u
#define SHOP_D3_OURS  0x24020004u

/* ---- packs --------------------------------------------------------------- */
/* Typed as the user specced: monsters cheapest, then magic (rituals ride
 * along), equip, traps dearest. LEFT/RIGHT walks the rarity tier, which
 * multiplies the price and narrows the pool to scarcer cards. */
typedef struct { const char *name; int base_price; int cards; } ShopPack;
static const ShopPack k_packs[] = {
    { "MONSTER", 20, 5 },
    { "MAGIC",   40, 5 },
    { "EQUIP",   60, 5 },
    { "TRAP",    80, 5 },
};
#define SHOP_PACKS 4
#define SHOP_TIERS 4
static const char *const k_tier_names[SHOP_TIERS] =
    { "NORMAL", "RARE", "SUPER", "ULTRA" };
static const int k_tier_mult[SHOP_TIERS] = { 1, 2, 4, 8 };
/* Rarity = drop scarcity: how many of the 39 duelists drop the card at all.
 * ULTRA (<=1) includes the 82 cards nobody drops — the set's true chase
 * cards, otherwise reachable only by password. Monsters also gain an ATK
 * floor per tier so a dear pack cannot pull a 300 ATK filler. */
static const int k_tier_droppers[SHOP_TIERS] = { 255, 8, 3, 1 };
static const int k_tier_atk_floor[SHOP_TIERS] = { 0, 1000, 1600, 2000 };
#define SHOP_PULL_MAX 5

/* Type codes (psx_card_db TYPE_NAMES order): 0..19 monsters, then: */
#define TYPE_MAGIC  20
#define TYPE_TRAP   21
#define TYPE_RITUAL 22
#define TYPE_EQUIP  23

/* ---- state --------------------------------------------------------------- */
static int      s_enabled = 1;
static int      s_row_handle;
static int      s_gate;              /* screen signature matched            */
static int      s_native;            /* widget is displaying OUR stream     */
static int      s_open;              /* pack panel is up                    */
static int      s_sel;               /* selected pack 0..3                  */
static int      s_tier[SHOP_PACKS];  /* per-pack rarity tier                */
static int      s_dirty = 1;
static uint32_t s_rng = 0x5EEDCA5Du;
static int      s_pull[SHOP_PULL_MAX];
static int      s_pull_n;
static char     s_msg[30];
static uint16_t s_stock_entry;       /* table[17] before our repoint        */
static unsigned s_buys, s_denied, s_opens, s_remaps;

/* Card pools per (pack, tier), built once card_db + drop scarcity are up. */
static uint16_t s_pool[SHOP_PACKS][SHOP_TIERS][PSX_CARD_DB_COUNT];
static int      s_pool_n[SHOP_PACKS][SHOP_TIERS];
static int      s_pools_built;

/* ---- helpers ------------------------------------------------------------- */
static uint32_t rng_next(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static int deck_resident(uint32_t base) {
    int prev = 0;
    for (int i = 0; i < 40; i++) {
        const int id = (int)psx_mod_read_half(base + (uint32_t)i * 2u);
        if (id < 1 || id > PSX_CARD_DB_COUNT || id < prev) return 0;
        prev = id;
    }
    return 1;
}
static int save_live(void) {
    return deck_resident(SHOP_SAVE_LIVE) && deck_resident(SHOP_SAVE_MIRROR);
}

static void build_pools(void) {
    if (s_pools_built || !psx_card_db_ready()) return;
    static uint8_t droppers[PSX_CARD_DB_COUNT + 1];
    memset(droppers, 0, sizeof droppers);
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++) {
        static uint8_t seen[PSX_CARD_DB_COUNT + 1];
        memset(seen, 0, sizeof seen);
        for (int t = 0; t < PSX_DROP_DB_TIERS; t++)
            for (int i = 0; i < PSX_DROP_DB[d].count[t]; i++) {
                const int c = PSX_DROP_DB[d].tier[t][i].card;
                if (c >= 1 && c <= PSX_CARD_DB_COUNT && !seen[c]) {
                    seen[c] = 1; droppers[c]++;
                }
            }
    }
    for (int id = 1; id <= PSX_CARD_DB_COUNT; id++) {
        int atk = 0, def = 0, type = 0;
        if (!psx_card_db_stats(id, &atk, &def, &type)) continue;
        int pack;
        if (type <= 19)                                     pack = 0;
        else if (type == TYPE_MAGIC || type == TYPE_RITUAL) pack = 1;
        else if (type == TYPE_EQUIP)                        pack = 2;
        else if (type == TYPE_TRAP)                         pack = 3;
        else continue;
        for (int t = 0; t < SHOP_TIERS; t++) {
            if (droppers[id] > k_tier_droppers[t]) continue;
            if (pack == 0 && atk < k_tier_atk_floor[t]) continue;
            s_pool[pack][t][s_pool_n[pack][t]++] = (uint16_t)id;
        }
    }
    /* An empty pool falls back down a tier rather than bricking a buy. */
    for (int p = 0; p < SHOP_PACKS; p++)
        for (int t = 1; t < SHOP_TIERS; t++)
            if (!s_pool_n[p][t]) {
                memcpy(s_pool[p][t], s_pool[p][t - 1],
                       (size_t)s_pool_n[p][t - 1] * sizeof(uint16_t));
                s_pool_n[p][t] = s_pool_n[p][t - 1];
            }
    s_pools_built = 1;
}

/* ---- screen + native detection ------------------------------------------- */
static int screen_match(void) {
    if (!s_enabled) return 0;
    if (psx_mod_read_half(SHOP_STATE_ADDR) != 0xE00Du) return 0;
    if (psx_mod_read_word(SHOP_MENUFLAG) != 1u) return 0;  /* menu really open */
    if (psx_mod_read_byte(SHOP_SIG_A) != 0x08u) return 0;
    /* SIG_B was dropped: it read 0xFF only on savestate-restored menus and
     * 0xFE on naturally-entered ones - a counter, not a discriminator. Its
     * false negative is what let an X press leak through to BUILD DECK. */
    if (psx_mod_read_byte(SHOP_SIG_C) != 0x20u) return 0;
    const uint8_t n = psx_mod_read_byte(SHOP_COUNT_ADDR);
    return n == 4u || n == 5u;
}

static int widget_on_our_stream(void) {
    const uint32_t cur = psx_mod_read_word(SHOP_WIDGET1);
    return cur >= SHOP_LBL_BASE + SHOP_ARENA_OFF &&
           cur <  SHOP_LBL_BASE + SHOP_ARENA_OFF + sizeof k_menu_stream + 8u;
}

/* Assert stream + table entry for the NEXT natural menu open. Compare before
 * writing: this runs every gated frame and the bytes rarely change. */
static void assert_stream(void) {
    const uint8_t *st = k_menu_stream;
    const unsigned n  = sizeof k_menu_stream;
    /* The height literal is code: psx_mod_write_code_word routes through the
     * dirty-RAM machinery so the patched instruction actually executes (the
     * LIFE POINTS cheat precedent). Asserted alongside the stream because a
     * savestate load restores stock code bytes too. */
    if (psx_mod_read_word(SHOP_H_PATCH_ADDR) == SHOP_H_STOCK)
        psx_mod_write_code_word(SHOP_H_PATCH_ADDR, SHOP_H_FIVE);
    if (psx_mod_read_word(SHOP_D1_ADDR) == SHOP_D1_STOCK)
        psx_mod_write_code_word(SHOP_D1_ADDR, SHOP_D1_OURS);
    if (psx_mod_read_word(SHOP_D2_ADDR) == SHOP_D2_STOCK)
        psx_mod_write_code_word(SHOP_D2_ADDR, SHOP_D2_OURS);
    if (psx_mod_read_word(SHOP_D3_ADDR) == SHOP_D3_STOCK)
        psx_mod_write_code_word(SHOP_D3_ADDR, SHOP_D3_OURS);
    for (unsigned i = 0; i < n; i++) {
        const uint32_t a = SHOP_LBL_BASE + SHOP_ARENA_OFF + i;
        if (psx_mod_read_byte(a) != st[i])
            psx_mod_write_byte(a, st[i]);
    }
    const uint16_t cur = psx_mod_read_half(SHOP_LBL_TABLE + 17u * 2u);
    if (cur != SHOP_ARENA_OFF) {
        if (cur) s_stock_entry = cur;
        psx_mod_write_half(SHOP_LBL_TABLE + 17u * 2u, SHOP_ARENA_OFF);
    }
}

/* ---- canvas -------------------------------------------------------------- */
#define ROW_W 132
#define ROW_H 18
#define ROW_X 12
#define ROW_Y 111
#define PANEL_W 208
#define PANEL_H 152
#define PANEL_X 56
#define PANEL_Y 34
#define CV_W PANEL_W
#define CV_H PANEL_H
static uint32_t s_px[CV_W * CV_H];
static int s_img_w, s_img_h;

#define C_BAND    0xEE060606u
#define C_FIELD   0xF0101C30u   /* the dialog boxes' deep blue */
#define C_GOLD    0xFFE0B84Cu
#define C_GOLD_D  0xFF8A6E24u
#define C_WHITE   0xFFF0F0F0u
#define C_GREY    0xFFB0B4C0u
#define C_RED     0xFFE06858u
#define C_SEL     0xFF283048u

static void px_fill(int x0, int y0, int w, int h, uint32_t c) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (int y = y0; y < y0 + h && y < CV_H; y++)
        for (int x = x0; x < x0 + w && x < CV_W; x++)
            s_px[y * CV_W + x] = c;
}

/* The stone frame the game's own dialog boxes wear, procedurally: a mottled
 * grey band with chisel shadows, so the panel reads as this game's furniture
 * rather than as a debug rectangle. Deterministic hash noise — no state. */
static void stone_frame(int thick) {
    for (int y = 0; y < CV_H; y++)
        for (int x = 0; x < CV_W; x++) {
            const int edge = x < thick || y < thick ||
                             x >= CV_W - thick || y >= CV_H - thick;
            if (!edge) continue;
            uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
            h = (h ^ (h >> 13)) * 1274126177u;
            int v = 132 + (int)((h >> 28) & 0xFu) * 6;
            if (x == 0 || y == 0 || x == CV_W - 1 || y == CV_H - 1) v -= 46;
            if (x == thick - 1 || y == thick - 1 ||
                x == CV_W - thick || y == CV_H - thick) v -= 28;
            if (v < 0) v = 0;
            s_px[y * CV_W + x] = 0xFF000000u | ((uint32_t)v * 0x010101u);
        }
}

static int put_glyph(int cell, int x0, int y0, uint32_t tint) {
    const PsxFusionFont *f = &psx_fusion_font;
    if (cell < 0) return 4;
    const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
    int hi = 0;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++) {
            const uint8_t v = g[y * f->w + x];
            if (!v) continue;
            if (x > hi) hi = x;
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= CV_W || py >= CV_H) continue;
            if (v == 1) { s_px[py * CV_W + px] = 0xFF101010u; continue; }
            const uint32_t k = v * 17u > 255u ? 255u : v * 17u;
            const uint32_t r = ((tint >> 16 & 0xFFu) * k) / 255u;
            const uint32_t gg = ((tint >> 8 & 0xFFu) * k) / 255u;
            const uint32_t b = ((tint & 0xFFu) * k) / 255u;
            s_px[py * CV_W + px] = 0xFF000000u | r << 16 | gg << 8 | b;
        }
    return hi + 1;
}
static int put_text(const char *s, int x, int y, uint32_t tint) {
    for (; *s; s++) {
        if (*s == ' ') { x += 4; continue; }
        const int w = put_glyph(psx_fusion_font_cell((unsigned char)*s), x, y, tint);
        x += (w > 2 ? w : 4) + 1;
    }
    return x;
}

static void draw_panel(void) {
    s_img_w = PANEL_W; s_img_h = PANEL_H;
    memset(s_px, 0, sizeof s_px);
    px_fill(0, 0, PANEL_W, PANEL_H, C_FIELD);
    stone_frame(7);
    px_fill(7, 7, PANEL_W - 14, 1, C_GOLD_D);
    px_fill(7, PANEL_H - 8, PANEL_W - 14, 1, C_GOLD_D);

    put_text("CARD SHOP", 14, 10, C_GOLD);
    char line[36];
    snprintf(line, sizeof line, "CHIPS %u",
             (unsigned)psx_mod_read_word(SHOP_CHIPS_ADDR));
    put_text(line, 132, 10, C_WHITE);
    px_fill(10, 24, PANEL_W - 20, 1, C_GOLD_D);

    build_pools();
    for (int i = 0; i < SHOP_PACKS; i++) {
        const int y = 28 + i * 14;
        const int tier = s_tier[i];
        if (i == s_sel) px_fill(9, y - 1, PANEL_W - 18, 14, C_SEL);
        put_text(i == s_sel ? ">" : " ", 12, y, C_GOLD);
        put_text(k_packs[i].name, 20, y, C_WHITE);
        put_text(k_tier_names[tier], 84, y,
                 tier == 0 ? C_GREY : tier == 1 ? C_WHITE
                 : tier == 2 ? C_GOLD : C_RED);
        snprintf(line, sizeof line, "%d",
                 k_packs[i].base_price * k_tier_mult[tier]);
        put_text(line, 172, y, C_GOLD);
    }
    px_fill(10, 86, PANEL_W - 20, 1, C_GOLD_D);

    if (s_msg[0]) put_text(s_msg, 12, 90, s_pull_n ? C_GREY : C_RED);
    for (int i = 0; i < s_pull_n && i < 3; i++) {
        const char *nm = psx_card_db_name(s_pull[i]);
        snprintf(line, sizeof line, "%.22s", nm ? nm : "?");
        put_text(line, 14, 103 + i * 12, C_WHITE);
    }
    if (s_pull_n > 3) {
        snprintf(line, sizeof line, "+%d MORE IN TRUNK", s_pull_n - 3);
        put_text(line, 14, 103 + 3 * 12, C_GREY);
    }
    if (!s_pull_n && !s_msg[0])
        put_text("X BUY  <> RARITY  O CLOSE", 30, 108, C_GREY);
}

/* ---- purchase ------------------------------------------------------------ */
static void grant_card(int id) {
    const uint32_t off = SHOP_TRUNK_OFF + (uint32_t)(id - 1);
    const uint8_t cur = psx_mod_read_byte(SHOP_SAVE_LIVE + off);
    const uint8_t nxt = cur < 250u ? (uint8_t)(cur + 1u) : cur;
    psx_mod_write_byte(SHOP_SAVE_LIVE + off, nxt);
    psx_mod_write_byte(SHOP_SAVE_MIRROR + off, nxt);
}

static void buy(int pack) {
    const int tier = s_tier[pack];
    const int price = k_packs[pack].base_price * k_tier_mult[tier];
    build_pools();
    if (!s_pools_built || !s_pool_n[pack][tier]) {
        snprintf(s_msg, sizeof s_msg, "SHOP NOT STOCKED YET");
        s_pull_n = 0; s_denied++; return;
    }
    if (!save_live()) {
        snprintf(s_msg, sizeof s_msg, "NO SAVE LOADED");
        s_pull_n = 0; s_denied++; return;
    }
    const uint32_t chips = psx_mod_read_word(SHOP_CHIPS_ADDR);
    if (chips < (uint32_t)price) {
        snprintf(s_msg, sizeof s_msg, "NOT ENOUGH CHIPS");
        s_pull_n = 0; s_denied++; return;
    }
    psx_mod_write_word(SHOP_CHIPS_ADDR, chips - (uint32_t)price);
    s_pull_n = 0;
    for (int i = 0; i < k_packs[pack].cards && i < SHOP_PULL_MAX; i++) {
        const int n = s_pool_n[pack][tier];
        const int id = s_pool[pack][tier][rng_next() % (uint32_t)n];
        grant_card(id);
        s_pull[s_pull_n++] = id;
    }
    snprintf(s_msg, sizeof s_msg, "%s %s:",
             k_tier_names[tier], k_packs[pack].name);
    s_buys++;
}

/* ---- per-frame driver ---------------------------------------------------- */
static void restore_stock_code(void) {
    /* The four patched immediates must not outlive the mod being enabled:
     * with the STOCK four-line stream, the shifted dispatch would send
     * BUILD DECK to RETURN TO TITLE's row and worse. Symmetric restore. */
    if (psx_mod_read_word(SHOP_H_PATCH_ADDR) == SHOP_H_FIVE)
        psx_mod_write_code_word(SHOP_H_PATCH_ADDR, SHOP_H_STOCK);
    if (psx_mod_read_word(SHOP_D1_ADDR) == SHOP_D1_OURS)
        psx_mod_write_code_word(SHOP_D1_ADDR, SHOP_D1_STOCK);
    if (psx_mod_read_word(SHOP_D2_ADDR) == SHOP_D2_OURS)
        psx_mod_write_code_word(SHOP_D2_ADDR, SHOP_D2_STOCK);
    if (psx_mod_read_word(SHOP_D3_ADDR) == SHOP_D3_OURS)
        psx_mod_write_code_word(SHOP_D3_ADDR, SHOP_D3_STOCK);
    /* And the label table: point entry 17 back at the stock stream. */
    if (s_stock_entry &&
        psx_mod_read_half(SHOP_LBL_TABLE + 17u * 2u) == SHOP_ARENA_OFF)
        psx_mod_write_half(SHOP_LBL_TABLE + 17u * 2u, s_stock_entry);
}

void psx_card_shop_tick(void) {
    if (!s_enabled) { restore_stock_code(); return; }
    const int gate = screen_match();
    if (gate != s_gate) { s_gate = gate; s_dirty = 1; }
    if (!gate) {
        if (s_open) { s_open = 0; s_dirty = 1; }
        s_native = 0;
        return;
    }

    /* Stage the stream + table for the next natural open, every frame: the
     * overlay reload and savestate loads both restore stock bytes. */
    assert_stream();

    const int native = widget_on_our_stream();
    if (native != s_native) { s_native = native; s_dirty = 1; }
    /* The header carries count=5, but the init's header read lands on a
     * different stream byte depending on how the greeting was skipped
     * (observed live: our five labels drawn, count byte 4, LEAVE SHOP
     * unreachable). The cursor driver reads this byte LIVE, so asserting it
     * here fixes navigation regardless of what the init sampled. Only 4->5:
     * the menu's done-path legitimately parks other values here. */
    if (native && psx_mod_read_byte(SHOP_COUNT_ADDR) == 4u)
        psx_mod_write_byte(SHOP_COUNT_ADDR, (uint8_t)SHOP_ROWS);
    if (!native) {
        /* Menu is showing stock labels (pre-repoint open, or a savestate
         * taken before this feature). Behave as absent: no fifth row, no
         * remap, stock count. */
        if (s_open) { s_open = 0; s_dirty = 1; }
        return;
    }

    const uint16_t np = psx_mod_read_half(SHOP_PAD_NEW_ADDR);
    const int cursor = (int)psx_mod_read_byte(SHOP_CURSOR_ADDR);

    if (!s_open) {
        static int prev_cursor = -1;
        if (cursor != prev_cursor) { prev_cursor = cursor; s_dirty = 1; }
        if (np & SHOP_NP_CROSS) {
            if (cursor == SHOP_ROW) {
                /* CARD SHOP: with the dispatch chain shifted, the game's own
                 * default case handles this press (dialog closes, nothing
                 * else). The panel opens on top; on close the player talks
                 * to the shopkeeper again, exactly like leaving any of the
                 * game's own sub-screens. */
                s_open = 1; s_sel = 0; s_msg[0] = 0; s_pull_n = 0; s_opens++;
                s_rng ^= psx_mod_read_word(0x8009B0C4u) * 2654435761u;
                s_dirty = 1;
            }
            /* Rows 2..4 dispatch natively through the patched chain. */
        }
        return;
    }

    /* Panel open: pin the game's cursor on the no-op index and eat everything
     * we act on — O would otherwise leave the whole shop screen. */
    psx_mod_write_byte(SHOP_CURSOR_ADDR, (uint8_t)(SHOP_ROWS - 1));
    uint16_t eat = 0;
    if (np & SHOP_NP_UP)    { s_sel = (s_sel + SHOP_PACKS - 1) % SHOP_PACKS; eat |= SHOP_NP_UP; s_dirty = 1; }
    if (np & SHOP_NP_DOWN)  { s_sel = (s_sel + 1) % SHOP_PACKS;              eat |= SHOP_NP_DOWN; s_dirty = 1; }
    if (np & SHOP_NP_LEFT)  { s_tier[s_sel] = (s_tier[s_sel] + SHOP_TIERS - 1) % SHOP_TIERS; eat |= SHOP_NP_LEFT; s_dirty = 1; }
    if (np & SHOP_NP_RIGHT) { s_tier[s_sel] = (s_tier[s_sel] + 1) % SHOP_TIERS;              eat |= SHOP_NP_RIGHT; s_dirty = 1; }
    if (np & SHOP_NP_CROSS) { buy(s_sel); eat |= SHOP_NP_CROSS; s_dirty = 1; }
    if (np & SHOP_NP_CIRCLE){ s_open = 0; eat |= SHOP_NP_CIRCLE; s_dirty = 1; }
    if (eat)
        psx_mod_write_half(SHOP_PAD_NEW_ADDR, (uint16_t)(np & ~eat));
    static uint32_t last_chips;
    const uint32_t chips = psx_mod_read_word(SHOP_CHIPS_ADDR);
    if (chips != last_chips) { last_chips = chips; s_dirty = 1; }
}

/* ---- overlay contract ---------------------------------------------------- */
int psx_card_shop_image(const uint32_t **px, int *w, int *h) {
    if (!s_gate || !s_native) return 0;
    if (!s_open) return 0;   /* all five rows are native; no band */
    draw_panel();
    *px = s_px; *w = s_img_w; *h = s_img_h;
    return 1;
}
void psx_card_shop_origin(int *x, int *y) {
    if (s_open) { *x = PANEL_X; *y = PANEL_Y; }
    else        { *x = ROW_X;   *y = ROW_Y;   }
}
int psx_card_shop_needs_present(void) {
    const int d = s_dirty; s_dirty = 0; return d;
}

/* ---- menu + debug -------------------------------------------------------- */
static void shop_changed(int v) { s_enabled = v ? 1 : 0; s_dirty = 1; }

void psx_card_shop_register_menu(void) {
    static const char *const ONOFF[] = { "OFF", "ON" };
    static const char *const HINTS[] = {
        "A CARD SHOP ROW ON THE SHOPKEEPER MENU",
        "BUY CARD PACKS WITH STARCHIPS AT THE SHOPKEEPER",
    };
    s_row_handle = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "CARD SHOP", HINTS[0],
        ONOFF, 2, "card_shop", 1, shop_changed);
    psx_video_menu_set_row_hints(s_row_handle, HINTS);
}

int psx_card_shop_state_json(char *out, unsigned cap) {
    const int t_state = psx_mod_read_half(SHOP_STATE_ADDR) == 0xE00Du;
    const int t_menu  = psx_mod_read_word(SHOP_MENUFLAG) == 1u;
    const int t_sig   = psx_mod_read_byte(SHOP_SIG_A) == 0x08u &&
                        psx_mod_read_byte(SHOP_SIG_C) == 0x20u;
    return snprintf(out, cap,
        "\"t_state\":%d,\"t_menu\":%d,\"t_sig\":%d,"
        "\"hpatch\":%u,"
        "\"enabled\":%d,\"gate\":%d,\"native\":%d,\"open\":%d,\"sel\":%d,"
        "\"tier\":%d,\"count_byte\":%u,\"cursor\":%u,\"chips\":%u,"
        "\"table17\":%u,\"stock_entry\":%u,\"widget_cur\":%u,"
        "\"pools\":[%d,%d,%d,%d],"
        "\"buys\":%u,\"denied\":%u,\"opens\":%u,\"remaps\":%u",
        t_state, t_menu, t_sig,
        (unsigned)(psx_mod_read_word(SHOP_H_PATCH_ADDR) == SHOP_H_FIVE),
        s_enabled, s_gate, s_native, s_open, s_sel, s_tier[s_sel],
        (unsigned)psx_mod_read_byte(SHOP_COUNT_ADDR),
        (unsigned)psx_mod_read_byte(SHOP_CURSOR_ADDR),
        (unsigned)psx_mod_read_word(SHOP_CHIPS_ADDR),
        (unsigned)psx_mod_read_half(SHOP_LBL_TABLE + 17u * 2u),
        (unsigned)s_stock_entry,
        (unsigned)psx_mod_read_word(SHOP_WIDGET1),
        s_pool_n[0][0] + s_pool_n[0][1] + s_pool_n[0][2] + s_pool_n[0][3],
        s_pool_n[1][0] + s_pool_n[1][1] + s_pool_n[1][2] + s_pool_n[1][3],
        s_pool_n[2][0] + s_pool_n[2][1] + s_pool_n[2][2] + s_pool_n[2][3],
        s_pool_n[3][0] + s_pool_n[3][1] + s_pool_n[3][2] + s_pool_n[3][3],
        s_buys, s_denied, s_opens, s_remaps);
}
