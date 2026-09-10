# The app icon

The Millennium Puzzle, as pixel art. `puzzle-source.png` is the drawing at its
native 54x58; almost everything else here is generated from it. It replaced the
framework's teal gamepad, which every psxrecomp title shipped with, so this
game now looks like itself in a taskbar.

## What is here

| | |
| --- | --- |
| `puzzle-source.png` | the art at its native 54x58 |
| `puzzle-16.png` | the 16 px size, hand-drawn, from `tools/draw_app_icon.py` |
| `png/ygofm-<n>.png` | 16, 32, 48, 64, 128, 256, 512, 1024 |
| `ygofm.svg` | the 64 master as one rect per run of pixels, for hicolor/scalable |
| `ygofm.icns` | macOS, ten slots from 16pt to 512pt@2x |
| `ygofm-recompiled.desktop` | the Linux menu entry (template: `Exec=Play.sh`) |

and three files outside this directory, all named by the framework rather than
by us: `assets/psxrecomp.ico` is what `runtime.cmake` hands the Windows
resource compiler, `assets/psxrecomp.png` is what the runtime loads at startup
for `SDL_SetWindowIcon` on all three platforms, and `assets/psxrecomp.svg` is
the same vector as `ygofm.svg`. Do not edit any of them by hand.

## How each size is made

The art is 54x58, and that number decides everything.

**64 and up cost nothing.** Draw the source at a whole multiple, center it, pad
the rest with transparency, and no pixel is ever resampled: k = floor(N / 58),
so 64 gets 1x, 128 gets 2x, 256 gets 4x, 512 gets 8x, 1024 gets 16x. The art
covers 58/64 of the canvas at every one of them, because the ladder doubles and
k doubles with it, so the object never appears to change size.

**32 and 48 have no k at all** (the drawing is bigger than the canvas), so they
are scaled by majority vote: each destination pixel takes the most common source
color under it, down to that same 58/64 share of the canvas. Averaging the
pixels instead greys the gold and softens the outline into the background.

**16 cannot be scaled.** That far down, the ring's hole, the stepped band and
the eye all want the same pixel; measured, the reduction has no readable ring
and no readable eye. So 16 is drawn by hand in `tools/draw_app_icon.py`, in the
source's own five colors, keeping only what carries the object: the ring with
its hole punched, the inverted triangle, and one dark eye band with a gold
center. It drops the white keyline (a pixel of white all round is six percent
of the width at that size) and leans gold rather than cream (cream over four
pixels reads as plain white).

**20, 22, 24 and 40 are deliberately absent.** No master can be a whole multiple
of those and of the powers of two as well (48 and 64 first meet at 192), so they
could only be resampled twice over. Windows and the icon themes scale from the
nearest size they find, which beats shipping mush.

## Regenerating

```sh
python3 tools/draw_app_icon.py --show 12   # redraw 16, plus a preview in /tmp
python3 tools/gen_app_icon.py              # rewrite every output
python3 tools/gen_app_icon.py --check      # non-zero if any output is stale
```

Pure Pillow, no rasterizer needed. Edit the source art or the 16 px script,
never the outputs.

## Installing on Linux

```sh
tools/install_desktop_entry.sh             # ~/.local/share, no root
tools/install_desktop_entry.sh --uninstall
```

That copies the hicolor sizes and writes the .desktop with this checkout's
absolute `Play.sh` in `Exec=`.
