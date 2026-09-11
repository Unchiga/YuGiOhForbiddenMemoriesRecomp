#include "psx_tool_window.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mod_plugins.h"
#include "psx_video_menu.h"
#include "psx_textfile.h"
#include "psx_ui_draw.h"
#include "psx_ui_font.h"
#include "psx_game_hooks.h"
#include "psx_ygo_netplay.h"
#include "psx_card_manager.h"
#include "psx_drop_viewer.h"
#include "psx_fusion_manager.h"
#include "psx_dialogue_manager.h"
#include "psx_cpu_manager.h"
#include "host_osd.h"

static int  s_row = -1;
static int  s_editor_row = -1;
static int  s_log_started;
static SDL_Window *s_editor_win;
static SDL_Renderer *s_editor_ren;
static SDL_Texture *s_editor_tabs;
static uint32_t *s_editor_tab_px;
static int s_editor_tab_w;
static int s_editor_page = -1;
static int s_editor_switching;
static unsigned s_editor_fit_count;
static int s_editor_last_fit;
static uint32_t s_editor_fit_due;

static const char *const s_page_names[PSX_FM_PAGE_COUNT] = {
    "Cards", "Drop Tables", "Fusions", "Dialogue", "CPU"
};

/* Keep a small horizontal gutter around a fitted window.  KWin may relocate a
 * client that consumes an output's exact full width to a larger neighbouring
 * output even when its reported decorations mathematically fit. */
#define PSX_FM_EDITOR_FIT_SIDE_GUTTER 16

/* Wayland deliberately does not expose server-side decoration extents to
 * clients. SDL consequently reports four zeroes on KWin even though the
 * Breeze title bar is part of the desktop footprint. Reserve a little more
 * than the observed decoration so FIT means the complete native window, not
 * only its client area, remains reachable. Other backends use SDL's measured
 * borders. */
static int editor_borders(SDL_Window *win, int *top, int *left,
                          int *bottom, int *right)
{
    *top = *left = *bottom = *right = 0;
    (void)SDL_GetWindowBordersSize(win, top, left, bottom, right);
    if (!*top && !*left && !*bottom && !*right) {
        const char *driver = SDL_GetCurrentVideoDriver();
        const unsigned long long flags =
            (unsigned long long)SDL_GetWindowFlags(win);
        if (driver && !strcmp(driver, "wayland") &&
            !(flags & SDL_WINDOW_BORDERLESS)) {
            *top = 32;
            *left = *bottom = *right = 4;
            return 1;
        }
    }
    return 0;
}

/* The Wayland protocol has no global work-area query. SDL therefore returns
 * the complete output on KWin even while a persistent desktop panel occupies
 * its bottom edge. KWin itself keeps newly placed windows above that panel,
 * but an explicit resize can otherwise cover it. When usable == bounds on
 * Wayland, reserve a conservative logical-pixel strip for system UI. */
static int editor_usable_bounds(int display, SDL_Rect *bounds,
                                SDL_Rect *usable)
{
    memset(bounds, 0, sizeof *bounds);
    memset(usable, 0, sizeof *usable);
    (void)SDL_GetDisplayBounds(display, bounds);
    if (SDL_GetDisplayUsableBounds(display, usable) != 0 ||
        usable->w <= 0 || usable->h <= 0)
        return 0;
    const char *driver = SDL_GetCurrentVideoDriver();
    if (driver && !strcmp(driver, "wayland") &&
        usable->x == bounds->x && usable->y == bounds->y &&
        usable->w == bounds->w && usable->h == bounds->h &&
        usable->h > 96) {
        usable->h -= 48;
        return 1;
    }
    return 0;
}

static void page_close(int page)
{
    switch (page) {
    case PSX_FM_PAGE_CARDS:    psx_card_manager_close(); break;
    case PSX_FM_PAGE_DROPS:    psx_drop_viewer_close(); break;
    case PSX_FM_PAGE_FUSIONS:  psx_fusion_manager_close(); break;
    case PSX_FM_PAGE_DIALOGUE: psx_dialogue_manager_close(); break;
    case PSX_FM_PAGE_CPU:      psx_cpu_manager_close(); break;
    default: break;
    }
}

