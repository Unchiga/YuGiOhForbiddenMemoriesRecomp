/* psx_card_name_color.c — tints a card's on-screen text by drop rarity.
 *
 * Rarity is the best odds any one duelist gives the card, scaled by that
 * duelist's tier and the drop's rank band (see the rarity model section
 * below). Nothing on disc is touched — the mod only overrides the glyph
 * colour byte the text engine is about to draw.
 *
 * WHICH CARD, WHICH TEXT
 * -----------------------
 * func_80037DA4 is the text engine; it draws a card's name, type, Guardian
 * Stars and description through the same code path, so its $ra alone can't
 * tell a name apart from the rest. What it CAN give reliably is the card:
 * at $ra == CARD_NAME_CALLER, 0x8009B338 already holds the id of the card
 * being drawn (Data Crystal's RAM map calls this "Selected card ID"; this
 * repo's psx_card_shop.c also uses it, as SHOP_SIG_A).
 *
 * Telling the name apart from the rest needs the glyphs themselves.
 * func_80036C14 delivers one decoded character at a time; matching it
 * position-by-position against the card's already-known name (never
 * searching for it) is safe, since no type line or description can equal a
 * card's full name character for character.
 *
 * THE COLOUR IS STICKY
 * ----------------------
 * func_80036C14's a0 is the glyph record; byte +84 is its colour index, and
 * it stays applied to every later glyph on that widget until overwritten
 * again. So each CARD_NAME_CALLER entry first reverts the widget to white
 * and arms an unconfirmed match against the card's name; the glyph hook
 * then re-applies the rarity colour only while that match keeps holding. A
 * type line or description is never touched, because a match against it
 * never starts succeeding.
 *
 * THE PALETTE
 * ------------
 * Usable colour indices: 0 white, 1 yellow, 2 blue, 3 green, 4 grey,
 * 5 orange, 6 red. Nothing past 6 renders as a usable colour.
 *
 * card_name_color.ini
 * --------------------
 * A plain, hand-editable file in the player-data directory, written with
 * the built-in defaults the first time colours are built.
 *
 *   [tiers]                    threshold and colour for each of the six
 *                              rarity tiers (legendary/ultra_rare/
 *                              super_rare/rare/uncommon/default, rarest
 *                              first) — a card takes the first tier whose
 *                              threshold it does not exceed
 *   [duelist_tier_multipliers] name a duelist tier and give it a multiplier
 *   [duelist_tiers]            DUELIST NAME = tier name, from the list above
 *   [rank_multipliers]         multiplier for each rank band: s_a_pow,
 *                              b_c_d, s_a_tec
 *   [cards]                    NAME = colour overrides, matched against the
 *                              game's own decoded card names
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_video_menu.h"

/* psx_drop_db.h lives in the GAME's src/, which is on psx-runtime's include
 * path; PSX_DROP_DB is a generated const array linked into this executable. */
#include "psx_drop_db.h"

#define PSX_TEXT_FN        0x80037DA4u
#define PSX_GLYPH_FN        0x80036C14u  /* per-decoded-character callback */
#define CARD_NAME_CALLER   0x80038B98u   /* $ra for any card-info widget's text */
#define PSX_SELECTED_CARD  0x8009B338u   /* u16, id of the card being drawn */

#define CARD_COUNT PSX_DROP_DB_CARDS     /* 722 */

#define GLYPH_COLOR_OFF  84u
#define COL_WHITE   0u
#define COL_YELLOW  1u
#define COL_BLUE    2u
#define COL_GREEN   3u
#define COL_GREY    4u    /* usable, just not part of the default ladder */
#define COL_ORANGE  5u
#define COL_RED     6u
/* Nothing past 6 is a usable colour — the renderer has no palette entry
 * for it (invisible text, corrupted glyphs, or a near-black maroon). */

/* ---- name decoding, so [cards] overrides can be matched by NAME ----------
 * Same RAM map and frequency-code table as psx_card_db.c. */
#define NAME_OFFSETS  0x801D5800u
#define NAME_SEGMENT  0x801D0000u
#define NAME_MAX      40u

