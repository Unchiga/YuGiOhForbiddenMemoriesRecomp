/* psx_card_effects_set.h -- the Card Effects set, shipped with the build.
 *
 * MODS ships STOCK: a new install changes nothing about any card, so anyone
 * starts from a blank slate and edits their own cards/ set. This is the other
 * thing -- one curated set of effects, chosen to sit as close to the original
 * TCG as this game's engine allows, that a player can switch to with the Card
 * Manager's "Dev Card Effects" button and then edit freely.
 *
 * It lives in the binary rather than in a folder because a folder is a thing
 * that can go missing from a zip: the switch used to point at an empty
 * directory in the player-data folder that nothing ever filled, so turning it
 * on correctly showed every card stock and read as the feature being broken.
 * psx_card_packs.c writes these out the first time the set is asked for, and
 * never again -- after that they are the player's files to change or delete.
 *
 * Adding a card: edit it in the Card Manager with Dev Card Effects ON, then
 * copy its card.ini body in here. Keep the keys the manager writes.
 */
#ifndef PSX_CARD_EFFECTS_SET_H
#define PSX_CARD_EFFECTS_SET_H

typedef struct { int id; const char *ini; } PsxCardEffectsSeed;

static const PsxCardEffectsSeed PSX_CARD_EFFECTS_SET[] = {
    { 12, /* Swamp Battleguard */
        "description = Effect: Gains 500 ATK for each \"Lava Battleguard\" you control.\n"
        "bonus = 500 per Lava Battleguard\n"
    },
    { 15, /* Flame Swordsman */
        "color = purple\n"
    },
    { 16, /* Time Wizard */
        "description = Effect: 50% chance to destroy enemy monsters. Otherwise destroy your own and take 50% total ATT from your LP\n"
        "on_summon = 50%: raigeki; else: destroy_own_lp\n"
    },
    { 21, /* Exodia the Forbidden */
        "description = If you posses all parts of Exodia in your hand, you win the duel.\n"
        "color = orange\n"
    },
};
#define PSX_CARD_EFFECTS_SET_N ((int)(sizeof PSX_CARD_EFFECTS_SET / sizeof PSX_CARD_EFFECTS_SET[0]))

#endif /* PSX_CARD_EFFECTS_SET_H */
