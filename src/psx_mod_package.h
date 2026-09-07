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

/* COMPATIBILITY CONTRACT for .ygomods files (and the share files inside them)
 *
 *  - A package is a zip of independent parts, each read only by the manager
 *    that owns it, in that manager's own plain-text format. A part the file
 *    lacks leaves that manager alone; a file the game does not know is
 *    ignored; an unknown key, row or line inside a part is skipped.
 *  - Keys never change meaning. A field gains a new key; it never reuses an
 *    old one for something else. A row a later version removed (a settings
 *    key, say) is simply not found and ignored.
 *  - PSX_MOD_PACKAGE_VERSION is bumped ONLY when an older game would misread
 *    the file. Older packages always import; a newer package is refused with
 *    "Made by a newer version (N); update the game" rather than half-read.
 *  - Every release runs tools/package_roundtrip.py against the debug build:
 *    export, Revert to Stock, import, export again, part-by-part equality,
 *    and an import of every earlier version's package in tools/fixtures/.
 *    The B package it keeps becomes the next version's fixture. */
#define PSX_MOD_PACKAGE_EXT     "ygomods"
#define PSX_MOD_PACKAGE_FORMAT  "YGOFM-MOD-PACKAGE"
#define PSX_MOD_PACKAGE_VERSION 1

int  psx_mod_package_export(const char *path, char *msg, unsigned cap);
int  psx_mod_package_import(const char *path, char *msg, unsigned cap);
/* What a file holds, one line, without importing it. */
int  psx_mod_package_inspect(const char *path, char *msg, unsigned cap);
/* Everything back to the disc's own: every manager's edits cleared through
 * that manager's own reset and every mod row back to its default. The MODS
 * row asks twice (a first choice warns, a second within ten seconds does it);
 * this call does it at once. */
int  psx_mod_package_reset_all(char *msg, unsigned cap);
/* The row's own two-step, for a test: call once to arm, again to revert. */
void psx_mod_package_revert_row(void);
int  psx_mod_package_revert_row_armed(void);
/* The default file name, in <player-data>/mod_packages. */
void psx_mod_package_default_path(char *out, unsigned cap);

void psx_mod_package_register_menu(void);
/* The last message the rows produced, for the debug command. */
const char *psx_mod_package_last_message(void);

#ifdef __cplusplus
}
#endif
#endif /* PSX_MOD_PACKAGE_H */
