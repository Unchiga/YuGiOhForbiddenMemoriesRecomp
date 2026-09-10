#!/bin/sh
# Install (or remove) the Linux desktop entry and icon set for this checkout.
#
#   tools/install_desktop_entry.sh              # into ~/.local/share
#   tools/install_desktop_entry.sh --uninstall
#
# Everything lands under $XDG_DATA_HOME (default ~/.local/share): no root, no
# files outside the user's own data directory. The .desktop is written here
# rather than copied from assets/icons/ because Exec has to be the absolute
# path of THIS checkout's Play.sh, and a relative Exec silently produces a
# launcher that does nothing when started from a menu (the working directory
# is the user's home, not the game folder).
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
ID=ygofm-recompiled
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor"
# The hicolor sizes; 1024 is a macOS slot with no hicolor directory, so it is
# not installed here.
SIZES="16 32 48 64 128 256 512"

refresh() {
    [ -x "$(command -v update-desktop-database)" ] && update-desktop-database "$APPS" 2>/dev/null || true
    [ -x "$(command -v gtk-update-icon-cache)" ] && gtk-update-icon-cache -qtf "$ICONS" 2>/dev/null || true
    return 0
}

if [ "$1" = "--uninstall" ]; then
    rm -f "$APPS/$ID.desktop"
    for s in $SIZES; do rm -f "$ICONS/${s}x${s}/apps/$ID.png"; done
    rm -f "$ICONS/scalable/apps/$ID.svg"
    refresh
    echo "removed $ID from $DATA"
    exit 0
fi

for s in $SIZES; do
    src="$HERE/assets/icons/png/ygofm-$s.png"
    [ -f "$src" ] || { echo "missing $src: run tools/gen_app_icon.py" >&2; exit 1; }
    mkdir -p "$ICONS/${s}x${s}/apps"
    cp "$src" "$ICONS/${s}x${s}/apps/$ID.png"
done
mkdir -p "$ICONS/scalable/apps"
cp "$HERE/assets/icons/ygofm.svg" "$ICONS/scalable/apps/$ID.svg"

mkdir -p "$APPS"
sed "s|^Exec=.*|Exec=$HERE/Play.sh|" "$HERE/assets/icons/$ID.desktop" > "$APPS/$ID.desktop"
chmod 644 "$APPS/$ID.desktop"
refresh
echo "installed $ID (Exec=$HERE/Play.sh) into $DATA"
