/* psx_dialogue.h -- export the game's text bank (story dialogue, duelist
 * lines, menu and duel messages) to a translator-friendly text file, and
 * bring an edited file back so the game shows it at runtime.
 *
 * Nothing on the disc or in the save changes: translated texts are kept as
 * <player-data>/dialogue/dialogue.txt, loaded at every start, and served to
 * the game from host-backed guest memory through a small cursor redirect in
 * the typewriter (see psx_dialogue.c for the mechanism). Card descriptions
 * are a separate table with their own file (psx_card_texts.h). */
#ifndef PSX_DIALOGUE_H
#define PSX_DIALOGUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Widths translators must respect (measured on the stock texts, see the
 * exported file's header): the story box shows this many characters per
 * line and lines per page. */
#define PSX_DIALOGUE_COLS  36
#define PSX_DIALOGUE_LINES 3

/* What the manager lists: one entry per FF-terminated text run in the bank. */
typedef struct {
    uint32_t    key;          /* the run's stock bank offset, the "[@XXXX]" in the file */
    int         nids;         /* string ids whose lookup lands in this run */
    const int  *ids;
    const char *stock;        /* decoded stock text, UTF-8, "\n" per line, "{..}" control codes */
    const char *current;      /* the text the game shows now: stock, or the imported one */
    int         translated;   /* 1 = current differs from stock */
    int         bytes;        /* encoded size of the current text incl. the terminator */
} PsxDialogueRun;

/* The bank has been read out of guest RAM (a few frames after boot). */
int  psx_dialogue_ready(void);
int  psx_dialogue_count(void);
int  psx_dialogue_run(int index, PsxDialogueRun *out);
/* How many runs are translated right now. */
int  psx_dialogue_translated_count(void);
/* Bumps on every apply, so a viewer can redraw. */
unsigned psx_dialogue_generation(void);

/* Write every run's CURRENT text to path. Returns 1 and a summary in err
 * (which doubles as the message line), 0 with the reason on failure. */
int  psx_dialogue_export(const char *path, char *err, unsigned errcap);
/* Read a file in the export format, validate it, apply it live and keep a
 * copy as <player-data>/dialogue/dialogue.txt. Returns 1 when applied (err
 * then holds the summary and any warnings), 0 when nothing was applied. */
int  psx_dialogue_import(const char *path, char *err, unsigned errcap);
/* Back to the original texts; deletes the kept file. */
void psx_dialogue_clear(void);
/* Where the kept translation lives ("" before boot). */
const char *psx_dialogue_file(void);

/* Game text codec, shared with the card-description file: the glyph code of
 * the UTF-8 character at s (advancing *len), or -1 when the font has none. */
int  psx_dialogue_encode_char(const char *s, int *len);
/* The UTF-8 spelling of glyph code c (0..0x5B), or NULL. */
const char *psx_dialogue_glyph(int c);

/* Debug-server state line. */
int  psx_dialogue_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DIALOGUE_H */
