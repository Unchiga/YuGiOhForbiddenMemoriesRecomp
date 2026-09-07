/* psx_card_share.h -- one file that carries every edited card.
 *
 * A .ygocards file is a zip (stored entries, so any zip tool opens it) with
 * a manifest.ini up front, every cards/<id>/ folder (card.ini and the PNGs)
 * and, when the player has any, drop_table_edits.ini. Import reads stored
 * and deflated entries, so a file re-zipped by hand still loads. */
#ifndef PSX_CARD_SHARE_H
#define PSX_CARD_SHARE_H

#include <stddef.h>
#include <stdint.h>

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

/* The zip reader and writer behind .ygocards, for any other share file that
 * wants the same container (the CPU Manager's .ygoduelists carries its ini
 * and the portrait PNGs). Stored entries, names under 64 bytes, sizes
 * bounded; the reader also inflates deflated entries so a hand re-zipped
 * file loads. */
typedef struct PsxZipWriter PsxZipWriter;
PsxZipWriter *psx_zip_writer_open(const char *path);
int  psx_zip_writer_add(PsxZipWriter *z, const char *name, const void *data, size_t n);
int  psx_zip_writer_close(PsxZipWriter *z);              /* writes the directory; frees z */
void psx_zip_writer_abandon(PsxZipWriter *z);            /* on a failed add: closes and frees */
typedef struct {
    char name[64];
    uint32_t method, csize, usize, offset, crc;
} PsxZipEntry;
/* The whole file (malloc'd, NUL-terminated past the end) or NULL. */
unsigned char *psx_zip_read_file(const char *path, long *size);
/* Entries of an archive already in memory; the count, or -1 with err set. */
int  psx_zip_list(const unsigned char *b, long n, PsxZipEntry *out, int max, char *err, unsigned errcap);
/* One entry, malloc'd and NUL-terminated, checked against its crc; NULL when damaged. */
unsigned char *psx_zip_extract(const unsigned char *b, long n, const PsxZipEntry *e, long *size);

/* Address the player's OWN cards/ set rather than whichever set is live.
 * The MOD package sets this around its calls: with Dev Card Effects on, the
 * live set is the shipped effects mod, and a package that carried it (or an
 * import that overwrote it) lost a player's Time Wizard on 2026-09-06. */
void psx_card_share_own_set(int on);

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
