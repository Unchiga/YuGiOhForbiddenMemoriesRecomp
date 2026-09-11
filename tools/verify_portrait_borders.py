#!/usr/bin/env python3
"""Prove the committed palette/run bake equals all eight source PNGs."""

import hashlib
from pathlib import Path
import re
import sys

from PIL import Image


EXPECTED_SHA256 = (
    "6bc5e56afa05e8071bee113796ca350a3493ed203e9cdd7e67813a03e3b02e1d",
    "598fb71777164a1c2d0f729e5f9f5bbd92e6fa832e69d5c4c00c0dac8d220ceb",
    "f492e2d4b5356bb5afae6f2f8c0f55191f7b9bf610255e52171b03dedd973aba",
    "eb0f2fb2df5a84369a0dbd5231ceda248c2d00a7373cbcbb972fb7df4128d26c",
    "d16688db94f033e8544ec5a775f055c4b38f5df30d7c1722f1ac0f50cfba75bd",
    "84f9a335d2e1c2a80142320570800ddca931f840eb855bc73781f00bb441fed9",
    "8ba00278b04cb3a7164c32ac6df0b16ed26dadf50ee4ee75c19cb71193c95d4d",
    "1b0f7411d5dbb27ced74a1d838d52c51de4ccc6cf6dfd47f10cae934b7a8e66e",
)


def section(source, declaration):
    try:
        return source.split(declaration + " {", 1)[1].split("};", 1)[0]
    except IndexError as error:
        raise SystemExit("missing generated section: %s" % declaration) from error


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_portrait_borders.py <png-directory>")
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/psx_free_duel_completion_art.c").read_text(
        encoding="utf-8")
    directory = Path(sys.argv[1])
    for frame, expected_hash in enumerate(EXPECTED_SHA256):
        path = directory / ("portraitBorder%d.png" % (frame + 1))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected_hash:
            raise SystemExit("%s SHA-256 changed: %s" % (path, digest))

        palette_text = section(
            source, "static const uint32_t s_palette_%d[] =" % frame)
        runs_text = section(
            source, "static const BorderRun s_runs_%d[] =" % frame)
        palette = [int(value, 16) for value in
                   re.findall(r"0x([0-9A-Fa-f]{8})u", palette_text)]
        runs = [tuple(map(int, pair)) for pair in
                re.findall(r"\{(\d+),(\d+)\}", runs_text)]
        decoded = []
        for length, color in runs:
            decoded.extend([palette[color]] * length)

        image = Image.open(path).convert("RGBA")
        get_pixels = getattr(image, "get_flattened_data", image.getdata)
        expected = [(a << 24) | (r << 16) | (g << 8) | b
                    for r, g, b, a in get_pixels()]
        if decoded != expected:
            raise SystemExit("frame %d C bake differs from %s" %
                             (frame + 1, path))
        print("frame %d exact: %d pixels, sha256 %s" %
              (frame + 1, len(decoded), digest))


if __name__ == "__main__":
    main()