static const char RAW_ASCII[128] = {
    /* 00 */ ' ',   'e',   't',   'a',   'o',   'i',   'n',   's',
    /* 08 */ 'r',   'h',   'l',   '.',   'd',   'u',   'm',   'c',
    /* 10 */ 'g',   'y',   'w',   'f',   'p',   'b',   'k',   0,
    /* 18 */ 'A',   'v',   'I',   '\'',  'T',   'S',   'M',   ',',
    /* 20 */ 'D',   'O',   'W',   'H',   'Y',   'E',   'R',   0,
    /* 28 */ 0,     'G',   'L',   'C',   'N',   'B',   0,     'P',
    /* 30 */ '-',   'F',   'z',   'K',   'j',   'U',   'x',   'q',
    /* 38 */ '0',   'V',   '2',   'J',   '#',   '1',   'Q',   'Z',
    /* 40 */ 0,     '3',   '5',   '&',   0,     '7',   'X',   0,
    /* 48 */ 0,     0,     '4',   0,     0,     0,     '6',   0,
    /* 50 */ 0,     0,     0,     0,     0,     'a',   0,     '8',
    /* 58 */ 0,     '9',   0,     0,     0,     0,     0,     0,
};
static char s_name[CARD_COUNT + 1][NAME_MAX];
static int  s_names_ready;

static int name_table_resident(void)
{
    return psx_mod_read_half(NAME_OFFSETS + 2u) != 0u;
}

static void decode_names(void)
{
    for (int id = 1; id <= CARD_COUNT; id++) {
        const uint32_t off = psx_mod_read_half(NAME_OFFSETS + (uint32_t)id * 2u);
        const uint32_t a = NAME_SEGMENT + off;
        unsigned n = 0;
        for (unsigned i = 0; i < NAME_MAX * 2u && n + 1u < NAME_MAX; i++) {
            const unsigned raw = psx_mod_read_byte(a + i);
            if (raw == 0xFFu || raw >= 128u)
                break;
            const char c = RAW_ASCII[raw];
            if (!c)
                break;
            s_name[id][n++] = c;
        }
        s_name[id][n] = '\0';
    }
    s_names_ready = 1;
}

static int name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

/* ---- rarity model ---------------------------------------------------------
 * Six tiers, rarest first: legendary, ultra_rare, super_rare, rare,
 * uncommon, default. A card takes the first tier, rarest first, whose
 * threshold it does not exceed. The value compared is the card's rarity
 * SCORE: the best score any one (duelist, rank band) entry gives it —
 *
 *     score = weight * duelist_tier_multiplier * rank_multiplier
 *
 * weight is the raw drop weight (out of PSX_DROP_DB_TOTAL = 2048);
 * duelist_tier_multiplier comes from the duelist's [duelist_tiers] tier
 * (ini [duelist_tier_multipliers]; unassigned duelists use "default");
 * rank_multiplier comes from ini [rank_multipliers] for that entry's S/A
 * POW, B/C/D or S/A TEC band. All multipliers default to 1, so a stock ini
 * uses the max raw weight. legendary (score 0) means no duelist drops it.
 *
 * THRESHOLD VALUES DECIDE THE RARITY ORDER, NOT THE TIER NAMES. The five
 * non-default tiers are re-sorted by threshold, ascending, every session,
 * so editing a threshold can change which tier is rarest without
 * relabelling anything. default has no threshold and is always the
 * fallback. */
enum { TIER_LEGENDARY, TIER_ULTRA_RARE, TIER_SUPER_RARE, TIER_RARE,
       TIER_UNCOMMON, TIER_DEFAULT, RARITY_TIERS };
static uint8_t s_tier_color[RARITY_TIERS] = {
    COL_BLUE, COL_RED, COL_ORANGE, COL_YELLOW, COL_GREEN, COL_WHITE,
};
/* Compared against a card's rarity score, not raw weight, once multipliers
 * are edited away from 1. Tunable via [tiers]. */
static double s_tier_threshold[RARITY_TIERS] = { 2, 5, 7, 11, 16, 2048 };

/* TIER_DEFAULT excluded — always the fallback. Recomputed once per
 * build_colors() from s_tier_threshold (defaults or ini). */
