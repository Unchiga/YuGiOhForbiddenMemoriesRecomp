/* psx_duelist_icon_cache.c — see psx_duelist_icon_cache.h. */

#include "psx_duelist_icon_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_textfile.h"      /* psx_fopen_utf8(): the player folder may have an accent (Windows) */

#define N       PSX_ICON_CACHE_N
#define TILE    PSX_ICON_CACHE_W
#define IDX_ADDR 0x8009B32Eu
#define IDX_BIAS 41

/* The FREE DUEL grid in NATIVE display pixels (320x240), measured by
 * template-matching the drawn portraits against reference captures: tile
 * columns sit at 24 + round(c * 56.25) — 24, 80, 137, 193, 249, the GPU's
 * fractional step — and rows at 45 + 52*r. The cursor's animated border box
 * is ~48x44 centred on its tile. Five columns, three visible rows. */
static int cell_x(int c) { return 24 + (c * 225 + 2) / 4; }
static int cell_y(int r) { return 45 + 52 * r; }
/* Column/row from a cursor-box centre; centres sit at x 43 + c*56.25,
 * y 64 + r*52. */
static int col_of(int cx) { return ((cx - 43) * 4 + 112) / 225; }
static int row_of(int cy) { return (cy - 64 + 26) / 52; }

/* Region of the display the grid occupies, for the frame diff. */
#define REG_X 16
#define REG_Y 37
#define REG_W 280
#define REG_H 158

static uint32_t s_icons[N][TILE * TILE];
static uint8_t  s_have[N];
static unsigned s_gen = 1;
static int      s_loaded;           /* disk cache read attempted */
static int      s_dirty_disk;       /* captures not yet persisted */
static uint32_t s_last_save_frame;

/* Capture state: the last sampled grid region, for the cursor diff. */
static uint32_t *s_prev, *s_cur;
static int       s_prev_valid;
static int       s_throttle;
static int       s_captured, s_rejected;   /* observability */

/* Per-stage counters, because a capture that silently does nothing has a
 * dozen distinct causes and pixels cannot tell them apart. */
static uint32_t  s_ct_tick, s_ct_gate, s_ct_grab, s_ct_nodiff, s_ct_toobig,
                 s_ct_offgrid, s_ct_disagree, s_ct_badscroll, s_ct_page;
static int       s_last_w, s_last_h, s_last_raw;

/* --- disk cache ----------------------------------------------------------- */

#define CACHE_MAGIC 0x59494331u    /* "YIC1" */

static void cache_path(char *out, size_t cap)
{
    const char *dir = psx_mod_player_data_dir();
    if (dir && dir[0]) snprintf(out, cap, "%s/duelist_icons.bin", dir);
    else               snprintf(out, cap, "duelist_icons.bin");
}

static void load_disk(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    char path[1024];
    cache_path(path, sizeof path);
    FILE *f = psx_fopen_utf8(path, "rb");
    if (!f) return;
    uint32_t magic = 0;
    uint8_t have[N];
    if (fread(&magic, 4, 1, f) == 1 && magic == CACHE_MAGIC
        && fread(have, 1, N, f) == N) {
        for (int d = 0; d < N; d++) {
            if (fread(s_icons[d], 4, TILE * TILE, f) != TILE * TILE) break;
            if (have[d] && !s_have[d]) { s_have[d] = 1; s_gen++; }
        }
    }
    fclose(f);
}

static void save_disk(void)
{
    char path[1024];
    cache_path(path, sizeof path);
    FILE *f = psx_fopen_utf8(path, "wb");
    if (!f) return;
    uint32_t magic = CACHE_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(s_have, 1, N, f);
    for (int d = 0; d < N; d++) fwrite(s_icons[d], 4, TILE * TILE, f);
    fclose(f);
    s_dirty_disk = 0;
}

/* --- capture -------------------------------------------------------------- */

