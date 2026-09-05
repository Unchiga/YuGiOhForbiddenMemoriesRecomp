/* psx_textfile.h -- files named by the user (file dialogs, hand-edited
 * text): open them by their UTF-8 name on every platform, and read text
 * files as UTF-8 whatever an editor saved them as. */
#ifndef PSX_TEXTFILE_H
#define PSX_TEXTFILE_H
#include <stdio.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* fopen for a UTF-8 path: on Windows the C runtime's fopen wants the ANSI
 * code page and fails on a name it cannot spell (an accented user folder);
 * the path goes through _wfopen instead. Elsewhere it is fopen. */
FILE *psx_fopen_utf8(const char *path, const char *mode);
/* The whole file as one NUL-terminated UTF-8 string (malloc'd, free it),
 * or NULL. A UTF-8 BOM is dropped; UTF-16 (either byte order, with its
 * BOM -- Notepad's "Unicode") is converted. *len gets the byte length. */
char *psx_read_text_utf8(const char *path, size_t *len, size_t max_bytes);
#ifdef __cplusplus
}
#endif
#endif
