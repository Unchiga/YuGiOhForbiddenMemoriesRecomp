/* psx_card_packs.h — replace a stock card in every screen at once.
 *
 * A "card pack" is a folder <player-data>/cards/<id>/ holding a card.ini and
 * optional PNGs. The mod turns it into (a) replacement disc sectors for the
 * card's 2D art record and its duel thumbnail, served through the runtime's
 * sector overrides, and (b) per-frame writes of the card's name, stats word
 * and level/attribute byte into the EXE tables. Library page, chest viewer,
 * password screen, build-deck list and the duel all read from those two
 * places, so one pack covers all of them. See psx_card_packs.c.
 */
#ifndef PSX_CARD_PACKS_H
#define PSX_CARD_PACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_CARD_PACK_NAME_MAX 40
#define PSX_CARD_PACK_DESC_MAX 255   /* "|" separates lines; auto-wrapped at 20 columns otherwise; the card shows 6 x 20 */
#define PSX_CARD_PACK_EQUIP_MAX   256
#define PSX_CARD_PACK_BOOST_UNSET (-32768)
#define PSX_CARD_PACK_EQUIP_ALL   (1u << 31)
#define PSX_CARD_PACK_FILTER_TYPE 1000
#define PSX_CARD_PACK_FILTER_HAND 999      /* "per card in hand" (own hand) */
/* equip mask: bits 0..19 monster types, bits 24..29 attributes (Light..Wind), bit 31 all */
#define PSX_CARD_PACK_EQUIP_ATTR_BIT(a) (1u << (24 + (a)))
/* words a bonus filter, e.g. "Lava Battleguard", "Dragon", "allied monster" */
const char *psx_card_packs_filter_name(int filter, int enemy);

/* One triggered effect: a magic effect class and its number. */
typedef struct { int fx, amount, target, terrain; } PsxCardFxSpec;

/* A trigger holds up to four branches, tried in order: each rolls its own
 * chance (100 = always), and an "else" branch fires only when the branch
 * before it did not. "50%: raigeki; else: destroy_own" is Time Wizard. */
#define PSX_CARD_BRANCHES 4
typedef struct { int chance; int is_else; int fx, amount, target, terrain; } PsxCardFxBranch;
typedef struct { int n; PsxCardFxBranch b[PSX_CARD_BRANCHES]; } PsxCardTrigger;

/* Everything a pack can set. A field left at its "unset" value (-1, or an
 * empty name) keeps the stock value; that is how a pack that only changes
 * the art leaves the numbers alone. */
typedef struct {
    int  id;                              /* 1..722 */
    char name[PSX_CARD_PACK_NAME_MAX + 1];
    char description[PSX_CARD_PACK_DESC_MAX + 1];
    int  attack, defense;                 /* 0..5110, multiples of 10 */
    int  star1, star2;                    /* 1..10 */
    int  type;                            /* 0..23 */
    int  level;                           /* 0..12 */
    int  attribute;                       /* 0..7 */
    int  price;                           /* 0..999999 */
    char password[9];                     /* 8 digits, or "" */
    int  has_art, has_thumb, has_title;   /* which PNGs exist */

    /* ---- effects (see psx_card_effects.c) ----------------------------------
     * A Magic card's effect and its magnitude; an Equip card's bonus and the
     * monsters it fits; a field card's boosts; a trap's ATK ceiling; a
     * ritual's recipe. Unset = -1 (lists: *_set = 0). */
    int  effect;                          /* PSX_CARD_FX_*, -1 = stock */
    int  amount;                          /* LP, ATK threshold or ATK/DEF delta; -1 = stock */
    int  target;                          /* monster type 0..19 for destroy_type; -1 */
    int  terrain;                         /* 1..6 for effect = field; -1 */
    int  equip_bonus;                     /* 0..9990; -1 = stock (+500, Megamorph +1000) */
    int  equips_set;                      /* the equip list below replaces the stock one */
    uint32_t equip_types;                 /* bit t = monsters of type t; bit 31 = every monster */
    int  equip_n;
    uint16_t equip_ids[PSX_CARD_PACK_EQUIP_MAX];
    int  boost_set;                       /* the 20 boosts below apply (field cards 330..335) */
    int  boost[20];                       /* per monster type, -1280..1270, x10; PSX_CARD_PACK_BOOST_UNSET = stock */
    int  trap_atk_max;                    /* 0..25500; -1 = stock (traps 681..686) */
    int  ritual_set;
    int  ritual_mat[3], ritual_result;    /* card ids */
    int  color;                           /* frame colour slot 0..5 (PSX_CARD_COLOR_*), -1 = stock */

    /* ---- monster effects (see psx_monster_effects.c) ---------------------- */
    int  battle;                          /* PSX_CARD_BATTLE_*, -1 = stock */
    PsxCardTrigger on_summon, on_flip, on_death, on_attack, each_turn;  /* n = 0 = none */
    int  bonus_flat, bonus_ally, bonus_enemy;   /* ATK/DEF while on the field; PSX_CARD_PACK_BOOST_UNSET */
    int  bonus_ally_filter, bonus_enemy_filter; /* which monsters count: 0 any, 1..722 that card, PSX_CARD_PACK_FILTER_TYPE+t that type */
    int  immune;                          /* -1 unset; bit 1 traps, bit 2 magic destruction */
} PsxCardPack;

