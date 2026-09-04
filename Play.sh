#!/bin/sh
# Yu-Gi-Oh! Forbidden Memories - Recompiled  (Linux launcher)
#
#   ./Play.sh          release build  (build/)
#   ./Play.sh -dbg     debug build    (build-dbg/, TCP debug server on 127.0.0.1:4370)
#
# Extra arguments are passed to the runtime (e.g. --disc <path>).
# Runs from the build dir so saves/settings/bios resolve beside the executable.
# Build first if missing:
#   cmake -S . -B build     -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build     --target psx-runtime
#   cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_REWIND=OFF -DPSX_DEBUG_TOOLS=ON && cmake --build build-dbg --target psx-runtime
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DIR="$HERE/build"
if [ "$1" = "-dbg" ]; then DIR="$HERE/build-dbg"; shift; fi
EXE="$DIR/Yu_Gi_Oh_Forbidden_Memories_Recompiled"
if [ ! -x "$EXE" ]; then
    echo "ERROR: build not found: $EXE" >&2
    exit 1
fi
cd "$DIR"
exec "$EXE" "$@"
