/* psx_tool_window.h -- what the five FM Editor pages share: their one native
 * window, tab lifecycle, renderer selection, checked presentation, and a log
 * file for the machines we cannot look at.
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

/* One native FM Editor window shared by all five page implementations.  Each
 * page keeps its existing renderer/canvas/backend; switching releases those
 * transient objects, retains the page's selection/edit state, and attaches
 * the next page to the same SDL_Window. */
enum {
    PSX_FM_PAGE_CARDS = 0,
    PSX_FM_PAGE_DROPS,
    PSX_FM_PAGE_FUSIONS,
    PSX_FM_PAGE_DIALOGUE,
    PSX_FM_PAGE_CPU,
    PSX_FM_PAGE_COUNT
};
#define PSX_FM_EDITOR_TAB_H 38
#define PSX_FM_EDITOR_MIN_W 720
#define PSX_FM_EDITOR_MIN_CONTENT_H 420
SDL_Window *psx_fm_editor_acquire(int page, int content_w, int content_h);
void psx_fm_editor_release(int page);
void psx_fm_editor_open_page(int page);
void psx_fm_editor_close(void);
int  psx_fm_editor_is_open(void);
int  psx_fm_editor_page(void);
int  psx_fm_editor_is_switching(void);
int  psx_fm_editor_content_height(int output_h);
int  psx_fm_editor_window_y(int content_y);
/* Call first in a page's event hook. Returns 1 for tab-strip/shortcut input;
 * otherwise copies the event to adjusted and translates its mouse y into the
 * page canvas below the tabs. */
int psx_fm_editor_filter_event(int page, const SDL_Event *event,
                               SDL_Event *adjusted);
/* Machine-readable state for the general debug command. */
int psx_fm_editor_state_json(char *out, unsigned cap);
/* Reset the cumulative low-overhead editor tick/event/present profiler. */
void psx_fm_editor_profile_reset(void);
/* Test/automation input through the same event path as a physical click or
 * Ctrl+1..5. keyboard=0 clicks the tab, nonzero sends the shortcut. */
int psx_fm_editor_inject_tab(int page, int keyboard);
/* Native-window test/control seam. Negative x/y/w/h leave that component
 * unchanged. restore happens before resize/move; maximize and fit happen
 * afterward. FIT clamps the complete decorated window inside its current
 * display's usable work area and requests centering where the compositor
 * permits it. */
int psx_fm_editor_set_geometry(int x, int y, int w, int h,
                               int restore, int maximize, int fit);
#ifdef __cplusplus
}
#endif
#endif
