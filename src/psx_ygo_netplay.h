/* psx_ygo_netplay.h -- is a netplay session running?
 *
 * Every game-side layer that feeds per-machine configuration into guest
 * state (RAM, disc sector overrides, code patches) asks this at the top of
 * its writing entry points and goes quiet while a session is up, so two
 * peers keep bit-identical guest state. Render-only work keeps running.
 */
#ifndef PSX_YGO_NETPLAY_H
#define PSX_YGO_NETPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 from session start until shutdown; 0 offline or when the runtime was
 * built without recomp-net. */
int psx_ygo_netplay_session(void);

/* This peer's sim slot (0 = P1) while a session is up; -1 offline. */
int psx_ygo_netplay_local_slot(void);
/* Display name behind a seat, from the lobby; 0 when unknown (out = ""). */
int psx_ygo_netplay_seat_name(int slot, char *out, size_t cap);

/* The hidden-information cover (psx_ygo_netplay.c): a present-time overlay
 * painting card backs over the acting side's hand, the detail panel and the
 * card-play zoom on the machine whose seat is not acting. Registered from
 * psx_ygo_overlays.c so it composites above every other overlay. */
int  psx_ygo_netplay_cover_image(const uint32_t **pixels, int *w, int *h);
void psx_ygo_netplay_cover_origin(int *x, int *y);
int  psx_ygo_netplay_cover_needs_present(void);

/* The rules-screen vblank hook and the cover's frame hook. */
void psx_ygo_netplay_install_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_YGO_NETPLAY_H */
