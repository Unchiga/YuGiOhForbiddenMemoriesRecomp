/* psx_rng_view.h — VIEW > RNG VIEWER, the RNG state on screen.
 *
 * Yu-Gi-Oh! Forbidden Memories drives everything random through the libc
 * rand() at 0x8008E590, whose 32-bit seed lives at 0x800FE6F8 and is set once
 * at boot by srand(0x55555555). Speedrunners manipulate it by frame timing
 * (the name-entry screen burns exactly one rand() per frame) and verify runs
 * by checking the seed only ever moves along the LCG. This overlay shows that
 * state live; it reads guest RAM and draws, and never writes anything.
 */
#ifndef PSX_RNG_VIEW_H
#define PSX_RNG_VIEW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest-overlay callbacks, registered centrally in psx_ygo_overlays.c
 * because registration order is draw order. */
int  psx_rng_view_image(const uint32_t **px, int *w, int *h);
void psx_rng_view_origin(int *x, int *y);
int  psx_rng_view_needs_present(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RNG_VIEW_H */
