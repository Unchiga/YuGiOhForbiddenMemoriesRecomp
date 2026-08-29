/* psx_ygo_widescreen.c — experimental 16:9, two ways in.
 *
 * The mod-catalog feature (psx.enhancement.widescreen, contributed by yamyi)
 * activates it at boot through the manifest; the VIEW row toggles the same
 * display aspect from the overlay menu and persists like any other setting.
 * Both funnel into psx_mod_set_fixed_display_aspect, so whichever the player
 * used last wins and there is one source of truth in the runtime.
 *
 * The geometry-side behaviour ([widescreen] gte_game_mode and the cull keys
 * in game.toml) follows the framework's WIDESCREEN.md pattern and is the
 * experimental part: culling pop-in at the wide edges has not been fully
 * checked for this title.
 */
#include "mod_plugins.h"
#include "psx_video_menu.h"

static const char *const WS_LABELS[] = { "Off", "16:9" };
static const char *const WS_HINTS[]  = {
    "Native 4:3",
    "Stretch to 16:9 \xe2\x80\x94 experimental"
};

static void ygo_widescreen_row_changed(int value) {
    (void)psx_mod_set_fixed_display_aspect(value ? 16u : 4u,
                                           value ? 9u : 3u);
}

static void ygo_widescreen_activate(void) {
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
}

PSX_MOD_CONSTRUCTOR(psx_register_ygo_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin(
        "psx.widescreen", ygo_widescreen_activate);
    {
        const int h = psx_video_menu_add_option(
            PSX_VM_MENU_VIEW, "Widescreen", WS_HINTS[0],
            WS_LABELS, 2, "widescreen", 0, ygo_widescreen_row_changed);
        psx_video_menu_set_row_hints(h, WS_HINTS);
    }
}
