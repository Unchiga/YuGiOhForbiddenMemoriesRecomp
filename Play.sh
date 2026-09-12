#!/bin/sh
# Yu-Gi-Oh! Forbidden Memories - Recompiled  (Linux launcher)
#
#   ./Play.sh                   release build (build/)
#   ./Play.sh -dbg|--debug      debug-tools build (build-dbg/)
#   ./Play.sh -rel|--release    release build (build/)
#
# Extra arguments are passed to the runtime (e.g. --disc <path>).
# Runs from the selected build directory so runtime assets resolve consistently.
# Build first if missing:
#   cmake -S . -B build     -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build     --target psx-runtime
#   cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_REWIND=OFF -DPSX_DEBUG_TOOLS=ON && cmake --build build-dbg --target psx-runtime
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DIR="$HERE/build"

case "${1-}" in
    -dbg|--debug)
        DIR="$HERE/build-dbg"
        shift
        ;;
    -rel|--release)
        shift
        ;;
    -h|--help)
        echo "Usage: $0 [-rel|--release|-dbg|--debug] [runtime arguments...]"
        exit 0
        ;;
esac

EXE="$DIR/Yu_Gi_Oh_Forbidden_Memories_Recompiled"
if [ ! -x "$EXE" ]; then
    echo "ERROR: build not found: $EXE" >&2
    if [ "$DIR" = "$HERE/build-dbg" ]; then
        echo "Build it with: cmake --build build-dbg --target psx-runtime" >&2
    else
        echo "Build it with: cmake --build build --target psx-runtime" >&2
    fi
    exit 1
fi
cd "$DIR"
exec "$EXE" "$@"
