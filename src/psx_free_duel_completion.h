/* FREE DUEL owned/obtainable progress and completed-opponent portrait glow. */
#ifndef PSX_FREE_DUEL_COMPLETION_H
#define PSX_FREE_DUEL_COMPLETION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  psx_free_duel_completion_image(const uint32_t **pixels, int *w, int *h);
void psx_free_duel_completion_origin(int *x, int *y);
int  psx_free_duel_completion_needs_present(void);
void psx_free_duel_completion_placed(const int *placement);

/* JSON object body used by the free_duel_completion debug command. */
int psx_free_duel_completion_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FREE_DUEL_COMPLETION_H */
