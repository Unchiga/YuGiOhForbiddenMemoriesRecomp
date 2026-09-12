/* psx_drop_edits.h — the player's own drop-table edits.
 *
 * The Drop Table Manager lets the player edit any duelist's drops: a card's
 * weight in a band, or which band it sits in. This module is that edit layer —
 * storage, the ini it persists to, and the application of the edits onto a
 * 722-entry tier.
 *
 * One entry per (duelist, card): the card's full desired weight vector across
 * the three bands. Applying a tier pins every edited card at its value for
 * that band — zeros included, which is how removal and band moves work — and
 * renormalizes everyone else through psx_drop_pins_rescale, the same single
 * piece of arithmetic the DROP MISSING CARDS mod uses. The 2048 invariant is
 * therefore kept by construction, never re-implemented.
 *
 * The edits reach the running game through psx_drop_missing.c's tick — the
 * one writer that already fingerprints the resident duelist and never writes
 * a table twice — NOT through a second writer of their own. Layer order is
 * stock -> mod transform (if the row is on) -> these edits, both in the game
 * and in the viewer, so what the viewer shows is what the game rolls.
 */
#ifndef PSX_DROP_EDITS_H
#define PSX_DROP_EDITS_H

#include <stdint.h>
#include "psx_drop_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read drop_table_edits.ini once. Every entry point below calls this itself;
 * exposed for symmetry with the mod's ensure_loaded. */
void psx_drop_edits_ensure_loaded(void);

/* Any entries at all? The tick uses this to decide whether to keep watching
 * when the mod row is off. */
int psx_drop_edits_any(void);

/* Any content owned by this manager that must survive sharing? Unlike
 * psx_drop_edits_any(), this includes scripted rewards. Reward-only edits do
 * not need the live random-table watcher, but they do belong in every Drop
 * Tables, .ygocards and .ygomods export. */
int psx_drop_edits_has_export_content(void);

/* Entries recorded for one duelist. */
int psx_drop_edits_count(int duelist);

/* Unsaved changes since the last save/load? */
int psx_drop_edits_dirty(void);

/* Bumped on every change (set/unset/clear/load), so a cached view of the
 * effective tables knows when to rebuild. */
unsigned psx_drop_edits_generation(void);

/* The recorded vector for (duelist, card). Returns 1 and fills w if an entry
 * exists, 0 otherwise. */
int psx_drop_edits_get(int duelist, int card, uint16_t w[3]);

/* Record (or replace) the vector for (duelist, card). Returns 1, or 0 when
 * full/out of range. No validation here — the caller trial-applies first. */
int psx_drop_edits_set(int duelist, int card, const uint16_t w[3]);

/* Remove one entry / every entry for a duelist (duelist -1 = all). Returns
 * how many entries were removed. */
int psx_drop_edits_unset(int duelist, int card);
int psx_drop_edits_clear(int duelist);

/* Exact replacement bands are used by the manager's clear-and-rebuild flow.
 * A zero table is a pending authoring state only: apply returns -5 and Save /
 * export reject it. A nonempty replacement must total exactly 2048 and is
 * persisted additively by format 2 files. */
int psx_drop_edits_replace_band(int duelist, int tier,
                                const uint16_t weights[PSX_DROP_DB_CARDS]);
int psx_drop_edits_replacement(int duelist, int tier);
int psx_drop_edits_band_empty(int duelist, int tier);
int psx_drop_edits_empty_count(void);

/* Explain whether the current layer can be saved/applied. */
int psx_drop_edits_validate(char *err, unsigned errcap);

/* Write the ini. Returns 1 on success. */
int psx_drop_edits_save(void);

/* Sharing, the same shape as the Card and Fusion managers': the player picks
 * a file. Export writes the current edit layer to path (a missing .ini is
 * added); import REPLACES the whole layer with that file's contents. Both
 * fill msg with the line the window shows.
 *
 * An import KEEPS what it imported: it writes drop_table_edits.ini itself, so
 * the table is live in the running game and still there at the next launch,
 * the way every other manager's import behaves. A missing or unreadable file
 * changes nothing at all.
 *
 * share_dir is where the dialogs start: <player-data>/drop_tables, created
 * on demand. load_file is the raw import a bare name resolves against that
 * folder; it returns the entry count, -1 when unreadable, or -2 when invalid. */
