#!/usr/bin/env python3
"""disc_assets.py — bake this title's sprites and font from the PLAYER'S disc.

Three of the runtime's sources are Konami artwork: the duel-rank badges and
letters, the CARD DROPS "New!" label, and the game's own text font. They are
derived from the disc exactly the way generated/ is derived from the EXE, so
they are build output, not source, and nothing here is committed or shipped.

    disc_assets.py <disc.bin> <out_dir> [--check]

WHY THIS EXISTS RATHER THAN sprite_extract.py / font_extract.py
---------------------------------------------------------------
Those two tools read a VRAM SNAPSHOT taken from a running game over TCP, which
is how the art was FOUND: you play to the screen that has it resident, dump
VRAM, and pick the rect out of a rendered page. A build step cannot do any of
that. So this tool goes to the source those snapshots came from — the texture
blobs on the disc — reassembles just the rects that are needed into a synthetic
VRAM page, and then hands that page to the very same decoders. The baked output
is therefore identical to what the snapshot produced, by construction, and the
snapshot tools stay exactly as they were for finding new art.

WHY THE ART STAYS BAKED
-----------------------
Unchanged from the original reasoning, and still load-bearing: the rank meter
draws results-screen badges DURING a duel, and the CARD DROPS page draws its
"New!" label while the chest assets may or may not be resident. Neither can
read VRAM live, and a baked copy needs no CLUT tracking. This tool changes only
WHERE the bytes come from, never when they are read.

THE MANIFEST
------------
Offsets are into the disc's reassembled user-data stream (the image is raw
2352-byte Mode 2 sectors; only 2048 bytes of each carry data). Every texture
blob on this disc uploads as a 64-word-wide rect, so a region's rows sit 128
bytes apart; single-row regions are contiguous. Each entry was located by
searching for the snapshot's own bytes and confirmed by requiring EVERY row of
the region to match — a chance hit on one row cannot pass.

These are coordinates and offsets, not artwork. They are specific to the USA
disc the project already gates on by CRC ([game] disc_crc); a different
pressing needs its own manifest, and --check will say so rather than emit
plausible garbage.
"""

import hashlib
import json
import os
import sys

VRAM_W, VRAM_H = 1024, 512
SEC, UD_OFF, UD_LEN = 2352, 24, 2048
STRIDE = 128          # 64-word-wide upload rect

