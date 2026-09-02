#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Yu-Gi-Oh! Forbidden Memories - Recompiled
# Linux / macOS counterpart of Play.bat. Launches the way a player does.
#
# Usage:   ./Play.sh          normal (Release build in build/)
#          ./Play.sh -dbg     instrumented build in build-dbg/, TCP debug
#                             server on 127.0.0.1:4370
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE="Yu_Gi_Oh_Forbidden_Memories_Recompiled"

if [ "${1:-}" = "-dbg" ]; then
    GAMEDIR="$HERE/build-dbg"
    shift
else
    GAMEDIR="$HERE/build"
fi

if [ ! -x "$GAMEDIR/$EXE" ]; then
    echo "ERROR: $EXE not found in \"$GAMEDIR\"." >&2
    echo "Build it first, from \"$HERE\":" >&2
    echo "    cmake --build build --target psx-runtime" >&2
    exit 1
fi

# Run from the game dir so saves/settings/bios resolve beside the executable.
cd "$GAMEDIR" || exit 1
"$GAMEDIR/$EXE" "$@"
RC=$?

if [ "$RC" -ne 0 ]; then
    echo
    echo "Exited with code $RC."
fi
exit "$RC"
