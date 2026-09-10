/* psx_story_rewards.h — the card a duelist is guaranteed to drop.
 *
 * A story stop: Simon good for a Lady of Faith the first time you beat him,
 * Jono for a Baby Dragon, whatever the player sets. Beat them again and the
 * drop is the game's own weighted roll as always, so this places key cards at
 * fixed points rather than turning a duelist into a vending machine — unless
 * the pair says "every win", which repeats it.
 *
 * Campaign duels only. It replaces the drop rather than adding to it: with
 * MODS > CARD DROPS on, the scripted card is the one the game itself awards
 * and the extras still roll normally, so N cards a duel stays N cards a duel.
 *
 * There is no MODS row. A stock install has no pairs, which is the same thing
 * as off; the pairs are set in the Drop Table Manager (right-click a row) and
 * live in drop_table_edits.ini beside the weight edits, so one Save keeps
 * them and one Export shares them. Nothing here writes to the save: the
 * reward arrives through the game's own drop path, so what lands in the trunk
 * is a card the game itself awarded.
 */
#ifndef PSX_STORY_REWARDS_H
#define PSX_STORY_REWARDS_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* The pairs, stored by psx_drop_edits.c (same file, same Save, same Import /
 * Export as the weight edits). Duelist is the DROP DB index 0..38, which is
 * the opponent id minus one. card 0 = none; `every` 0 = first campaign win
 * only, 1 = every campaign win. */
int  psx_story_rewards_get(int duelist, int *out_every);
int  psx_story_rewards_set(int duelist, int card, int every);
int  psx_story_rewards_count(void);

/* ---- the drop path ------------------------------------------------------
 *
 * Both called from psx_card_drops.c, which owns the hooks on the duel's drop
 * roll and award. steer_roll runs LAST in the roll hook, after CARD DROPS has
 * rolled its extras from the stock table: it points the resident weight table
 * at the scripted card so the game's own in-flight roll returns it, and every
 * copy the results screen makes is then right by construction. restore_table
 * puts the table back on the award that follows. */
void psx_story_rewards_steer_roll(struct CPUState *cpu, unsigned tier);
/* Split form used by multi-drop: decide eligibility without changing RAM,
 * then steer the visible in-flight roll after the reward has already been
 * awarded first. select returns 0 when this duel has no scripted reward. */
int  psx_story_rewards_select(void);
void psx_story_rewards_steer_card(struct CPUState *cpu, unsigned tier, int card);
void psx_story_rewards_restore_table(void);

/* `story_rewards` debug command: the pairs and what the last duel decided. */
int  psx_story_rewards_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_STORY_REWARDS_H */