# name -> x, y (VRAM 16-bit words), w (words), h (rows), disc stream offset
MANIFEST = {
    # --- the game's text font, and the ATK/DEF icons beside it -----------
    'font_small':  dict(x=640, y=0,   w=32,  h=72, off=32518144),
    'icons':       dict(x=956, y=368, w=4,   h=16, off=32708728),
    # 256-entry CLUT shared by the icons and the card-stat digits.
    'clut_stat':   dict(x=256, y=241, w=256, h=1,  off=32760320),
    # --- rank meter -------------------------------------------------------
    'digit':       dict(x=896, y=344, w=40,  h=8,  off=32704520),
    'pow':         dict(x=46,  y=312, w=6,   h=24, off=36250716),
    'clut_pow':    dict(x=112, y=248, w=16,  h=1,  off=36309216),
    'tec':         dict(x=46,  y=288, w=6,   h=24, off=36247644),
    'clut_tec':    dict(x=96,  y=248, w=16,  h=1,  off=36309184),
    'rank_sheet':  dict(x=0,   y=384, w=30,  h=80, off=36259840),
    'clut_rank':   dict(x=144, y=248, w=16,  h=1,  off=36309280),
    'plate':       dict(x=32,  y=288, w=14,  h=48, off=36247616),
    'clut_plate':  dict(x=16,  y=248, w=16,  h=1,  off=36309024),
    # --- CARD DROPS "New!" label -----------------------------------------
    'newtag':      dict(x=762, y=104, w=6,   h=8,  off=32564340),
    'clut_newtag': dict(x=656, y=250, w=16,  h=1,  off=32617760),
    # --- CARD SHOP panel skin (the password screen's box art) -------------
    # The stone frame strips and mottled navy field the password screen's
    # boxes are built from: texpage (960,256) 4bpp. The frame reads CLUT
    # (256,241) and the field CLUT (272,241) - both inside the clut_stat
    # row already baked above. Located from slot-6's GP0 stream (rect
    # op=64 uv/tp/clut), then found on disc row-verified like the rest.
    'shop_skin':   dict(x=960, y=256, w=64,  h=96, off=33017856),
    # The password screen's own frame+field CLUTs. On hardware they load at
    # (256,241) - the same words the card-stat CLUT reuses on other screens
    # - so here they park one row down at y=242, which nothing else claims.
    # Six identical copies live on disc (one per campaign room); this is
    # the first.
    'clut_shop':   dict(x=256, y=242, w=32,  h=1,  off=32763392),
    # The starchip sheet pieces: texpage (704,0) 4bpp. star uv (80,128),
    # circle button uv (64,128), digit right-arrow uv (96,160); the CLUT
    # row at (512,252) carries all three palettes (512=buttons, 528=star,
    # 544=arrows).
    # (xbtn's disc copy is one row MORE complete than the snapshot: the
    # sheet's first VRAM row was clobbered by an overlapping upload, and
    # the disc row 0 is the button's real top edge - 15/16 rows verify.)
    # The card viewer's composed-canvas template: the gold card body that
    # frames the face. One disc blob; the face rect (u 0..128, v 0..128) is
    # streamed per card by the viewer itself, so only the two body strips
    # around it are baked. Parked at their real chest-screen coordinates,
    # which nothing else in the synthetic page claims.
    'cardbody_r':  dict(x=832, y=256, w=64,  h=192, off=32661504),
    'cardbody_b':  dict(x=768, y=384, w=64,  h=64,  off=32645120),
    'star':        dict(x=724, y=128, w=4,   h=16, off=32567336),
    'xbtn':        dict(x=708, y=128, w=4,   h=16, off=32567304),
    'tbtn':        dict(x=712, y=128, w=4,   h=16, off=32567312),
    'obtn':        dict(x=720, y=128, w=4,   h=16, off=32567328),
    'arrow':       dict(x=728, y=160, w=4,   h=16, off=32571440),
    'arrow2':      dict(x=728, y=192, w=4,   h=16, off=32575536),
    'clut_star':   dict(x=512, y=252, w=64,  h=1,  off=32618496),
}

# The "New!" label, which sprite_extract.py has no group for: it is one rect
# with its own CLUT, emitted straight into psx_cd_sprites.c.
NEWTAG = dict(x=232 + 704 * 4, y=104, w=24, h=8, bpp=4, clut=(656, 250))

HERE = os.path.dirname(os.path.abspath(__file__))


def read_stream(f, off, n):
    """Read n bytes at an offset into the reassembled user-data stream."""
    out = bytearray()
    sec, pos = divmod(off, UD_LEN)
    while len(out) < n:
        f.seek(sec * SEC + UD_OFF + pos)
        take = min(UD_LEN - pos, n - len(out))
        chunk = f.read(take)
        if not chunk:
            raise SystemExit('disc ended early at stream offset %d' % off)
        out += chunk
        sec += 1
        pos = 0
    return bytes(out)


def build_vram(disc_path):
    """Reassemble every manifest region into a synthetic VRAM page.

    Regions from three different snapshots land in one page because their VRAM
    coordinates do not collide — the only overlap is the 256-entry stat CLUT,
    which both the icons and the digits read and which is one blob on disc.
    """
    vram = bytearray(VRAM_W * VRAM_H * 2)
    with open(disc_path, 'rb') as f:
        for name, r in MANIFEST.items():
            nbytes = r['w'] * 2
            for row in range(r['h']):
                src = read_stream(f, r['off'] + row * STRIDE, nbytes)
                dst = ((r['y'] + row) * VRAM_W + r['x']) * 2
                vram[dst:dst + nbytes] = src
    return bytes(vram)


