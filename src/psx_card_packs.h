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
#define PSX_CARD_PACK_DESC_MAX 199   /* "|" separates lines; auto-wrapped at 20 columns otherwise */
#define PSX_CARD_PACK_EQUIP_MAX   256
#define PSX_CARD_PACK_BOOST_UNSET (-32768)
#define PSX_CARD_PACK_EQUIP_ALL   (1u << 31)

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
} PsxCardPack;


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
} PsxCardStock;

/* Player folder holding the packs (".../cards"); "" before boot. */
const char *psx_card_packs_dir(void);

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
