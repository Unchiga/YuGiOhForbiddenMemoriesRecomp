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
 * and this module behaves as if absent until the next natural open.
 *
 * On CARD SHOP the shopkeeper first asks "What are you looking for?" in the
 * game's own textbox: a second arena stream is repointed under a dead label
 * table entry and the campaign dialog widget (SHOP_DLG_*) is started the
 * same way the scene starts the greeting. The engine types it, waits, and
 * dismisses on X natively; the pack panel opens on that dismissal. */

#include "psx_card_shop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_card_db.h"
#include "psx_drop_db.h"
#include "psx_fusion_font.h"
#include "gpu_render.h"
#include "psx_game_hooks.h"
#include "psx_shop_skin.h"
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
#define SHOP_SAY_OFF      0xFFD8u       /* dialog stream, past the menu one */
#define SHOP_SAY_ID       252u          /* dead table entry (offset 0; 143
                                         * such entries measured live)      */

/* The campaign dialog widget slot — the textbox the shopkeeper talks in.
 * Measured live 2026-08-22 (greeting "Hello there!" = string id 1086):
 *   +0  u32 stream cursor (the id resolver at 0x8003944C stores it)
 *   +52 mode  byte: 0x0a dialog active, 0x02 idle
 *   +53 phase byte: 0x80 start-pending, 0xc0 typing, 0xe0 done/waiting
 *   +54 u16 string id (direct band: <0x500 = label-table index)
 *   +56 pen x
 * Writing id+mode+phase starts a line; the engine types it, shows the wait
 * arrow, and its own X handler drops mode to 0x02. */
#define SHOP_DLG_MODE   0x800EB12Cu
#define SHOP_DLG_PHASE  0x800EB12Du
#define SHOP_DLG_ID     0x800EB12Eu
#define SHOP_DLG_PENX   0x800EB130u

/* The campaign menu cursor driver. Its first loads are the new-press mask
 * (0x8009B394 & 0x5008: UP|DOWN|START); the frame hook's press-eating can
 * NEVER beat it - the driver reads and acts within the same guest frame,
 * before any host tick runs - so eaten arrows still stepped the menu
 * highlight behind the pack panel. An entry hook fires exactly before that
 * first read, which is the only race-free place to take the arrows away. */
#define SHOP_MENU_NAV_FN 0x8003700Cu

/* The game's own sound-effect dispatcher, play_se(a0 = id): found via the
 * cursor driver's jal at 0x800370E8 (it plays id 6 on every menu step) and
 * confirmed with fntrace. Ids captured live 2026-08-22:
 *   6    the menu cursor tick (every campaign menu move)
 *   9    the deck builder's "nothing here" thunk (X on a blank slot)
 *   0x30 the password screen's reward chime (the accept press also plays
 *        0x0C first; the chime is the part that reads as "purchased")
 * Calls are made from the driver-entry hook - guest context - with the
 * card_drops $ra rule, around a full CPUState snapshot so the driver call
 * in flight is untouched. SHOP_SE_RET is the driver's own return site for
 * its play_se call, i.e. a real post-call address. */
#define SHOP_SE_FN      0x8003FEE0u
#define SHOP_SE_RET     0x800370ECu
#define SHOP_SE_CURSOR  0x06u
#define SHOP_SE_DENY    0x09u
#define SHOP_SE_BUY     0x30u
#define SHOP_SE_LEAVE   0x08u   /* the dispatch chain's LEAVE SHOP sound */
#define SHOP_SE_FLIP    0x0Cu   /* the password screen's confirm click   */

/* The game's subscreen mailbox and pump. Screens with subscreens (chest,
 * deck build) write a request into gp-relative bytes and call the pump
 * every frame; command 2 with type 0x14 is THE CARD VIEWER (the triangle
 * viewer from duels and the deck builder). Measured at the chest: writing
 * card id + type + command from the debug server alone opened the viewer,
 * art streamed from disc and all - the pump reads only the mailbox and
 * allocates from system pools, so its a0 is ignorable. The shop screen
 * never pumps, so while our viewer is up the driver-entry hook pumps once
 * per frame; the pump clears the command byte back to 0 when the viewer
 * closes on Circle. */
#define SHOP_SUB_CARD   0x8009B246u   /* u16 card id for the viewer        */
#define SHOP_SUB_TYPE   0x8009B24Bu   /* 0x14 on every chest viewer open   */
#define SHOP_SUB_CMD    0x8009B254u   /* 2 = spawn; pump acks with |0x80   */
#define SHOP_SUB_PUMP   0x8002892Cu
#define SHOP_SUB_RET    0x80033C48u   /* the chest tick's own return site  */
/* NOTE: 0x800282E8 (the viewer entry's first call) is a full screen
 * teardown, not a backdrop clear - calling it per frame wiped the
 * viewer's own draw list and left the shop room showing. The viewer
 * composites over whatever the shop already drew, which reads fine. */

/* The game's own card award (trunk count + the 15-slot New! ring). Called
 * with the DUEL reward's return address so the CARD DROPS extended New!
 * tracker counts shop cards exactly like duel drops. */
#define SHOP_AWARD_FN   0x80021894u
#define SHOP_AWARD_RET  0x80021F1Cu

#define SHOP_NP_TRIANGLE 0x0010u     /* byte-swapped new-press bit */
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

/* "What are you looking for?" — the shopkeeper's line when CARD SHOP is
 * picked, in the game's frequency-ordered codes ('?' = 0x2E, decode table
 * at 0x801D9000 maps it to Shift-JIS 0x8148). Plain glyphs, FF terminator:
 * the dialog face needs no header. */
static const uint8_t k_say_stream[] = {
    0x22, 0x09, 0x03, 0x02, 0x00,                    /* What  */
    0x03, 0x08, 0x01, 0x00,                          /* are   */
    0x11, 0x04, 0x0D, 0x00,                          /* you   */
    0x0A, 0x04, 0x04, 0x16, 0x05, 0x06, 0x10, 0x00,  /* looking */
    0x13, 0x04, 0x08, 0x2E,                          /* for?  */
    0xFF,
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
/* LEFT/RIGHT walks the rarity tier; the price is the tier's, same for
 * every pack type, as the user specced. */
typedef struct { const char *name; int cards; } ShopPack;
static const ShopPack k_packs[] = {
    { "MONSTER", 3 },
    { "MAGIC",   3 },
    { "EQUIP",   3 },
    { "TRAP",    3 },
};
#define SHOP_PACKS 4
#define SHOP_TIERS 4
static const char *const k_tier_names[SHOP_TIERS] =
    { "COMMON", "UNCOMMON", "RARE", "LEGENDARY" };
static const int k_tier_price[SHOP_TIERS] = { 20, 80, 200, 800 };
#define SHOP_PULL_MAX 5
/* Rows the results box can print. A pull name is a 12-row glyph and the box
 * is a fixed piece of the password screen's furniture, so this is a hard
 * ceiling, not a preference: the ini's `cards` is bounded by it rather than
 * letting a pack award cards the player is never shown. */
#define SHOP_PULL_ROWS 3
/* A tier the player pays for should never be a two-card lottery: any pool
 * shorter than this borrows the tier(s) below it for variety. */
#define SHOP_POOL_MIN 12

/* Home-tier placements the user pinned by name; matched against the game's
 * own decoded names so no id table can rot. */
static const char *const k_force_legendary[] = {
    "Exodia the Forbidden One",      /* the win condition itself */
    "Swords of Revealing Light",
    "Raigeki",
    "Megamorph",
    "Widespread Ruin",               /* judgment: FM's heaviest traps */
    "Acid Trap Hole",
    "Invisible Wire",
};
static const char *const k_force_rare[] = {
    "Crush Card",
    "Bright Castle",
    "Dragon Capture Jar",
    "Dragon Treasure",
    /* judgment: the limbs are chase cards but not the win itself */
    "Right Arm of the Forbidden One",
    "Left Arm of the Forbidden One",
    "Right Leg of the Forbidden One",
    "Left Leg of the Forbidden One",
    "Dark Hole",
};
/* Cards the user wants in BOTH the rare and legendary pools. */
static const char *const k_dual_rare_legendary[] = {
    "Bright Castle",
    "Dragon Treasure",
};

static int name_in(const char *nm, const char *const *list, int n) {
    for (int i = 0; i < n; i++) {
        const char *a = nm, *b = list[i];
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}
#define NAME_IN(nm, list) name_in(nm, list, (int)(sizeof list / sizeof *list))

/* ---- card_shop.ini ------------------------------------------------------- */
/* A plain, hand-editable file in the player-data directory (beside
 * menu_settings.ini and the savestates). Written with the built-in values
 * the first time the shop builds its pools, so the file itself documents
 * every knob; edited values are picked up on the next pool build (leaving
 * the shop screen, or a savestate load, is enough).
 *
 *   [prices]      one price per rarity
 *   [monster]     the ATK cut-offs for monster rarity
 *   [pools]       min_choices, the variety floor
 *   [cards]       NAME = RARITY lines, one per forced placement, where
 *                 RARITY is common|uncommon|rare|legendary, optionally
 *                 two of them ("rare+legendary") for a card that should
 *                 appear in both pools.
 */
#define SHOP_INI_NAME "card_shop.ini"
#define SHOP_CFG_FORCED_MAX 64

typedef struct { char name[40]; uint8_t mask; } ShopForced;
static int        s_cfg_price[SHOP_TIERS] = { 20, 80, 200, 800 };
static int        s_cfg_atk[3]            = { 2500, 1600, 850 };
static int        s_cfg_pool_min          = SHOP_POOL_MIN;
static int        s_cfg_pack_cards        = 3;
static ShopForced s_cfg_forced[SHOP_CFG_FORCED_MAX];
static int        s_cfg_forced_n;
static int        s_cfg_loaded;

static int shop_ini_path(char *out, unsigned cap) {
    const char *dir = psx_mod_player_data_dir();
    if (!dir || !dir[0]) return 0;
    const int n = snprintf(out, cap, "%s/%s", dir, SHOP_INI_NAME);
    return n > 0 && (unsigned)n < cap;
}

static void cfg_trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                     e[-1] == '\t')) *--e = 0;
}

