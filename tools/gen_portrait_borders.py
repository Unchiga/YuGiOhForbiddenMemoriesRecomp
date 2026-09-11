#!/usr/bin/env python3
"""Bake portraitBorder1.png .. portraitBorder8.png into compact C tables.

The frames are project-owner artwork rather than disc content. This keeps the
exact RGBA pixels in a dependency-free, source-reviewable form suitable for the
runtime's existing guest-overlay compositor.

    tools/gen_portrait_borders.py <png-directory> <output.c>
"""

from itertools import groupby
from pathlib import Path
import sys

from PIL import Image


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_portrait_borders.py <png-directory> <output.c>")
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    frames = []
    for number in range(1, 9):
        path = source / ("portraitBorder%d.png" % number)
        image = Image.open(path).convert("RGBA")
        if image.size != (48, 48):
            raise SystemExit("%s must be 48x48" % path)
        get_pixels = getattr(image, "get_flattened_data", image.getdata)
        pixels = list(get_pixels())
        palette = list(dict.fromkeys(pixels))
        if len(palette) > 256:
            raise SystemExit("%s has more than 256 colors" % path)
        index = {color: i for i, color in enumerate(palette)}
        runs = [(len(list(items)), index[color])
                for color, items in groupby(pixels)]
        if any(length > 255 for length, _ in runs):
            raise SystemExit("%s contains an overlong run" % path)
        frames.append((palette, runs))

    lines = [
        "/* Generated losslessly from portraitBorder1.png .. portraitBorder8.png. */",
        '#include "psx_free_duel_completion_art.h"', "",
        "#include <stddef.h>", "",
        "typedef struct { uint8_t count, color; } BorderRun;",
        "typedef struct {", "    const uint32_t *palette;",
        "    const BorderRun *runs;", "    uint16_t run_count;",
        "} BorderFrame;", "",
    ]
    for frame, (palette, runs) in enumerate(frames):
        lines.append("static const uint32_t s_palette_%d[] = {" % frame)
        for at in range(0, len(palette), 8):
            values = []
            for r, g, b, a in palette[at:at + 8]:
                values.append("0x%02X%02X%02X%02Xu" % (a, r, g, b))
            lines.append("    " + ", ".join(values) + ",")
        lines.append("};")
        lines.append("static const BorderRun s_runs_%d[] = {" % frame)
        for at in range(0, len(runs), 12):
            values = ["{%d,%d}" % pair for pair in runs[at:at + 12]]
            lines.append("    " + ", ".join(values) + ",")
        lines += ["};", ""]
    lines.append("static const BorderFrame s_frames[PSX_FD_COMPLETION_FRAME_COUNT] = {")
    for frame, (_, runs) in enumerate(frames):
        lines.append("    { s_palette_%d, s_runs_%d, %d }," %
                     (frame, frame, len(runs)))
    lines += ["};", "", "void psx_free_duel_completion_art_frame(int frame, uint32_t *out)",
              "{", "    if (!out) return;",
              "    const BorderFrame *f = &s_frames[(unsigned)frame %",
              "                                      PSX_FD_COMPLETION_FRAME_COUNT];",
              "    size_t at = 0;",
              "    for (unsigned i = 0; i < f->run_count; i++)",
              "        for (unsigned n = 0; n < f->runs[i].count; n++)",
              "            out[at++] = f->palette[f->runs[i].color];",
              "    while (at < PSX_FD_COMPLETION_FRAME_SIZE * PSX_FD_COMPLETION_FRAME_SIZE)",
              "        out[at++] = 0;", "}", ""]
    output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print("wrote %s (%d exact frames)" % (output, len(frames)))


if __name__ == "__main__":
    main()