static int s_tier_order[TIER_DEFAULT];

static void sort_tiers_by_threshold(void)
{
    for (int i = 0; i < TIER_DEFAULT; i++)
        s_tier_order[i] = i;
    /* Small, stable insertion sort: ties keep their original tier order. */
    for (int i = 1; i < TIER_DEFAULT; i++) {
        const int key = s_tier_order[i];
        const double key_threshold = s_tier_threshold[key];
        int j = i - 1;
        while (j >= 0 && s_tier_threshold[s_tier_order[j]] > key_threshold) {
            s_tier_order[j + 1] = s_tier_order[j];
            j--;
        }
        s_tier_order[j + 1] = key;
    }
}

/* ---- duelist tiers & rank multipliers --------------------------------------
 * Two multipliers feed a card's rarity score: which tier a duelist belongs
 * to, and which rank band (S/A POW, B/C/D, S/A TEC) the drop came from.
 * Tougher duelists get a lower multiplier, so a card a hard opponent drops
 * generously still reads as rarer than one an easy opponent drops rarely.
 * Both are tunable via [duelist_tier_multipliers]/[duelist_tiers]/
 * [rank_multipliers]; the values below are just the shipped defaults. */
#define DUELIST_TIER_MAX 16
typedef struct { char name[24]; double multiplier; } DuelistTierDef;
static DuelistTierDef s_duelist_tier[DUELIST_TIER_MAX] = {
    { "default",   1.0  },
    { "tutorial",  2.0  },
    { "rookie",    1.5  },
    { "normal",    1.0  },
    { "tough",     0.75 },
    { "boss",      0.5  },
    { "superboss", 0.25 },
};
static int s_duelist_tier_n = 7;

#define DUELIST_ASSIGN_MAX (PSX_DROP_DB_DUELISTS + 8)
typedef struct { char duelist[32]; char tier[24]; } DuelistTierAssign;
static DuelistTierAssign s_duelist_assign[DUELIST_ASSIGN_MAX] = {
    { "Simon Muran",        "tutorial"  },
    { "Teana",               "tutorial"  },
    { "Jono",                "tutorial"  },
    { "Villager1",           "tutorial"  },
    { "Villager2",           "tutorial"  },
    { "Villager3",           "tutorial"  },
    { "Seto",                "normal"    },
    { "Heishin",             "boss"      },
    { "Rex Raptor",          "normal"    },
    { "Weevil Underwood",    "normal"    },
    { "Mai Valentine",       "normal"    },
    { "Bandit Keith",        "normal"    },
    { "Shadi",               "tough"     },
    { "Yami Bakura",         "tough"     },
    { "Pegasus",             "boss"      },
    { "Isis",                "boss"      },
    { "Kaiba",               "boss"      },
    { "Mage Soldier",        "rookie"    },
    { "Jono 2nd",            "normal"    },
    { "Teana 2nd",           "normal"    },
    { "Ocean Mage",          "tough"     },
    { "High Mage Secmeton",  "tough"     },
    { "Forest Mage",         "tough"     },
    { "High Mage Anubisius", "tough"     },
    { "Mountain Mage",       "tough"     },
    { "High Mage Atenza",    "tough"     },
    { "Desert Mage",         "tough"     },
    { "High Mage Martis",    "tough"     },
    { "Meadow Mage",         "tough"     },
    { "High Mage Kepura",    "tough"     },
    { "Labyrinth Mage",      "boss"      },
    { "Seto 2nd",            "boss"      },
    { "Guardian Sebek",      "boss"      },
    { "Guardian Neku",       "boss"      },
    { "Heishin 2nd",         "boss"      },
    { "Seto 3rd",            "superboss" },
    { "DarkNite",            "superboss" },
    { "Nitemare",            "superboss" },
    { "Duel Master K",       "superboss" },
};
static int s_duelist_assign_n = 39;

/* Order matches PSX_DROP_DB's PsxDropDbDuelist.tier[]: S/A POW, B/C/D,
 * S/A TEC. Keyed in the ini as s_a_pow / b_c_d / s_a_tec. */