void psx_fm_editor_open_page(int page)
{
    if (page < 0 || page >= PSX_FM_PAGE_COUNT || psx_ygo_netplay_session()) {
        if (psx_ygo_netplay_session())
            host_osd_push("FM Editor unavailable during netplay", 1600);
        return;
    }
    switch (page) {
    case PSX_FM_PAGE_CARDS:    psx_card_manager_open(); break;
    case PSX_FM_PAGE_DROPS:    psx_drop_viewer_open(); break;
    case PSX_FM_PAGE_FUSIONS:  psx_fusion_manager_open(); break;
    case PSX_FM_PAGE_DIALOGUE: psx_dialogue_manager_open(); break;
    case PSX_FM_PAGE_CPU:      psx_cpu_manager_open(); break;
    default: break;
    }
}

SDL_Window *psx_fm_editor_acquire(int page, int content_w, int content_h)
{
    int created = 0;
    if (page < 0 || page >= PSX_FM_PAGE_COUNT || psx_ygo_netplay_session())
        return NULL;
    if (s_editor_win && s_editor_page == page) {
        SDL_RaiseWindow(s_editor_win);
        return s_editor_win;
    }
    if (s_editor_page >= 0 && s_editor_page != page) {
        const int old = s_editor_page;
        s_editor_switching = 1;
        page_close(old);
        s_editor_switching = 0;
    }
    if (!s_editor_win) {
        s_editor_win = SDL_CreateWindow(
            "FM Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            content_w, content_h + PSX_FM_EDITOR_TAB_H, SDL_WINDOW_RESIZABLE);
        if (!s_editor_win) return NULL;
        SDL_SetWindowMinimumSize(
            s_editor_win, PSX_FM_EDITOR_MIN_W,
            PSX_FM_EDITOR_MIN_CONTENT_H + PSX_FM_EDITOR_TAB_H);
        s_editor_fit_count = 0;
        s_editor_last_fit = 0;
        s_editor_fit_due = 0;
        created = 1;
    }
    s_editor_page = page;
    SDL_SetWindowTitle(s_editor_win, "FM Editor");
    SDL_ShowWindow(s_editor_win);
#if defined(PSX_SDL3)
    (void)SDL_SyncWindow(s_editor_win);
#endif
    /* SDL sizes the client area; the desktop limit includes the title bar and
     * borders. Fit only after Show, otherwise KWin's initial placement arrives
     * later and overwrites the centred position. */
    if (created)
        (void)psx_fm_editor_set_geometry(-1, -1, -1, -1, 0, 0, 1);
    if (created)
        s_editor_fit_due = SDL_GetTicks() + 250u;
    SDL_RaiseWindow(s_editor_win);
    return s_editor_win;
}

void psx_fm_editor_release(int page)
{
    if (page != s_editor_page) return;
    s_editor_page = -1;
    s_editor_ren = NULL;
    s_editor_tabs = NULL; /* the page renderer owned and destroyed it */
    if (!s_editor_switching && s_editor_win) {
        SDL_DestroyWindow(s_editor_win);
        s_editor_win = NULL;
        s_editor_fit_due = 0;
        free(s_editor_tab_px); s_editor_tab_px = NULL; s_editor_tab_w = 0;
    }
}

void psx_fm_editor_close(void)
{
    if (s_editor_page >= 0) page_close(s_editor_page);
}

int psx_fm_editor_is_open(void) { return s_editor_win != NULL; }
int psx_fm_editor_page(void) { return s_editor_page; }
int psx_fm_editor_is_switching(void) { return s_editor_switching; }
int psx_fm_editor_content_height(int output_h)
{
    return output_h > PSX_FM_EDITOR_TAB_H ? output_h - PSX_FM_EDITOR_TAB_H : 1;
}
int psx_fm_editor_window_y(int content_y)
{
    return content_y + PSX_FM_EDITOR_TAB_H;
}

static int tab_at(int x, int w)
{
    if (x < 0 || w <= 0) return -1;
    int p = (int)((long long)x * PSX_FM_PAGE_COUNT / w);
    return p >= 0 && p < PSX_FM_PAGE_COUNT ? p : -1;
}

int psx_fm_editor_filter_event(int page, const SDL_Event *event,
                               SDL_Event *adjusted)
{
    if (!event || !adjusted || page != s_editor_page || !s_editor_win)
        return 0;
    *adjusted = *event;
    const Uint32 id = SDL_GetWindowID(s_editor_win);
    if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
        if (event->button.windowID != id) return 0;
        if (event->button.y < PSX_FM_EDITOR_TAB_H) {
            if (event->type == SDL_MOUSEBUTTONDOWN &&
                event->button.button == SDL_BUTTON_LEFT) {
                int w = 0, h = 0;
                SDL_GetWindowSize(s_editor_win, &w, &h);
                const int target = tab_at(event->button.x, w);
                if (target >= 0 && target != page)
                    psx_fm_editor_open_page(target);
            }
            return 1;
        }
        adjusted->button.y -= PSX_FM_EDITOR_TAB_H;
    } else if (event->type == SDL_MOUSEMOTION) {
        if (event->motion.windowID != id) return 0;
        if (event->motion.y < PSX_FM_EDITOR_TAB_H) return 1;
        adjusted->motion.y -= PSX_FM_EDITOR_TAB_H;
    } else if (event->type == SDL_KEYDOWN) {
#if defined(PSX_SDL3)
        const SDL_Keycode key = event->key.key;
        const SDL_Keymod mod = event->key.mod;
#else
        const SDL_Keycode key = event->key.keysym.sym;
        const SDL_Keymod mod = event->key.keysym.mod;
#endif
        if (event->key.windowID == id && (mod & KMOD_CTRL) &&
            key >= SDLK_1 && key <= SDLK_5) {
            psx_fm_editor_open_page((int)(key - SDLK_1));
            return 1;
        }
    }
    return 0;
}

