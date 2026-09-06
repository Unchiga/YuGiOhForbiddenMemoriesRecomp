#ifndef PSX_FUSION_OVERLAY_H
#define PSX_FUSION_OVERLAY_H

/* The in-duel fusion assistant's one line of text.
 *
 * Sits just above the hand and answers the question the game never does: what
 * do these cards make? With nothing picked it names the best fusion the hand
 * can reach; as cards are picked it shows the card that would stand if they
 * were summoned now, which IS the running fusion, because the game folds a
 * multi-card summon two at a time in pick order.
 *
 * Same shape as the rank meter and the CARD DROPS tags: a small ARGB canvas
 * authored in GUEST pixels that the renderer maps into the letterboxed game
 * rect. Pure state and rasterisation — no GL, and no guest reads of its own;
 * psx_fusion_assist supplies the content.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VIEW > FUSION HINT. Off, the pick-order numbers on their own, those plus the
 * card the line would make, or HINT ONLY: one line saying a fusion is there,
 * with neither the cards to pick nor what they make.
 *
 * HINT ONLY is last rather than second because the value is what
 * menu_settings.ini stores, and renumbering the three that shipped would
 * quietly move every existing player's setting. */
enum { PSX_FUSION_HINT_OFF = 0, PSX_FUSION_HINT_NUMBERS = 1,
       PSX_FUSION_HINT_FULL = 2, PSX_FUSION_HINT_ONLY = 3 };
#define PSX_FUSION_HINT_MAX PSX_FUSION_HINT_ONLY

/* Once a frame: re-reads the duel through psx_fusion_assist and re-rasterises
 * only when the line actually changes. Cheap to call always. */
void psx_fusion_overlay_tick(void);

/* 1 + fills the ARGB canvas when there is something to draw. Guest pixels. */
int  psx_fusion_overlay_image(const uint32_t **pixels, int *w, int *h);

/* Top-left corner in guest pixels. */
void psx_fusion_overlay_origin(int *x, int *y);

/* 1 while the line is on screen, so a present can be forced. */
int  psx_fusion_overlay_needs_present(void);

/* Live layout tuning over the debug server — placing text against the game's
 * own art is a by-eye job and a rebuild per nudge costs the player their duel.
 * ABSOLUTE values; PSX_FUSION_OVERLAY_KEEP leaves a field alone. */
#define PSX_FUSION_OVERLAY_KEEP (-100000)
void psx_fusion_overlay_tune(int x, int y, int text_x, int mode);
void psx_fusion_overlay_tune_get(int *x, int *y, int *text_x, int *mode);

/* Hand-card geometry the order badges ride on. Same KEEP convention. */
void psx_fusion_overlay_tune_cards(int card_x, int card_dx, int badge_dy,
                                   int badge_dx, int text_y);
void psx_fusion_overlay_tune_cards_get(int *card_x, int *card_dx, int *badge_dy,
                                       int *badge_dx, int *text_y);

/* Menu handoff; takes effect immediately. */
/* Fusion-assistant display modes. Owned here for the same reason as the rank
 * modes: they describe this game's duel, not the framework's menu. */
enum { PSX_VM_FUSION_HINT_OFF = 0, PSX_VM_FUSION_HINT_NUMBERS = 1,
       PSX_VM_FUSION_HINT_FULL = 2, PSX_VM_FUSION_HINT_ONLY = 3 };

void psx_fusion_overlay_set_mode(int mode);

/* Adds VIEW > FUSION HINT and VIEW > SUGGEST FUSION BY. Both rows belong to
 * one feature, so one call registers the pair. */
void psx_fusion_overlay_register_menu(void);

/* What the line currently says, for `fusion_overlay`. The tab in it separates
 * the part set in the alphabet from the part set in the small digits. */
const char *psx_fusion_overlay_text(void);

/* The recommended pick order per hand slot, 1..5, 0 where there is none.
 * Returns how many slots carry a badge. */
int psx_fusion_overlay_badges(uint8_t *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FUSION_OVERLAY_H */
