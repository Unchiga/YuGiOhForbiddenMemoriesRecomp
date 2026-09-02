#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Yu-Gi-Oh! Forbidden Memories - Recompiled  (DEBUG BUILD)
# Linux / macOS counterpart of PlayDebug.bat.
#
# Same game as Play.sh, but the build in build-dbg/ was configured with
# PSX_DEBUG_TOOLS=ON, which adds the TCP debug server on 127.0.0.1:4370.
# That is what lets memory scanning, write-tracing, framebuffer screenshots
# and the decomp's RAM captures (tools/recomp_capture.py) work while you play.
#
# Run it from a terminal and leave that terminal visible: the runtime's log
# prints here, and the last lines usually say why if something misbehaves.
#
# Usage:   ./PlayDebug.sh [runtime args...]
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE="Yu_Gi_Oh_Forbidden_Memories_Recompiled"
GAMEDIR="$HERE/build-dbg"

# Only pause for a keypress when a person is actually at the terminal; a
# script or tool driving this must never block on stdin.
hold_open() {
    if [ -t 0 ]; then
        read -r -p "Press Enter to close... " _
    fi
}

if [ ! -x "$GAMEDIR/$EXE" ]; then
    echo "ERROR: debug build not found in \"$GAMEDIR\"." >&2
    echo "Build it from \"$HERE\":" >&2
    echo "    cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_REWIND=OFF -DPSX_DEBUG_TOOLS=ON" >&2
    echo "    cmake --build build-dbg --target psx-runtime" >&2
    hold_open
    exit 1
fi

echo "==============================================================="
echo " Yu-Gi-Oh! Forbidden Memories - Recompiled   [DEBUG]"
echo " Debug server: 127.0.0.1:4370"
echo " Keep this terminal open; the runtime log prints below."
echo "==============================================================="
echo

# Run from the game dir so saves/settings/bios resolve beside the executable,
# and invoke by full path so no PATH lookup is involved.
cd "$GAMEDIR" || exit 1
"$GAMEDIR/$EXE" "$@"
RC=$?

echo
echo "=== exited with code $RC ==="

# Close with the game on a normal exit. Only hold the terminal when something
# went wrong, so the log above stays readable instead of vanishing.
if [ "$RC" -ne 0 ]; then
    echo "Non-zero exit - leaving this open so you can read the log."
    hold_open
fi

exit "$RC"
