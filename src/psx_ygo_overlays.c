/* psx_ygo_overlays.c — see psx_ygo_overlays.h.
 *
 * The three overlays this title draws in the game's own coordinate space. They
 * used to be three near-identical blocks compiled into the framework's
 * renderer, which named their functions directly and so failed to link for any
 * other title. The mapping they all shared stayed in the renderer; what each
 * one contributes — its image, its origin, whether anything can cover it — is
 * described here.
 */

#include <string.h>

#include "mod_plugins.h"
#include "psx_game_hooks.h"
#include "psx_guest_overlay.h"

#include "psx_card_shop.h"
#include "psx_cd_overlay.h"
#include "psx_fusion_overlay.h"
#include "psx_mode_select_confirm.h"
#include "psx_rank_meter.h"
#include "psx_ygo_overlays.h"

/* Sprite-watch group the card-view plates are tracked in. The meter sits beside
 * the FIELD box, and a card view drawn over that box must hide it. */
#define PSX_YGO_RANK_OCCLUSION_GROUP 2

static int s_rank_placement[10];

static void rank_placed(const int *o)
{
    memcpy(s_rank_placement, o, sizeof s_rank_placement);
}

void psx_ygo_rank_placement(int *o)
{
    memcpy(o, s_rank_placement, sizeof s_rank_placement);
}

/* Back to front. The meter first because it is the only one anything is ever
 * drawn over; the other two only appear on screens that draw nothing above
 * them, which is why neither watches for an occluder. */
PSX_MOD_CONSTRUCTOR(psx_ygo_overlays_install) {
    PsxGuestOverlay rank = {
        psx_rank_meter_image,
        psx_rank_meter_origin,
        psx_rank_meter_subpixel_y,
        psx_rank_meter_needs_present,
        PSX_YGO_RANK_OCCLUSION_GROUP,
        rank_placed,
    };
    /* CARD DROPS page "New!" tags. */
    PsxGuestOverlay drops = {
        psx_cd_overlay_image,
        psx_cd_overlay_origin,
        NULL,
        psx_cd_overlay_needs_present,
        -1,
        NULL,
    };
    /* The fusion assistant's line above the hand. It only draws while the hand
     * is pickable, which is exactly when nothing covers that strip. */
    PsxGuestOverlay fusion = {
        psx_fusion_overlay_image,
        psx_fusion_overlay_origin,
        NULL,
        psx_fusion_overlay_needs_present,
        -1,
        NULL,
    };
    (void)psx_guest_overlay_register(&rank);
    (void)psx_guest_overlay_register(&drops);
    (void)psx_guest_overlay_register(&fusion);
    /* CARD SHOP: the shopkeeper-menu fifth row and its pack panel. Drawn on
     * campaign menu screens, which draw nothing above them. */
    {
        PsxGuestOverlay shop = {
            psx_card_shop_image,
            psx_card_shop_origin,
            NULL,
            psx_card_shop_needs_present,
            -1,
            NULL,
        };
        (void)psx_guest_overlay_register(&shop);
        psx_card_shop_register_menu();
        (void)psx_game_add_frame_hook(psx_card_shop_tick);
    }
    /* MODE SELECT CIRCLE CONFIRM. Last, so its prompt draws over everything
     * else a title/mode-select screen could have on it. Registers its own
     * vblank hook and pad filter — it has to sample at the guest's cadence,
     * not the host's, and it takes the pad while the prompt is up. */
    {
        PsxGuestOverlay confirm = {
            psx_mode_select_confirm_image,
            psx_mode_select_confirm_origin,
            NULL,
            psx_mode_select_confirm_needs_present,
            -1,
            NULL,
        };
        (void)psx_guest_overlay_register(&confirm);
        psx_mode_select_confirm_register();
    }
}