static int cfg_rarity_mask(const char *v) {
    static const char *const names[SHOP_TIERS] =
        { "common", "uncommon", "rare", "legendary" };
    int mask = 0;
    for (const char *p = v; *p; ) {
        while (*p == ' ' || *p == '+' || *p == ',') p++;
        if (!*p) break;
        for (int t = 0; t < SHOP_TIERS; t++) {
            const char *a = p, *b = names[t];
            while (*b && *a) {
                char ca = *a; if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                if (ca != *b) break;
                a++; b++;
            }
            if (!*b) { mask |= 1 << t; p = a; break; }
        }
        while (*p && *p != '+' && *p != ',') p++;
    }
    return mask;
}

static void shop_cfg_write_default(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# CARD SHOP - pack prices, rarity bands and card placements.\n"
        "# Edit and reload the shop screen (leaving and re-entering the\n"
        "# shop rebuilds the pools). Delete this file to restore defaults.\n"
        "\n[prices]\n"
        "common    = %d\nuncommon  = %d\nrare      = %d\nlegendary = %d\n"
        "\n[packs]\n"
        "# cards drawn per pack (1-3: the results box prints three)\n"
        "cards = %d\n"
        "\n[monster]\n"
        "# a monster lands in the highest band its ATK reaches\n"
        "legendary_atk = %d\nrare_atk      = %d\nuncommon_atk  = %d\n"
        "\n[pools]\n"
        "# a pool shorter than this borrows whole tiers below it\n"
        "min_choices = %d\n"
        "\n[cards]\n"
        "# NAME = rarity   (or rare+legendary to put a card in both)\n",
        s_cfg_price[0], s_cfg_price[1], s_cfg_price[2], s_cfg_price[3],
        s_cfg_pack_cards, s_cfg_atk[0], s_cfg_atk[1], s_cfg_atk[2],
        s_cfg_pool_min);
    for (unsigned i = 0; i < sizeof k_force_legendary / sizeof *k_force_legendary; i++)
        fprintf(f, "%s = legendary\n", k_force_legendary[i]);
    for (unsigned i = 0; i < sizeof k_force_rare / sizeof *k_force_rare; i++) {
        const char *nm = k_force_rare[i];
        fprintf(f, "%s = %s\n", nm,
                NAME_IN(nm, k_dual_rare_legendary) ? "rare+legendary" : "rare");
    }
    fclose(f);
}

static void shop_cfg_load(void) {
    if (s_cfg_loaded) return;
    s_cfg_loaded = 1;
    char path[512];
    if (!shop_ini_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) { shop_cfg_write_default(path); return; }
    s_cfg_forced_n = 0;
    char line[160], sect[24] = "";
    while (fgets(line, sizeof line, f)) {
        cfg_trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (!e) continue;
            *e = 0;
            snprintf(sect, sizeof sect, "%s", p + 1);
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        cfg_trim(p);
        if (!strcmp(sect, "prices")) {
            const int n = atoi(v);
            if (n <= 0) continue;
            if      (!strcmp(p, "common"))    s_cfg_price[0] = n;
            else if (!strcmp(p, "uncommon"))  s_cfg_price[1] = n;
            else if (!strcmp(p, "rare"))      s_cfg_price[2] = n;
            else if (!strcmp(p, "legendary")) s_cfg_price[3] = n;
        } else if (!strcmp(sect, "packs")) {
            const int n = atoi(v);
            if (!strcmp(p, "cards") && n >= 1 && n <= SHOP_PULL_ROWS)
                s_cfg_pack_cards = n;
        } else if (!strcmp(sect, "monster")) {
            const int n = atoi(v);
            if (n < 0) continue;
            if      (!strcmp(p, "legendary_atk")) s_cfg_atk[0] = n;
            else if (!strcmp(p, "rare_atk"))      s_cfg_atk[1] = n;
            else if (!strcmp(p, "uncommon_atk"))  s_cfg_atk[2] = n;
        } else if (!strcmp(sect, "pools")) {
            const int n = atoi(v);
            if (!strcmp(p, "min_choices") && n >= 0) s_cfg_pool_min = n;
        } else if (!strcmp(sect, "cards")) {
            const int mask = cfg_rarity_mask(v);
            if (!mask || s_cfg_forced_n >= SHOP_CFG_FORCED_MAX) continue;
            ShopForced *e = &s_cfg_forced[s_cfg_forced_n++];
            snprintf(e->name, sizeof e->name, "%s", p);
            e->mask = (uint8_t)mask;
        }
    }
    fclose(f);
}

/* A card's configured placement mask, 0 if the file does not mention it. */
static int cfg_mask_for(const char *nm) {
    for (int i = 0; i < s_cfg_forced_n; i++)
        if (name_in(nm, (const char *const *)&(const char *){ s_cfg_forced[i].name }, 1))
            return s_cfg_forced[i].mask;
    return 0;
}

/* Home tiers: monsters by the user's ATK brackets (2500+ legendary,
 * 1600-2450 rare, 850-1550 uncommon, the rest common), everything else by
 * how hard the game guards it (droppers = how many of the 39 duelists ever
 * drop it; 0 means password-only in the stock game). */
static int card_rarity(int id, int atk, int type, int droppers) {
    const char *nm = psx_card_db_name(id);
    const int m = cfg_mask_for(nm);
    if (m) {   /* the file wins; its home tier is the highest bit set */
        for (int t = SHOP_TIERS - 1; t >= 0; t--) if (m & (1 << t)) return t;
    }
    if (type <= 19)
        return atk >= s_cfg_atk[0] ? 3 : atk >= s_cfg_atk[1] ? 2
             : atk >= s_cfg_atk[2] ? 1 : 0;
    if (droppers == 0) return 2;
    if (droppers <= 3) return 1;
    return 0;
}

