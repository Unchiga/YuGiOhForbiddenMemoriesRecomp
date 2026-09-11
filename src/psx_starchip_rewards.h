#ifndef PSX_STARCHIP_REWARDS_H
#define PSX_STARCHIP_REWARDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the duel-results hook and per-frame lifetime tracking. */
void psx_starchip_rewards_init(void);

/* Present-only replacement for the stock row of one-to-five star icons when
 * an authored rule matched. The canvas shows one starchip followed by xN. */
int  psx_starchip_rewards_image(const uint32_t **pixels, int *w, int *h);
void psx_starchip_rewards_origin(int *x, int *y);
int  psx_starchip_rewards_needs_present(void);
void psx_starchip_rewards_placed(const int *placement);

/* Debug/test state for the last completed rule decision. */
int psx_starchip_rewards_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_STARCHIP_REWARDS_H */
