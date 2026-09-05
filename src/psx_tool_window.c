#include "psx_tool_window.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mod_plugins.h"
#include "psx_video_menu.h"
#include "psx_textfile.h"

static int  s_row = -1;
static int  s_log_started;

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

int psx_tool_present(SDL_Renderer *ren, SDL_Texture *tex, const void *px, int w, int h, const char *title)
{
    static char noted[4][48]; static int nnoted;
    int ok = 1;
    const char *step = "";
#if defined(PSX_SDL3)
    if (!SDL_UpdateTexture(tex, NULL, px, w * 4)) { ok = 0; step = "UpdateTexture"; }
    else if (!SDL_RenderClear(ren)) { ok = 0; step = "RenderClear"; }
    else if (SDL_RenderCopy(ren, tex, NULL, NULL) != 0) { ok = 0; step = "RenderCopy"; }   /* the shim keeps SDL2's 0 = ok */
    else if (!SDL_RenderPresent(ren)) { ok = 0; step = "RenderPresent"; }
#else
    if (SDL_UpdateTexture(tex, NULL, px, w * 4) != 0) { ok = 0; step = "UpdateTexture"; }
    else if (SDL_RenderClear(ren) != 0) { ok = 0; step = "RenderClear"; }
    else if (SDL_RenderCopy(ren, tex, NULL, NULL) != 0) { ok = 0; step = "RenderCopy"; }
    else SDL_RenderPresent(ren);
#endif
    if (!ok) { psx_tool_log("%s: %s failed (%dx%d): %s", title, step, w, h, SDL_GetError()); return 0; }
    /* the first successful present of each window, once */
    for (int i = 0; i < nnoted; i++) if (!strcmp(noted[i], title)) return 1;
    if (nnoted < 4) { snprintf(noted[nnoted], sizeof noted[nnoted], "%s", title); nnoted++; }
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
        "How the Card, Drop Table and Dialogue manager windows are drawn. Switch if one shows blank; takes effect when the window is next opened",
        CHOICES, 2, "tool_renderer", def, NULL);
}