/* Type codes (psx_card_db TYPE_NAMES order): 0..19 monsters, then: */
#define TYPE_MAGIC  20
#define TYPE_TRAP   21
#define TYPE_RITUAL 22
#define TYPE_EQUIP  23

/* ---- state --------------------------------------------------------------- */
static int      s_enabled;           /* off until the player opts in       */
static int      s_row_handle;
static int      s_gate;              /* screen signature matched            */
static int      s_native;            /* widget is displaying OUR stream     */
static int      s_open;              /* pack panel is up                    */
static int      s_sel;               /* selected pack 0..3                  */
static int      s_tier[SHOP_PACKS];  /* per-pack rarity tier                */
static int      s_dirty = 1;
static int      s_anim;              /* arrow animation frame               */
static uint32_t s_rng = 0x5EEDCA5Du;
static int      s_pull[SHOP_PULL_MAX];
static int      s_pull_n;
static int      s_shown;             /* pack-open ceremony: cards revealed  */
static int      s_card_sel;          /* browse highlight after the reveal   */
static int      s_ceremony;          /* 1 from buy until X in browse mode   */
static int      s_view;              /* 0 idle, 1 requested, 2 viewer alive */
static uint16_t s_view_card;
/* The shop screen never stocks the viewer's template atlas, and the viewer
 * streams each card's face into pages the shop is actively displaying. So
 * around every viewer open: save the three VRAM regions it touches, upload
 * the baked template to (832,0), and put everything back on close - which
 * is also what removes the "distorted card in the corner" the face upload
 * used to leave in the shop's own room texture. */
static uint16_t s_vs_tmpl[64 * 96];      /* (832,0)   template area  */
static uint16_t s_vs_face[128 * 256];    /* (768,256) body canvas    */
static uint16_t s_vs_clut[448 * 16];     /* (256,240) palette band   */
static uint16_t s_vs_face2[64 * 128];    /* (0,256)   face slot: from the
                                          * SHOP the viewer streams each
                                          * card's art here, not to the
                                          * chest's (768,256) - measured
                                          * by diffing VRAM after a view */
static uint16_t s_vs_clut2[256 * 1];     /* (0,255)   the face's palette */
static uint16_t s_vs_glyphs[64 * 128];   /* (960,384) orb/stars/ATK glyphs */
static uint16_t s_vs_back[64 * 64];      /* (896,256) the card back        */
static int      s_vs_valid;
static uint16_t s_award_q[SHOP_PULL_MAX];
static int      s_award_n;
static char     s_msg[30];
static uint16_t s_stock_entry;       /* table[17] before our repoint        */
static int      s_say;               /* 0 idle, 1 kick pending, 2 line up   */
static int      s_say_timer;         /* frames until the pending kick       */
static uint16_t s_say_stock;         /* table[SHOP_SAY_ID] before repoint   */
static int      s_say_stock_ok;
static uint16_t s_nav_latch;         /* arrows eaten at the driver's door   */
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
    shop_cfg_load();
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
    static uint8_t home[PSX_CARD_DB_COUNT + 1];
    static int8_t  hpack[PSX_CARD_DB_COUNT + 1];
    memset(home, 0, sizeof home);
    memset(hpack, -1, sizeof hpack);
    for (int id = 1; id <= PSX_CARD_DB_COUNT; id++) {
        int atk = 0, def = 0, type = 0;
        if (!psx_card_db_stats(id, &atk, &def, &type)) continue;
        int pack;
        if (type <= 19)                                     pack = 0;
        else if (type == TYPE_MAGIC || type == TYPE_RITUAL) pack = 1;
        else if (type == TYPE_EQUIP)                        pack = 2;
        else if (type == TYPE_TRAP)                         pack = 3;
        else continue;
        const int t = card_rarity(id, atk, type, droppers[id]);
        hpack[id] = (int8_t)pack;
        home[id] = (uint8_t)t;
        s_pool[pack][t][s_pool_n[pack][t]++] = (uint16_t)id;
        /* A card listed with two rarities appears in both pools. */
        const int m = cfg_mask_for(psx_card_db_name(id));
        for (int tt = 0; tt < SHOP_TIERS; tt++)
            if (tt != t && (m & (1 << tt)))
                s_pool[pack][tt][s_pool_n[pack][tt]++] = (uint16_t)id;
    }
    /* Variety floor: a short pool borrows whole tiers below it until it
     * offers a real spread (a legendary TRAP pack would otherwise be a
     * three-card lottery - the game only has 18 traps). */
    for (int p = 0; p < SHOP_PACKS; p++)
        for (int t = SHOP_TIERS - 1; t >= 1; t--)
            for (int tt = t - 1; tt >= 0 && s_pool_n[p][t] < s_cfg_pool_min; tt--)
                for (int id = 1; id <= PSX_CARD_DB_COUNT; id++)
                    if (hpack[id] == p && home[id] == (uint8_t)tt)
                        s_pool[p][t][s_pool_n[p][t]++] = (uint16_t)id;
    s_pools_built = 1;
}

/* ---- screen + native detection ------------------------------------------- */
static int widget_on_menu17(void) {
    /* Is the campaign menu widget displaying string 17 - the shopkeeper's
     * menu labels? True for our arena stream and for the stock stream (the
     * pre-repoint table entry up to table[18], the next stream's start). */
    const uint32_t cur = psx_mod_read_word(SHOP_WIDGET1);
    if (cur >= SHOP_LBL_BASE + SHOP_ARENA_OFF &&
        cur <  SHOP_LBL_BASE + SHOP_ARENA_OFF + sizeof k_menu_stream + 8u)
        return 1;
    uint16_t e17 = psx_mod_read_half(SHOP_LBL_TABLE + 17u * 2u);
    if (e17 == SHOP_ARENA_OFF) e17 = s_stock_entry;
    const uint16_t e18 = psx_mod_read_half(SHOP_LBL_TABLE + 18u * 2u);
    if (!e17 || e18 <= e17) return 0;
    return cur >= SHOP_LBL_BASE + e17 && cur < SHOP_LBL_BASE + e18;
}

static int greeting_live(void) {
    /* The shopkeeper's "Hello there!" (string 1086) playing in the dialog
     * widget. The id is stored either direct or in +256 bank form. */
    if (psx_mod_read_byte(SHOP_DLG_MODE) != 0x0Au) return 0;
    const uint16_t id = psx_mod_read_half(SHOP_DLG_ID);
    return id == 1086u || id == 1086u + 256u;
}

static int menu_widget_live(void) {
    /* The campaign menu widget's mode byte: 0x26 while a menu is open on
     * screen, 0x00 once the screen tears it down. Measured 2026-08-22:
     * without this the widget's stale stream cursor kept the gate - and a
     * left-open panel - alive on the map and even on the title screen. */
    return psx_mod_read_byte(SHOP_WIDGET1 + 52u) != 0u;
}

static int screen_match(void) {
    if (!s_enabled) return 0;
    if (psx_mod_read_word(SHOP_MENUFLAG) != 1u) return 0;
    /* The greeting arms the gate before the menu's first open. */
    if (greeting_live()) return 1;
    /* Everything below is about the MENU, so the menu widget must be live;
     * a dead widget means the shop screen is gone and all its bytes are
     * stale, whatever they say. */
    if (!menu_widget_live()) return 0;
    /* Our own flow keeps the gate through widget-content transitions. */
    if (s_say || s_open) return 1;
    /* Signature 1: the campaign overlay state left by entering the shop
     * FROM THE MAP. A save made inside the shop resumes with ALL of these
     * zero (measured on a natural CAMPAIGN resume, 2026-08-22, where the
     * old gate never matched and the menu stayed stock) - so this is one
     * sufficient signature, not a requirement.
     * SIG_B was dropped earlier: it read 0xFF only on savestate-restored
     * menus and 0xFE on naturally-entered ones - a counter, not a
     * discriminator. */
    if (psx_mod_read_half(SHOP_STATE_ADDR) == 0xE00Du &&
        psx_mod_read_byte(SHOP_SIG_A) == 0x08u &&
        psx_mod_read_byte(SHOP_SIG_C) == 0x20u) {
        const uint8_t n = psx_mod_read_byte(SHOP_COUNT_ADDR);
        if (n == 4u || n == 5u) return 1;
    }
    /* Signature 2: the shopkeeper menu itself is on screen. */
    return widget_on_menu17();
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
    for (unsigned i = 0; i < sizeof k_say_stream; i++) {
        const uint32_t a = SHOP_LBL_BASE + SHOP_SAY_OFF + i;
        if (psx_mod_read_byte(a) != k_say_stream[i])
            psx_mod_write_byte(a, k_say_stream[i]);
    }
    const uint16_t say = psx_mod_read_half(SHOP_LBL_TABLE + SHOP_SAY_ID * 2u);
    if (say != SHOP_SAY_OFF) {
        s_say_stock = say; s_say_stock_ok = 1;   /* stock is 0 (dead entry) */
        psx_mod_write_half(SHOP_LBL_TABLE + SHOP_SAY_ID * 2u, SHOP_SAY_OFF);
    }
}

