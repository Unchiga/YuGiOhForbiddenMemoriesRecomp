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
/* remove() for a UTF-8 path, for the same reason as the above: on Windows
 * the ANSI-code-page name is not always spellable, and a delete that quietly
 * fails leaves the file to be read back on the next launch. Returns 0 on
 * success, like remove(). */
int psx_remove_utf8(const char *path);
/* rename() for a UTF-8 path pair, same reason as the above. 0 on success,
 * like rename(). */
int psx_rename_utf8(const char *from, const char *to);
/* 1 if a file or directory exists at a UTF-8 path, 0 otherwise (including
 * "could not tell"). Same ANSI-code-page problem as the above: on Windows,
 * plain stat() on an unspellable name reports "missing" for something that
 * is actually there. */
int psx_path_exists_utf8(const char *path);
/* mtime and size of a UTF-8 path. 1 on success; 0 (leaving both outputs
 * untouched) if the path does not exist or could not be read -- same
 * ANSI-code-page problem as the rest of this file. Either output pointer
 * may be NULL. */
int psx_stat_utf8(const char *path, long *out_mtime, long *out_size);
/* mkdir() for a UTF-8 path -- same signature and "don't care, best effort"
 * usage as mkdir()/_mkdir() (every caller in this codebase ignores the
 * return and is really doing mkdir -p one segment at a time), just resolved
 * through the wide APIs on Windows first. Returns 0 on success. */
int psx_mkdir_utf8(const char *path);
/* Lists one directory's immediate entries ("." and ".." excluded), calling
 * cb(name, is_dir, ctx) with each name in UTF-8. Not recursive -- callers
 * that need a tree walk (texture_pack.c's scan_dir) recurse themselves.
 * Returns 0 if the directory could not be opened, 1 otherwise. Exists so the
 * FindFirstFileA/readdir split -- and the ANSI-code-page bug that comes with
 * the "A" API on Windows -- is written once instead of once per caller. */
int psx_dir_list_utf8(const char *dir, void (*cb)(const char *name, int is_dir, void *ctx), void *ctx);
/* The whole file as one NUL-terminated UTF-8 string (malloc'd, free it),
 * or NULL. A UTF-8 BOM is dropped; UTF-16 (either byte order, with its
 * BOM -- Notepad's "Unicode") is converted. *len gets the byte length. */
char *psx_read_text_utf8(const char *path, size_t *len, size_t max_bytes);
#ifdef __cplusplus
}
#endif
#endif
