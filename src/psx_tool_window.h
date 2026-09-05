/* psx_tool_window.h -- what the tool windows (Card Manager, Drop Table
 * Manager, Dialogue Manager) share: which renderer draws them, a checked
 * present, and a log file for the machines we cannot look at.
 *
 * Renderer choice, in order: PSX_TOOL_RENDERER=software|accelerated|<driver>
 * in the environment, else the VIEW menu row "Tool windows" (kept in
 * menu_settings.ini; Software by default on Windows, Accelerated elsewhere).
 * When presenting keeps failing on one backend the window switches to the
 * other by itself and says so in the log. The log is
 * <player-data>/tool_windows.log, started afresh each run. */
#ifndef PSX_TOOL_WINDOW_H
#define PSX_TOOL_WINDOW_H
#include "psx_sdl.h"
#ifdef __cplusplus
extern "C" {
#endif
void psx_tool_log(const char *fmt, ...);
/* prefer: -1 the configured choice, 0 software, 1 accelerated. *software
 * tells whether the result paints through the window surface (no GL
 * context to put back). NULL when nothing could be made (logged). */
SDL_Renderer *psx_tool_renderer_create(SDL_Window *win, const char *title, int prefer, int *software);
/* Upload the canvas and show it. 1 = shown, 0 = something failed (logged
 * the first few times per window). */
int psx_tool_present(SDL_Renderer *ren, SDL_Texture *tex, const void *px, int w, int h, const char *title);
/* the configured choice right now: 0 software, 1 accelerated */
int psx_tool_renderer_choice(void);
#ifdef __cplusplus
}
#endif
#endif