/* ---- the shopkeeper's line ------------------------------------------------ */
static int say_line_ours(void) {
    return psx_mod_read_byte(SHOP_DLG_MODE) == 0x0Au &&
           psx_mod_read_half(SHOP_DLG_ID) == (uint16_t)SHOP_SAY_ID;
}

static void say_kick(void) {
    psx_mod_write_byte(SHOP_DLG_PENX, 0);
    psx_mod_write_byte(SHOP_DLG_ID,     (uint8_t)SHOP_SAY_ID);
    psx_mod_write_byte(SHOP_DLG_ID + 1, 0);
    psx_mod_write_byte(SHOP_DLG_MODE,  0x0A);
    psx_mod_write_byte(SHOP_DLG_PHASE, 0x80);
}

/* The engine's own wind-down entry state, measured at the natural greeting
 * dismiss: id 0, pen 0, phase 0x80, mode 0x02 makes it decode the empty
 * string, which clears the glyph display list and relinks the widget's
 * draw nodes. Without this, dismissed text stays painted in the box. */
static void say_winddown(void) {
    psx_mod_write_byte(SHOP_DLG_ID,     0);
    psx_mod_write_byte(SHOP_DLG_ID + 1, 0);
    psx_mod_write_byte(SHOP_DLG_PENX,   0);
    psx_mod_write_byte(SHOP_DLG_PHASE,  0x80);
    psx_mod_write_byte(SHOP_DLG_MODE,   0x02);
}

static void say_reset(void) {
    if (s_say && say_line_ours()) say_winddown();
    s_say = 0;
}

/* Sound requests from the tick, played from the hook below: the tick is a
 * host frame hook with no guest context, while the driver entry has the
 * live CPUState the nested call needs. */
static uint8_t s_sfx_q[4];
static int     s_sfx_n;
static void sfx_req(uint8_t id) {
    if (s_sfx_n < (int)sizeof s_sfx_q) s_sfx_q[s_sfx_n++] = id;
}

/* Entry hook on the menu cursor driver: while the panel (or the say line)
 * owns input, take UP/DOWN out of the new-press mask BEFORE the driver's
 * own read of it, and latch them for the panel. The frame-hook eat runs a
 * frame too late for this driver (see SHOP_MENU_NAV_FN above).
 * Also the shop's speaker: queued sounds go through the game's own
 * play_se here, in guest context, snapshot/restore around the nested call
 * so the driver call in flight is untouched (the card_drops $ra rule). */
void psx_mod_card_shop_on_menu_nav(CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled) { s_sfx_n = 0; s_award_n = 0; s_view = 0; return; }
    if (s_sfx_n || s_award_n) {
        CPUState saved = *cpu;
        for (int i = 0; i < s_award_n; i++) {
            cpu->pc = 0;
            cpu->gpr[4] = s_award_q[i];
            cpu->gpr[31] = SHOP_AWARD_RET;
            psx_dispatch_call(cpu, SHOP_AWARD_FN, SHOP_AWARD_RET);
        }
        for (int i = 0; i < s_sfx_n; i++) {
            cpu->pc = 0;
            cpu->gpr[4] = s_sfx_q[i];
            cpu->gpr[31] = SHOP_SE_RET;
            psx_dispatch_call(cpu, SHOP_SE_FN, SHOP_SE_RET);
        }
        *cpu = saved;
        s_sfx_n = 0;
        s_award_n = 0;
    }
    /* The card viewer: post the mailbox request, then pump the game's own
     * subscreen dispatcher once per frame until it clears the command byte
     * (the viewer's Circle). */
    if (s_view == 1) {
        gr_vram_transfer_out(832,   0,  64,  96, s_vs_tmpl);
        gr_vram_transfer_out(768, 256, 128, 256, s_vs_face);
        gr_vram_transfer_out(256, 240, 448,  16, s_vs_clut);
        gr_vram_transfer_out(0,   256,  64, 128, s_vs_face2);
        gr_vram_transfer_out(0,   255, 256,   1, s_vs_clut2);
        gr_vram_transfer_out(960, 384,  64, 128, s_vs_glyphs);
        gr_vram_transfer_out(896, 256,  64,  64, s_vs_back);
        s_vs_valid = 1;
        gr_vram_transfer_in(832, 0, 64, 96, psx_shop_tmpl_raw);
        /* The card-body canvas - BOTH 64-word columns, before the pump's
         * first run so the plate is there when the viewer draws on it.
         * Everything that varies per card is a SEPARATE primitive over this
         * art: the face out of (0,256)/8bpp through CLUT (0,255) into the
         * black window, the name one textured rect per glyph out of
         * (960,256)/4bpp through CLUT (496,248), the level stars, the
         * attribute orb and the ATK/DFD digits. Nothing is composed INTO
         * the canvas, which is why the whole thing can be baked.
         *
         * All 256 rows: the canvas carries the MAGIC/TRAP body as well as
         * the monster one - same pixels, read through CLUT (256,249) green
         * instead of (256,248) gold - and the magic variant's wide bottom
         * sticker lives in rows 192..256. At 192 every magic and trap card
         * bought here was cut off below the art. */
        gr_vram_transfer_in(768, 256, 64, 256, psx_shop_cardbody_l);
        gr_vram_transfer_in(832, 256, 64, 256, psx_shop_cardbody_r);
        /* And the palettes all of that is read through. The deck screens and
         * the FIRST shop happen to have rows 244..255 resident and identical;
         * the TOURNAMENT shop has none of them, so there the viewer drew the
         * streamed face and the text - which carry their own CLUTs - over a
         * card body and stone boxes that decoded to nothing. Uploading the
         * block makes the viewer look the same from all three shops. Inside
         * the (256,240,448,16) rect already saved on open, so the screen's
         * own palettes come back untouched on close. */
        gr_vram_transfer_in(256, 244, 256, 12, psx_shop_clut_view);
        /* The lower half of the (960,256) page: attribute orb, level stars,
         * ATK/DFD labels and digits, and the "[ ... Card ]" subtitle strip.
         * The card's NAME is not in here - that comes from elsewhere and drew
         * fine at the tournament shop while everything above was blank. */
        gr_vram_transfer_in(960, 384, 64, 128, psx_shop_cardglyphs);
        /* The card BACK. The viewer does not cut to the face - it FLIPS the
         * card in, drawing it as a stack of horizontal strips taken from this
         * page while it is edge-on and from the canvas once it is face up.
         * Without it the spin played with the back's lower half missing. */
        gr_vram_transfer_in(896, 256, 64, 64, psx_shop_cardback);
        psx_mod_write_byte(SHOP_SUB_CARD,     (uint8_t)(s_view_card & 0xFFu));
        psx_mod_write_byte(SHOP_SUB_CARD + 1, (uint8_t)(s_view_card >> 8));
        psx_mod_write_byte(SHOP_SUB_TYPE, 0x14u);
        psx_mod_write_byte(SHOP_SUB_CMD,  0x02u);
        s_view = 2;
    }
    if (s_view == 2) {
        CPUState saved = *cpu;
        cpu->pc = 0;
        cpu->gpr[4] = 0;
        cpu->gpr[5] = 0;
        cpu->gpr[31] = SHOP_SUB_RET;
        psx_dispatch_call(cpu, SHOP_SUB_PUMP, SHOP_SUB_RET);
        *cpu = saved;
        if (g_psx_call_bail || psx_mod_read_byte(SHOP_SUB_CMD) == 0u) {
            if (s_vs_valid) {
                gr_vram_transfer_in(832,   0,  64,  96, s_vs_tmpl);
                gr_vram_transfer_in(768, 256, 128, 256, s_vs_face);
                gr_vram_transfer_in(256, 240, 448,  16, s_vs_clut);
                gr_vram_transfer_in(0,   256,  64, 128, s_vs_face2);
                gr_vram_transfer_in(0,   255, 256,   1, s_vs_clut2);
                gr_vram_transfer_in(960, 384,  64, 128, s_vs_glyphs);
                gr_vram_transfer_in(896, 256,  64,  64, s_vs_back);
                s_vs_valid = 0;
            }
            s_view = 0;
            s_dirty = 1;
        }
        return;   /* the viewer owns input; leave the arrows alone */
    }
    if (!s_open && !s_say) return;
    const uint16_t np = psx_mod_read_half(SHOP_PAD_NEW_ADDR);
    const uint16_t arrows = (uint16_t)(SHOP_NP_UP | SHOP_NP_DOWN);
    if (np & arrows) {
        s_nav_latch |= (uint16_t)(np & arrows);
        psx_mod_write_half(SHOP_PAD_NEW_ADDR, (uint16_t)(np & ~arrows));
    }
}

