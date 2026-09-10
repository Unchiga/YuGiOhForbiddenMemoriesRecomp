#!/usr/bin/env python3
"""Render the app icon set from assets/icons/puzzle-source.png.

Every icon this project ships is generated here, so the Windows .ico, the
macOS .icns, the Linux hicolor PNGs and the SDL window icon can never drift
apart: they are all the same drawing, scaled once per size.

    python3 tools/gen_app_icon.py            # write everything
    python3 tools/gen_app_icon.py --check    # fail if anything is stale

The art is pixel art, and its size decides how each icon can be made.

The source is 54x58. An icon can be built from it with no resampling at all
whenever the source fits at a whole multiple: draw it at k times size, center
it, and pad the rest with transparency. 58 is the taller side, so k =
floor(N / 58), which works from 64 up and nowhere below:

    64    k=1    54x58      128   k=2    108x116
    256   k=4    216x232    512   k=8    432x464
    1024  k=16   864x928

The art covers 58/64 of the canvas at every one of those, because the ladder
doubles and k doubles with it, so the object never appears to change size.

Below 64 there is no k at all: the drawing is bigger than the canvas. 32 and 48
are therefore scaled by majority vote, each destination pixel taking the most
common source color under it, to that same 58/64 share of their canvas so they
match the rest of the ladder. Averaging the pixels instead was tried and it
greys the gold and softens the outline into the background.

16 cannot be scaled at all: a reduction that far leaves the ring's hole, the
stepped band and the eye each fighting for the same pixel, and measured, the
result has no readable ring and no readable eye. That size is hand-drawn in
tools/draw_app_icon.py, in the source's own five colors.

20, 22, 24 and 40 are deliberately not shipped. No master can be a whole
multiple of those AND of the powers of two (48 and 64 first meet at 192), so
they could only ever be resampled twice over. Windows and the icon themes both
scale from the nearest size they find, which beats shipping mush.
"""

import argparse
import struct
import sys
from collections import Counter
from io import BytesIO
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ICONS = ROOT / "assets" / "icons"
PNG_DIR = ICONS / "png"
SOURCE = ICONS / "puzzle-source.png"
HAND16 = ICONS / "puzzle-16.png"

# The canvas the source is padded into, and the share of it the art covers.
# Every other size copies that share so the object never appears to resize.
BASE = 64
FILL = 58.0 / BASE

PNG_SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]

# Vista and later read PNG-compressed entries, but a BMP entry is what every
# older shell, and every .lnk thumbnailer, is guaranteed to understand. Small
# sizes are cheap as BMP (a 48px entry is 9 KB) so they stay BMP; the big ones
# would be far larger than their PNG and go in compressed.
ICO_SIZES = [16, 32, 48, 64, 128, 256]
ICO_PNG_FROM = 64

# (type, pixels). iconutil's mapping for an .iconset, in the order Finder walks
# it: 16pt, 16pt@2x, 32pt, 32pt@2x, and so on up to 512pt@2x. Every slot lands
# on a size this set has.
ICNS_ENTRIES = [
    (b"icp4", 16), (b"ic11", 32), (b"icp5", 32), (b"ic12", 64),
    (b"ic07", 128), (b"ic13", 256), (b"ic08", 256), (b"ic14", 512),
    (b"ic09", 512), (b"ic10", 1024),
]

DESKTOP_ID = "ygofm-recompiled"