int psx_fm_editor_state_json(char *out, unsigned cap)
{
    if (!out || cap < 512u) return 0;
    int x = 0, y = 0, w = 0, h = 0, pw = 0, ph = 0;
    int top = 0, left = 0, bottom = 0, right = 0;
    int raw_top = 0, raw_left = 0, raw_bottom = 0, raw_right = 0;
    int estimated = 0;
    int min_w = 0, min_h = 0;
    SDL_Rect bounds = { 0, 0, 0, 0 }, usable = { 0, 0, 0, 0 };
    int display = -1, fit = 0, workarea_estimated = 0;
    unsigned long long flags = 0;
    if (s_editor_win) {
        SDL_GetWindowPosition(s_editor_win, &x, &y);
        SDL_GetWindowSize(s_editor_win, &w, &h);
#if defined(PSX_SDL3)
        (void)SDL_GetWindowSizeInPixels(s_editor_win, &pw, &ph);
        (void)SDL_GetWindowBordersSize(s_editor_win, &raw_top, &raw_left,
                                       &raw_bottom, &raw_right);
#else
        SDL_GL_GetDrawableSize(s_editor_win, &pw, &ph);
        (void)SDL_GetWindowBordersSize(s_editor_win, &raw_top, &raw_left,
                                       &raw_bottom, &raw_right);
#endif
        estimated = editor_borders(s_editor_win, &top, &left,
                                   &bottom, &right);
        flags = (unsigned long long)SDL_GetWindowFlags(s_editor_win);
        /* A maximized compositor-managed window already occupies its exact
         * work area. Applying the Wayland estimate there would double-count
         * the server-side title bar. */
        if (estimated && (flags & SDL_WINDOW_MAXIMIZED))
            top = left = bottom = right = estimated = 0;
        SDL_GetWindowMinimumSize(s_editor_win, &min_w, &min_h);
        display = SDL_GetWindowDisplayIndex(s_editor_win);
        workarea_estimated = editor_usable_bounds(display, &bounds, &usable);
        fit = x >= usable.x && y >= usable.y &&
              x + w + right <= usable.x + usable.w &&
              y + h + bottom <= usable.y + usable.h;
    }
    const int n = snprintf(out, cap,
        "\"open\":%d,\"page\":%d,\"page_name\":\"%s\",\"tabs\":5,"
        "\"window_id\":%u,\"window\":[%d,%d],\"position\":[%d,%d],"
        "\"pixels\":[%d,%d],\"borders_reported\":[%d,%d,%d,%d],"
        "\"borders\":[%d,%d,%d,%d],\"borders_estimated\":%d,"
        "\"outer\":[%d,%d,%d,%d],\"minimum\":[%d,%d],"
        "\"display\":%d,\"display_bounds\":[%d,%d,%d,%d],"
        "\"usable\":[%d,%d,%d,%d],\"workarea_estimated\":%d,"
        "\"fits_usable\":%d,"
        "\"maximized\":%d,\"minimized\":%d,\"resizable\":%d,"
        "\"fit_count\":%u,\"last_fit\":%d",
        s_editor_win != NULL, s_editor_page,
        s_editor_page >= 0 ? s_page_names[s_editor_page] : "",
        s_editor_win ? (unsigned)SDL_GetWindowID(s_editor_win) : 0u,
        w, h, x, y, pw, ph,
        raw_top, raw_left, raw_bottom, raw_right,
        top, left, bottom, right, estimated,
        x, y, w + left + right, h + top + bottom,
        min_w, min_h, display,
        bounds.x, bounds.y, bounds.w, bounds.h,
        usable.x, usable.y, usable.w, usable.h, workarea_estimated, fit,
        (flags & SDL_WINDOW_MAXIMIZED) != 0,
        (flags & SDL_WINDOW_MINIMIZED) != 0,
        (flags & SDL_WINDOW_RESIZABLE) != 0,
        s_editor_fit_count, s_editor_last_fit);
    return n > 0 && n < (int)cap;
}