static void shop_register_hooks(void) {
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_shop.menu_nav", SHOP_MENU_NAV_FN,
        psx_mod_card_shop_on_menu_nav);
}

/* ---- canvas -------------------------------------------------------------- */
#define ROW_W 132
#define ROW_H 18
#define ROW_X 12
#define ROW_Y 111
#define PANEL_W 304
/* 230 tall at y=5 keeps the panel centred (5px of screen top and bottom, was
 * 10) and hands all 10 extra rows to the results box: a pull name is a 12-row
 * glyph, so three of them, each on a plate that actually CONTAINS one and
 * clears the box frame, needs more than the 70 the box used to have. The old
 * highlight was SHORTER than the font and ruled a line through every name. */
#define PANEL_H 230
#define PANEL_X 8
#define PANEL_Y 5
#define CV_W PANEL_W
#define CV_H PANEL_H
static uint32_t s_px[CV_W * CV_H];
static int s_img_w, s_img_h;

#define C_GOLD    0xFFE0B84Cu
#define C_WHITE   0xFFF0F0F0u
#define C_GREY    0xFFB0B4C0u
#define C_RED     0xFFE06858u
#define C_GREEN   0xFF6CD86Cu   /* uncommon */
#define C_BLUE    0xFF6C9CF0u   /* rare (readable on the navy field)   */
#define C_YELLOW  0xFFF0E048u   /* legendary */
#define C_SEL     0xFF2A3454u
#define C_SLOT    0xFF44506Cu   /* outline of a not-yet-revealed pull */

static void px_fill(int x0, int y0, int w, int h, uint32_t c) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (int y = y0; y < y0 + h && y < CV_H; y++)
        for (int x = x0; x < x0 + w && x < CV_W; x++)
            s_px[y * CV_W + x] = c;
}

/* ---- the password screen's box furniture (psx_shop_skin) ----------------- */
static void skin_blit(const PsxSprite *s, int dx, int dy,
                      int sx, int sy, int w, int h) {
    if (!s->px) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int px = dx + x, py = dy + y;
            if (px < 0 || py < 0 || px >= CV_W || py >= CV_H) continue;
            const uint32_t c = s->px[(sy + y) * s->w + (sx + x)];
            if (c >> 24) s_px[py * CV_W + px] = c;
        }
}

static void skin_blit_mirror(const PsxSprite *s, int dx, int dy) {
    if (!s->px) return;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const int px = dx + x, py = dy + y;
            if (px < 0 || py < 0 || px >= CV_W || py >= CV_H) continue;
            const uint32_t c = s->px[y * s->w + (s->w - 1 - x)];
            if (c >> 24) s_px[py * CV_W + px] = c;
        }
}

/* One password-screen box: field, side edges, then the top/bottom strips
 * whose ends carry the corners — assembled exactly the way the screen's own
 * draw list assembles one, just at our width. The field is STRETCHED, not
 * tiled: its art carries the box's own light-to-dark shading top to bottom,
 * so tiling puts the dark bottom mid-box; nearest-neighbour stretch keeps
 * the composition at any size and the mottle hides the resampling. */