static double s_rank_multiplier[PSX_DROP_DB_TIERS] = { 0.75, 1.0, 0.5 };

static double duelist_tier_multiplier(const char *tier_name)
{
    for (int i = 0; i < s_duelist_tier_n; i++)
        if (name_ieq(s_duelist_tier[i].name, tier_name))
            return s_duelist_tier[i].multiplier;
    return 1.0;   /* tier named in [duelist_tiers] but never defined above */
}

static double duelist_multiplier_for(const char *duelist_name)
{
    for (int i = 0; i < s_duelist_assign_n; i++)
        if (name_ieq(s_duelist_assign[i].duelist, duelist_name))
            return duelist_tier_multiplier(s_duelist_assign[i].tier);
    return duelist_tier_multiplier("default");   /* never assigned a tier */
}

/* Per-card rarity colour, indexed by id. Built once from the baked drop DB,
 * the ini's thresholds and the ini's per-card overrides. */
static uint8_t s_card_color[CARD_COUNT + 1];
static int     s_color_built;

/* ---- card_name_color.ini --------------------------------------------------- */
#define INI_NAME "card_name_color.ini"
#define OVERRIDE_MAX 128
typedef struct { char name[NAME_MAX]; uint8_t color; } ColorOverride;
static ColorOverride s_override[OVERRIDE_MAX];
static int           s_override_n;

static int ini_path(char *out, unsigned cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (!dir || !dir[0]) return 0;
    const int n = snprintf(out, cap, "%s/%s", dir, INI_NAME);
    return n > 0 && (unsigned)n < cap;
}

static void ini_trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' '  || e[-1] == '\t')) *--e = 0;
}

/* A plain number (e.g. "color = 4") is passed straight through un-named,
 * for any value that doesn't have a name below. */
static int color_name_to_val(const char *v, uint8_t *out)
{
    if (name_ieq(v, "white"))  { *out = COL_WHITE;  return 1; }
    if (name_ieq(v, "yellow") || name_ieq(v, "gold")) { *out = COL_YELLOW; return 1; }
    if (name_ieq(v, "blue"))   { *out = COL_BLUE;   return 1; }
    if (name_ieq(v, "green"))  { *out = COL_GREEN;  return 1; }
    if (name_ieq(v, "grey") || name_ieq(v, "gray")) { *out = COL_GREY; return 1; }
    if (name_ieq(v, "orange")) { *out = COL_ORANGE; return 1; }
    if (name_ieq(v, "red"))    { *out = COL_RED;    return 1; }
    if (v[0] >= '0' && v[0] <= '9') {
        const int n = atoi(v);
        if (n >= 0 && n <= 255) { *out = (uint8_t)n; return 1; }
    }
    return 0;
}

/* For writing readable defaults; ini_load() never needs this direction. */
static const char *color_val_to_name(uint8_t c)
{
    switch (c) {
    case COL_WHITE:  return "white";
    case COL_YELLOW: return "yellow";
    case COL_BLUE:   return "blue";
    case COL_GREEN:  return "green";
    case COL_GREY:   return "grey";
    case COL_ORANGE: return "orange";
    case COL_RED:    return "red";
    default:         return "white";
    }
}