int psx_fm_editor_set_geometry(int x, int y, int w, int h,
                               int restore, int maximize, int fit)
{
    if (!s_editor_win) return 0;
    /* Remember the target before applying an untrusted requested size.  Some
     * compositors migrate an enormous window to a larger neighbouring output,
     * which must not make FIT abandon the desktop the editor was on. */
    const int fit_display = fit ? SDL_GetWindowDisplayIndex(s_editor_win) : -1;
    SDL_Rect fit_bounds = { 0, 0, 0, 0 }, fit_usable = { 0, 0, 0, 0 };
    int fit_top = 0, fit_left = 0, fit_bottom = 0, fit_right = 0;
    int fit_max_w = 0, fit_max_h = 0, have_fit_target = 0;
    if (fit) {
        (void)editor_usable_bounds(fit_display, &fit_bounds, &fit_usable);
        have_fit_target = fit_usable.w > 0 && fit_usable.h > 0;
        if (have_fit_target) {
            (void)editor_borders(s_editor_win, &fit_top, &fit_left,
                                 &fit_bottom, &fit_right);
            fit_max_w = fit_usable.w - fit_left - fit_right -
                        2 * PSX_FM_EDITOR_FIT_SIDE_GUTTER;
            fit_max_h = fit_usable.h - fit_top - fit_bottom;
            if (fit_max_w < 1) fit_max_w = 1;
            if (fit_max_h < 1) fit_max_h = 1;
        }
    }
    if (restore) SDL_RestoreWindow(s_editor_win);
    if (w > 0 || h > 0) {
        int old_w = 0, old_h = 0;
        SDL_GetWindowSize(s_editor_win, &old_w, &old_h);
        int new_w = w > 0 ? w : old_w;
        int new_h = h > 0 ? h : old_h;
        /* Clamp before asking the compositor.  Sending the enormous size and
         * shrinking it afterward is too late on multi-monitor desktops. */
        if (have_fit_target && new_w > fit_max_w) new_w = fit_max_w;
        if (have_fit_target && new_h > fit_max_h) new_h = fit_max_h;
        SDL_SetWindowSize(s_editor_win, new_w, new_h);
    }
    if (x >= 0 || y >= 0) {
        int old_x = 0, old_y = 0;
        SDL_GetWindowPosition(s_editor_win, &old_x, &old_y);
        SDL_SetWindowPosition(s_editor_win, x >= 0 ? x : old_x,
                              y >= 0 ? y : old_y);
    }
    if (fit) {
        if (have_fit_target) {
            int cw = 0, ch = 0;
            SDL_GetWindowSize(s_editor_win, &cw, &ch);
            const int min_w = fit_max_w < PSX_FM_EDITOR_MIN_W
                                  ? fit_max_w : PSX_FM_EDITOR_MIN_W;
            const int min_h = fit_max_h < PSX_FM_EDITOR_MIN_CONTENT_H +
                                           PSX_FM_EDITOR_TAB_H
                                  ? fit_max_h
                                  : PSX_FM_EDITOR_MIN_CONTENT_H +
                                        PSX_FM_EDITOR_TAB_H;
            SDL_SetWindowMinimumSize(s_editor_win, min_w, min_h);
            if (cw > fit_max_w) cw = fit_max_w;
            if (ch > fit_max_h) ch = fit_max_h;
            SDL_SetWindowSize(s_editor_win, cw, ch);
            SDL_SetWindowPosition(
                s_editor_win,
                fit_usable.x +
                    (fit_usable.w - cw - fit_left - fit_right) / 2,
                fit_usable.y +
                    (fit_usable.h - ch - fit_top - fit_bottom) / 2);
            s_editor_last_fit = 1;
        } else {
            s_editor_last_fit = 0;
        }
        s_editor_fit_count++;
    }
    if (maximize) SDL_MaximizeWindow(s_editor_win);
    SDL_RaiseWindow(s_editor_win);
    return 1;
}