static void skin_box(int x0, int y0, int w, int h) {
    const PsxSprite *f = &psx_spr_shop_field;
    const int iw = w - 8, ih = h - 8;
    if (f->px && iw > 1 && ih > 1)
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                const int sy = y * (f->h - 1) / (ih - 1);
                const int sx = x * (f->w - 1) / (iw - 1);
                const uint32_t c = f->px[sy * f->w + sx];
                const int px = x0 + 4 + x, py = y0 + 4 + y;
                if (px >= 0 && py >= 0 && px < CV_W && py < CV_H)
                    s_px[py * CV_W + px] = c | 0xFF000000u;
            }
    const PsxSprite *le = &psx_spr_shop_left, *re = &psx_spr_shop_right;
    for (int y = 8; y < h - 8; y += le->h) {
        const int hh = (h - 8 - y) < le->h ? (h - 8 - y) : le->h;
        skin_blit(le, x0, y0 + y, 0, 0, le->w, hh);
        skin_blit(re, x0 + w - re->w, y0 + y, 0, 0, re->w, hh);
    }
    const PsxSprite *ts = &psx_spr_shop_top, *bs = &psx_spr_shop_bot;
    const int end = ts->w / 2;               /* 88: each half owns a corner */
    for (int pass = 0; pass < 2; pass++) {
        const PsxSprite *s = pass ? bs : ts;
        const int dy = pass ? y0 + h - s->h : y0;
        skin_blit(s, x0, dy, 0, 0, end, s->h);
        skin_blit(s, x0 + w - end, dy, s->w - end, 0, end, s->h);
        for (int x = end; x < w - end; ) {
            const int seg = (w - end - x) < 136 ? (w - end - x) : 136;
            skin_blit(s, x0 + x, dy, 20, 0, seg, s->h);
            x += seg;
        }
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

/* Advance-width of a string without drawing it, mirroring put_text's
 * metrics, so variable-width labels can be centred in a column. */
static int text_width(const char *s) {
    const PsxFusionFont *f = &psx_fusion_font;
    int x = 0;
    for (; *s; s++) {
        if (*s == ' ') { x += 4; continue; }
        const int cell = psx_fusion_font_cell((unsigned char)*s);
        if (cell < 0) { x += 5; continue; }
        const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
        int hi = 0;
        for (int yy = 0; yy < f->h; yy++)
            for (int xx = 0; xx < f->w; xx++)
                if (g[yy * f->w + xx] && xx > hi) hi = xx;
        const int w = hi + 1;
        x += (w > 2 ? w : 4) + 1;
    }
    return x;
}

/* Three stacked password-screen boxes: header (title + starchip readout),
 * the pack list, and the message/pull box. */
#define BOX_A_Y 0
#define BOX_A_H 40
#define BOX_B_Y 43
#define BOX_B_H 104
#define BOX_C_Y 150
#define BOX_C_H 80

static void draw_panel(void) {
    s_img_w = PANEL_W; s_img_h = PANEL_H;
    memset(s_px, 0, sizeof s_px);
    skin_box(0, BOX_A_Y, PANEL_W, BOX_A_H);
    skin_box(0, BOX_B_Y, PANEL_W, BOX_B_H);
    skin_box(0, BOX_C_Y, PANEL_W, BOX_C_H);

    put_text("Buy Card Packs", 16, BOX_A_Y + 15, C_GOLD);
    skin_blit(&psx_spr_shop_star, 206, BOX_A_Y + 12, 0, 0,
              psx_spr_shop_star.w, psx_spr_shop_star.h);
    char line[36];
    snprintf(line, sizeof line, "x %u",
             (unsigned)psx_mod_read_word(SHOP_CHIPS_ADDR));
    put_text(line, 226, BOX_A_Y + 16, C_WHITE);

    build_pools();
    for (int i = 0; i < SHOP_PACKS; i++) {
        const int y = BOX_B_Y + 12 + i * 21;
        const int tier = s_tier[i];
        if (i == s_sel) px_fill(10, y - 3, PANEL_W - 20, 17, C_SEL);
        put_text(k_packs[i].name, 18, y, C_WHITE);
        /* Rarity centred between the arrow columns (arrows at 94 and 194,
         * so the span between them is 110..194). */
        put_text(k_tier_names[tier],
                 152 - text_width(k_tier_names[tier]) / 2, y,
                 tier == 0 ? C_WHITE : tier == 1 ? C_GREEN
                 : tier == 2 ? C_BLUE : C_YELLOW);
        if (i == s_sel) {
            /* The password screen's digit arrows flanking the rarity,
             * alternating between the sheet's two animation frames the way
             * the screen itself blinks them. The sheet only carries the
             * right-pointing arrow; the game mirrors it for left (poly
             * with reversed UVs) and so do we. */
            const PsxSprite *ar = s_anim ? &psx_spr_shop_arrow2
                                         : &psx_spr_shop_arrow;
            skin_blit_mirror(ar, 94, y - 3);
            skin_blit(ar, 194, y - 3, 0, 0, ar->w, ar->h);
        }
        skin_blit(&psx_spr_shop_star, 234, y - 4, 0, 0,
                  psx_spr_shop_star.w, psx_spr_shop_star.h);
        snprintf(line, sizeof line, "%d", s_cfg_price[tier]);
        put_text(line, 254, y, C_GOLD);
    }

    if (s_msg[0]) put_text(s_msg, 16, BOX_C_Y + 8, s_pull_n ? C_GREY : C_RED);
    /* Button hints share the header line with the short "RESULTS:" label,
     * right-aligned. (With the old "LEGENDARY MONSTER:" header there was no
     * room here and the triangle button sat on top of the text.)
     * The pull list occupies the box for the rest of the visit, so once the
     * ceremony is over this line is where BUY/CLOSE live - without it the
     * player is left with a screen that has no legend at all. The same goes
     * for a refusal ("NOT ENOUGH CHIPS"): the message takes the header, so
     * the centred legend below is suppressed and this line is the only one
     * left to carry it. */
    if (s_pull_n || s_msg[0]) {
        const int hy = BOX_C_Y + 4;
        const PsxSprite *btn[2];
        const char *lbl[2];
        if (s_ceremony) {
            /* Reveal phase: X flips the NEXT card, it does not dismiss the
             * list. Labelling it "Continue" there named the button it only
             * becomes once the last card has landed. */
            btn[0] = &psx_spr_shop_tbtn; lbl[0] = "View";
            btn[1] = &psx_spr_shop_xbtn;
            lbl[1] = s_shown < s_pull_n ? "Next" : "Continue";
        } else {
            btn[0] = &psx_spr_shop_xbtn; lbl[0] = "BUY";
            btn[1] = &psx_spr_shop_obtn; lbl[1] = "CLOSE";
        }
        /* Measured then laid out LEFT to right, so the pair keeps the panel's
         * own X-before-O order; laying it out backwards from the right edge
         * (which is all the fixed ceremony pair needed) reversed it. */
        int w = 0;
        for (int i = 0; i < 2; i++)
            w += (i ? 12 : 0) + btn[i]->w + 2 + text_width(lbl[i]);
        int x = PANEL_W - 14 - w;
        for (int i = 0; i < 2; i++) {
            if (i) x += 12;
            skin_blit(btn[i], x, hy, 0, 0, btn[i]->w, btn[i]->h);
            x += btn[i]->w + 2;
            put_text(lbl[i], x, hy + 4, C_WHITE);
            x += text_width(lbl[i]);
        }
    }
    /* One row per card the pack will yield, revealed or not: the empty ones
     * are drawn as the highlight's own outline, so the pack size reads from
     * the first frame and every X visibly FILLS a waiting slot instead of
     * growing the list from nowhere.
     *
     * A row is 16 tall for a 12-row glyph: the plate spans y-2..y+13, so its
     * rules land OUTSIDE the ink - which is what the 10-tall bar on an 11px
     * pitch could not do (it ruled a line straight through the middle of
     * every name it was meant to be highlighting). The pitch is 16 to match,
     * because at 15 the next row's outline landed on the highlight's own
     * bottom rule and erased it. */
    const int shown = s_ceremony ? s_shown : s_pull_n;
    for (int i = 0; i < s_pull_n && i < SHOP_PULL_ROWS; i++) {
        const int y = BOX_C_Y + 23 + i * 16;
        if (i >= shown) {
            px_fill(12, y - 2, PANEL_W - 24, 1, C_SLOT);
            px_fill(12, y + 13, PANEL_W - 24, 1, C_SLOT);
            px_fill(12, y - 2, 1, 16, C_SLOT);
            px_fill(PANEL_W - 13, y - 2, 1, 16, C_SLOT);
            continue;
        }
        if (s_ceremony && i == s_card_sel) {
            px_fill(12, y - 2, PANEL_W - 24, 16, C_SEL);
            px_fill(12, y - 2, PANEL_W - 24, 1, C_GOLD);
            px_fill(12, y + 13, PANEL_W - 24, 1, C_GOLD);
            px_fill(12, y - 2, 3, 16, C_GOLD);
        }
        const char *nm = psx_card_db_name(s_pull[i]);
        snprintf(line, sizeof line, "%.30s", nm ? nm : "?");
        put_text(line, 20, y, C_WHITE);
    }
    if (!s_pull_n && !s_msg[0]) {
        /* Nothing bought yet: the box is empty, so the password screen's own
         * OK/END buttons get the middle of it. */
        skin_blit(&psx_spr_shop_xbtn, 88, BOX_C_Y + 28, 0, 0,
                  psx_spr_shop_xbtn.w, psx_spr_shop_xbtn.h);
        put_text("BUY", 108, BOX_C_Y + 32, C_WHITE);
        skin_blit(&psx_spr_shop_obtn, 160, BOX_C_Y + 28, 0, 0,
                  psx_spr_shop_obtn.w, psx_spr_shop_obtn.h);
        put_text("CLOSE", 180, BOX_C_Y + 32, C_WHITE);
    }
}

/* ---- purchase ------------------------------------------------------------ */
static void buy(int pack) {
    const int tier = s_tier[pack];
    const int price = s_cfg_price[tier];
    build_pools();
    if (!s_pools_built || !s_pool_n[pack][tier]) {
        snprintf(s_msg, sizeof s_msg, "SHOP NOT STOCKED YET");
        s_pull_n = 0; s_denied++; sfx_req(SHOP_SE_DENY); return;
    }
    if (!save_live()) {
        snprintf(s_msg, sizeof s_msg, "NO SAVE LOADED");
        s_pull_n = 0; s_denied++; sfx_req(SHOP_SE_DENY); return;
    }
    const uint32_t chips = psx_mod_read_word(SHOP_CHIPS_ADDR);
    if (chips < (uint32_t)price) {
        snprintf(s_msg, sizeof s_msg, "NOT ENOUGH CHIPS");
        s_pull_n = 0; s_denied++; sfx_req(SHOP_SE_DENY); return;
    }
    sfx_req(SHOP_SE_BUY);
    psx_mod_write_word(SHOP_CHIPS_ADDR, chips - (uint32_t)price);
    s_pull_n = 0;
    s_award_n = 0;
    for (int i = 0; i < s_cfg_pack_cards && i < SHOP_PULL_MAX; i++) {
        const int n = s_pool_n[pack][tier];
        const int id = s_pool[pack][tier][rng_next() % (uint32_t)n];
        /* Granted through the game's own award (trunk + New! ring) from
         * the driver-entry hook - see SHOP_AWARD_FN. */
        s_award_q[s_award_n++] = (uint16_t)id;
        s_pull[s_pull_n++] = id;
    }
    /* Short label: the rarity and pack are both still on screen in the
     * highlighted row above, and the room this leaves on the header line
     * is what lets the button hints share it. */
    snprintf(s_msg, sizeof s_msg, "RESULTS:");
    /* The pack-open ceremony: the first card is on the table already;
     * every X flips the next, then the list can be browsed and viewed. */
    s_ceremony = 1;
    s_shown = 1;
    s_card_sel = 0;
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
    if (s_say_stock_ok &&
        psx_mod_read_half(SHOP_LBL_TABLE + SHOP_SAY_ID * 2u) == SHOP_SAY_OFF)
        psx_mod_write_half(SHOP_LBL_TABLE + SHOP_SAY_ID * 2u, s_say_stock);
    say_reset();
}

void psx_card_shop_tick(void) {
    if (!s_enabled) { restore_stock_code(); return; }
    /* Stage the stream, the label-table entry and the four code immediates
     * from the moment the game is up, NOT only while the gate is open.
     *
     * The game has THREE shopkeeper screens and all three run this same
     * routine - the geometry literals at 0x8002F008.. and the dispatch chain
     * at 0x8002F35C.. are one shared open/dispatch, and all three menus draw
     * label stream 17 - so staging is global by nature, not per-shop.
     *
     * It has to be, because the menu widget latches the table entry when it
     * OPENS, and the gate cannot be up before that on every screen: it armed
     * early at the first shop only through `greeting_live()`, which matches
     * one hard-coded string id (1086). The tournament shop's greeting is a
     * different string (1527), so there the gate first went up on signature 2
     * - the menu already on screen, already latched onto the STOCK stream -
     * and CARD SHOP simply was not in the list. Staging unconditionally means
     * whichever shop the player walks into has the entry repointed before its
     * menu opens. `restore_stock_code()` still puts every byte back the frame
     * the mod is switched off. */
    if (psx_mod_game_started()) assert_stream();
    const int gate = screen_match();
    if (gate != s_gate) { s_gate = gate; s_dirty = 1; }
    if (!gate) {
        if (s_open) { s_open = 0; s_dirty = 1; }
        /* Re-read card_shop.ini and rebuild pools on the next visit, so an
         * edit takes effect by walking out of the shop and back in. */
        if (s_pools_built) { s_pools_built = 0; s_cfg_loaded = 0; }
        s_ceremony = 0;
        s_view = 0;
        s_vs_valid = 0;
        say_reset();
        s_native = 0;
        return;
    }

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
    /* A popup (count parked at 2) saves and restores the menu cursor in
     * STOCK indexing: backing out of RETURN TO TITLE's confirm restored 2 -
     * BUILD DECK on the five-row menu - while the highlight still stood on
     * row 3, so the next DOWN looked eaten and an UP jumped two rows.
     * Remember the row the popup opened from and undo the off-by-one for
     * the rows our insert shifted. */
    if (native) {
        static int last5cur = -1, popup_from = -1;
        const uint8_t n = psx_mod_read_byte(SHOP_COUNT_ADDR);
        const int cur = (int)psx_mod_read_byte(SHOP_CURSOR_ADDR);
        if (n == (uint8_t)SHOP_ROWS) {
            if (popup_from >= 2 && cur == popup_from - 1)
                psx_mod_write_byte(SHOP_CURSOR_ADDR, (uint8_t)popup_from);
            popup_from = -1;
            last5cur = (int)psx_mod_read_byte(SHOP_CURSOR_ADDR);
        } else if (n == 2u && popup_from < 0) {
            popup_from = last5cur;
        }
    }
    if (!native) {
        /* Menu is showing stock labels (pre-repoint open, or a savestate
         * taken before this feature). Behave as absent: no fifth row, no
         * remap, stock count. */
        if (s_open) { s_open = 0; s_dirty = 1; }
        say_reset();
        return;
    }

    uint16_t np = psx_mod_read_half(SHOP_PAD_NEW_ADDR);
    /* Arrows the driver-entry hook took while the panel owns input arrive
     * here; anything latched during the say line is deliberately dropped
     * (the line is modal). */
    if (s_open) np |= s_nav_latch;
    s_nav_latch = 0;
    const int cursor = (int)psx_mod_read_byte(SHOP_CURSOR_ADDR);

    if (!s_open) {
        static int prev_cursor = -1;
        if (cursor != prev_cursor) { prev_cursor = cursor; s_dirty = 1; }
        /* A savestate can restore RAM with our line mid-display while this
         * module's state starts over: adopt it instead of orphaning it. */
        if (s_say == 0 && say_line_ours()) s_say = 2;
        if (s_say == 1) {
            /* One-tick grace so the game's accept default (a no-op for the
             * CARD SHOP row) fully runs before the widget is touched. */
            psx_mod_write_byte(SHOP_CURSOR_ADDR, (uint8_t)SHOP_ROW);
            if (--s_say_timer <= 0) { say_kick(); s_say = 2; s_dirty = 1; }
            return;
        }
        if (s_say == 2) {
            /* The line is up and modal. Park the cursor on CARD SHOP - the
             * shifted chain's no-op default - so the dismiss X, which the
             * menu ALSO receives (measured: one press once opened the SAVE
             * dialog, another dispatched LEAVE SHOP), lands on nothing. Eat
             * everything except that X: it is the dialog's own dismiss. */
            psx_mod_write_byte(SHOP_CURSOR_ADDR, (uint8_t)SHOP_ROW);
            const uint16_t eat = (uint16_t)(SHOP_NP_UP | SHOP_NP_DOWN |
                                            SHOP_NP_LEFT | SHOP_NP_RIGHT |
                                            SHOP_NP_CIRCLE);
            if (np & eat)
                psx_mod_write_half(SHOP_PAD_NEW_ADDR, (uint16_t)(np & ~eat));
            if (psx_mod_read_byte(SHOP_DLG_MODE) != 0x0Au) {
                /* Dismissed: the engine's X handler dropped the arrow and
                 * the mode; finish its teardown, then open the panel. */
                say_winddown();
                s_say = 0;
                s_open = 1; s_sel = 0; s_msg[0] = 0; s_pull_n = 0; s_opens++;
                s_rng ^= psx_mod_read_word(0x8009B0C4u) * 2654435761u;
                s_dirty = 1;
            }
            return;
        }
        if (np & SHOP_NP_CROSS) {
            /* The cursor byte is shared with every popup the screen opens
             * (SAVE?'s NO is also index 1, with count parked at 2), so the
             * accept requires OUR five-row count too - X on a popup row
             * must never start the shop. */
            if (cursor == SHOP_ROW &&
                psx_mod_read_byte(SHOP_COUNT_ADDR) == (uint8_t)SHOP_ROWS) {
                /* CARD SHOP: with the dispatch chain shifted, the game's own
                 * default case handles this press (nothing). The shopkeeper
                 * asks his line first; the panel opens when it is dismissed
                 * (the s_say == 2 branch above). */
                s_say = 1; s_say_timer = 2; s_dirty = 1;
            }
            /* Rows 2..4 dispatch natively through the patched chain. */
        }
        return;
    }

    /* Panel open: pin the game's cursor on CARD SHOP — the shifted chain's
     * no-op default — and eat everything we act on. The eat rewrite loses
     * the race against the game's own dispatch (measured: it samples at the
     * accept instant), and with the say-line flow the menu is X-live again
     * under the panel, so a BUY press leaks into the chain; pinned at 4 it
     * dispatched LEAVE SHOP and dumped the player on the map mid-purchase.
     * Pinned here it lands on the row that does nothing. */
    psx_mod_write_byte(SHOP_CURSOR_ADDR, (uint8_t)SHOP_ROW);
    uint16_t eat = 0;
    if (s_view) {
        /* The game's card viewer is up: every button is its. The hook
         * clears s_view when the viewer's own Circle closes it. */
    } else if (s_ceremony && s_shown < s_pull_n) {
        /* Reveal phase: X flips the next card. The newest card is hovered
         * from the moment it lands, so TRIANGLE can view it right away. */
        s_card_sel = s_shown - 1;
        if (np & SHOP_NP_CROSS) {
            s_shown++; s_card_sel = s_shown - 1;
            sfx_req(SHOP_SE_FLIP); eat |= SHOP_NP_CROSS; s_dirty = 1;
        }
        if (np & SHOP_NP_TRIANGLE) {
            s_view = 1; s_view_card = (uint16_t)s_pull[s_card_sel];
            s_dirty = 1;
        }
        eat |= (uint16_t)(np & (SHOP_NP_UP | SHOP_NP_DOWN | SHOP_NP_LEFT |
                                SHOP_NP_RIGHT | SHOP_NP_CIRCLE |
                                SHOP_NP_TRIANGLE));
    } else if (s_ceremony) {
        /* Browse phase: pick a card, TRIANGLE views it, X continues. */
        if (np & SHOP_NP_UP)   { s_card_sel = (s_card_sel + s_pull_n - 1) % s_pull_n; sfx_req(SHOP_SE_CURSOR); eat |= SHOP_NP_UP; s_dirty = 1; }
        if (np & SHOP_NP_DOWN) { s_card_sel = (s_card_sel + 1) % s_pull_n;            sfx_req(SHOP_SE_CURSOR); eat |= SHOP_NP_DOWN; s_dirty = 1; }
        if (np & SHOP_NP_TRIANGLE) {
            s_view = 1; s_view_card = (uint16_t)s_pull[s_card_sel];
            eat |= SHOP_NP_TRIANGLE; s_dirty = 1;
        }
        if (np & (SHOP_NP_CROSS | SHOP_NP_CIRCLE)) {
            s_ceremony = 0; sfx_req(SHOP_SE_CURSOR);
            eat |= (uint16_t)(np & (SHOP_NP_CROSS | SHOP_NP_CIRCLE));
            s_dirty = 1;
        }
    } else {
        if (np & SHOP_NP_UP)    { s_sel = (s_sel + SHOP_PACKS - 1) % SHOP_PACKS; eat |= SHOP_NP_UP; sfx_req(SHOP_SE_CURSOR); s_dirty = 1; }
        if (np & SHOP_NP_DOWN)  { s_sel = (s_sel + 1) % SHOP_PACKS;              eat |= SHOP_NP_DOWN; sfx_req(SHOP_SE_CURSOR); s_dirty = 1; }
        if (np & SHOP_NP_LEFT)  { s_tier[s_sel] = (s_tier[s_sel] + SHOP_TIERS - 1) % SHOP_TIERS; eat |= SHOP_NP_LEFT; sfx_req(SHOP_SE_CURSOR); s_dirty = 1; }
        if (np & SHOP_NP_RIGHT) { s_tier[s_sel] = (s_tier[s_sel] + 1) % SHOP_TIERS;              eat |= SHOP_NP_RIGHT; sfx_req(SHOP_SE_CURSOR); s_dirty = 1; }
        if (np & SHOP_NP_CROSS) { buy(s_sel); eat |= SHOP_NP_CROSS; s_dirty = 1; }
        if (np & SHOP_NP_CIRCLE){ s_open = 0; eat |= SHOP_NP_CIRCLE; sfx_req(SHOP_SE_LEAVE); s_dirty = 1; }
    }
    if (eat)
        psx_mod_write_half(SHOP_PAD_NEW_ADDR, (uint16_t)(np & ~eat));
    static uint32_t last_chips;
    const uint32_t chips = psx_mod_read_word(SHOP_CHIPS_ADDR);
    if (chips != last_chips) { last_chips = chips; s_dirty = 1; }
    /* The rarity arrows blink like the password screen's. */
    static unsigned anim_clk;
    if (++anim_clk >= 16u) { anim_clk = 0; s_anim ^= 1; s_dirty = 1; }
}

/* ---- overlay contract ---------------------------------------------------- */
int psx_card_shop_image(const uint32_t **px, int *w, int *h) {
    if (!s_gate || !s_native) return 0;
    if (!s_open) return 0;   /* all five rows are native; no band */
    if (s_view) return 0;    /* the game's card viewer has the screen */
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
        "TURN ON TO BUY CARD PACKS AT THE SHOPKEEPER",
        "BUY CARD PACKS WITH STARCHIPS AT THE SHOPKEEPER",
    };
    (void)psx_game_add_start_hook(shop_register_hooks);
    /* Ships OFF, like the other MODS rows: it adds a row to a menu the game
     * already has and grants cards, so a player meets it only by choosing
     * to switch it on. */
    s_row_handle = psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "CARD SHOP", HINTS[0],
        ONOFF, 2, "card_shop", 0, shop_changed);
    psx_video_menu_set_row_hints(s_row_handle, HINTS);
}

