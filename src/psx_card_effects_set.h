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

/* NAME COLORS
 *
 * Most of the entries below carry nothing but a name_color. They are the
 * rarity ladder yamyi worked out for the card-name mod, baked into the set
 * rather than computed at run time: a card's score is the best any one
 * (duelist, rank band) drop gives it -- weight x duelist tier x band -- and
 * the tiers are legendary (nobody drops it) blue, ultra rare red, super rare
 * orange, rare yellow, uncommon green, everything else the game's own white.
 * 228 of the 722 cards are not white; the other 494 are absent from this
 * list, which is the same thing as stock.
 *
 * The colors are the player's from the moment the set is written: change one
 * in the Card Manager, or delete the card's folder to put it back.
 */
static const PsxCardEffectsSeed PSX_CARD_EFFECTS_SET[] = {
    { 1, /* Blue-eyes White Dragon */
        "name_color = red\n"
    },
    { 7, /* Winged Dragon #1 */
        "name_color = blue\n"
    },
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
    { 17, /* Right Leg of the Forbidden One */
        "name_color = blue\n"
    },
    { 18, /* Left Leg of the Forbidden One */
        "name_color = blue\n"
    },
    { 20, /* Left Arm of the Forbidden One */
        "name_color = green\n"
    },
    { 21, /* Exodia the Forbidden */
        "description = If you posses all parts of Exodia in your hand, you win the duel.\n"
        "color = orange\n"
        "name_color = red\n"
    },
    { 22, /* Summoned Skull */
        "name_color = blue\n"
    },
    { 28, /* Rock Ogre Grotto #1 */
        "name_color = blue\n"
    },
    { 37, /* Gaia the Dragon Champion */
        "name_color = blue\n"
    },
    { 51, /* Armored Lizard */
        "name_color = blue\n"
    },
    { 52, /* Hercules Beetle */
        "name_color = blue\n"
    },
    { 56, /* Larvae Moth */
        "name_color = blue\n"
    },
    { 57, /* Great Moth */
        "name_color = blue\n"
    },
    { 60, /* Great White */
        "name_color = blue\n"
    },
    { 62, /* Harpie Lady */
        "name_color = blue\n"
    },
    { 63, /* Harpie Lady Sisters */
        "name_color = blue\n"
    },
    { 64, /* Tiger Axe */
        "name_color = blue\n"
    },
    { 66, /* Kojikocy */
        "name_color = orange\n"
    },
    { 67, /* Perfectly Ultimate Great Moth */
        "name_color = blue\n"
    },
    { 69, /* Thousand Dragon */
        "name_color = blue\n"
    },
    { 72, /* Cocoon of Evolution */
        "name_color = blue\n"
    },
    { 92, /* Rabid Horseman */
        "name_color = blue\n"
    },
    { 117, /* Spirit of the Books */
        "name_color = orange\n"
    },
    { 128, /* LaMoon */
        "name_color = yellow\n"
    },
    { 151, /* Rhaimundos of the Red Sword */
        "name_color = orange\n"
    },
    { 163, /* Lisark */
        "name_color = orange\n"
    },
    { 216, /* Dryad */
        "name_color = yellow\n"
    },
    { 217, /* B. Skull Dragon */
        "name_color = blue\n"
    },
    { 235, /* Wodan the Resident of the Forest */
        "name_color = blue\n"
    },
    { 249, /* Water Omotics */
        "name_color = orange\n"
    },
    { 252, /* Nekogal #1 */
        "name_color = blue\n"
    },
    { 275, /* Ground Attacker Bugroth */
        "name_color = yellow\n"
    },
    { 278, /* Petit Moth */
        "name_color = blue\n"
    },
    { 281, /* Mystic Clown */
        "name_color = green\n"
    },
    { 284, /* Tao the Chanter */
        "name_color = blue\n"
    },
    { 287, /* Ogre of the Black Shadow */
        "name_color = yellow\n"
    },
    { 288, /* Dark Artist */
        "name_color = blue\n"
    },
    { 297, /* Cyber Soldier of Darkworld */
        "name_color = yellow\n"
    },
    { 299, /* Sonic Maid */
        "name_color = blue\n"
    },
    { 309, /* Steel Shell */
        "name_color = green\n"
    },
    { 310, /* Vile Germs */
        "name_color = green\n"
    },
    { 315, /* Dragon Treasure */
        "name_color = green\n"
    },
    { 318, /* Elegant Egotist */
        "name_color = blue\n"
    },
    { 327, /* Follow Wind */
        "name_color = green\n"
    },
    { 328, /* Power of Kaishin */
        "name_color = green\n"
    },
    { 337, /* Raigeki */
        "name_color = green\n"
    },
    { 341, /* Soul of the Pure */
        "name_color = green\n"
    },
    { 342, /* Dian Keto the Cure Master */
        "name_color = red\n"
    },
    { 346, /* Ookazi */
        "name_color = yellow\n"
    },
    { 347, /* Tremendous Fire */
        "name_color = yellow\n"
    },
    { 348, /* Swords of Revealing Light */
        "name_color = blue\n"
    },
    { 351, /* Yaranzo */
        "name_color = blue\n"
    },
    { 352, /* Kanan the Swordmistress */
        "name_color = blue\n"
    },
    { 353, /* Takriminos */
        "name_color = blue\n"
    },
    { 354, /* Stuffed Animal */
        "name_color = blue\n"
    },
    { 355, /* Megasonic Eye */
        "name_color = blue\n"
    },
    { 356, /* Super War-lion */
        "name_color = blue\n"
    },
    { 357, /* Yamadron */
        "name_color = blue\n"
    },
    { 358, /* Seiyaryu */
        "name_color = blue\n"
    },
    { 359, /* Three-legged Zombies */
        "name_color = blue\n"
    },
    { 360, /* Zera The Mant */
        "name_color = blue\n"
    },
    { 361, /* Flying Penguin */
        "name_color = blue\n"
    },
    { 362, /* Millennium Shield */
        "name_color = blue\n"
    },
    { 363, /* Fairy's Gift */
        "name_color = blue\n"
    },
    { 364, /* Black Luster Soldier */
        "name_color = blue\n"
    },
    { 365, /* Fiend's Mirror */
        "name_color = blue\n"
    },
    { 367, /* Jirai Gumo */
        "name_color = red\n"
    },
    { 369, /* Wall Shadow */
        "name_color = blue\n"
    },
    { 370, /* Labyrinth Tank */
        "name_color = blue\n"
    },
    { 371, /* Sanga of the Thunder */
        "name_color = blue\n"
    },
    { 372, /* Kazejin */
        "name_color = blue\n"
    },
    { 373, /* Suijin */
        "name_color = blue\n"
    },
    { 374, /* Gate Guardian */
        "name_color = blue\n"
    },
    { 375, /* Dungeon Worm */
        "name_color = blue\n"
    },
    { 377, /* Ryu-kishin Powered */
        "name_color = blue\n"
    },
    { 378, /* Swordstalker */
        "name_color = red\n"
    },
    { 380, /* Blue-eyes Ultimate Dragon */
        "name_color = blue\n"
    },
    { 381, /* Toon Alligator */
        "name_color = red\n"
    },
    { 382, /* Rude Kaiser */
        "name_color = blue\n"
    },
    { 383, /* Parrot Dragon */
        "name_color = blue\n"
    },
    { 384, /* Dark Rabbit */
        "name_color = yellow\n"
    },
    { 385, /* Bickuribox */
        "name_color = blue\n"
    },
    { 386, /* Harpie's Pet Dragon */
        "name_color = blue\n"
    },
    { 389, /* Giltia the D. Knight */
        "name_color = red\n"
    },
    { 390, /* Launcher Spider */
        "name_color = blue\n"
    },
    { 392, /* Metalzoa */
        "name_color = blue\n"
    },
    { 401, /* Ushi Oni */
        "name_color = green\n"
    },
    { 404, /* Tatsunootoshigo */
        "name_color = orange\n"
    },
    { 407, /* Machine King */
        "name_color = yellow\n"
    },
    { 418, /* Golgoil */
        "name_color = yellow\n"
    },
    { 424, /* Sky Dragon */
        "name_color = red\n"
    },
    { 427, /* Kaiser Dragon */
        "name_color = blue\n"
    },
    { 428, /* Magician of Faith */
        "name_color = blue\n"
    },
    { 429, /* Goddess of Whim */
        "name_color = blue\n"
    },
    { 433, /* Ancient Elf */
        "name_color = yellow\n"
    },
    { 434, /* Beautiful Beast Trainer */
        "name_color = red\n"
    },
    { 442, /* Aqua Dragon */
        "name_color = blue\n"
    },
    { 443, /* Sea King Dragon */
        "name_color = green\n"
    },
    { 447, /* Giant Red Seasnake */
        "name_color = green\n"
    },
    { 448, /* Spike Seadra */
        "name_color = orange\n"
    },
    { 449, /* 30,000-Year White Turtle */
        "name_color = red\n"
    },
    { 454, /* Destroyer Golem */
        "name_color = orange\n"
    },
    { 456, /* Minomushi Warrior */
        "name_color = orange\n"
    },
    { 470, /* Magical Ghost */
        "name_color = green\n"
    },
    { 471, /* Soul Hunter */
        "name_color = green\n"
    },
    { 472, /* Air Eater */
        "name_color = yellow\n"
    },
    { 483, /* Garvas */
        "name_color = green\n"
    },
    { 489, /* Barrel Lily */
        "name_color = blue\n"
    },
    { 493, /* Maha Vailo */
        "name_color = green\n"
    },
    { 494, /* Rainbow Marine Mermaid */
        "name_color = red\n"
    },
    { 495, /* Musician King */
        "name_color = red\n"
    },
    { 497, /* Yado Karu */
        "name_color = yellow\n"
    },
    { 498, /* Morinphen */
        "name_color = yellow\n"
    },
    { 499, /* Kattapillar */
        "name_color = blue\n"
    },
    { 500, /* Dragon Seeker */
        "name_color = green\n"
    },
    { 507, /* Crazy Fish */
        "name_color = green\n"
    },
    { 509, /* Bracchio-raidus */
        "name_color = green\n"
    },
    { 515, /* The Statue of Easter Island */
        "name_color = green\n"
    },
    { 517, /* Sand Stone */
        "name_color = orange\n"
    },
    { 518, /* Boulder Tortoise */
        "name_color = red\n"
    },
    { 519, /* Fire Kraken */
        "name_color = red\n"
    },
    { 520, /* Turtle Bird */
        "name_color = red\n"
    },
    { 521, /* Skullbird */
        "name_color = green\n"
    },
    { 523, /* The Bistro Butcher */
        "name_color = yellow\n"
    },
    { 525, /* Spirit of the Mountain */
        "name_color = yellow\n"
    },
    { 526, /* Neck Hunter */
        "name_color = yellow\n"
    },
    { 529, /* Flame Cerebrus */
        "name_color = red\n"
    },
    { 532, /* Gemini Elf */
        "name_color = green\n"
    },
    { 533, /* Kwagar Hercules */
        "name_color = green\n"
    },
    { 535, /* Kamakiriman */
        "name_color = yellow\n"
    },
    { 537, /* Mega Thunderball */
        "name_color = blue\n"
    },
    { 539, /* Corroding Shark */
        "name_color = green\n"
    },
    { 541, /* Hane-Hane */
        "name_color = blue\n"
    },
    { 545, /* Skelgon */
        "name_color = blue\n"
    },
    { 551, /* Dark Elf */
        "name_color = yellow\n"
    },
    { 553, /* Mushroom Man #2 */
        "name_color = green\n"
    },
    { 554, /* Lava Battleguard */
        "name_color = blue\n"
    },
    { 555, /* Tyhone #2 */
        "name_color = blue\n"
    },
    { 560, /* Invader from Another Dimension */
        "name_color = green\n"
    },
    { 562, /* Needle Worm */
        "name_color = blue\n"
    },
    { 565, /* Man-eating Black Shark */
        "name_color = orange\n"
    },
    { 570, /* Trakadon */
        "name_color = green\n"
    },
    { 572, /* Empress Judge */
        "name_color = green\n"
    },
    { 574, /* Witch of the Black Forest */
        "name_color = green\n"
    },
    { 575, /* Ancient One of the Deep Forest */
        "name_color = green\n"
    },
    { 578, /* Leo Wizard */
        "name_color = yellow\n"
    },
    { 580, /* Patrol Robo */
        "name_color = green\n"
    },
    { 581, /* Takuhee */
        "name_color = red\n"
    },
    { 583, /* Weather Report */
        "name_color = green\n"
    },
    { 587, /* Mon Larvas */
        "name_color = yellow\n"
    },
    { 593, /* Giant Turtle Who Feeds on Flames */
        "name_color = yellow\n"
    },
    { 594, /* Rose Spectre of Dunn */
        "name_color = green\n"
    },
    { 595, /* Fiend Refrection #1 */
        "name_color = orange\n"
    },
    { 600, /* Key Mace #2 */
        "name_color = green\n"
    },
    { 603, /* Fairy Dragon */
        "name_color = blue\n"
    },
    { 607, /* Great Bill */
        "name_color = red\n"
    },
    { 610, /* Electric Lizard */
        "name_color = red\n"
    },
    { 613, /* Twin-headed Thunder Dragon */
        "name_color = yellow\n"
    },
    { 614, /* Hunter Spider */
        "name_color = yellow\n"
    },
    { 616, /* Hourglass of Courage */
        "name_color = red\n"
    },
    { 617, /* Marine Beast */
        "name_color = red\n"
    },
    { 618, /* Warrior of Tradition */
        "name_color = green\n"
    },
    { 619, /* Rock Spirit */
        "name_color = red\n"
    },
    { 621, /* Succubus Knight */
        "name_color = orange\n"
    },
    { 622, /* Ill Witch */
        "name_color = orange\n"
    },
    { 623, /* The Thing That Hides in the Mud */
        "name_color = green\n"
    },
    { 624, /* High Tide Gyojin */
        "name_color = red\n"
    },
    { 627, /* Nekogal #2 */
        "name_color = green\n"
    },
    { 628, /* Witch's Apprentice */
        "name_color = blue\n"
    },
    { 630, /* Ancient Lizard Warrior */
        "name_color = yellow\n"
    },
    { 631, /* Maiden of the Moonlight */
        "name_color = green\n"
    },
    { 632, /* Stone Ogre Grotto */
        "name_color = yellow\n"
    },
    { 634, /* Night Lizard */
        "name_color = green\n"
    },
    { 639, /* Amphibious Bugroth */
        "name_color = red\n"
    },
    { 640, /* Acid Crawler */
        "name_color = blue\n"
    },
    { 641, /* Invader of the Throne */
        "name_color = red\n"
    },
    { 644, /* Flame Viper */
        "name_color = blue\n"
    },
    { 645, /* Royal Guard */
        "name_color = red\n"
    },
    { 647, /* Hyosube */
        "name_color = yellow\n"
    },
    { 656, /* Eternal Rest */
        "name_color = green\n"
    },
    { 657, /* Megamorph */
        "name_color = green\n"
    },
    { 659, /* Winged Trumpeter */
        "name_color = green\n"
    },
    { 661, /* Crush Card */
        "name_color = green\n"
    },
    { 663, /* Breath of Light */
        "name_color = green\n"
    },
    { 664, /* Eternal Draught */
        "name_color = green\n"
    },
    { 665, /* Curse of Millennium Shield */
        "name_color = green\n"
    },
    { 667, /* Gate Guardian Ritual */
        "name_color = blue\n"
    },
    { 668, /* Bright Castle */
        "name_color = green\n"
    },
    { 669, /* Shadow Spell */
        "name_color = blue\n"
    },
    { 670, /* Black Luster Ritual */
        "name_color = blue\n"
    },
    { 671, /* Zera Ritual */
        "name_color = green\n"
    },
    { 673, /* War-lion Ritual */
        "name_color = green\n"
    },
    { 674, /* Beastry Mirror Ritual */
        "name_color = green\n"
    },
    { 675, /* Ultimate Dragon */
        "name_color = blue\n"
    },
    { 678, /* Revival of Sennen Genjin */
        "name_color = green\n"
    },
    { 679, /* Novox's Prayer */
        "name_color = green\n"
    },
    { 680, /* Curse of Tri-Horned Dragon */
        "name_color = green\n"
    },
    { 689, /* Reverse Trap */
        "name_color = blue\n"
    },
    { 691, /* Revived of Serpent Night Dragon */
        "name_color = green\n"
    },
    { 692, /* Turtle Oath */
        "name_color = green\n"
    },
    { 694, /* Resurrection of Chakra */
        "name_color = green\n"
    },
    { 696, /* Javelin Beetle Pact */
        "name_color = blue\n"
    },
    { 697, /* Garma Sword Oath */
        "name_color = green\n"
    },
    { 698, /* Cosmo Queen's Prayer */
        "name_color = green\n"
    },
    { 699, /* Revival of Skeleton Rider */
        "name_color = green\n"
    },
    { 700, /* Fortress Whale's Oath */
        "name_color = green\n"
    },
    { 701, /* Performance of Sword */
        "name_color = blue\n"
    },
    { 702, /* Hungry Burger */
        "name_color = blue\n"
    },
    { 703, /* Sengenjin */
        "name_color = blue\n"
    },
    { 704, /* Skull Guardian */
        "name_color = blue\n"
    },
    { 705, /* Tri-horned Dragon */
        "name_color = blue\n"
    },
    { 706, /* Serpent Night Dragon */
        "name_color = blue\n"
    },
    { 707, /* Skull Knight */
        "name_color = green\n"
    },
    { 708, /* Cosmo Queen */
        "name_color = blue\n"
    },
    { 709, /* Chakra */
        "name_color = blue\n"
    },
    { 710, /* Crab Turtle */
        "name_color = blue\n"
    },
    { 711, /* Mikazukinoyaiba */
        "name_color = blue\n"
    },
    { 712, /* Meteor Dragon */
        "name_color = green\n"
    },
    { 713, /* Meteor B. Dragon */
        "name_color = green\n"
    },
    { 714, /* Firewing Pegasus */
        "name_color = green\n"
    },
    { 715, /* Psycho-Puppet */
        "name_color = blue\n"
    },
    { 716, /* Garma Sword */
        "name_color = blue\n"
    },
    { 717, /* Javelin Beetle */
        "name_color = blue\n"
    },
    { 718, /* Fortress Whale */
        "name_color = blue\n"
    },
    { 719, /* Dokurorider */
        "name_color = blue\n"
    },
    { 720, /* Mask of Shine & Dark */
        "name_color = blue\n"
    },
    { 721, /* Dark Magic Ritual */
        "name_color = blue\n"
    },
    { 722, /* Magician of Black Chaos */
        "name_color = blue\n"
    },
};
#define PSX_CARD_EFFECTS_SET_N ((int)(sizeof PSX_CARD_EFFECTS_SET / sizeof PSX_CARD_EFFECTS_SET[0]))

#endif /* PSX_CARD_EFFECTS_SET_H */
