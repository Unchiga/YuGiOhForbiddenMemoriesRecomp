/* psx_card_texts.h -- every card's name and description as one text file,
 * for translating or bulk-editing the edited-card set.
 *
 * Export writes what the ACTIVE card set shows (the player's cards/, or the
 * Card Effects set when that mod is on): an edited card's text where it has
 * one, the stock text otherwise. Import reads the file back and stores each
 * changed name / description into that set's cards/<id>/card.ini through
 * psx_card_packs, keeping every other key of the card; unchanged cards are
 * left alone and a text put back to stock drops the key. The change shows
 * live. Never touches the disc image or the save. */
#ifndef PSX_CARD_TEXTS_H
#define PSX_CARD_TEXTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Write all 722 cards to path. Returns 1 with a summary in err (usable as
 * the status line), 0 with the reason. */
int psx_card_texts_export(const char *path, char *err, unsigned errcap);
/* Read a file in the export format, apply and persist the changed cards.
 * Returns 1 when anything could be applied (err = summary incl. skipped
 * cards), 0 when nothing was (err = why). */
int psx_card_texts_import(const char *path, char *err, unsigned errcap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_TEXTS_H */
