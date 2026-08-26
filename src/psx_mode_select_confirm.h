#ifndef PSX_MODE_SELECT_CONFIRM_H
#define PSX_MODE_SELECT_CONFIRM_H

/* MODS > CONFIRM MENU CIRCLE EXIT — the yes/no prompt Circle raises on
 * mode-select. See psx_mode_select_confirm.c.
 *
 * Same shape as this title's other host overlays: the module owns its state
 * and rasterises an ARGB canvas in GUEST pixels; psx_ygo_overlays registers
 * it with the renderer.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PsxGuestOverlay entry points. */
int  psx_mode_select_confirm_image(const uint32_t **px, int *w, int *h);
void psx_mode_select_confirm_origin(int *x, int *y);
int  psx_mode_select_confirm_needs_present(void);

/* Adds the MODS row and arms the vblank hook and the pad hook. */
void psx_mode_select_confirm_register(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MODE_SELECT_CONFIRM_H */
