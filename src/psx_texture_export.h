/* psx_texture_export.h -- see psx_texture_export.c.
 *
 * Decodes this disc's stock art (or, for genuinely multi-palette UI sheets,
 * whatever the game has drawn live this session through its own CLUT) into a
 * PNG named exactly as psx_wa_catalog.c registers it. Replace that file with
 * a higher-resolution version and the injector draws it instead.
 *
 * On demand only -- psx_asset_manager.c's Export/Export All buttons are the
 * only callers. There used to also be an automatic "Mods > Export stock
 * textures" action walking the whole table in the background every frame;
 * removed in favour of driving this from the Asset Manager instead, so this
 * file has no menu/frame-hook registration of its own to worry about.
 *
 * Existing files are never overwritten -- see the .c for why that is the whole
 * point rather than a nicety. */
#ifndef PSX_TEXTURE_EXPORT_H
#define PSX_TEXTURE_EXPORT_H

#include <stdint.h>
#include "psx_wa_catalog.h"   /* PsxWaAsset -- an anonymous-struct typedef,
                               * so it cannot be forward-declared; a real
                               * include is the only option */

#ifdef __cplusplus
extern "C" {
#endif

/* Exports ONE catalog entry (a->name_fmt's `index`'th record) to `root` --
 * an arbitrary caller-chosen destination, not necessarily the active pack's
 * own folder (see psx_asset_manager.c's Export/Export All, which prompt for
 * one each time). Returns 1 if something was written OR the file already
 * existed and was correctly left alone (see "never overwrite" above), 0 on a
 * real failure (bad path, decode failure, disk write failure). */
int psx_texture_export_one(const PsxWaAsset *a, int index, const char *root);

/* The exporter's own tiny PNG writer (no external zlib needed -- DEFLATE
 * "stored" blocks) and mkdir -p, shared so a second writer of this exact file
 * format never has to duplicate the CRC/Adler32 plumbing. `path` for mkdir_p
 * is edited in place and restored; pass a mutable buffer. */
int  psx_texture_export_write_png(const char *path, const uint8_t *rgba, int w, int h);
void psx_texture_export_mkdir_p(char *path);

#ifdef __cplusplus
}
#endif

#endif /* PSX_TEXTURE_EXPORT_H */
