/* psx_asset_manager.h -- browse the HD texture pack's catalog, compare stock
 * art against the pack's replacement, and upload one without leaving the
 * game. See psx_asset_manager.c for the design. */
#ifndef PSX_ASSET_MANAGER_H
#define PSX_ASSET_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

void psx_asset_manager_open(void);
void psx_asset_manager_close(void);
int  psx_asset_manager_is_open(void);

/* --- debug side, same shape as the other tool windows --------------------- */
void psx_asset_manager_request_open(int open);   /* >0 open, <=0 close */
int  psx_asset_manager_click(int x, int y, int button);
int  psx_asset_manager_shot(const char *path);
int  psx_asset_manager_state_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_ASSET_MANAGER_H */