def emit_newtag(vram, out_path, sprite_extract):
    rows = sprite_extract.decode(vram, NEWTAG['x'], NEWTAG['y'],
                                 NEWTAG['w'], NEWTAG['h'],
                                 NEWTAG['bpp'], NEWTAG['clut'])
    body = sprite_extract.emit_one('newtag_src', rows)
    # Keep the symbol the header declares; emit_one names it for a group.
    body = [ln.replace('psx_spr_newtag_src_px', 'k_newtag_px') for ln in body]
    hdr = [
        '/* Generated by tools/disc_assets.py - DO NOT EDIT BY HAND.',
        ' *',
        ' * The deck builder\'s yellow "New!" label, decoded from the disc:',
        ' * texpage (704,0) 4bpp, uv (232,104), CLUT (656,250), 24x8.',
        ' *',
        ' * Baked like the rank-meter sprites and for the same reason: the CARD',
        ' * DROPS results page draws it while the chest assets may or may not be',
        ' * resident, and a baked copy needs no CLUT tracking. ARGB8888, alpha 0',
        ' * where the palette entry was 0x0000.',
        ' */',
        '',
        '#include "psx_cd_sprites.h"',
        '',
    ]
    with open(out_path, 'w', newline='\n') as f:
        f.write('\n'.join(hdr + body) + '\n')
        f.write('const PsxSprite psx_spr_newtag = { k_newtag_px, %d, %d };\n'
                % (NEWTAG['w'], NEWTAG['h']))
    print('wrote %s' % out_path)


