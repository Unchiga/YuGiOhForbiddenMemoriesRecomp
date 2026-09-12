#!/usr/bin/env python3
"""The 16 px app icon, drawn by hand.

Every other size comes from assets/icons/puzzle-source.png by scaling (see
tools/gen_app_icon.py). 16 cannot: the source is 54x58, and reducing it that
far leaves the ring's hole, the stepped band and the eye each fighting for the
same pixel. Measured, a 4:1 block reduction of the source at 16 px produces a
shape with no readable ring and no readable eye. So this size is redrawn from
nothing, in the source's own five colors, keeping the three things that carry
the object: the ring with its hole punched, the inverted triangle, and one dark
eye band with a gold center.

    python3 tools/draw_app_icon.py --show 12
"""

import sys
from pathlib import Path

from PIL import Image

# The source art's palette, exactly.
PAL = {
    '.': (0, 0, 0, 0),
    'K': (0x00, 0x00, 0x00, 255),   # outline
    'W': (0xfc, 0xfc, 0xfc, 255),   # keyline (unused at 16: see below)
    'C': (0xfd, 0xfd, 0xcd, 255),   # cream
    'G': (0xef, 0xc4, 0x43, 255),   # gold
    'B': (0x9d, 0x4e, 0x15, 255),   # brown
}

# No white keyline here. One pixel of white all the way round is six percent of
# the width at this size and swallows the silhouette; the source can afford it
# at 54 px, this cannot. The body also leans gold rather than the source's
# cream, because cream over four pixels reads as plain white.
ICON16 = [
    "......KKKK......",
    ".....KGGGGK.....",
    ".....KG..GK.....",
    ".....KGGGGK.....",
    "......KGGK......",
    "KKKKKKKKKKKKKKKK",
    ".KCCCCGGGGGCCBK.",
    "..KCKKKKKKKKCK..",
    "...KCKGGGGKCK...",
    "....KCKKKKBK....",
    ".....KCGGBK.....",
    ".....KCGGBK.....",
    "......KGBK......",
    "......KGBK......",
    ".......KK.......",
    "................",
]


def render(rows=ICON16):
    n = len(rows)
    for i, r in enumerate(rows):
        if len(r) != n:
            sys.exit("row %d is %d characters, expected %d" % (i, len(r), n))
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    p = im.load()
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            p[x, y] = PAL[ch]
    return im


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--show", type=int, metavar="SCALE",
                    help="also write a magnified preview to /tmp for eyeballing")
    args = ap.parse_args()
    im = render()
    out = Path(__file__).resolve().parent.parent / "assets" / "icons" / "puzzle-16.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    im.save(out, optimize=True)
    print(out)
    if args.show:
        p = "/tmp/puzzle-16-%dx.png" % args.show
        im.resize((16 * args.show, 16 * args.show), Image.NEAREST).save(p)
        print(p)


if __name__ == "__main__":
    main()