enum { PSX_CARD_BATTLE_NONE = 0, PSX_CARD_BATTLE_INDESTRUCTIBLE, PSX_CARD_BATTLE_MUTUAL, PSX_CARD_BATTLE_SLAYER, PSX_CARD_BATTLE_COUNT };
#define PSX_CARD_IMMUNE_TRAPS 1
#define PSX_CARD_IMMUNE_MAGIC 2
const char *psx_card_packs_battle_name(int b);
int  psx_card_packs_parse_battle(const char *v);
/* "damage 1000", "destroy_type Dragon", "field Yami", "raigeki", "none" */
int  psx_card_packs_parse_spec(const char *v, PsxCardFxSpec *out, char *err, unsigned errcap);
void psx_card_packs_format_spec(const PsxCardFxSpec *f, char *out, unsigned cap);
/* "50%: raigeki; else: destroy_own; 25%: damage 500" */
int  psx_card_packs_parse_trigger(const char *v, PsxCardTrigger *out, char *err, unsigned errcap);
void psx_card_packs_format_trigger(const PsxCardTrigger *t, char *out, unsigned cap);
/* "500, 200 per ally, 100 per enemy" */
int  psx_card_packs_parse_bonus(const char *v, PsxCardPack *c, char *err, unsigned errcap);
void psx_card_packs_format_bonus(const PsxCardPack *c, char *out, unsigned cap);
const char *psx_card_packs_immune_name(int bits);   /* 0..3 */
int  psx_card_packs_parse_immune(const char *v);
/* 1 when any monster-effect field is set. */
int  psx_card_packs_has_monster_effect(const PsxCardPack *c);

/* Frame colours: the disc carries six palettes (only four are used in stock:
 * monsters yellow, magic green, traps pink, rituals blue; purple and orange
 * sit unused). See psx_card_colors.c. */
enum {
    PSX_CARD_COLOR_YELLOW = 0,    /* normal monster */
    PSX_CARD_COLOR_GREEN,         /* spell */
    PSX_CARD_COLOR_PINK,          /* trap */
    PSX_CARD_COLOR_BLUE,          /* ritual */
    PSX_CARD_COLOR_PURPLE,        /* fusion */
    PSX_CARD_COLOR_ORANGE,        /* effect monster */
    PSX_CARD_COLOR_COUNT          /* the disc's seventh palette is empty and the grid has no sprite for it */
};
const char *psx_card_packs_color_name(int slot);   /* "Yellow (normal)" */
int  psx_card_packs_parse_color(const char *v);


/* Effects a Magic card can carry. Each is a stock effect class the game
 * already implements; the layer only chooses the class and its number. */
enum {
    PSX_CARD_FX_NONE = 0,         /* does nothing when played */
    PSX_CARD_FX_HEAL,             /* amount LP to the player */
    PSX_CARD_FX_DAMAGE,           /* amount LP off the opponent */
    PSX_CARD_FX_DESTROY_TYPE,     /* destroy the opponent's monsters of `target` type */
    PSX_CARD_FX_DESTROY_ATK,      /* destroy the opponent's monsters with ATK >= amount */
    PSX_CARD_FX_RAIGEKI,          /* destroy every opponent monster */
    PSX_CARD_FX_DARK_HOLE,        /* destroy every card on both fields */
    PSX_CARD_FX_DRAGON_JAR,       /* destroy the opponent's Dragons */
    PSX_CARD_FX_STOP_DEFENSE,     /* opponent's defenders to attack position */
    PSX_CARD_FX_FLIP,             /* flip every face-down monster face up */
    PSX_CARD_FX_WEAKEN,           /* opponent's monsters: ATK and DEF - amount (negative = boost) */
    PSX_CARD_FX_SWORDS,           /* Swords of Revealing Light */
    PSX_CARD_FX_CURSEBREAKER,     /* clear the maluses on your monsters */
    PSX_CARD_FX_HARPIE,           /* destroy the opponent's magic/trap zone */
    PSX_CARD_FX_FIELD,            /* change the field to `terrain` */
    PSX_CARD_FX_RITUAL,           /* the ritual recipe below */
    PSX_CARD_FX_DESTROY_STRONGEST,/* destroy the opponent's strongest monster (ties all go) */
    PSX_CARD_FX_LOSE_LP,          /* the caster / owner loses `amount` LP */
    PSX_CARD_FX_GAMBLE_LP,        /* Jirai Gumo's coin: tails, the owner loses half their LP */
    PSX_CARD_FX_GAMBLE,           /* Time Wizard's coin: heads destroys the opponent's monsters, tails your own
                                     and their total ATK comes off your LP (monster triggers only) */
    PSX_CARD_FX_DESTROY_OWN,      /* destroy all of the owner's own monsters (monster triggers only) */
    PSX_CARD_FX_DESTROY_OWN_LP,   /* ... and lose LP equal to half their total ATK (Time Wizard's tails) */
    PSX_CARD_FX_COUNT
};
const char *psx_card_packs_effect_name(int fx);      /* ini spelling */
const char *psx_card_packs_effect_label(int fx);     /* for the manager */
const char *psx_card_packs_terrain_name(int terrain);/* 1..6 */
int  psx_card_packs_parse_effect(const char *v);
/* Parse / print the list forms used in card.ini and the manager:
 *   equips: "Dragon, Warrior, 12, 15" or "all" or "none"   (types, ids)
 *   boost:  "Dragon +500, Fairy -500"
 *   ritual: "1, 1, 1 -> 380"
 * Return 1 on success and describe a problem in err when given. */
