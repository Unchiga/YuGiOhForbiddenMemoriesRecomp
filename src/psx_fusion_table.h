/* psx_fusion_table.h — the game's fusion table as an editable thing.
 *
 * psx_fusion_db.c answers "what do these two cards make?" out of the RAM the
 * duel is using. This owns the other half: the table's bytes ON THE DISC, the
 * player's changes to them, and putting the two together.
 *
 * WHY THE DISC AND NOT RAM. The table is duel data — the loader streams it in
 * at the start of every duel and outside one the RAM copy reads as zeros — so
 * anything sourced from RAM can only work inside a duel, which is the one
 * place you are not building a deck. The same bytes sit in WA_MRG.MRG and
 * `psx_mod_cd_read_stock_sector` will hand them over at any moment, game
 * running or not. Everything here is therefore available from boot.
 *
 * HOW AN EDIT REACHES THE GAME. Not by patching code: the seven per-terrain
 * copies of the table are replaced as DISC SECTORS (`psx_mod_cd_override_*`),
 * exactly the way psx_card_effects.c already replaces the equip and ritual
 * tables in the same block, so the game's own loader brings the edited table
 * in and every reader — the summon resolver and all three AI planners — sees
 * it. A duel already in progress is additionally poked in RAM so a change
 * lands without leaving the duel.
 *
 * THE ONE HARD LIMIT is the format's, not this module's: the loader copies
 * exactly 0x10000 bytes and record offsets are 16-bit, so the whole table must
 * pack into 65535 bytes. Stock uses 65002 of them. An edit that would not fit
 * is refused with the numbers, and nothing changes.
 */
#ifndef PSX_FUSION_TABLE_H
#define PSX_FUSION_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_FUSION_TABLE_CARDS  722
#define PSX_FUSION_TABLE_BYTES  0x10000

/* a <= b always: the game's lookup sorts its arguments, so a pair has exactly
 * one spelling. `glitch` marks a pair the packed bytes answer by accident
 * rather than on purpose — see psx_fusion_db.c on the three-byte tail. */
typedef struct { uint16_t a, b, r; uint8_t glitch; } PsxFusionRecipe;
typedef struct { uint16_t equip, mon; } PsxFusionEquip;

/* Are the stock bytes in hand? Reads them on first use; 0 only when no disc
 * is mounted or the sectors will not read. */
int psx_fusion_table_ready(void);
/* Re-read from the disc and rebuild, dropping nothing the player typed. */
int psx_fusion_table_refresh(void);

/* WHAT THE GAME WILL ANSWER, glitch pairs included: the packed bytes decoded
 * back through the guest's own walk. This is what a browser should show.
 * Sorted by (a, b). Valid until the next edit. */
const PsxFusionRecipe *psx_fusion_table_recipes(int *n);
/* The equip table, read from the same block. Read-only here — equips are
 * psx_card_effects.c's to edit. Grouped by equip id, as stored. */
const PsxFusionEquip *psx_fusion_table_equips(int *n, int *groups);
/* The DISC's own recipe list, edits or no edits: how many, and pair i. */
int psx_fusion_table_stock_count(void);
int psx_fusion_table_stock_pair(int i, int *a, int *b, int *r);

/* Effective result for a pair, 0 when they do not combine. */
int psx_fusion_table_result(int a, int b);
/* What the pair made before any edit, so a row can show what it was. */
int psx_fusion_table_stock_result(int a, int b);
/* 1 when the player has changed this pair. */
int psx_fusion_table_is_edited(int a, int b);

/* Change one pair. `result` 0 REMOVES the fusion; otherwise it is the card the
 * pair makes. Setting a pair back to its stock result drops the edit rather
 * than recording a no-op. Returns 1, or 0 with the reason in `err` (out of
 * range, more than 511 partners on one card, or the table would not fit) — and
 * on failure nothing has changed. Applies immediately when the override is on. */
int psx_fusion_table_edit(int a, int b, int result, char *err, unsigned cap);

int  psx_fusion_table_edit_count(void);
void psx_fusion_table_reset(void);          /* drop the edits, keep the switch */

/* Empty the table: nothing fuses with anything until something is added or
 * imported. Kept as a FLAG rather than 25131 delete-entries, so the saved set
 * stays a handful of lines and an addition on top of it is still one line. */
int  psx_fusion_table_clear_all(char *err, unsigned cap);
/* Every fusion this one card takes part in, gone. */
int  psx_fusion_table_clear_card(int id, char *err, unsigned cap);
/* 1 while the table has been emptied. */
int  psx_fusion_table_cleared(void);

/* Put EVERYTHING back: drop every edit, take the sector override out, turn
 * the MODS switch off and remove the saved set -- but copy that set to
 * fusion_edits_backup.txt on the way, because one click should not be able
 * to destroy an afternoon's work. Returns 1 with what happened in `err`. */
int  psx_fusion_table_restore_stock(char *err, unsigned cap);

/* Bytes the packed table needs, the 65535 it has, and how many pairs it holds. */
void psx_fusion_table_budget(int *used, int *capacity, int *pairs);

/* Install / remove the seven sector overrides. Applying also writes a running
 * duel's RAM copy, so an edit shows in the duel you are in. */
int  psx_fusion_table_apply(char *err, unsigned cap);
void psx_fusion_table_revert(void);
int  psx_fusion_table_applied(void);
/* MODS > FUSION EDITS. Registered here; the manager does not own it. */
void psx_fusion_table_register_menu(void);

/* <player-data>/fusion_edits.txt, in the same format Export writes. Saving
 * happens on every edit, so the set is back after a restart. */
int psx_fusion_table_save(char *err, unsigned cap);
int psx_fusion_table_load(char *err, unsigned cap);

/* `edits_only` writes just what the player changed (small, shareable);
 * otherwise every recipe in the table. Import reads either back, and also
 * reads a plain `card1,card2,result` CSV. */
int psx_fusion_table_export(const char *path, int edits_only, char *err, unsigned cap);
int psx_fusion_table_import(const char *path, char *err, unsigned cap);

/* Bumped whenever the recipe list changes, so a view knows to rebuild. */
unsigned psx_fusion_table_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FUSION_TABLE_H */