static int lum(uint32_t p)
{
    return (int)(((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) / 3;
}

/* A drawn portrait, or the stone wall behind an empty cell? Contrast decides
 * — the wall is flat (luminance std ~7 at presented scale, similar native),
 * every portrait including near-monochrome ones is drawn art (std 50+). */
static int occupied(const uint32_t *px)
{
    double mean = 0.0, var = 0.0;
    for (int i = 0; i < TILE * TILE; i++) mean += lum(px[i]);
    mean /= TILE * TILE;
    for (int i = 0; i < TILE * TILE; i++) {
        const double d = lum(px[i]) - mean;
        var += d * d;
    }
    return var / (TILE * TILE) > 20.0 * 20.0;
}

static void grab_region(const GpuDisplayInfo *di, uint32_t *dst)
{
    for (int y = 0; y < REG_H; y++)
        for (int x = 0; x < REG_W; x++)
            dst[y * REG_W + x] =
                gpu_display_pixel_argb(di, (uint32_t)(REG_X + x),
                                       (uint32_t)(REG_Y + y));
}

static void capture_cell(const uint32_t *reg, int rr, int cc, int duelist)
{
    if (duelist < 0 || duelist >= N || s_have[duelist]) return;
    uint32_t px[TILE * TILE];
    const int bx = cell_x(cc) - REG_X;
    const int by = cell_y(rr) - REG_Y;
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++)
            px[y * TILE + x] = reg[(by + y) * REG_W + bx + x] | 0xFF000000u;
    if (!occupied(px)) { s_rejected++; return; }
    memcpy(s_icons[duelist], px, sizeof px);
    s_have[duelist] = 1;
    s_captured++;
    s_dirty_disk = 1;
    s_gen++;
}

static uint32_t s_ticks;

static void tick(void)
{
    s_ticks++;
    s_ct_tick++;
    /* Cheap gate first: the highlighted-duelist byte only holds 40..79 while
     * the FREE DUEL grid owns the screen (40 = the Build Deck cell). The
     * byte can hold a STALE grid value on other screens, which is why the
     * capture below also demands geometric agreement between the animated
     * cursor and this byte before believing anything. */
    const int raw = (int)psx_mod_read_byte(IDX_ADDR);
    s_last_raw = raw;
    if (raw < IDX_BIAS - 1 || raw > IDX_BIAS + N - 1) { s_prev_valid = 0; return; }
    s_ct_gate++;
    const int cursor_d = raw - IDX_BIAS;
    if (cursor_d < 0) { s_prev_valid = 0; return; }  /* Build Deck: no anchor */

    int missing = 0;
    load_disk();
    for (int d = 0; d < N; d++) missing += !s_have[d];
    if (!missing) return;

    if (--s_throttle > 0) return;
    /* ~4 samples a second, at a VARYING spacing: the cursor border's
     * animation has a short period, and a fixed 15-frame cadence sampled it
     * at the same phase every time — every diff came back empty and the
     * capture silently never fired. 13..19 frames breaks the aliasing. */
    s_throttle = 13 + (int)(s_ticks % 7u);

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    s_last_w = (int)di.width;
    s_last_h = (int)di.height;
    if (di.disabled || di.depth24) { s_prev_valid = 0; return; }
    if ((int)di.width < REG_X + REG_W || (int)di.height < REG_Y + REG_H) {
        s_prev_valid = 0;
        return;
    }
    s_ct_grab++;

    /* Under the GL/Vulkan FBO-present paths CPU VRAM is STALE — the freshest
     * frame lives on the GPU and is presented without a readback, so two
     * samples of the CPU mirror are identical forever and the cursor diff
     * never fires. Sync it down first, exactly as the screenshot commands
     * do. Four times a second, only on this screen, only while portraits are
     * still missing. (Never on depth24; the guard above already returned.) */
    {
        extern void gl_renderer_sync_cpu(void);
        extern void vk_renderer_sync_cpu(void);
        gl_renderer_sync_cpu();
        vk_renderer_sync_cpu();
    }

    if (!s_prev) {
        s_prev = (uint32_t *)malloc((size_t)REG_W * REG_H * 4u);
        s_cur  = (uint32_t *)malloc((size_t)REG_W * REG_H * 4u);
        if (!s_prev || !s_cur) return;
    }
    grab_region(&di, s_cur);
    if (!s_prev_valid) {
        uint32_t *t = s_prev; s_prev = s_cur; s_cur = t;
        s_prev_valid = 1;
        return;
    }

    /* The cursor's border animates, so two samples at rest differ exactly at
     * the cursor cell. Its bounding box locates that cell on screen; the RAM
     * byte names the duelist; drop-order (cell k = duelist k-1, five per
     * row) then names every other visible cell. The column check between the
     * diff and the byte is the false-positive gate: on any screen where this
     * geometry is wrong the two disagree and nothing is captured. */
    int x0 = REG_W, y0 = REG_H, x1 = -1, y1 = -1;
    for (int y = 0; y < REG_H; y++)
        for (int x = 0; x < REG_W; x++)
            if (s_cur[y * REG_W + x] != s_prev[y * REG_W + x]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    { uint32_t *t = s_prev; s_prev = s_cur; s_cur = t; }

    if (x1 < 0) { s_ct_nodiff++; return; }       /* identical frames */
    if (x1 - x0 > 60 || y1 - y0 > 56) { s_ct_toobig++; return; }
    const int cx = (x0 + x1) / 2 + REG_X, cy = (y0 + y1) / 2 + REG_Y;
    const int col = col_of(cx);
    const int row = row_of(cy);
    if (col < 0 || col > 4 || row < 0 || row > 2) { s_ct_offgrid++; return; }

    const int cell = cursor_d + 1;               /* Build Deck is cell 0 */
    if (cell % 5 != col) { s_ct_disagree++; return; }
    const int scroll = cell / 5 - row;
    if (scroll < 0 || scroll > 5) { s_ct_badscroll++; return; }
    s_ct_page++;

    for (int rr = 0; rr < 3; rr++)
        for (int cc2 = 0; cc2 < 5; cc2++)
            capture_cell(s_prev /* newest after swap */, rr, cc2,
                         (scroll + rr) * 5 + cc2 - 1);

    if (s_dirty_disk) {
        if (s_ticks - s_last_save_frame > 300u) { /* at most one write per ~5 s */
            save_disk();
            s_last_save_frame = s_ticks;
        }
    }
}

/* --- API ------------------------------------------------------------------ */

const uint32_t *psx_duelist_icon_cache_get(int duelist)
{
    load_disk();
    if (duelist < 0 || duelist >= N || !s_have[duelist]) return NULL;
    return s_icons[duelist];
}

unsigned psx_duelist_icon_cache_generation(void) { return s_gen; }

int psx_duelist_icon_cache_missing(void)
{
    load_disk();
    int missing = 0;
    for (int d = 0; d < N; d++) missing += !s_have[d];
    return missing;
}

int psx_duelist_icon_cache_state_json(char *out, unsigned cap)
{
    if (!out || cap < 96u) return 0;
    load_disk();
    int have = 0;
    for (int d = 0; d < N; d++) have += s_have[d];
    return snprintf(out, cap,
                    "\"have\":%d,\"captured\":%d,\"rejected\":%d,"
                    "\"gen\":%u,\"disk_dirty\":%d,"
                    "\"ticks\":%u,\"gate\":%u,\"grabs\":%u,"
                    "\"nodiff\":%u,\"toobig\":%u,\"offgrid\":%u,"
                    "\"disagree\":%u,\"badscroll\":%u,\"pages\":%u,"
                    "\"raw\":%d,\"disp\":[%d,%d]",
                    have, s_captured, s_rejected, s_gen, s_dirty_disk,
                    s_ct_tick, s_ct_gate, s_ct_grab, s_ct_nodiff, s_ct_toobig,
                    s_ct_offgrid, s_ct_disagree, s_ct_badscroll, s_ct_page,
                    s_last_raw, s_last_w, s_last_h);
}

PSX_MOD_CONSTRUCTOR(psx_duelist_icon_cache_install)
{
    (void)psx_game_add_frame_hook(tick);
}