def majority(src, w, h):
    """Scale by vote: each destination pixel takes the most common source
    color under it. Transparent wins only when it is a strict majority, so a
    one-pixel highlight with background on three sides is not eaten."""
    sw, sh = src.size
    px = src.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    o = out.load()
    for y in range(h):
        y0, y1 = int(y * sh / h), max(int(y * sh / h) + 1, -(-(y + 1) * sh // h))
        for x in range(w):
            x0, x1 = int(x * sw / w), max(int(x * sw / w) + 1, -(-(x + 1) * sw // w))
            seen = Counter()
            for sy in range(y0, min(y1, sh)):
                for sx in range(x0, min(x1, sw)):
                    seen[px[sx, sy]] += 1
            opaque = Counter({k: v for k, v in seen.items() if k[3]})
            if not opaque or seen[(0, 0, 0, 0)] > sum(opaque.values()):
                continue
            o[x, y] = opaque.most_common(1)[0][0]
    return out


def center(art, n):
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(art, ((n - art.size[0]) // 2, (n - art.size[1]) // 2))
    return out


class Renderer:
    def __init__(self, source, hand16):
        self.src = source
        self.hand16 = hand16
        self.base = center(source, BASE)   # 54x58 in a 64 canvas
        self.cache = {}

    def get(self, size):
        if size in self.cache:
            return self.cache[size]
        if size == 16:
            im = self.hand16
        elif size % BASE == 0:
            # Whole multiple: every source pixel becomes an exact square block.
            im = self.base.resize((size, size), Image.NEAREST)
        else:
            h = int(round(size * FILL))
            w = int(round(self.src.size[0] * h / float(self.src.size[1])))
            im = center(majority(self.src, w, h), size)
        self.cache[size] = im
        return im


def png_bytes(im):
    buf = BytesIO()
    # optimize, and no timestamp chunk, so re-running produces byte-identical
    # output and --check does not report churn that is not there.
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def svg_bytes(base):
    """The base master as one <rect> per run of same-colored pixels.

    A .desktop entry wants something in hicolor/scalable/apps, and for pixel
    art the honest scalable form is the pixels themselves: shape-rendering
    crispEdges keeps them square at any size instead of blurring them.
    """
    n = base.size[0]
    px = base.load()
    parts = []
    for y in range(n):
        x = 0
        while x < n:
            c = px[x, y]
            if c[3] == 0:
                x += 1
                continue
            run = 1
            while x + run < n and px[x + run, y] == c:
                run += 1
            parts.append('<rect x="%d" y="%d" width="%d" height="1" fill="#%02x%02x%02x"/>'
                         % (x, y, run, c[0], c[1], c[2]))
            x += run
    return (
        '<!--\n'
        '  Yu-Gi-Oh! Forbidden Memories Recompiled: the app icon, as vector.\n'
        '  Generated by tools/gen_app_icon.py from assets/icons/puzzle-source.png,\n'
        '  one rect per run of same-colored pixels. Do not edit: edit the source\n'
        '  art and re-run.\n'
        '-->\n'
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="512" height="512"\n'
        '     shape-rendering="crispEdges" role="img"\n'
        '     aria-label="Yu-Gi-Oh Forbidden Memories Recompiled">\n'
        '  %s\n</svg>\n' % (n, n, "\n  ".join(parts))
    ).encode()


def bmp_entry(im):
    """A 32-bit BITMAPINFOHEADER entry: bottom-up BGRA plus the AND mask.

    The mask is all zeroes (nothing masked out) because the alpha channel
    already carries the shape, but Windows still requires the bytes: leaving
    them off makes the entry a size Explorer skips without saying why.
    """
    w, h = im.size
    px = im.load()
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_stride = ((w + 31) // 32) * 4
    return header + b"".join(rows) + bytes(mask_stride * h)


def build_ico(renderer):
    entries = []
    for size in ICO_SIZES:
        im = renderer.get(size)
        entries.append((size, png_bytes(im) if size >= ICO_PNG_FROM else bmp_entry(im)))
    head = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    body = bytearray()
    for size, data in entries:
        dim = 0 if size >= 256 else size
        head += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    return bytes(head + body)


def build_icns(renderer):
    """PNG-payload .icns, the form macOS 10.7 and later read.

    Written here rather than shelled out to iconutil because iconutil is macOS
    only and this set has to be reproducible from the Linux box it is built on.
    """
    chunks = bytearray()
    for kind, pixels in ICNS_ENTRIES:
        data = png_bytes(renderer.get(pixels))
        chunks += kind + struct.pack(">I", len(data) + 8) + data
    return b"icns" + struct.pack(">I", len(chunks) + 8) + bytes(chunks)


def desktop_entry(exec_path, icon_name=DESKTOP_ID):
    return """[Desktop Entry]
Type=Application
Version=1.0
Name=Yu-Gi-Oh! Forbidden Memories Recompiled
GenericName=Card battle game
Comment=Yu-Gi-Oh! Forbidden Memories, recompiled to native code
Exec=%s
Icon=%s
Terminal=false
Categories=Game;StrategyGame;
Keywords=yugioh;forbidden memories;recompilation;psx;
StartupWMClass=Yu_Gi_Oh_Forbidden_Memories_Recompiled
""" % (exec_path, icon_name)


def write(path, data, check, stale):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    if path.exists() and path.read_bytes() == data:
        return
    if check:
        stale.append(path.relative_to(ROOT))
        return
    path.write_bytes(data)
    print("  %s" % path.relative_to(ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="report what would change instead of writing")
    args = ap.parse_args()
    stale = []

    source = Image.open(SOURCE).convert("RGBA")
    if source.size[0] > BASE or source.size[1] > BASE:
        sys.exit("%s is %dx%d, which does not fit the %d px base canvas"
                 % (SOURCE, source.size[0], source.size[1], BASE))
    hand16 = Image.open(HAND16).convert("RGBA")
    if hand16.size != (16, 16):
        sys.exit("%s is %dx%d, expected 16x16"
                 % (HAND16, hand16.size[0], hand16.size[1]))
    r = Renderer(source, hand16)

    for size in PNG_SIZES:
        write(PNG_DIR / ("ygofm-%d.png" % size), png_bytes(r.get(size)), args.check, stale)

    # The framework's own names. runtime.cmake feeds assets/psxrecomp.ico to
    # the Windows resource compiler and stages assets/psxrecomp.png beside the
    # exe for SDL_SetWindowIcon, both by exact filename, so the game's icon has
    # to land on those two names to be used at all.
    write(ROOT / "assets" / "psxrecomp.ico", build_ico(r), args.check, stale)
    write(ROOT / "assets" / "psxrecomp.png", png_bytes(r.get(512)), args.check, stale)
    write(ROOT / "assets" / "psxrecomp.svg", svg_bytes(r.base), args.check, stale)
    write(ICONS / "ygofm.svg", svg_bytes(r.base), args.check, stale)
    write(ICONS / "ygofm.icns", build_icns(r), args.check, stale)
    write(ICONS / (DESKTOP_ID + ".desktop"), desktop_entry("Play.sh"), args.check, stale)

    if args.check and stale:
        print("stale, re-run without --check:")
        for p in stale:
            print("  %s" % p)
        return 1
    if args.check:
        print("up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