int  psx_card_packs_parse_equips(const char *v, PsxCardPack *c, char *err, unsigned errcap);
int  psx_card_packs_parse_boost(const char *v, PsxCardPack *c, char *err, unsigned errcap);
int  psx_card_packs_parse_ritual(const char *v, PsxCardPack *c, char *err, unsigned errcap);
void psx_card_packs_format_equips(const PsxCardPack *c, char *out, unsigned cap);
void psx_card_packs_format_boost(const PsxCardPack *c, char *out, unsigned cap);
void psx_card_packs_format_ritual(const PsxCardPack *c, char *out, unsigned cap);
/* Set every effect field of a pack to unset. */
void psx_card_packs_effects_reset(PsxCardPack *c);

/* How the game will lay a description out: the number of lines (auto-wrapped
 * at 20 columns, or as broken by "|"), the longest line, and the first line
 * (1-based) longer than 20 columns, 0 when none. The game shows 6 lines. */
#define PSX_CARD_PACK_DESC_COLS  20
#define PSX_CARD_PACK_DESC_LINES 6
int  psx_card_packs_desc_layout(const char *text, int *lines, int *longest, int *first_wide);

/* The stock values of a card, read from the game's own tables. Valid once
 * psx_card_db_ready(). */
typedef struct {
    char name[PSX_CARD_PACK_NAME_MAX + 1];
    char description[PSX_CARD_PACK_DESC_MAX + 1];
    int  attack, defense, star1, star2, type, level, attribute;
    int  price;
    char password[9];
    /* stock effect facts, from the game's tables (see psx_card_effects.c) */
    int  effect;            /* PSX_CARD_FX_* the stock card has, or -1 = a code-only card */
    int  amount;            /* its magnitude, or -1 */
    int  equip_bonus;       /* 500 / 1000, equips only */
    int  trap_atk_max;      /* traps 681..686 */
    int  boost[20];         /* field cards */
    int  ritual_mat[3], ritual_result;
    int  color;             /* the frame colour the stock card draws with */
} PsxCardStock;

/* Player folder holding the packs (".../cards"); "" before boot. */
const char *psx_card_packs_dir(void);

/* Two card sets share the machinery: the player's own edits in cards/, and
 * the Card Effects mod's set in mods/card_effects/cards/. One is live at a
 * time; switching unloads one and loads the other (the game sees the change
 * on the next screen), and the Card Manager, Export and Import work on the
 * live set. Persisted as menu_settings.ini `card_effects`. */
void psx_card_packs_set_dev(int dev);
int  psx_card_packs_is_dev(void);
/* the Card Effects mod's menu row, so a button elsewhere can drive it */
void psx_card_packs_register_menu(void);

/* Pack for a card, or 0 when none is loaded. */
int  psx_card_packs_get(int id, PsxCardPack *out);
/* Stock values (the game's own tables and the disc). */
int  psx_card_packs_stock(int id, PsxCardStock *out);
/* Write card.ini for a pack (creating the folder) and apply it. Fields at
 * their unset value are omitted from the file. Returns 0 on I/O failure. */
int  psx_card_packs_save(const PsxCardPack *pack);
/* Delete the pack folder's files and restore the card to stock. */
int  psx_card_packs_remove(int id);
/* Re-read one pack (or all, id <= 0) from disk and re-apply. */
void psx_card_packs_reload(int id);
/* Bumps on every apply, so a viewer can redraw when something changed. */
unsigned psx_card_packs_generation(void);
/* Debug-server state line: dir, generation, and the loaded pack ids. */
int psx_card_packs_state_json(char *out, unsigned cap);

/* Card art as RGB, 102x96, for previews: what the game will show (the pack's
 * art when it has one, else stock). out is 102*96*3 bytes. */
int  psx_card_packs_art_rgb(int id, uint8_t *out);
/* The thumbnail the same way: 40x32x3. */
int  psx_card_packs_thumb_rgb(int id, uint8_t *out);

/* Type / attribute / star names, for editors and ini files. */
const char *psx_card_packs_type_name(int type);
const char *psx_card_packs_attribute_name(int attribute);
const char *psx_card_packs_star_name(int star);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_PACKS_H */
