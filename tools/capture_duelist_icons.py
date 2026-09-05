#!/usr/bin/env python3
"""capture_duelist_icons.py — collect FREE DUEL portraits from a running game.

    tools/goto_freeduel.py            # cold boot to the opponent grid
    tools/capture_duelist_icons.py assets/duelist_icons

Writes `duelist_NN.png`, NN being the duelist index the drop tables use, which
tools/gen_duelist_icons.py then bakes. Re-run it as often as you like: existing
files are kept, so several passes accumulate.

WHY THIS AND NOT THE DISC (historical)
--------------------------------------
This was written believing the portrait atlas was compressed on disc. It is
not: the forty 48x48 tiles sit plain at WA_MRG.MRG offset 0xF55000 (disc LBA
17952), 2432 bytes each, in grid order = drop-table order + 1, and the recomp
now decodes them at runtime (src/psx_duelist_portraits.c). The search that
"found nothing" looked for the 8bpp VRAM layout the game uploads to, which is
column-interleaved and never appears on disc as such. This capture remains as
the fallback path and as the way the reference set was first built.

THE TWO OBSERVATIONS THAT MAKE IT RELIABLE
------------------------------------------
* **`0x8009B32E` holds the highlighted duelist's index + 41.** Fitted on five
  names read off the name bar and confirmed on a sixth. Byte 40 means the Build
  Deck button, which occupies the grid's first cell and is not a duelist. This
  is what makes the identity READ rather than counted — and counting does not
  work, because the free-duel list is NOT in drop-table order.
* **The cursor's border animates**, so two screenshots taken at rest differ in
  exactly one place: the highlighted cell. That locates the portrait to crop.

Between them, every stop yields one (duelist, portrait) pair that is correct by
construction rather than by assumption.

WHAT THIS SCREEN DOES THAT WILL WASTE YOUR TIME
-----------------------------------------------
* **Short presses do nothing.** A 6-frame hold changes nothing at all while
  `pad_probe` shows the guest reading it and the frame counter advances — it
  reads exactly like a soft-lock and is not one. 40 frames works.
* **Read the index by POLLING for a change, not once.** A single read after a
  press sees the stale value, the retry fires, and the cursor moves two or
  three cells at a time. That is why an early version only ever reached the
  grid's four corners.
* **The list scrolls vertically.** At the bottom row DOWN brings in a new row
  instead of moving the cursor; RIGHT at the right edge does nothing.
* Duelists the player has never beaten are absent from the list, so their
  index simply never appears. That is not a failure — gen_duelist_icons.py
  emits NULL and the viewer draws a plain plate.
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'psxrecomp', 'tools'))
import debug_client
from PIL import Image, ImageChops

HOST, PORT = '127.0.0.1', 4370
TMP = os.environ.get('TEMP', '.')
IDX_ADDR, IDX_BIAS = 0x8009B32E, 41
RIGHT, DOWN, LEFT, UP = 0x0020, 0x0040, 0x0080, 0x0010

# Grid geometry in presented pixels. slot_of's constants only have to CLASSIFY
# a cursor rect into one of 15 slots, so they tolerate slack; the page-crop
# steps do not — they extrapolate cells away from the anchor, and a wrong X
# pitch is exactly how a batch of portraits came out shifted a growing amount
# per column. X and Y pitch DIFFER (337.875 vs 312, measured 2026-08-21 from
# cursor rects four columns / two rows apart in one window).
GRID_X0, GRID_Y0, GRID_STEP = 211.0, 316.0, 313.5
X_STEP, Y_STEP = 337.875, 312.0
SCALE, TILE = 6, 38          # 38, not 44: 44 bakes in the selection border


def q(c):
    return debug_client.query(HOST, PORT, c)


def idx():
    b = int(bytes.fromhex(q({'cmd': 'read_ram',
                             'addr': '0x%08X' % IDX_ADDR, 'len': 1})['hex'])[0])
    return b - IDX_BIAS


def shot(n):
    p = os.path.join(TMP, 'ygofm_%s.png' % n)
    q({'cmd': 'screenshot_present', 'path': p.replace('\\', '/')})
    time.sleep(0.9)
    return Image.open(p).convert('RGB')


def slot_of(bb):
    col = int(round((bb[0] - GRID_X0) / GRID_STEP))
    row = int(round((bb[1] - GRID_Y0) / GRID_STEP))
    return row * 5 + col if 0 <= col < 5 and 0 <= row < 3 else None


def cursor_box():
    """Where the cursor is, from the border's animation.

    Retried: the animation has phases where two samples taken a second apart
    look identical, and a single try then reports "no cursor" and silently
    skips that duelist. About half of them were being lost that way.
    """
    for _ in range(4):
        bb = ImageChops.difference(shot('a'), shot('b')).getbbox()
        if bb and slot_of(bb) is not None:
            return bb
        time.sleep(0.5)
    return None


def press(mask, frames=40):
    q({'cmd': 'press', 'buttons': 0xFFFF & ~mask, 'frames': frames})
    time.sleep(0.9)
    q({'cmd': 'clear_input'})
    time.sleep(2.0)


def step(mask, tries=15, gap=0.3):
    """One CONFIRMED move. Polls for the index to change; see the docstring.

    A single press sometimes simply does not land on this screen — the same
    hold that moves the cursor nine times in a row is ignored the tenth — so
    treat one failed press as "try again", never as an edge; the WALK decides
    an edge from two consecutive failures. After a confirmed change a further
    settle lets the scroll animation finish before the cursor-diff screenshots
    run (a mid-scroll frame has misaligned rows and diffs everywhere)."""
    before = idx()
    press(mask)
    for _ in range(tries):
        if idx() != before:
            time.sleep(0.8)
            return True
        time.sleep(gap)
    return False


def crop_at(im, cx, cy):
    half = TILE * SCALE // 2
    box = (int(cx - half), int(cy - half), int(cx + half), int(cy + half))
    if box[0] < 0 or box[1] < 0 or box[2] > im.width or box[3] > im.height:
        return None
    return im.crop(box).resize((TILE, TILE), Image.BOX)


def crop(im, bb):
    return crop_at(im, (bb[0] + bb[2]) // 2, (bb[1] + bb[3]) // 2)


def occupied(im):
    """A portrait, or the stone behind a HOLE? A never-met duelist leaves an
    empty cell, and saving the wall as a face would bake a wrong icon.
    Saturation is the WRONG discriminator — Nitemare's near-monochrome skull
    measures 6.6, BELOW the stone's 7.9, and any threshold files one of them
    wrongly. Contrast separates them cleanly: the wall is flat (luminance
    std ~7) while every portrait, the skull included, is drawn art (std 54+)."""
    px = list(im.getdata())
    n = float(len(px))
    lum = [sum(p) / 3.0 for p in px]
    mean = sum(lum) / n
    var = sum((v - mean) ** 2 for v in lum) / n
    return var ** 0.5 > 20.0


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'assets/duelist_icons'
    os.makedirs(out, exist_ok=True)
    have = {int(n[8:10]) for n in os.listdir(out)
            if n.startswith('duelist_') and n.endswith('.png')}
    print('already have %d' % len(have), flush=True)

    # THE GRID IS THE DROP-TABLE ORDER. Cell 0 is Build Deck, duelist k sits
    # at cell k+1, five cells per row, and a duelist the save never even MET
    # leaves a hole. The cursor normally skips locked (unbeaten) cells — which
    # is what made a cell-counting traversal conclude the list was "not in
    # drop order", and why a cursor walk alone reaches only beaten duelists.
    #
    # The intended run therefore UNLOCKS the roster first: from the door menu
    # (Circle backs out of the grid to it), patch the roster-build loop's lock
    # branch — poke_code 801683D4 = 0805A0F8, the RAM form of the published
    # "all free duel opponents" GameShark code — and re-enter FREE DUEL so the
    # screen initializes with nobody locked. Then every cell is visited by
    # cursor and captured from its border diff, which centers exactly. Run
    # WITHOUT the unlock it still works, just only for beaten duelists, with
    # the geometry fallback (anchor + measured X/Y pitch) covering the rest
    # less precisely.
    def record():
        i = idx()
        if i < 0 or i in have:
            return False
        bb = cursor_box()
        if not bb:
            print('  (no cursor fix at %d)' % i, flush=True)
            return False
        im = crop(shot('c'), bb)
        if not im:
            return False
        im.save(os.path.join(out, 'duelist_%02d.png' % i))
        have.add(i)
        print('  duelist %2d' % i, flush=True)
        return True

    def page_capture():
        i = idx()
        if i < 0:
            return
        bb = cursor_box()
        if not bb:
            return
        slot = slot_of(bb)
        if slot is None:
            return
        vr, vc = slot // 5, slot % 5
        scroll = (i + 1) // 5 - vr          # global row of the top visible row
        cx0 = (bb[0] + bb[2]) / 2.0 - vc * X_STEP
        cy0 = (bb[1] + bb[3]) / 2.0 - vr * Y_STEP
        im = shot('page')
        for r in range(3):
            for c in range(5):
                d = (scroll + r) * 5 + c - 1
                if d < 0 or d > 38 or d in have:
                    continue
                cell = crop_at(im, cx0 + c * X_STEP, cy0 + r * Y_STEP)
                if not cell:
                    continue
                if not occupied(cell):
                    print('  duelist %2d: empty cell (never met)' % d,
                          flush=True)
                    continue
                cell.save(os.path.join(out, 'duelist_%02d.png' % d))
                have.add(d)
                print('  duelist %2d (page anchor %d)' % (d, i), flush=True)

    def move(mask):
        return step(mask) or step(mask)     # single presses fail spuriously

    def sweep(direction):
        while move(direction):
            record()

    # Snake the grid: climb to the top-left, then sweep each row, dropping a
    # row at either edge. With the roster unlocked the cursor stops on every
    # cell and each stop records from its own border diff. Where the cursor
    # cannot go (locked cells on a normal save), the page capture at each
    # anchor stop picks up everything visible around it — the anchor windows
    # of the top and bottom stops alone tile the whole eight-row grid.
    stuck = 0
    while stuck < 2:
        if move(UP):
            stuck = 0
        else:
            stuck += 1
    sweep(LEFT)
    if idx() < 0:
        move(RIGHT)                          # off the Build Deck cell
    record()
    page_capture()
    sweep(RIGHT)
    going = LEFT
    while len(have) < 39:
        if not move(DOWN):
            break
        record()
        page_capture()
        sweep(going)
        going = RIGHT if going == LEFT else LEFT
    print('have %d duelists: %s' % (len(have), sorted(have)))


if __name__ == '__main__':
    main()