int  psx_drop_edits_export_file(const char *path, char *msg, unsigned cap);
/* Export every current stock+edit weight as exact pow/bcd/tec tables. The
 * result is accepted by the normal importer and is intended for bulk edits. */
int  psx_drop_edits_export_all_file(const char *path, char *msg, unsigned cap);
int  psx_drop_edits_import_file(const char *path, char *msg, unsigned cap);
void psx_drop_edits_share_dir(char *out, unsigned cap);
int  psx_drop_edits_load_file(const char *name_or_path);

/* Randomize: every duelist's drop tables rebuilt from stock with random
 * monsters and random weights (the non-monster drops keep their cards), as a
 * complete set of edits that replaces the current layer. Nothing is written:
 * Save keeps it, like any other edit. seed 0 picks a fixed seed; a caller
 * wanting variety passes the clock. Returns the number of entries recorded,
 * 0 (and msg) when it could not be done: the card types come from the
 * running game, so it needs the game up. Scripted rewards are untouched. */
int psx_drop_edits_randomize(uint32_t seed, char *msg, unsigned cap);

/* --- scripted story rewards ------------------------------------------------
 *
 * The card a duelist is guaranteed to drop when the CAMPAIGN beats them: the
 * Drop Table Manager's other per-duelist setting, and the reason it lives in
 * this file rather than one of its own. It is edited in the same window, kept
 * by the same Save, and carried by the same Import / Export, so a table
 * someone shares brings its scripted drops with it.
 *
 * card 0 means none. `every` 0 gives the card on the first campaign win only
 * (the duelist's Free Duel unlock bit is what "first" means, see
 * psx_story_rewards.c); `every` 1 gives it on every campaign win. Delivery
 * lives in psx_story_rewards.c; this is only the storage.
 */
int psx_drop_edits_reward(int duelist, int *out_every);
int psx_drop_edits_reward_set(int duelist, int card, int every);
int psx_drop_edits_reward_count(void);

/* Smart Drops is authored beside the tables it governs. Missing means the
 * stock-compatible default Off; `present` distinguishes an explicit Off in a
 * share file, which must be able to turn a recipient's previous On back off. */
int psx_drop_edits_smart_drop(void);
int psx_drop_edits_smart_drop_present(void);
int psx_drop_edits_smart_drop_set(int enabled);
int psx_drop_edits_smart_drop_reset(void);

/* --- duel starchip reward rules ------------------------------------------
 *
 * Ordered, first-match-wins rules stored in the same Drop Tables file. -1 is
 * the wildcard for mode/opponent/outcome/rank. Opponents are 0..38 internally;
 * rank is letter 0..4 plus 5 for POW (0..4 are D..S TEC, 5..9 D..S POW).
 * Amount is 0..999999, the guest save's established cap. */
#define PSX_STARCHIP_RULE_MAX 64
enum { PSX_STARCHIP_MODE_CAMPAIGN = 0, PSX_STARCHIP_MODE_FREE_DUEL = 1 };
enum { PSX_STARCHIP_OUTCOME_LOSS = 0, PSX_STARCHIP_OUTCOME_WIN = 1 };
typedef struct PsxStarchipRule {
    int mode;
    int opponent;
    int outcome;
    int rank;
    uint32_t amount;
} PsxStarchipRule;

int psx_drop_edits_starchip_count(void);
int psx_drop_edits_starchip_get(int index, PsxStarchipRule *out);
/* index == count appends; an existing index replaces in place. */
int psx_drop_edits_starchip_set(int index, const PsxStarchipRule *rule);
int psx_drop_edits_starchip_remove(int index);
int psx_drop_edits_starchip_move(int index, int delta);
int psx_drop_edits_starchip_clear(void);
/* Returns 1 and the first match, or 0 for exact stock behavior. */
int psx_drop_edits_starchip_select(int mode, int opponent, int outcome,
                                   int rank, uint32_t *amount,
                                   int *rule_index);

/* Apply this duelist's edits to one tier, in place, over a plain 722-entry
 * weight array — the same contract as psx_drop_missing_transform: 1 means w
 * was transformed, negative means it was left untouched (-1 nothing edited
 * for this duelist, else psx_drop_pins_rescale's code). */
int psx_drop_edits_apply(int duelist, int tier, uint16_t *w);

/* Debug-server read-back. */
int psx_drop_edits_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DROP_EDITS_H */