static void ini_write_default(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# CARD NAME COLOR - tints each card's name by drop rarity.\n"
        "# Edit and restart to apply. Delete this file to reset.\n"
        "\n[tiers]\n"
        "# Six tiers, rarest to most common: legendary, ultra_rare,\n"
        "# super_rare, rare, uncommon, default. A card gets the rarest\n"
        "# tier whose threshold it does not exceed. Thresholds decide\n"
        "# the order, not the tier names.\n"
        "#\n"
        "# threshold = best rarity score any duelist gives the card:\n"
        "# weight (out of 2048) times the multipliers below.\n"
        "# color = white|yellow|orange|red|blue|green|grey, or a number 0-6.\n"
        "legendary_threshold   = %g\n"
        "legendary_color       = %s\n"
        "ultra_rare_threshold  = %g\n"
        "ultra_rare_color      = %s\n"
        "super_rare_threshold  = %g\n"
        "super_rare_color      = %s\n"
        "rare_threshold        = %g\n"
        "rare_color            = %s\n"
        "uncommon_threshold    = %g\n"
        "uncommon_color        = %s\n"
        "default_color         = %s\n"
        "\n[duelist_tier_multipliers]\n"
        "# Name a duelist tier and give it a multiplier, applied to every\n"
        "# card that tier's duelists can drop. \"default\" always exists.\n",
        s_tier_threshold[TIER_LEGENDARY],  color_val_to_name(s_tier_color[TIER_LEGENDARY]),
        s_tier_threshold[TIER_ULTRA_RARE], color_val_to_name(s_tier_color[TIER_ULTRA_RARE]),
        s_tier_threshold[TIER_SUPER_RARE], color_val_to_name(s_tier_color[TIER_SUPER_RARE]),
        s_tier_threshold[TIER_RARE],       color_val_to_name(s_tier_color[TIER_RARE]),
        s_tier_threshold[TIER_UNCOMMON],   color_val_to_name(s_tier_color[TIER_UNCOMMON]),
        color_val_to_name(s_tier_color[TIER_DEFAULT]));
    for (int i = 0; i < s_duelist_tier_n; i++)
        fprintf(f, "%-9s = %g\n", s_duelist_tier[i].name, s_duelist_tier[i].multiplier);
    fprintf(f,
        "\n[duelist_tiers]\n"
        "# DUELIST NAME = tier name (must be defined above). Blank,\n"
        "# invalid or undefined tiers fall back to a multiplier of 1.\n");
    for (int i = 0; i < s_duelist_assign_n; i++)
        fprintf(f, "%s = %s\n", s_duelist_assign[i].duelist, s_duelist_assign[i].tier);
    fprintf(f,
        "\n[rank_multipliers]\n"
        "# Multiplier for each rank band: s_a_pow, b_c_d, s_a_tec.\n"
        "s_a_pow = %g\n"
        "b_c_d   = %g\n"
        "s_a_tec = %g\n"
        "\n[cards]\n"
        "# NAME = color, overrides the tier for that card. Blank or\n"
        "# invalid values fall back to [tiers]. Every card is listed\n"
        "# below, commented out; uncomment and set a color to pin one.\n",
        s_rank_multiplier[0], s_rank_multiplier[1], s_rank_multiplier[2]);
    for (int id = 1; id <= CARD_COUNT; id++)
        if (s_name[id][0])
            fprintf(f, "# %s = white\n", s_name[id]);
    fclose(f);
}

