/* psx_cpu_data.h — what a CPU duelist is made of, and the player's edits to it.
 *
 * Three things belong to an opponent and are kept here; the CPU Manager
 * window (psx_cpu_manager.c) is the UI over them.
 *
 *   DECK    the weighted pool the game draws their 40 cards from, one array
 *           per duelist in their disc record. Edited through a sector
 *           override, so the game loads the edited pool itself.
 *   AI      the nine-byte profile at gDuel_aOpponentData, which is what the
 *           duel AI reads about the opponent it is playing as.
 *   RECORD  wins and losses, in the save.
 *
 * Deck and AI edits live in cpu_manager.ini beside the player's saves; the
 * record is the save's own and is written straight into it.
 */
#ifndef PSX_CPU_DATA_H
#define PSX_CPU_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_CPU_AI_BYTES 9

/* Duelists are the DROP DB's index order, 0..38, which is the opponent id
 * minus one (Simon Muran 0/1 ... Duel Master K 38/39). */

/* ---- the AI profile ------------------------------------------------------
 * Nine bytes per duelist. Only the first has a name in the decompilation
 * (Ai_GetHandSize); the rest are what the AI scripts read through
 * AiScript_LoadOpponentData, so they are exposed as themselves with their
 * stock ranges rather than dressed up in guesses. */
const char *psx_cpu_ai_label(int field);     /* short label, 0..8 */
const char *psx_cpu_ai_hint(int field);      /* one line for the window */
/* The live table (what the game will read). Returns 0 when the EXE's data is
 * not resident yet. */
int  psx_cpu_ai_live(int duelist, uint8_t out[PSX_CPU_AI_BYTES]);
/* The stock profile, snapshotted before anything was written. */
int  psx_cpu_ai_stock(int duelist, uint8_t out[PSX_CPU_AI_BYTES]);
/* The player's edit, if any; 1 when this duelist carries one. */
int  psx_cpu_ai_edit(int duelist, uint8_t out[PSX_CPU_AI_BYTES]);
/* Set or clear one field. value < 0 clears the whole duelist's edit. */
int  psx_cpu_ai_set(int duelist, int field, int value);
int  psx_cpu_ai_clear(int duelist);

/* ---- the deck pool -------------------------------------------------------
 * 722 weights that sum to 2048, the same shape as a drop tier. The baked
 * stock pool is PSX_DROP_DB[d].deck; this is the effective one, edits on
 * top. */
int  psx_cpu_deck_weight(int duelist, int card);        /* effective */
int  psx_cpu_deck_stock_weight(int duelist, int card);
/* Set a card's weight. The rest of the pool is rescaled so the 2048 total
 * holds, exactly as the drop editor does it; returns 0 when that cannot be
 * done (and changes nothing). */
int  psx_cpu_deck_set(int duelist, int card, int weight);
int  psx_cpu_deck_clear(int duelist);                   /* back to stock */
int  psx_cpu_deck_edited(int duelist);                  /* 1 when edited */
/* The effective pool, sparse and sorted by weight, for the window. Returns
 * how many entries were written. */
int  psx_cpu_deck_list(int duelist, uint16_t *cards, uint16_t *weights, int cap);

/* ---- the record ----------------------------------------------------------
 * gFreeDuel_aDuelistRecords in the save. Reads give -1 when no save is
 * resident; writes do nothing then. */
int  psx_cpu_record(int duelist, int *wins, int *losses);
int  psx_cpu_record_set(int duelist, int wins, int losses);

/* ---- persistence ---------------------------------------------------------
 * One file for the window, cpu_manager.ini, the same way the Drop Table
 * Manager keeps drop_table_edits.ini. Import replaces every edit with the
 * file's and keeps it, like the other managers. */
int  psx_cpu_dirty(void);
int  psx_cpu_save(void);
unsigned psx_cpu_generation(void);
void psx_cpu_ensure_loaded(void);
int  psx_cpu_export_file(const char *path, char *msg, unsigned cap);
int  psx_cpu_import_file(const char *path, char *msg, unsigned cap);
void psx_cpu_share_dir(char *out, unsigned cap);

/* Registers the frame hook that keeps the AI table and the disc overrides in
 * step with the edits. Called from this module's constructor. */
void psx_cpu_data_install(void);

/* `cpu_data` debug command. */
int  psx_cpu_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CPU_DATA_H */