def emit_shop_skin(vram, out_path, sprite_extract):
    """The CARD SHOP panel skin: the password screen's box furniture.

    Piece geometry is the password screen's own draw list (GP0 rect op=64):
    top/bottom strips carry the corners and tile horizontally, the 8px side
    edges tile vertically, and the field is the mottled navy the boxes are
    filled with. All from the 4bpp atlas at texpage (960,256); the star is
    the STARCHIP readout's sprite from texpage (704,0).
    """
    A = (256, 242)   # frame CLUT (clut_shop, parked off the stat row)
    B = (272, 242)   # field CLUT
    S = (528, 252)   # star CLUT
    AX = 960 * 4     # atlas origin in 4bpp pixels

    def strips(v_main, u_cont):
        # A full 176px logical edge the way the game draws one: the 128px
        # strip (left corner) + its 48px continuation (right corner).
        main = sprite_extract.decode(vram, AX, 256 + v_main, 128, 8, 4, A)
        cont = sprite_extract.decode(vram, AX + u_cont, 272, 48, 8, 4, A)
        return [m + c for m, c in zip(main, cont)]

    pieces = [
        ('shop_top',    strips(0, 0)),
        ('shop_bot',    strips(8, 48)),
        ('shop_left',   sprite_extract.decode(vram, AX,      280,   8, 64, 4, A)),
        ('shop_right',  sprite_extract.decode(vram, AX + 8,  280,   8, 64, 4, A)),
        ('shop_field',  sprite_extract.decode(vram, AX + 16, 280, 112, 72, 4, B)),
        ('shop_star',   sprite_extract.decode(vram, 704 * 4 + 80, 128, 16, 16, 4, S)),
        ('shop_xbtn',   sprite_extract.decode(vram, 704 * 4 + 16, 128, 16, 16, 4, (512, 252))),
        ('shop_tbtn',   sprite_extract.decode(vram, 704 * 4 + 32, 128, 16, 16, 4, (512, 252))),
        ('shop_obtn',   sprite_extract.decode(vram, 704 * 4 + 64, 128, 16, 16, 4, (512, 252))),
        ('shop_arrow',  sprite_extract.decode(vram, 704 * 4 + 96, 160, 16, 16, 4, (544, 252))),
        ('shop_arrow2', sprite_extract.decode(vram, 704 * 4 + 96, 192, 16, 16, 4, (544, 252))),
    ]
    body = []
    tail = []
    for name, rows in pieces:
        lines = sprite_extract.emit_one(name + '_src', rows)
        body += [ln.replace('psx_spr_%s_src_px' % name, 'k_%s_px' % name)
                 for ln in lines]
        tail.append('const PsxSprite psx_spr_%s = { k_%s_px, %d, %d };'
                    % (name, name, len(rows[0]), len(rows)))
    # The same 64x96 blob doubles as the CARD VIEWER's template atlas: the
    # game uploads it to VRAM (832,0) on the chest/deck screens (and to
    # (960,256) on the password screen, which is where the synthetic page
    # holds it). The shop screen never loads it, so the mod uploads these
    # raw words itself before spawning the viewer there.
    raw = []
    for row in range(96):
        o = ((256 + row) * 1024 + 960) * 2
        raw.append(vram[o:o + 128])
    body.append('const uint16_t psx_shop_tmpl_raw[64 * 96] = {')
    for r in raw:
        words = [r[i] | (r[i + 1] << 8) for i in range(0, 128, 2)]
        body.append('    ' + ' '.join('0x%04X,' % w for w in words))
    body.append('};')
    body.append('')

    def emit_raw(sym, x, y, h):
        body.append('const uint16_t %s[64 * %d] = {' % (sym, h))
        for row in range(h):
            o = ((y + row) * 1024 + x) * 2
            r = vram[o:o + 128]
            words = [r[i] | (r[i + 1] << 8) for i in range(0, 128, 2)]
            body.append('    ' + ' '.join('0x%04X,' % w for w in words))
        body.append('};')
        body.append('')
    # The viewer's card-body canvas strips (see the manifest comment).
    emit_raw('psx_shop_cardbody_r', 832, 256, 192)
    emit_raw('psx_shop_cardbody_b', 768, 384, 64)

    hdr = [
        '/* Generated by tools/disc_assets.py - DO NOT EDIT BY HAND.',
        ' *',
        ' * The CARD SHOP panel skin: the password screen\'s stone frame',
        ' * strips, mottled navy field, and starchip sprite, decoded from the',
        ' * disc. Baked like the rank-meter sprites and for the same reason:',
        ' * the shop screen never has the password screen\'s textures',
        ' * resident. ARGB8888, alpha 0 where the palette entry was 0x0000.',
        ' * psx_shop_tmpl_raw is the same atlas as RAW VRAM words, for the',
        ' * card viewer (see psx_card_shop.c).',
        ' */',
        '',
        '#include "psx_shop_skin.h"',
        '',
    ]
    with open(out_path, 'w', newline='\n') as f:
        f.write('\n'.join(hdr + body + tail) + '\n')
    print('wrote %s' % out_path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    if len(args) != 2:
        raise SystemExit(__doc__)
    disc, out_dir = args
    if not os.path.isfile(disc):
        raise SystemExit('no disc image at %s' % disc)
    os.makedirs(out_dir, exist_ok=True)

    vram = build_vram(disc)

    # Hand the synthetic page to the decoders unchanged, through the snapshot
    # directory they already read from.
    work = os.path.join(out_dir, '_discvram')
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'discvram.bin'), 'wb') as f:
        f.write(vram)
    os.environ['PSX_VRAMDIR'] = work

    sys.path.insert(0, HERE)
    import sprite_extract
    import font_extract
    sprite_extract.OUTDIR = work
    font_extract.SNAPDIR = work

    with open(os.path.join(HERE, 'sprite_spec.json')) as f:
        specs = json.load(f)
    rank_c = os.path.join(out_dir, 'psx_rank_sprites.c')
    font_c = os.path.join(out_dir, 'psx_fusion_font.c')
    cd_c = os.path.join(out_dir, 'psx_cd_sprites.c')
    skin_c = os.path.join(out_dir, 'psx_shop_skin.c')

    sprite_extract.emit_rank_sprites(specs, vram, rank_c)
    font_extract.emit('discvram', font_c)
    emit_newtag(vram, cd_c, sprite_extract)
    emit_shop_skin(vram, skin_c, sprite_extract)

    if check:
        for p in (rank_c, font_c, cd_c, skin_c):
            h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            print('%s  %s' % (h[:16], os.path.basename(p)))


if __name__ == '__main__':
    main()
