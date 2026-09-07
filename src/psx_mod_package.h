/* psx_mod_package.h -- one file that carries everything a player has changed.
 *
 * A .ygomods file is a zip (the .ygocards container, stored entries, so any
 * zip tool opens it) holding every manager's share file flattened together
 * and the MODS / CHEATS rows of the menu:
 *
 *   manifest.ini                   format, version, what is inside
 *   cards-manifest.ini             the Card Manager's own manifest
 *   cards/<id>/<file>              edited cards (card.ini, art, thumb, title)
 *   drop_table_edits.ini           drop tables and scripted story drops
 *   cpu-duelists.ini               CPU decks, AI, names
 *   duelists/<id>/portrait.png     CPU portraits
 *   fusion-edits.txt               the Fusion Manager's edits
 *   dialogue.txt                   the Dialogue Manager's translation
 *   drop_missing_cards.ini         the Drop missing cards placements
 *   card_shop.ini                  the Card shop's configuration
 *   mod_settings.ini               key = value for every MODS and CHEATS row
 *
 * Export gathers whatever exists; Import hands each part to the manager that
 * owns it, through that manager's own import, so the semantics are the
 * manager's (each replaces its own domain and keeps the result). A part the
 * file does not carry leaves that manager alone. MODS > Import MOD package
 * and Export MOD package are the two rows; the debug command mod_package
 * drives the same code without a dialog. */
#ifndef PSX_MOD_PACKAGE_H
#define PSX_MOD_PACKAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_MOD_PACKAGE_EXT     "ygomods"
#define PSX_MOD_PACKAGE_FORMAT  "YGOFM-MOD-PACKAGE"
#define PSX_MOD_PACKAGE_VERSION 1

int  psx_mod_package_export(const char *path, char *msg, unsigned cap);
int  psx_mod_package_import(const char *path, char *msg, unsigned cap);
/* What a file holds, one line, without importing it. */
int  psx_mod_package_inspect(const char *path, char *msg, unsigned cap);
/* The default file name, in <player-data>/mod_packages. */
void psx_mod_package_default_path(char *out, unsigned cap);

void psx_mod_package_register_menu(void);
/* The last message the rows produced, for the debug command. */
const char *psx_mod_package_last_message(void);

#ifdef __cplusplus
}
#endif
#endif /* PSX_MOD_PACKAGE_H */
