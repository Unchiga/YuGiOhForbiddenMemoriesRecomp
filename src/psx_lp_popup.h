/* psx_lp_popup.h -- the "-2000" / "+500" that floats over the field when an
 * edited heal or burn resolves. The game's own popups for those are fixed
 * sprites for the stock amounts, so this draws the real number, in the
 * duel's digit font, as a host overlay in guest coordinates. */
#ifndef PSX_LP_POPUP_H
#define PSX_LP_POPUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show "-amount" (damage) or "+amount" (heal) at the stock popup's spot. */
void psx_lp_popup_show(int amount, int heal);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LP_POPUP_H */
