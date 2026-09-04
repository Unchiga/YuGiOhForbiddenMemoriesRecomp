/* psx_card_share.h -- one file that carries every edited card.
 *
 * A .ygocards file is a zip (stored entries, so any zip tool opens it) with
 * a manifest.ini up front, every cards/<id>/ folder (card.ini and the PNGs)
 * and, when the player has any, drop_table_edits.ini. Import reads stored
 * and deflated entries, so a file re-zipped by hand still loads. */
#ifndef PSX_CARD_SHARE_H
#define PSX_CARD_SHARE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_CARD_SHARE_EXT     "ygocards"
#define PSX_CARD_SHARE_FORMAT  "YGOFM-EDITED-CARDS"
#define PSX_CARD_SHARE_VERSION 1

typedef struct {
    int  ok;
    char error[160];
    int  version;
    int  card_n;              /* cards in the file */
    int  card_ids[722];
    int  replace_n;           /* of those, cards the player already edited */
    int  replace_ids[722];
    int  has_drops;           /* drop_table_edits.ini in the file */
    int  drops_here;          /* the player already has drop table edits */
    long bytes;
    char title[96];           /* the manifest's title line, if any */
} PsxCardShareInfo;

/* Write every edited card (and drop table edits) to path. */
int  psx_card_share_export(const char *path, char *msg, unsigned cap);
/* Read what a file would do, without doing it. */
int  psx_card_share_inspect(const char *path, PsxCardShareInfo *info);
/* Replace the listed cards' folders with the file's, reload everything. */
int  psx_card_share_import(const char *path, char *msg, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_SHARE_H */