int psx_card_shop_state_json(char *out, unsigned cap) {
    const int t_state = psx_mod_read_half(SHOP_STATE_ADDR) == 0xE00Du;
    const int t_menu  = psx_mod_read_word(SHOP_MENUFLAG) == 1u;
    const int t_sig   = psx_mod_read_byte(SHOP_SIG_A) == 0x08u &&
                        psx_mod_read_byte(SHOP_SIG_C) == 0x20u;
    return snprintf(out, cap,
        "\"t_state\":%d,\"t_menu\":%d,\"t_sig\":%d,"
        "\"t_menu17\":%d,\"t_greet\":%d,"
        "\"hpatch\":%u,"
        "\"enabled\":%d,\"gate\":%d,\"native\":%d,\"open\":%d,\"sel\":%d,"
        "\"tier\":%d,\"count_byte\":%u,\"cursor\":%u,\"chips\":%u,"
        "\"table17\":%u,\"stock_entry\":%u,\"widget_cur\":%u,"
        "\"say\":%d,\"say_table\":%u,\"dlg_mode\":%u,\"dlg_id\":%u,"
        "\"cer\":%d,\"shown\":%d,\"csel\":%d,\"view\":%d,\"sub_cmd\":%u,"
        "\"pools\":[%d,%d,%d,%d],"
        "\"buys\":%u,\"denied\":%u,\"opens\":%u,\"remaps\":%u",
        t_state, t_menu, t_sig,
        widget_on_menu17(), greeting_live(),
        (unsigned)(psx_mod_read_word(SHOP_H_PATCH_ADDR) == SHOP_H_FIVE),
        s_enabled, s_gate, s_native, s_open, s_sel, s_tier[s_sel],
        (unsigned)psx_mod_read_byte(SHOP_COUNT_ADDR),
        (unsigned)psx_mod_read_byte(SHOP_CURSOR_ADDR),
        (unsigned)psx_mod_read_word(SHOP_CHIPS_ADDR),
        (unsigned)psx_mod_read_half(SHOP_LBL_TABLE + 17u * 2u),
        (unsigned)s_stock_entry,
        (unsigned)psx_mod_read_word(SHOP_WIDGET1),
        s_say,
        (unsigned)psx_mod_read_half(SHOP_LBL_TABLE + SHOP_SAY_ID * 2u),
        (unsigned)psx_mod_read_byte(SHOP_DLG_MODE),
        (unsigned)psx_mod_read_half(SHOP_DLG_ID),
        s_ceremony, s_shown, s_card_sel, s_view,
        (unsigned)psx_mod_read_byte(SHOP_SUB_CMD),
        s_pool_n[0][0] + s_pool_n[0][1] + s_pool_n[0][2] + s_pool_n[0][3],
        s_pool_n[1][0] + s_pool_n[1][1] + s_pool_n[1][2] + s_pool_n[1][3],
        s_pool_n[2][0] + s_pool_n[2][1] + s_pool_n[2][2] + s_pool_n[2][3],
        s_pool_n[3][0] + s_pool_n[3][1] + s_pool_n[3][2] + s_pool_n[3][3],
        s_buys, s_denied, s_opens, s_remaps);
}
