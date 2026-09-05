/* psx_duelist_portraits.c — see psx_duelist_portraits.h. */

#include "psx_duelist_portraits.h"

#include <string.h>

#include "psx_sdl.h"

#include "mod_plugins.h"

#define TILE_LBA     17952u                /* WA_MRG sector 0x1EAA */
#define TILE_BYTES   2432u                 /* 48*48 indices + 64*2 CLUT */
#define TILES        40u
#define SECTOR       2048u
#define SECTORS      ((TILES * TILE_BYTES + SECTOR - 1u) / SECTOR)   /* 48 */

static uint32_t s_px[PSX_PORTRAIT_N][PSX_PORTRAIT_W * PSX_PORTRAIT_W];
static int      s_ready;
static int      s_gave_up;
static uint32_t s_next_try;               /* SDL ticks; the disc can be busy */

/* 15-bit PSX colour -> opaque ARGB. The STP bit is ignored on purpose: the
 * tiles are solid squares and stock stores black as 0x8000 (STP set) so
 * that it draws instead of keying out. */
static uint32_t argb(unsigned c)
{
    const unsigned r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int load(void)
{
    static uint8_t raw[SECTORS * SECTOR];
    for (uint32_t s = 0; s < SECTORS; s++)
        if (!psx_mod_cd_read_stock_sector(TILE_LBA + s, raw + s * SECTOR)) return 0;
    /* Sanity: this disc's tiles carry the STP bit on every CLUT entry and
     * index no colour past the CLUT. A different revision, or a dump with
     * the sectors elsewhere, fails here and the drawn-screen capture keeps
     * doing the job. */
    for (uint32_t i = 1; i < TILES; i++) {
        const uint8_t *t = raw + i * TILE_BYTES, *clut = t + 2304;
        for (int k = 0; k < 64; k++)
            if (!(clut[2 * k + 1] & 0x80u)) return -1;
    }
    for (int d = 0; d < PSX_PORTRAIT_N; d++) {
        const uint8_t *t = raw + (uint32_t)(d + 1) * TILE_BYTES, *clut = t + 2304;
        uint32_t pal[64];
        for (int k = 0; k < 64; k++) pal[k] = argb((unsigned)clut[2 * k] | ((unsigned)clut[2 * k + 1] << 8));
        for (int i = 0; i < PSX_PORTRAIT_W * PSX_PORTRAIT_W; i++) s_px[d][i] = pal[t[i] & 63u];
    }
    return 1;
}

int psx_duelist_portraits_ready(void)
{
    if (s_ready || s_gave_up) return s_ready;
    const uint32_t now = SDL_GetTicks();
    if (now < s_next_try) return 0;
    const int rc = load();
    if (rc > 0) { s_ready = 1; return 1; }
    if (rc < 0) { s_gave_up = 1; return 0; }
    s_next_try = now + 1000u;             /* not readable yet; ask again later */
    return 0;
}

const uint32_t *psx_duelist_portraits_get(int duelist)
{
    if (duelist < 0 || duelist >= PSX_PORTRAIT_N) return NULL;
    if (!psx_duelist_portraits_ready()) return NULL;
    return s_px[duelist];
}
