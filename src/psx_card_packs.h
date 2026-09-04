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
} PsxCardPack;

/* The stock values of a card, read from the game's own tables. Valid once
 * psx_card_db_ready(). */
typedef struct {
    char name[PSX_CARD_PACK_NAME_MAX + 1];
    char description[PSX_CARD_PACK_DESC_MAX + 1];
    int  attack, defense, star1, star2, type, level, attribute;
    int  price;
    char password[9];
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