int psx_fm_editor_inject_tab(int page, int keyboard)
{
    SDL_Event ev;
    int w = 0, h = 0;
    if (!s_editor_win || page < 0 || page >= PSX_FM_PAGE_COUNT) return 0;
    SDL_GetWindowSize(s_editor_win, &w, &h);
    SDL_zero(ev);
    if (keyboard) {
        ev.type = SDL_KEYDOWN;
        ev.key.windowID = SDL_GetWindowID(s_editor_win);
#if defined(PSX_SDL3)
        ev.key.key = SDLK_1 + page;
        ev.key.mod = KMOD_CTRL;
#else
        ev.key.keysym.sym = SDLK_1 + page;
        ev.key.keysym.mod = KMOD_CTRL;
#endif
    } else {
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.windowID = SDL_GetWindowID(s_editor_win);
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.x = (page * 2 + 1) * w / (PSX_FM_PAGE_COUNT * 2);
        ev.button.y = PSX_FM_EDITOR_TAB_H / 2;
    }
    return SDL_PushEvent(&ev) == 1;
}

static void editor_row_activate(void)
{
    psx_fm_editor_open_page(s_editor_page >= 0 ? s_editor_page
                                               : PSX_FM_PAGE_CARDS);
}

static void editor_policy(void)
{
    static int last = -1;
    if (s_editor_win && s_editor_fit_due &&
        (int32_t)(SDL_GetTicks() - s_editor_fit_due) >= 0) {
        s_editor_fit_due = 0;
        (void)psx_fm_editor_set_geometry(-1, -1, -1, -1, 0, 0, 1);
    }
    const int enabled = !psx_ygo_netplay_session();
    if (enabled == last) return;
    last = enabled;
    psx_video_menu_set_row_enabled(
        s_editor_row, enabled,
        enabled ? NULL : "Unavailable during netplay (stock gameplay only)");
    if (!enabled) psx_fm_editor_close();
}

static void editor_stop(void)
{
    /* The active page owns the shared window. The other calls reset preserved
     * per-page UI state left by tab switches, without creating anything. */
    psx_fm_editor_close();
    psx_card_manager_close();
    psx_drop_viewer_close();
    psx_fusion_manager_close();
    psx_dialogue_manager_close();
    psx_cpu_manager_close();
    s_log_started = 0;
}

void psx_tool_log(const char *fmt, ...)
{
    const char *dir = psx_mod_player_data_dir();
    if (!dir || !dir[0]) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/tool_windows.log", dir);
    FILE *f = psx_fopen_utf8(path, s_log_started ? "ab" : "wb");
    if (!f) return;
    s_log_started = 1;
    char stamp[32];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
    fprintf(f, "%s  ", stamp);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int psx_tool_renderer_choice(void)
{
#if defined(_WIN32)
    int choice = 0;
#else
    int choice = 1;
#endif
    if (s_row >= 0) { const int v = psx_video_menu_get_row(s_row); if (v == 0 || v == 1) choice = v; }
    return choice;
}

SDL_Renderer *psx_tool_renderer_create(SDL_Window *win, const char *title, int prefer, int *software)
{
    SDL_Renderer *ren = NULL;
    *software = 0;
    const char *pick = getenv("PSX_TOOL_RENDERER");
    int want_software = prefer >= 0 ? (prefer == 0) : (psx_tool_renderer_choice() == 0);
    if (prefer < 0 && pick && *pick) want_software = !strcmp(pick, "software");
#if defined(PSX_SDL3)
    if (prefer < 0 && pick && *pick && strcmp(pick, "software") && strcmp(pick, "accelerated")) {
        /* a named driver (opengl, direct3d11, vulkan, ...) through the
         * properties API, which the SDL2-shape shim does not cover */
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, pick);
        SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, win);
        ren = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
        if (!ren) psx_tool_log("%s: renderer '%s' failed: %s", title, pick, SDL_GetError());
    } else