static void ini_load(void)
{
    char path[512];
    if (!ini_path(path, sizeof path)) return;
    FILE *f = fopen(path, "r");
    if (!f) { ini_write_default(path); return; }
    s_override_n = 0;
    char line[160], sect[24] = "";
    while (fgets(line, sizeof line, f)) {
        /* '#' and ';' both introduce a comment; ';' also ends an inline one
         * (e.g. "8   ; rare") without needing it at line start. */
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        ini_trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
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
        ini_trim(p);
        ini_trim(v);
        if (!strcmp(sect, "tiers")) {
            int tier = -1;
            if      (!strncmp(p, "legendary_", 10))  tier = TIER_LEGENDARY;
            else if (!strncmp(p, "ultra_rare_", 11))  tier = TIER_ULTRA_RARE;
            else if (!strncmp(p, "super_rare_", 11))  tier = TIER_SUPER_RARE;
            else if (!strncmp(p, "rare_", 5))         tier = TIER_RARE;
            else if (!strncmp(p, "uncommon_", 9))     tier = TIER_UNCOMMON;
            else if (!strncmp(p, "default_", 8))      tier = TIER_DEFAULT;
            if (tier < 0) continue;
            if (strstr(p, "threshold")) {
                const double n = atof(v);
                if (n >= 0) s_tier_threshold[tier] = n;
            } else if (strstr(p, "color") || strstr(p, "colour")) {
                uint8_t col;
                if (color_name_to_val(v, &col)) s_tier_color[tier] = col;
            }
        } else if (!strcmp(sect, "cards")) {
            uint8_t col;
            if (!p[0] || !color_name_to_val(v, &col)) continue;
            if (s_override_n >= OVERRIDE_MAX) continue;
            snprintf(s_override[s_override_n].name,
                     sizeof s_override[0].name, "%s", p);
            s_override[s_override_n].color = col;
            s_override_n++;
        } else if (!strcmp(sect, "duelist_tier_multipliers")) {
            if (!p[0]) continue;
            int i;
            for (i = 0; i < s_duelist_tier_n; i++)
                if (name_ieq(s_duelist_tier[i].name, p)) break;
            if (i == s_duelist_tier_n) {
                if (s_duelist_tier_n >= DUELIST_TIER_MAX) continue;
                snprintf(s_duelist_tier[i].name, sizeof s_duelist_tier[0].name, "%s", p);
                s_duelist_tier_n++;
            }
            s_duelist_tier[i].multiplier = atof(v);
        } else if (!strcmp(sect, "duelist_tiers")) {
            if (!p[0] || !v[0]) continue;
            if (s_duelist_assign_n >= DUELIST_ASSIGN_MAX) continue;
            snprintf(s_duelist_assign[s_duelist_assign_n].duelist,
                     sizeof s_duelist_assign[0].duelist, "%s", p);
            snprintf(s_duelist_assign[s_duelist_assign_n].tier,
                     sizeof s_duelist_assign[0].tier, "%s", v);
            s_duelist_assign_n++;
        } else if (!strcmp(sect, "rank_multipliers")) {
            int t = -1;
            if      (name_ieq(p, "s_a_pow")) t = 0;
            else if (name_ieq(p, "b_c_d"))   t = 1;
            else if (name_ieq(p, "s_a_tec")) t = 2;
            if (t >= 0) s_rank_multiplier[t] = atof(v);
        }
    }
    fclose(f);
}

/* An override for card id, or -1 if none. Needs s_names_ready. */
static int override_for(int id)
{
    for (int i = 0; i < s_override_n; i++)
        if (name_ieq(s_override[i].name, s_name[id]))
            return (int)s_override[i].color;
    return -1;
}

static void build_colors(void)
{
    if (s_color_built)
        return;
    if (!name_table_resident())
        return;               /* try again next call, once the EXE is up */
    decode_names();
    ini_load();
    sort_tiers_by_threshold();

    /* card_rarity[card] = the single best (highest) rarity SCORE — weight *
     * duelist tier multiplier * rank multiplier, see the rarity model
     * comment above — any one (duelist, band) entry ever gives this card. */
    static double card_rarity[CARD_COUNT + 1];
    memset(card_rarity, 0, sizeof card_rarity);
    for (int d = 0; d < PSX_DROP_DB_DUELISTS; d++) {
        const double dmult = duelist_multiplier_for(PSX_DROP_DB[d].name);
        for (int t = 0; t < PSX_DROP_DB_TIERS; t++) {
            const double rmult = s_rank_multiplier[t];
            for (int i = 0; i < PSX_DROP_DB[d].count[t]; i++) {
                const int c = PSX_DROP_DB[d].tier[t][i].card;
                const uint16_t w = PSX_DROP_DB[d].tier[t][i].weight;
                const double r = (double)w * dmult * rmult;
                if (c >= 1 && c <= CARD_COUNT && r > card_rarity[c])
                    card_rarity[c] = r;
            }
        }
    }
    for (int id = 1; id <= CARD_COUNT; id++) {
        const int ov = override_for(id);
        if (ov >= 0) {
            s_card_color[id] = (uint8_t)ov;
            continue;
        }
        const double n = card_rarity[id];
        uint8_t col = s_tier_color[TIER_DEFAULT];
        for (int i = 0; i < TIER_DEFAULT; i++) {
            const int t = s_tier_order[i];
            if (n <= s_tier_threshold[t]) { col = s_tier_color[t]; break; }
        }
        s_card_color[id] = col;
    }
    s_color_built = 1;
}

/* On by default: purely cosmetic, nothing to opt into. */
static int s_enabled = 1;

