/* Exact ARGB bake of the player's eight 48x48 completion borders. */
#ifndef PSX_FREE_DUEL_COMPLETION_ART_H
#define PSX_FREE_DUEL_COMPLETION_ART_H

#include <stdint.h>

#define PSX_FD_COMPLETION_FRAME_COUNT 8
#define PSX_FD_COMPLETION_FRAME_SIZE 48

/* Decode FRAME (wrapped to 0..7) into a 48x48 row-major ARGB canvas. */
void psx_free_duel_completion_art_frame(int frame, uint32_t *out);

#endif /* PSX_FREE_DUEL_COMPLETION_ART_H */