#endif
    if (want_software) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        *software = ren != NULL;
        if (!ren) psx_tool_log("%s: software renderer failed: %s", title, SDL_GetError());
    }
    if (!ren) { ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED); if (!ren) psx_tool_log("%s: accelerated renderer failed: %s", title, SDL_GetError()); }
    if (!ren) { ren = SDL_CreateRenderer(win, -1, 0); if (!ren) psx_tool_log("%s: any renderer failed: %s", title, SDL_GetError()); }
    if (ren) {
        if (win == s_editor_win) {
            s_editor_ren = ren;
            s_editor_tabs = NULL;
        }
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren, &w, &h);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
#if defined(PSX_SDL3)
        psx_tool_log("%s: renderer %s%s, output %dx%d, window %dx%d%s", title, SDL_GetRendererName(ren), *software ? " (window surface)" : "", w, h, ww, wh,
                     (pick && *pick) ? " [PSX_TOOL_RENDERER]" : "");
        fprintf(stderr, "%s: renderer %s%s\n", title, SDL_GetRendererName(ren), *software ? " (window surface)" : "");
#else
        psx_tool_log("%s: renderer%s, output %dx%d, window %dx%d", title, *software ? " software (window surface)" : "", w, h, ww, wh);
#endif
    }
    return ren;
}

static int editor_tabs_prepare(SDL_Renderer *ren, int w)
{
    if (ren != s_editor_ren || w <= 0) return 0;
    if (w != s_editor_tab_w || !s_editor_tab_px) {
        free(s_editor_tab_px);
        s_editor_tab_px = (uint32_t *)calloc(
            (size_t)w * PSX_FM_EDITOR_TAB_H, sizeof(uint32_t));
        if (!s_editor_tab_px) { s_editor_tab_w = 0; return 0; }
        s_editor_tab_w = w;
        s_editor_tabs = NULL;
    }
    if (!s_editor_tabs) {
        s_editor_tabs = SDL_CreateTexture(
            ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            w, PSX_FM_EDITOR_TAB_H);
        if (!s_editor_tabs) return 0;
    }
    memset(s_editor_tab_px, 0,
           (size_t)w * PSX_FM_EDITOR_TAB_H * sizeof(uint32_t));
    PsxUiCanvas c = { s_editor_tab_px, w, PSX_FM_EDITOR_TAB_H, 0, 0, 0, 0 };
    psx_ui_fill(&c, 0, 0, w, PSX_FM_EDITOR_TAB_H, 0xFF171B25u);
    const PsxUiFace *face = psx_ui_font_face(13.0f, PSX_UI_FONT_SEMIBOLD);
    for (int p = 0; p < PSX_FM_PAGE_COUNT; p++) {
        const int x0 = p * w / PSX_FM_PAGE_COUNT;
        const int x1 = (p + 1) * w / PSX_FM_PAGE_COUNT;
        if (p == s_editor_page)
            psx_ui_round_rect(&c, x0 + 3, 3, x1 - x0 - 6,
                              PSX_FM_EDITOR_TAB_H - 6, 5.0f, 0xFF2D3850u);
        if (p) psx_ui_fill(&c, x0, 8, 1, PSX_FM_EDITOR_TAB_H - 16,
                           0xFF3B4354u);
        if (face) {
            const int tw = psx_ui_font_text_w(face, s_page_names[p]);
            psx_ui_text(&c, x0 + (x1 - x0 - tw) / 2,
                        psx_ui_baseline_in(0, PSX_FM_EDITOR_TAB_H, face),
                        s_page_names[p],
                        p == s_editor_page ? 0xFFF4F7FFu : 0xFFADB6C8u,
                        face);
        }
    }
#if defined(PSX_SDL3)
    return SDL_UpdateTexture(s_editor_tabs, NULL, s_editor_tab_px, w * 4) ? 1 : 0;
#else
    return SDL_UpdateTexture(s_editor_tabs, NULL, s_editor_tab_px, w * 4) == 0;
#endif
}

