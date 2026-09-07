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

/* ---- the portrait --------------------------------------------------------
 * The 48x48 tile the FREE DUEL grid draws, forty of them at WA_MRG sector
 * 0x1EAA: 2304 six-bit indices then a 64-entry 15-bit CLUT. A replacement is
 * any PNG; it is scaled, quantised to 64 colors and written back through a
 * sector override, and the PNG is kept in <player-data>/duelists/<id>/ so it
 * comes back at the next launch. */
int  psx_cpu_portrait_set(int duelist, const char *png_path, char *msg, unsigned cap);
int  psx_cpu_portrait_clear(int duelist);
int  psx_cpu_portrait_edited(int duelist);
int  psx_cpu_portraits_count(void);

/* ---- the name ------------------------------------------------------------
 * What the FREE DUEL grid prints under the portrait, and what the campaign
 * prints wherever a text carries the opponent's name: string 0x8328 + id in
 * the same name table the card names live in (entries 808..847 of
 * gText_aGlobalOffsets, measured 2026-09-06 against the SLUS). A rename is
 * asserted per frame like a card rename: the string goes to reclaimed RAM
 * and the table entry is repointed at it. The game's font has no accents,
 * so a name is ASCII; characters the font lacks are refused by
 * psx_cpu_name_set. Empty clears. */
#define PSX_CPU_NAME_MAX 20
int  psx_cpu_name_set(int duelist, const char *name);
int  psx_cpu_name_clear(int duelist);
int  psx_cpu_name_edited(int duelist);
/* The name every window should print for a duelist: the edit when there is
 * one, else the stock name. PSX_DROP_DB[d].name stays the stock name, which
 * is what the ini section headers and "back to stock" need. */
const char *psx_cpu_display_name(int duelist);

/* ---- persistence ---------------------------------------------------------
 * One file for the window, cpu_manager.ini, the same way the Drop Table
 * Manager keeps drop_table_edits.ini. Import replaces every edit with the
 * file's and keeps it, like the other managers. Export writes that ini, or,
 * when any portrait is edited, a .ygoduelists zip (the .ygocards container)
 * with the ini and the portrait PNGs inside; import takes either. */
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