/* ---- confirming a run of glyphs actually IS the card's name ---------------
 * CARD_NAME_CALLER fires for every piece of text a card-info widget draws
 * (name, type, Guardian Stars, description), not just the name. The id
 * from PSX_SELECTED_CARD says which card; telling the name apart from the
 * rest needs the glyphs, matched against the one already-known target
 * (s_name[id]) rather than searched for — a type line or description can
 * never coincidentally equal a card's full name.
 *
 * func_80036C14 delivers one decoded character at a time: space=0x00,
 * 'A'-'Z'=0x60-0x79, 'a'-'z'=0x81-0x9A. This is a different cipher from the
 * frequency code s_name[] was decoded from, but resolves to the same ASCII
 * either way, so comparing them works. Apostrophe's code in this cipher is
 * not known; names containing one (e.g. "Fiend's Hand") only colour up to
 * the apostrophe. */
static char glyph_ascii(unsigned code)
{
    if (code == 0x00u) return ' ';
    if (code >= 0x60u && code <= 0x79u) return (char)('A' + (int)(code - 0x60u));
    if (code >= 0x81u && code <= 0x9Au) return (char)('a' + (int)(code - 0x81u));
    return 0;   /* not a name-stream code: an escape, a digit run, etc. */
}

/* State for matching one widget's incoming glyphs against s_match_target.
 * Reset at every CARD_NAME_CALLER entry; consumed glyph by glyph below. */
static uint32_t    s_match_obj;
static int         s_match_active;
static int         s_match_pos;
static const char *s_match_target;   /* == s_name[id]; owned by s_name[] */
static uint8_t      s_match_color;

static void card_name_text(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_TEXT_FN)
        return;

    const uint32_t obj = cpu->gpr[4];

    if (s_enabled && cpu->gpr[31] == CARD_NAME_CALLER) {
        const int id = (int)psx_mod_read_half(PSX_SELECTED_CARD);
        build_colors();
        if (s_color_built && id >= 1 && id <= CARD_COUNT && s_name[id][0]) {
            /* Revert first: this run has not been confirmed as the name
             * yet, so start from the stock colour and let the glyph hook
             * re-apply the rarity colour only while the match holds. */
            psx_mod_write_byte(obj + GLYPH_COLOR_OFF, COL_WHITE);
            s_match_obj    = obj;
            s_match_active = 1;
            s_match_pos    = 0;
            s_match_target = s_name[id];
            s_match_color  = s_card_color[id];
            return;
        }
    }

    /* Any other widget entry: nothing here is a candidate name run. */
    if (s_match_obj == obj)
        s_match_active = 0;
}

static void card_name_glyph(CPUState *cpu, uint32_t address)
{
    if (!cpu || address != PSX_GLYPH_FN)
        return;
    if (!s_match_active || cpu->gpr[4] != s_match_obj)
        return;

    const char c = glyph_ascii((unsigned)(cpu->gpr[5] & 0xFFu));
    if (c && c == s_match_target[s_match_pos]) {
        psx_mod_write_byte(cpu->gpr[4] + GLYPH_COLOR_OFF, s_match_color);
        s_match_pos++;
        if (!s_match_target[s_match_pos])
            s_match_active = 0;   /* full name matched; nothing more to do */
    } else {
        s_match_active = 0;      /* mismatch, or ran past the name's length:
                                   * this run is not the name after all */
    }
}

static const char *const ONOFF[] = { "Off", "On" };

static void card_name_color_enabled_changed(int value)
{
    s_enabled = value ? 1 : 0;
}

static void card_name_color_register_menu(void)
{
    (void)psx_video_menu_add_option(
        PSX_VM_MENU_MODS, "Card name color",
        "Tints card names by drop rarity",
        ONOFF, 2, "card_name_color", 1, card_name_color_enabled_changed);
}

PSX_MOD_CONSTRUCTOR(psx_card_name_color_install)
{
    card_name_color_register_menu();
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.text",
        PSX_TEXT_FN,
        card_name_text);
    (void)psx_mod_register_function_entry_plugin(
        "ygofm.card_name_color.glyph",
        PSX_GLYPH_FN,
        card_name_glyph);
}
