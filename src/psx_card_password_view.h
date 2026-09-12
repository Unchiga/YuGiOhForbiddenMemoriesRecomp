/* Compact password value for the game's shared full-card detail view.
 *
 * The Cards editor already owns password editing. This module only presents
 * the effective eight-digit value wherever the stock card-detail widget draws
 * a description (Library, deck building, chest and duel inspection). */
#ifndef PSX_CARD_PASSWORD_VIEW_H
#define PSX_CARD_PASSWORD_VIEW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  psx_card_password_view_image(const uint32_t **px, int *w, int *h);
void psx_card_password_view_origin(int *x, int *y);
int  psx_card_password_view_needs_present(void);
void psx_card_password_view_placed(const int *placement);
void psx_card_password_view_registered(int ok);

/* Registers the shared text-command observer and frame lifetime. Overlay
 * registration stays centralized in psx_ygo_overlays.c for draw order. */
void psx_card_password_view_init(void);

/* Debug readback: detection, effective card/password and overlay geometry. */
int psx_card_password_view_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CARD_PASSWORD_VIEW_H */