int psx_tool_present(SDL_Renderer *ren, SDL_Texture *tex, const void *px, int w, int h, const char *title)
{
    static char noted[8][48]; static int nnoted;
    int ok = 1;
    const char *step = "";
#if defined(PSX_SDL3)
    if (!SDL_UpdateTexture(tex, NULL, px, w * 4)) { ok = 0; step = "UpdateTexture"; }
    else if (!SDL_RenderClear(ren)) { ok = 0; step = "RenderClear"; }
    else if (ren == s_editor_ren) {
        SDL_Rect dst = { 0, PSX_FM_EDITOR_TAB_H, w, h };
        if (SDL_RenderCopy(ren, tex, NULL, &dst) != 0) { ok = 0; step = "RenderCopy"; }
        else if (!editor_tabs_prepare(ren, w)) { ok = 0; step = "EditorTabs"; }
        else { SDL_Rect tabs = { 0, 0, w, PSX_FM_EDITOR_TAB_H };
               if (SDL_RenderCopy(ren, s_editor_tabs, NULL, &tabs) != 0) { ok = 0; step = "EditorTabsCopy"; } }
    }
    else if (SDL_RenderCopy(ren, tex, NULL, NULL) != 0) { ok = 0; step = "RenderCopy"; }   /* the shim keeps SDL2's 0 = ok */
    else if (!SDL_RenderPresent(ren)) { ok = 0; step = "RenderPresent"; }
#else
    if (SDL_UpdateTexture(tex, NULL, px, w * 4) != 0) { ok = 0; step = "UpdateTexture"; }
    else if (SDL_RenderClear(ren) != 0) { ok = 0; step = "RenderClear"; }
    else if (ren == s_editor_ren) {
        SDL_Rect dst = { 0, PSX_FM_EDITOR_TAB_H, w, h };
        if (SDL_RenderCopy(ren, tex, NULL, &dst) != 0) { ok = 0; step = "RenderCopy"; }
        else if (!editor_tabs_prepare(ren, w)) { ok = 0; step = "EditorTabs"; }
        else { SDL_Rect tabs = { 0, 0, w, PSX_FM_EDITOR_TAB_H };
               if (SDL_RenderCopy(ren, s_editor_tabs, NULL, &tabs) != 0) { ok = 0; step = "EditorTabsCopy"; } }
    }
    else if (SDL_RenderCopy(ren, tex, NULL, NULL) != 0) { ok = 0; step = "RenderCopy"; }
    else SDL_RenderPresent(ren);
#endif
    if (ok && ren == s_editor_ren) {
#if defined(PSX_SDL3)
        if (!SDL_RenderPresent(ren)) { ok = 0; step = "RenderPresent"; }
#else
        SDL_RenderPresent(ren);
#endif
    }
    if (!ok) { psx_tool_log("%s: %s failed (%dx%d): %s", title, step, w, h, SDL_GetError()); return 0; }
    /* the first successful present of each window, once */
    for (int i = 0; i < nnoted; i++) if (!strcmp(noted[i], title)) return 1;
    if (nnoted < 8) { snprintf(noted[nnoted], sizeof noted[nnoted], "%s", title); nnoted++; }
    psx_tool_log("%s: first frame shown (%dx%d)", title, w, h);
    return 1;
}

PSX_MOD_CONSTRUCTOR(psx_tool_window_install)
{
    static const char *const CHOICES[2] = { "Software", "Accelerated" };
#if defined(_WIN32)
    const int def = 0;
#else
    const int def = 1;
#endif
    s_row = psx_video_menu_add_option(PSX_VM_MENU_VIEW, "Tool windows",
        "How FM Editor pages are drawn. Switch if one shows blank; takes effect when the editor is next opened",
        CHOICES, 2, "tool_renderer", def, NULL);
    s_editor_row = psx_video_menu_add_action(
        PSX_VM_MENU_VIEW, "FM Editor",
        "Cards, Drop Tables, Fusions, Dialogue and CPU in one window",
        editor_row_activate);
    (void)psx_game_add_start_hook(editor_policy);
    (void)psx_game_add_frame_hook(editor_policy);
    (void)psx_game_add_stop_hook(editor_stop);
}
