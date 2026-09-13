#!/usr/bin/env python3
"""model_extract.py -- pull the game's 3D models off the player's disc.

    model_extract.py <disc.cue|.bin|.iso> <out_dir> [what...] [options]

  what:   card ids (1, 5, 1-30), 'monsters' (all 621), 'exodia' (the assembled
          Exodia), 'field' (the duel mat plus its seven floor textures),
          'scenes' (the seven duel backdrops in SU.MRG), or 'all'.
  --formats gltf,obj,json,png,raw    what to write (default: all of them)
  --preview                          also render a quick PNG of the bind pose

Everything here is derived from the disc the way generated/ is derived from the
EXE, so the output is build output, not source: nothing it writes is committed.

WHERE THE MODELS LIVE (all offsets measured on the USA disc, SLUS-01411)
------------------------------------------------------------------------
  DATA/MODEL.MRG  (LBA 28534)  621 blocks of 276 sectors, one per monster
                  card.  Block m = card - 1, minus 50 for every non-monster
                  range below the card (301..350, 651..700) and minus 1 for
                  card 722 (721 is a ritual).  That is exactly the arithmetic
                  Model_LoadMonsterMerge (0x80056504) does before streaming
                  sector 276*m of the file, and it accounts for all 621 blocks.
  DATA/SU.MRG     (LBA 954)    seven 116-sector backdrop packages at sector
                  136 + 116*k (the duel's 3D scene per terrain), then the
                  assembled Exodia at sector 948 (275 sectors), which the duel
                  loads for the fake card id 0x309 when all five pieces meet.
  DATA/WA_MRG.MRG (LBA 10102)  the duel mat: sector 198 of every 235-sector
                  terrain package (package t starts at sector 0x16C6 + 235*t);
                  identical in all seven, only the textures (package sectors
                  203..234, two texture columns) differ.

A MONSTER BLOCK (276 sectors), as func_80056D7C's 17 load stages route it:
  0..95    the model: PsyQ HMD data (see below)             -> RAM
  96..143  texture, 8bpp, three 128-px columns, column-major -> VRAM (x=0,y=256)
  144..145 8 palette rows of 256 entries                    -> VRAM (x=0,y=248)
  146      one extra 256-entry palette                      -> VRAM (512,242)
  147..162 texture column 3 (effects), loaded to RAM first
  163      duplicate of 146
  164..179 duplicate of 147..162 (used when the loader's 'alt' flag is set)
  180..189 attack-effect code module, linked for slot 0 (0x8013B000)
  190..199 the same module linked for slot 1 (0x8017B000)
  200..219 the 'alt' pair of effect modules
  220..223 effect data, one 2-sector copy per slot
  224      a u16 id list (purpose not established)
  225..274 SPU ADPCM: the monster's cries, 0x19000 bytes -> SPU RAM 0xD810
  275      parameters: 0x40 bytes of sound cues, ATK/DEF and stats at +0x100

THE HMD VARIANT
---------------
Konami exported PsyQ HMD ("hierarchical model data") but wrote the file size
where the 0x50 magic normally sits, and stripped the image primitives (the
loader streams textures itself).  Everything else is stock libgs: GsMapUnit,
GsSortUnit and GsU_03000000 (the sequence-descriptor animation driver, whose
assert still names S3000000.C) are all in the EXE and were read to get this
layout.  All pointers are long-word offsets from the top of the model.
  +0   file size  +4 map flag  +8 primitive-header section  +12 block count
  +16  block table: block[0] carries the animation chain, block[1+i] belongs
       to coordinate i, and a final extra block holds the skinned polygons.
  then the coordinate section: count, then GsCOORDUNIT records (0x50 bytes:
       flag, local MATRIX (3x3 s16 /4096 + s32 xyz), work matrix, rotation
       vector (4096 = one turn), parent pointer).
  then the primitive headers: [size, words...]; words with bit 31 set are
       pointers.  Polygon header: poly data, vertex table, normal table,
       (coordinate section).  Skinned header: poly data, vertex table,
       transformed-vertex work, normal table, lit-color work, (coords).
       Animation header: 5, interpolator code list, control stream, keys,
       coords.
  primitive blocks: {next, header, count|0x80000000, entries}; each entry
       is {type, (count << 16) | size, data...}; polygon entries hold the
       offset of their first polygon in the header's poly table.

Polygon type codes (low 10 bits pick the layout, the rest are flags):
  0x09 flat tex tri  [uv0|clut][uv1|tpage][uv2|-][n0|v0][v1|v2]
  0x0D gouraud tri   [uv0|clut][uv1|tpage][uv2|-][n0|v0][n1|v1][n2|v2]
  0x11 flat quad     [uv0|clut][uv1|tpage][uv2|-][uv3|n0][v0|v1][v2|v3]
  0x15 gouraud quad  [uv0|clut][uv1|tpage][uv2|n0][uv3|v0][n1|v1][n2|v2][n3|v3]
  0x08/0x0C/0x10/0x14 the untextured versions (3/4/4/5 words, color first)
  +0x200         an extra leading color word (drawn flat, no lighting)
  0x00100000     double sided: the driver skips the backface test
  0x00200000     semi-transparent: ABE set, blend mode from the tpage bits
  0x00020000     the duel mat's own drivers (0x80033DB0 / 0x80034830)
  0x00800000     "map the coordinates now" marker on a block's first entry
  0x01000000     skinned: indices go into the shared vertex/normal tables
Vertex words are (normal index << 0) | (vertex index << 16).

Animation: block[0]'s 0x03800000 entry holds one 7-word sequence descriptor
per animated bone.  A bone's control stream is a run of u32 words: a key
entry is (interpolator << 24) | (frames << 16) | key offset, a control word
0x80000000 | (new sid << 23) | (match sid << 16) | target jumps when the
descriptor's sequence id matches (0 = always), 0xC0800000 | (sid << 16)
stops.  Keys are 5 words: translation xyz, rotation xyz (YXZ Euler, 4096 =
turn), scale xyz (4096 = 1.0); code 0x2019 keys omit the scale.  Each bone
loops its idle keys; after the last key a jump table sends sequence ids
2..8 to their own sections and, because every jump carries new-sid 0, the
descriptor drops back to 0 so the section plays once and the idle loop
resumes.  The game sets the id through func_800597C8 (all bones at once).
Bind matrices were built with RotMatrix (X*Y*Z); animation keys go through
RotMatrixYXZ (Y*X*Z); both were confirmed by running the EXE's libgte.
"""

import json
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disc_image import open_disc  # noqa: E402

SECTOR = 2048

# --- disc layout ----------------------------------------------------------
MODEL_MRG_LBA = 28534
MODEL_BLOCK = 276
MODEL_COUNT = 621
SU_MRG_LBA = 954
WA_MRG_LBA = 10102
TERRAIN_PKG_SECTOR = 0x16C6
TERRAIN_PKG_LEN = 235
FIELD_HMD_SECTOR = 198
FLOOR_TEX_SECTOR = 203
FLOOR_TEX_LEN = 16
SCENE_FIRST_SECTOR = 136
SCENE_STRIDE = 116
SCENE_COUNT = 7
EXODIA_SECTOR = 948
EXODIA_LEN = 275
TERRAIN_NAMES = ['arena', 'forest', 'wasteland', 'mountain', 'sogen', 'umi', 'yami']

# Non-monster card ranges (1-based, inclusive); every one is skipped in MODEL.MRG.
NON_MONSTER = [(301, 350), (651, 700), (721, 721)]


def card_to_model(card):
    """Card id (1..722) -> MODEL.MRG block index, or None for spells/traps."""
    if not 1 <= card <= 722:
        return None
    m = card - 1
    for lo, hi in NON_MONSTER:
        if lo <= card <= hi:
            return None
        if card > hi:
            m -= hi - lo + 1
    return m


def model_to_card(m):
    for card in range(1, 723):
        if card_to_model(card) == m:
            return card
    return None


# --- polygon layouts ------------------------------------------------------
# words per primitive, number of corners, textured?
PRIM_LAYOUT = {
    0x08: (3, 3, False), 0x09: (5, 3, True),
    0x0C: (4, 3, False), 0x0D: (6, 3, True),
    0x10: (4, 4, False), 0x11: (6, 4, True),
    0x14: (5, 4, False), 0x15: (7, 4, True),
}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def s16(b, o):
    return struct.unpack_from('<h', b, o)[0]


class Prim:
    __slots__ = ('type', 'corners', 'verts', 'norms', 'uvs', 'clut', 'tpage',
                 'color', 'shared', 'coord', 'flags', 'gouraud', 'textured')

    def to_dict(self):
        return {k: getattr(self, k) for k in self.__slots__}


class Hmd:
    """One model.  Vertices and normals are (x, y, z) in the bone's frame."""

    def __init__(self, data, label=''):
        self.data = data
        self.label = label
        self.size = u32(data, 0)
        self.map_flag = u32(data, 4)
        self.hdr_top = u32(data, 8) * 4
        self.nblk = u32(data, 12)
        self.blocks = [u32(data, 0x10 + 4 * i) for i in range(self.nblk)]
        ct = 0x10 + 4 * self.nblk
        self.coord_top = ct
        self.ncoord = u32(data, ct)
        self.coords = []
        for c in range(self.ncoord):
            o = ct + 4 + c * 0x50
            m = struct.unpack_from('<9h', data, o + 4)
            t = struct.unpack_from('<3i', data, o + 0x18)
            rot = struct.unpack_from('<3h', data, o + 0x44)
            sup = u32(data, o + 0x4C)
            parent = None
            if sup:
                parent = (sup * 4 - ct - 4) // 0x50
            self.coords.append(dict(flag=u32(data, o), matrix=list(m), trans=list(t),
                                    rot=list(rot), parent=parent))
        # primitive headers, keyed by byte offset of their size word
        self.headers = {}
        nh = u32(data, self.hdr_top)
        p = self.hdr_top + 4
        for i in range(nh):
            sz = u32(data, p)
            self.headers[p] = [u32(data, p + 4 + 4 * j) for j in range(sz)]
            p += 4 + 4 * sz
        self.prims = []            # every polygon, with its coord
        self.shared_calc = []      # (coord, vcount, voff, vwork, ncount, noff, nwork)
        self.anim = None
        self.entries = []          # raw (block, chain, type, count, words)
        self.vtab = {}             # header offset -> (vert top, norm top)
        self._walk()

    # -- pointers: bit 31 marks a long-word offset
    @staticmethod
    def ptr(v):
        return (v & 0x7FFFFFFF) * 4

    def coord_of_block(self, bi):
        """block[1+i] is coordinate i; the trailing extra block is the skin."""
        if 1 <= bi <= self.ncoord:
            return bi - 1
        return None

    def _walk(self):
        d = self.data
        for bi, bo in enumerate(self.blocks):
            if not bo:
                continue
            p = bo * 4
            chain = 0
            guard = 0
            while guard < 4096:
                guard += 1
                nxt = u32(d, p)
                hp = u32(d, p + 4) * 4
                n = u32(d, p + 8) & 0x7FFFFFFF
                q = p + 12
                hdr = self.headers.get(hp, [])
                for _ in range(n):
                    t = u32(d, q)
                    sc = u32(d, q + 4)
                    cnt = (sc >> 16) & 0x7FFF
                    sz = sc & 0xFFFF
                    words = [u32(d, q + 8 + 4 * k) for k in range(max(sz - 1, 0))]
                    self.entries.append((bi, chain, t, cnt, words))
                    cat = t >> 24
                    if cat in (0, 1) and (t & 0x3FF):
                        self._polys(bi, hp, hdr, t, cnt, words[0] if words else 0)
                    elif t == 0x01000000 or t == 0x01800000:
                        w = words
                        self.shared_calc.append(dict(coord=self.coord_of_block(bi), block=bi,
                                                     vcount=w[0], voff=w[1], vwork=w[2],
                                                     ncount=w[3], noff=w[4], nwork=w[5]))
                    elif cat == 3:
                        self._anim(hp, hdr, cnt, q + 8)
                    q += 4 + sz * 4
                chain += 1
                if nxt == 0xFFFFFFFF:
                    break
                p = nxt * 4

    def _polys(self, bi, hp, hdr, t, cnt, off):
        d = self.data
        low = t & 0x1FF
        extra = 1 if t & 0x200 else 0
        shared = (t >> 24) == 1
        lay = PRIM_LAYOUT.get(low)
        if lay is None:
            raise ValueError('%s: unknown polygon type %08x' % (self.label, t))
        nwords, ncorner, textured = lay
        nwords += extra
        poly = self.ptr(hdr[0]) + off * 4
        vt = self.ptr(hdr[1])
        nt = self.ptr(hdr[3]) if shared else self.ptr(hdr[2])
        self.vtab[hp] = (vt, nt, shared)
        gouraud = bool(low & 4)
        coord = self.coord_of_block(bi)
        for k in range(cnt):
            w = struct.unpack_from('<%dI' % nwords, d, poly + k * nwords * 4)
            pr = Prim()
            pr.type = t
            pr.corners = ncorner
            pr.shared = shared
            pr.coord = coord
            pr.flags = t & 0xFFFFFC00
            pr.gouraud = gouraud
            pr.textured = textured
            pr.color = None
            if extra:
                c = w[0]
                pr.color = [c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF]
                w = w[1:]
            if textured:
                pr.clut = w[0] >> 16
                pr.tpage = w[1] >> 16
                uv = [(w[0] & 0xFF, (w[0] >> 8) & 0xFF), (w[1] & 0xFF, (w[1] >> 8) & 0xFF),
                      (w[2] & 0xFF, (w[2] >> 8) & 0xFF)]
                if ncorner == 4:
                    uv.append((w[3] & 0xFF, (w[3] >> 8) & 0xFF))
                pr.uvs = uv
                if low == 0x09:
                    pr.norms = [w[3] & 0xFFFF] * 3
                    pr.verts = [w[3] >> 16, w[4] & 0xFFFF, w[4] >> 16]
                elif low == 0x0D:
                    pr.norms = [w[3] & 0xFFFF, w[4] & 0xFFFF, w[5] & 0xFFFF]
                    pr.verts = [w[3] >> 16, w[4] >> 16, w[5] >> 16]
                elif low == 0x11:
                    pr.norms = [w[3] >> 16] * 4
                    pr.verts = [w[4] & 0xFFFF, w[4] >> 16, w[5] & 0xFFFF, w[5] >> 16]
                else:  # 0x15
                    pr.norms = [w[2] >> 16, w[4] & 0xFFFF, w[5] & 0xFFFF, w[6] & 0xFFFF]
                    pr.verts = [w[3] >> 16, w[4] >> 16, w[5] >> 16, w[6] >> 16]
            else:
                # Untextured: a color word first, then the index words in the
                # same order as the textured layouts.  Measured sizes 3/4/4/5
                # words fit this; the color placement is inferred, not traced.
                c = w[0]
                pr.color = [c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF]
                pr.clut = pr.tpage = None
                pr.uvs = None
                if low == 0x08:
                    pr.norms = [w[1] & 0xFFFF] * 3
                    pr.verts = [w[1] >> 16, w[2] & 0xFFFF, w[2] >> 16]
                elif low == 0x0C:
                    pr.norms = [w[1] & 0xFFFF, w[2] & 0xFFFF, w[3] & 0xFFFF]
                    pr.verts = [w[1] >> 16, w[2] >> 16, w[3] >> 16]
                elif low == 0x10:
                    pr.norms = [w[1] >> 16] * 4
                    pr.verts = [w[2] & 0xFFFF, w[2] >> 16, w[3] & 0xFFFF, w[3] >> 16]
                else:  # 0x14
                    pr.norms = [w[1] & 0xFFFF, w[2] & 0xFFFF, w[3] & 0xFFFF, w[4] & 0xFFFF]
                    pr.verts = [w[1] >> 16, w[2] >> 16, w[3] >> 16, w[4] >> 16]
            pr.verts = [(vt, v) for v in pr.verts]
            pr.norms = [(nt, n) for n in pr.norms]
            self.prims.append(pr)

    def vertex(self, ref):
        top, i = ref
        return struct.unpack_from('<3h', self.data, top + 8 * i)

    def normal(self, ref):
        top, i = ref
        return struct.unpack_from('<3h', self.data, top + 8 * i)

    # -- animation --------------------------------------------------------
    def _anim(self, hp, hdr, cnt, q):
        d = self.data
        codes_top = self.ptr(hdr[1])
        ctrl_top = self.ptr(hdr[2])
        keys_top = self.ptr(hdr[3])
        ncodes = u32(d, codes_top) & 0x7FFFFFFF
        codes = [u32(d, codes_top + 4 + 4 * i) for i in range(ncodes)]
        descs = []
        p = q
        for i in range(cnt):
            w = struct.unpack_from('<7I', d, p)
            size = w[1] & 0xFFFF
            sel = w[0] >> 24
            coord_off = w[0] & 0xFFFFFF
            coord = None
            if sel < len(hdr):
                # the descriptor points at a coord through header word `sel`
                base = self.ptr(hdr[sel])
                coord = (base + coord_off * 4 - self.coord_top - 4) // 0x50
            descs.append(dict(coord=coord, size=size, num=w[1] >> 16, speed=(w[3] & 0xFF00) >> 8,
                              sid=w[3] & 0xFF, start=w[6], raw=list(w)))
            p += size * 4
        # the control stream ends where the key table begins
        nctrl = (keys_top - ctrl_top) // 4
        ctrl = [u32(d, ctrl_top + 4 * i) for i in range(nctrl)]
        # keys: 5 words each (TRS) for code 0x2119, 3 words (TR) for 0x2019.
        self.anim = dict(codes=codes, ctrl=ctrl, keys_top=keys_top, descs=descs,
                         ctrl_top=ctrl_top)

    def key(self, off, code):
        """Key at word offset `off` of the key table."""
        o = self.anim['keys_top'] + off * 4
        t = struct.unpack_from('<3h', self.data, o)
        r = struct.unpack_from('<3h', self.data, o + 6)
        if code & 0xFFFF == 0x2119:
            s = struct.unpack_from('<3h', self.data, o + 12)
        else:
            s = (4096, 4096, 4096)
        return dict(t=list(t), r=list(r), s=list(s))

    def sequences(self, sid):
        """Replay GsU_03000000's stream walk for every bone with the given
        sequence id.  Returns per bone the loop it settles into: a list of
        (key, frames, interp) starting at the loop's first key."""
        out = []
        a = self.anim
        if not a:
            return out
        ctrl = a['ctrl']
        for desc in a['descs']:
            if desc['coord'] is None:
                out.append(None)
                continue
            cur = desc['start']
            seq = self._walk_stream(ctrl, cur, sid)
            out.append(seq)
        return out

    def _next_key(self, ctrl, idx, sid, dir=1):
        """Scan from idx (exclusive) applying control words as the driver does.
        Returns (index of next key, sid after the walk) or (None, sid)."""
        i = idx
        guard = 0
        while guard < 10000:
            guard += 1
            if i < 0 or i >= len(ctrl):
                return None, sid
            w = ctrl[i]
            if not w & 0x80000000:
                return i, sid
            if (w & 0xC0000000) == 0x80000000:
                match = (w >> 16) & 0x7F
                newsid = (w >> 23) & 0x7F
                if match == 0 or match == sid:
                    if match != 0 or newsid != 0:
                        sid = newsid
                    i = w & 0xFFFF
                    continue
                i += dir
            else:  # 0xC0000000 family: stop
                match = (w >> 16) & 0x7F
                if ((w >> 23) & 0x7F) == 1 and (match == 0 or match == sid):
                    return None, sid
                i += dir
        return None, sid

    def _walk_stream(self, ctrl, start, sid):
        """Replay the driver from the bone's start index with the given
        sequence id.  Every jump table entry carries new-sid 0, so a
        non-idle sequence plays once and the bone falls back into the idle
        loop; the clip returned is the one-shot section (for sid 0/1 the
        idle loop itself) as a list of (key_from, key_to, frames, code)."""
        codes = self.anim['codes']
        cur = start
        if cur >= len(ctrl):
            return []
        if ctrl[cur] & 0x80000000:
            cur, sid = self._next_key(ctrl, cur + 1, sid)
            if cur is None:
                return []
        path = []
        seen = {}
        idle_keys = set([cur])
        guard = 0
        entered = None   # index in path where the walk left the idle loop
        while guard < 4096:
            guard += 1
            prev = cur
            sid_before = sid
            cur, sid = self._next_key(ctrl, prev + 1, sid)
            if cur is None:
                break
            state = (prev, cur, sid)
            if entered is None and state in seen:
                return path[seen[state]:]          # the idle loop
            seen[state] = len(path)
            if entered is None and sid != sid_before:
                entered = len(path)
            w = ctrl[cur]
            frames = (w >> 16) & 0xFF
            code = codes[(w >> 24) & 0x7F] if codes else 0x03002119
            path.append(dict(key_from=ctrl[prev] & 0xFFFF, key_to=w & 0xFFFF,
                             frames=frames, code=code, idx=cur))
            if entered is None:
                idle_keys.add(cur)
            elif cur in idle_keys:
                return path[entered:]              # one-shot done, idle resumes
        return path[entered:] if entered is not None else path


# --- matrices ---------------------------------------------------------------
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_vec(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def rot_axis(axis, ang):
    c, s = math.cos(ang), math.sin(ang)
    if axis == 'x':
        return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == 'y':
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def rot_matrix_yxz(r):
    """PsyQ RotMatrixYXZ: rotate about Z, then X, then Y (M = Ry * Rx * Rz).
    Verified against the stored bind matrices of every coordinate with a
    nonzero rotation vector (see selftest)."""
    ax, ay, az = [v * 2 * math.pi / 4096.0 for v in r]
    return mat_mul(rot_axis('y', ay), mat_mul(rot_axis('x', ax), rot_axis('z', az)))


def coord_local(c):
    m = c['matrix']
    return [[m[0] / 4096.0, m[1] / 4096.0, m[2] / 4096.0],
            [m[3] / 4096.0, m[4] / 4096.0, m[5] / 4096.0],
            [m[6] / 4096.0, m[7] / 4096.0, m[8] / 4096.0]], list(c['trans'])


def world_matrices(coords):
    """(rotation, translation) of every coordinate in model space."""
    out = [None] * len(coords)

    def get(i, depth=0):
        if out[i] is not None or depth > 64:
            return out[i]
        m, t = coord_local(coords[i])
        p = coords[i]['parent']
        if p is None or p == i or p >= len(coords):
            out[i] = (m, t)
        else:
            pm, pt = get(p, depth + 1)
            out[i] = (mat_mul(pm, m), [a + b for a, b in zip(mat_vec(pm, t), pt)])
        return out[i]

    for i in range(len(coords)):
        get(i)
    return out


def mat_to_quat(m):
    """Rotation matrix (row-major, applied as m*v) -> (x, y, z, w)."""
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return [x / n, y / n, z / n, w / n]


# PSX space is x right, y down, z into the screen; glTF wants y up, z out.
# A half turn about x maps one onto the other and keeps the handedness, so
# polygon winding survives.  SCALE turns the game's integer units into meters
# (a dragon is ~1200 units tall).
FLIP = [[1, 0, 0], [0, -1, 0], [0, 0, -1]]
SCALE = 1.0 / 400.0


def to_gl_vec(v):
    return [v[0] * SCALE, -v[1] * SCALE, -v[2] * SCALE]


def to_gl_dir(v):
    return [v[0], -v[1], -v[2]]


def to_gl_mat(m):
    return mat_mul(FLIP, mat_mul(m, FLIP))


# --- VRAM emulation ---------------------------------------------------------
class Vram:
    """1024x512 halfwords.  The loaders upload sectors as 64-halfword-wide,
    16-row rects, column by column (16 sectors per 256-row column)."""

    def __init__(self):
        self.buf = bytearray(1024 * 512 * 2)

    def upload(self, data, x, y, w, h):
        for row in range(h):
            src = row * w * 2
            dst = ((y + row) * 1024 + x) * 2
            self.buf[dst:dst + w * 2] = data[src:src + w * 2]

    def upload_columns(self, data, x, y, nsectors, rows_per_col=256):
        """Stream `nsectors` sectors the way the CD stage engine does."""
        col = 0
        row = 0
        for s in range(nsectors):
            chunk = data[s * SECTOR:(s + 1) * SECTOR]
            self.upload(chunk, x + col * 64, y + row, 64, 16)
            row += 16
            if row >= rows_per_col:
                row = 0
                col += 1

    def hw(self, x, y):
        o = ((y & 511) * 1024 + (x & 1023)) * 2
        return self.buf[o] | (self.buf[o + 1] << 8)

    def clut(self, clut, n):
        cx = (clut & 0x3F) * 16
        cy = clut >> 6
        return [self.hw(cx + i, cy) for i in range(n)]

    def texel_index(self, tpage, u, v):
        """Palette index of texel (u, v) in the 256x256 window of `tpage`."""
        bx = (tpage & 0xF) * 64
        by = ((tpage >> 4) & 1) * 256
        mode = (tpage >> 7) & 3
        if mode == 0:
            w = self.hw(bx + (u >> 2), by + v)
            return (w >> ((u & 3) * 4)) & 0xF
        if mode == 1:
            w = self.hw(bx + (u >> 1), by + v)
            return (w >> ((u & 1) * 8)) & 0xFF
        return self.hw(bx + u, by + v)

    def texture_png(self, tpage, clut):
        """RGBA rows for the 256x256 texel window drawn through `clut`."""
        mode = (tpage >> 7) & 3
        pal = self.clut(clut, 16 if mode == 0 else 256) if mode < 2 else None
        rows = []
        for v in range(256):
            row = bytearray()
            for u in range(256):
                idx = self.texel_index(tpage, u, v)
                c = pal[idx] if pal is not None else idx
                row += bytes(rgba5551(c))
            rows.append(bytes(row))
        return rows


def rgba5551(c):
    r = (c & 31) * 255 // 31
    g = ((c >> 5) & 31) * 255 // 31
    b = ((c >> 10) & 31) * 255 // 31
    # halfword 0 is the GPU's fully transparent color; STP alone is opaque.
    a = 0 if c == 0 else 255
    return r, g, b, a


def write_png(path, w, h, rows, rgba=True):
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6 if rgba else 2, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


# --- sources ------------------------------------------------------------------
class Source:
    """One model plus the VRAM it expects, plus the raw side files."""

    def __init__(self, name, hmd, vram, extras=None, info=None):
        self.name = name
        self.hmd = hmd
        self.vram = vram
        self.extras = extras or {}
        self.info = info or {}


def read_sectors(disc, lba, n):
    return disc.read(lba * SECTOR, n * SECTOR)


def monster_source(disc, card):
    m = card_to_model(card)
    if m is None:
        raise ValueError('card %d has no model (spell/trap/ritual)' % card)
    blk = read_sectors(disc, MODEL_MRG_LBA + MODEL_BLOCK * m, MODEL_BLOCK)
    sec = lambda a, b: blk[a * SECTOR:b * SECTOR]  # noqa: E731
    hmd = Hmd(sec(0, 96), 'card %d' % card)
    v = Vram()
    # The polygons reference the exporter's layout: tpages 10.. at y=256 and
    # palette rows at (640, 0..7).  The duel loader relocates them per slot
    # (tpage - 10 + 4*slot, CLUT + 0x3BD8 + 16*slot); we keep the raw one.
    v.upload_columns(sec(96, 144), 640, 256, 48)
    v.upload_columns(sec(147, 163), 640 + 192, 256, 16)
    v.upload(sec(144, 146), 640, 0, 256, 8)
    v.upload(sec(146, 147), 512, 3, 256, 1)   # the single palette (slot-relative in game)
    extras = {
        'effect_slot0.bin': sec(180, 190), 'effect_slot1.bin': sec(190, 200),
        'effect_alt_slot0.bin': sec(200, 210), 'effect_alt_slot1.bin': sec(210, 220),
        'effect_data_slot0.bin': sec(220, 222), 'effect_data_slot1.bin': sec(222, 224),
        'idlist.bin': sec(224, 225), 'spu_samples.bin': sec(225, 275), 'params.bin': sec(275, 276),
        'hmd.bin': sec(0, 96), 'texture_raw.bin': sec(96, 147) + sec(147, 163),
    }
    p = blk[275 * SECTOR:]
    info = dict(card=card, model_index=m, lba=MODEL_MRG_LBA + MODEL_BLOCK * m,
                atk=struct.unpack_from('<H', p, 0x100)[0], defense=struct.unpack_from('<H', p, 0x102)[0],
                sound_cues=[list(struct.unpack_from('<BBH', p, 4 * i)) for i in range(16)],
                param_words=[u32(p, 0x100 + 4 * i) for i in range(7)])
    return Source('card%03d' % card, hmd, v, extras, info)


def exodia_source(disc):
    blk = read_sectors(disc, SU_MRG_LBA + EXODIA_SECTOR, EXODIA_LEN)
    sec = lambda a, b: blk[a * SECTOR:b * SECTOR]  # noqa: E731
    hmd = Hmd(sec(0, 96), 'exodia')
    v = Vram()
    # func_800577B0: 96 sectors to (0,256), 2 palette sectors to (0,248), 10+10
    # effect sectors, one sector -> (256,240) 256x2, 8 sectors to (384,256).
    # Kept in the exporter layout like the monsters (tpage 10 = x 640).
    v.upload_columns(sec(96, 192), 640, 256, 96)
    v.upload(sec(192, 194), 640, 0, 256, 8)
    v.upload(sec(214, 215)[:1024], 640 + 256, 0, 256, 2)
    v.upload_columns(sec(215, 223), 640 + 384, 256, 8)
    extras = {'effect_slot0.bin': sec(194, 204), 'effect_slot1.bin': sec(204, 214),
              'idlist.bin': sec(223, 224), 'spu_samples.bin': sec(224, 274), 'params.bin': sec(274, 275),
              'hmd.bin': sec(0, 96)}
    p = blk[274 * SECTOR:]
    info = dict(card=0x309, lba=SU_MRG_LBA + EXODIA_SECTOR,
                atk=struct.unpack_from('<H', p, 0x100)[0], defense=struct.unpack_from('<H', p, 0x102)[0])
    return Source('exodia', hmd, v, extras, info)


def scene_source(disc, k):
    base = SU_MRG_LBA + SCENE_FIRST_SECTOR + SCENE_STRIDE * k
    blk = read_sectors(disc, base, SCENE_STRIDE)
    sec = lambda a, b: blk[a * SECTOR:b * SECTOR]  # noqa: E731
    hmd = Hmd(sec(0, 18), 'scene %d' % k)
    v = Vram()
    # func_80057544: 18 sectors model, 64 texture sectors to (512,256), one
    # sector -> palette (512,241) 256x1, one sector -> (512,244) 256x2, then
    # 32 texture sectors to (896,0).
    v.upload_columns(sec(18, 82), 512, 256, 64)
    v.upload(sec(82, 83)[:512], 512, 241, 256, 1)
    v.upload(sec(83, 84)[:1024], 512, 244, 256, 2)
    v.upload_columns(sec(84, 116), 896, 0, 32)
    extras = {'hmd.bin': sec(0, 18)}
    return Source('scene%d' % k, hmd, v, extras, dict(lba=base, package=k))


def field_source(disc, terrain=0):
    pkg = WA_MRG_LBA + TERRAIN_PKG_SECTOR + TERRAIN_PKG_LEN * terrain
    hmd_bytes = read_sectors(disc, pkg + FIELD_HMD_SECTOR, 5)
    hmd = Hmd(hmd_bytes, 'field')
    v = Vram()
    # Duel_LoadPackageStage's last stage streams package sectors 203..234 to
    # (640, 256) as two 64-halfword columns: the floor (tpage 10, 4bpp, its
    # palettes on rows 254..255) and the mat's border art (tpage 11, with
    # palettes at rows 226..228).  Both are what the mat's polygons cite.
    v.upload_columns(read_sectors(disc, pkg + FLOOR_TEX_SECTOR, 32), 640, 256, 32)
    extras = {'hmd.bin': hmd_bytes[:u32(hmd_bytes, 0)],
              'floor_texture_raw.bin': read_sectors(disc, pkg + FLOOR_TEX_SECTOR, 32)}
    return Source('field_%s' % TERRAIN_NAMES[terrain], hmd, v, extras,
                  dict(lba=pkg + FIELD_HMD_SECTOR, terrain=terrain))


# --- geometry assembly ----------------------------------------------------
class Mesh:
    """De-indexed triangles grouped by (material key, bone)."""

    def __init__(self):
        self.groups = {}   # key -> dict(verts=[], tris=[])

    def add(self, key, corners):
        g = self.groups.setdefault(key, dict(verts=[], tris=[]))
        base = len(g['verts'])
        g['verts'].extend(corners)
        n = len(corners)
        if n == 3:
            g['tris'].append((base, base + 1, base + 2))
        else:
            # PSX quads are v0 v1 v2 v3 in strip order (two triangles 012, 132)
            g['tris'].append((base, base + 1, base + 2))
            g['tris'].append((base + 1, base + 3, base + 2))


def material_key(pr):
    if pr.textured:
        return ('tex', pr.tpage, pr.clut, pr.flags & 0x00300000)
    return ('flat', tuple(pr.color or (128, 128, 128)), pr.flags & 0x00300000)


def build_mesh(src):
    """Model-space bind pose.  Returns (rigid: bone -> Mesh, skinned: Mesh with
    per-corner bone ids)."""
    hmd = src.hmd
    worlds = world_matrices(hmd.coords)
    # which bone owns each shared vertex / normal
    vbone = {}
    nbone = {}
    for r in hmd.shared_calc:
        for i in range(r['vcount']):
            vbone[r['voff'] + i] = r['coord']
        for i in range(r['ncount']):
            nbone[r['noff'] + i] = r['coord']
    rigid = {}
    skinned = Mesh()
    for pr in hmd.prims:
        key = material_key(pr)
        corners = []
        for c in range(pr.corners):
            vt, vi = pr.verts[c]
            nt, ni = pr.norms[c]
            p = hmd.vertex((vt, vi))
            n = hmd.normal((nt, ni))
            bone = vbone.get(vi, pr.coord) if pr.shared else pr.coord
            uv = pr.uvs[c] if pr.uvs else (0, 0)
            corners.append(dict(pos=p, nrm=n, uv=uv, bone=bone,
                                color=pr.color))
        if pr.shared:
            skinned.add(key, corners)
        else:
            rigid.setdefault(pr.coord, Mesh()).add(key, corners)
    return worlds, rigid, skinned


# --- exporters ------------------------------------------------------------------
def export_textures(src, outdir, keys):
    """One PNG per (tpage, clut) the model uses.  Returns key -> filename."""
    names = {}
    for key in keys:
        if key[0] != 'tex':
            continue
        _, tpage, clut, _ = key
        fn = 'tex_tp%02x_clut%04x.png' % (tpage, clut)
        path = os.path.join(outdir, fn)
        if not os.path.exists(path):
            write_png(path, 256, 256, src.vram.texture_png(tpage, clut))
        names[key] = fn
    return names


def export_obj(src, outdir, worlds, rigid, skinned, texnames):
    name = src.name
    mtl = []
    lines = ['# %s, extracted from the disc by model_extract.py' % name,
             'mtllib %s.mtl' % name]
    vcount = 0
    mats = set()

    def emit(mesh, label, transform):
        nonlocal vcount, lines
        for key, g in mesh.groups.items():
            mats.add(key)
            lines.append('g %s_%s' % (label, matname(key)))
            lines.append('usemtl %s' % matname(key))
            for c in g['verts']:
                m, t = transform(c)
                p = [a + b for a, b in zip(mat_vec(m, c['pos']), t)]
                p = to_gl_vec(p)
                n = to_gl_dir(mat_vec(m, [x / 4096.0 for x in c['nrm']]))
                lines.append('v %.5f %.5f %.5f' % tuple(p))
                lines.append('vn %.4f %.4f %.4f' % tuple(n))
                lines.append('vt %.5f %.5f' % (c['uv'][0] / 256.0, 1.0 - c['uv'][1] / 256.0))
            for a, b, cc in g['tris']:
                i, j, k = vcount + a + 1, vcount + b + 1, vcount + cc + 1
                lines.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (i, i, i, j, j, j, k, k, k))
            vcount += len(g['verts'])

    ident = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    for bone, mesh in sorted(rigid.items(), key=lambda kv: (kv[0] is None, kv[0])):
        w = worlds[bone] if bone is not None else ident
        emit(mesh, 'bone%02d' % (bone if bone is not None else 99), lambda c, w=w: w)
    emit(skinned, 'skin', lambda c: worlds[c['bone']] if c['bone'] is not None else ident)
    for key in sorted(mats, key=str):
        mtl.append('newmtl %s' % matname(key))
        if key[0] == 'tex':
            mtl.append('Kd 1 1 1')
            mtl.append('map_Kd %s' % texnames[key])
            mtl.append('map_d %s' % texnames[key])
        else:
            r, g, b = key[1]
            mtl.append('Kd %.3f %.3f %.3f' % (r / 255.0, g / 255.0, b / 255.0))
        if key[-1] & 0x00200000:
            mtl.append('d 0.5')
    with open(os.path.join(outdir, name + '.obj'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(outdir, name + '.mtl'), 'w') as f:
        f.write('\n'.join(mtl) + '\n')


def matname(key):
    if key[0] == 'tex':
        return 'tex_tp%02x_clut%04x%s' % (key[1], key[2], flagsuffix(key[3]))
    r, g, b = key[1]
    return 'flat_%02x%02x%02x%s' % (r, g, b, flagsuffix(key[2]))


def flagsuffix(f):
    s = ''
    if f & 0x00100000:
        s += '_2side'
    if f & 0x00200000:
        s += '_blend'
    return s


class GltfWriter:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []

    def add(self, data, comp_type, count, atype, target=None, mn=None, mx=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        view = dict(buffer=0, byteOffset=off, byteLength=len(data))
        if target:
            view['target'] = target
        self.views.append(view)
        acc = dict(bufferView=len(self.views) - 1, componentType=comp_type, count=count, type=atype)
        if mn is not None:
            acc['min'] = mn
            acc['max'] = mx
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def floats(self, values, atype, target=34962, minmax=False):
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[atype]
        flat = [x for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        data = struct.pack('<%df' % len(flat), *flat)
        mn = mx = None
        if minmax and values:
            if n == 1:
                mn, mx = [min(values)], [max(values)]
            else:
                mn = [min(v[i] for v in values) for i in range(n)]
                mx = [max(v[i] for v in values) for i in range(n)]
        return self.add(data, 5126, len(flat) // n, atype, target, mn, mx)


def export_gltf(src, outdir, worlds, rigid, skinned, texnames, fps=60.0):
    hmd = src.hmd
    name = src.name
    g = GltfWriter()
    materials = {}
    images = []
    textures = []
    mat_list = []

    def material(key):
        if key in materials:
            return materials[key]
        m = dict(name=matname(key), doubleSided=bool(key[-1] & 0x00100000))
        if key[0] == 'tex':
            fn = texnames[key]
            images.append(dict(uri=fn))
            textures.append(dict(source=len(images) - 1, sampler=0))
            m['pbrMetallicRoughness'] = dict(baseColorTexture=dict(index=len(textures) - 1),
                                             metallicFactor=0.0, roughnessFactor=1.0)
            m['alphaMode'] = 'BLEND' if key[3] & 0x00200000 else 'MASK'
            if m['alphaMode'] == 'MASK':
                m['alphaCutoff'] = 0.5
        else:
            r, gg, b = key[1]
            m['pbrMetallicRoughness'] = dict(baseColorFactor=[r / 255.0, gg / 255.0, b / 255.0, 1.0],
                                             metallicFactor=0.0, roughnessFactor=1.0)
        mat_list.append(m)
        materials[key] = len(mat_list) - 1
        return materials[key]

    nodes = []
    # bones
    bone_node = {}
    for i, c in enumerate(hmd.coords):
        m, t = coord_local(c)
        q = mat_to_quat(to_gl_mat(m))
        nodes.append(dict(name='bone%02d' % i, translation=to_gl_vec(t), rotation=q))
        bone_node[i] = i
    for i, c in enumerate(hmd.coords):
        p = c['parent']
        if p is not None and p != i and p < len(hmd.coords):
            nodes[p].setdefault('children', []).append(i)
    roots = [i for i, c in enumerate(hmd.coords) if c['parent'] is None or c['parent'] == i or c['parent'] >= len(hmd.coords)]
    meshes = []

    def mesh_from(mesh, skin_bones=None, transform=None):
        prims = []
        for key, grp in mesh.groups.items():
            pos, nrm, uv, joints, weights = [], [], [], [], []
            for c in grp['verts']:
                p = list(c['pos'])
                n = [x / 4096.0 for x in c['nrm']]
                if transform:
                    m, t = transform(c)
                    p = [a + b for a, b in zip(mat_vec(m, p), t)]
                    n = mat_vec(m, n)
                pos.append(to_gl_vec(p))
                ln = math.sqrt(sum(x * x for x in n)) or 1.0
                nrm.append([x / ln for x in to_gl_dir(n)])
                uv.append([c['uv'][0] / 256.0, c['uv'][1] / 256.0])
                if skin_bones is not None:
                    j = skin_bones.index(c['bone']) if c['bone'] in skin_bones else 0
                    joints.append([j, 0, 0, 0])
                    weights.append([1.0, 0, 0, 0])
            idx = [i for tri in grp['tris'] for i in tri]
            attrs = dict(POSITION=g.floats(pos, 'VEC3', minmax=True), NORMAL=g.floats(nrm, 'VEC3'),
                         TEXCOORD_0=g.floats(uv, 'VEC2'))
            if skin_bones is not None:
                attrs['JOINTS_0'] = g.add(struct.pack('<%dH' % (4 * len(joints)), *[x for j in joints for x in j]),
                                          5123, len(joints), 'VEC4', 34962)
                attrs['WEIGHTS_0'] = g.floats(weights, 'VEC4')
            ind = g.add(struct.pack('<%dI' % len(idx), *idx), 5125, len(idx), 'SCALAR', 34963)
            prims.append(dict(attributes=attrs, indices=ind, material=material(key), mode=4))
        return prims

    # rigid parts: a mesh node under each bone, vertices stay in bone space
    for bone, mesh in rigid.items():
        prims = mesh_from(mesh)
        if not prims:
            continue
        meshes.append(dict(name='part_bone%02d' % (bone if bone is not None else 99), primitives=prims))
        node = dict(name='part_bone%02d' % (bone if bone is not None else 99), mesh=len(meshes) - 1)
        nodes.append(node)
        if bone is not None:
            nodes[bone].setdefault('children', []).append(len(nodes) - 1)
        else:
            roots.append(len(nodes) - 1)
    skins = []
    if skinned.groups:
        bones = sorted(set(c['bone'] for grp in skinned.groups.values() for c in grp['verts'] if c['bone'] is not None))
        ibm = []
        for b in bones:
            m, t = worlds[b]
            gm = to_gl_mat(m)
            gt = to_gl_vec(t)
            # inverse of [R t]: [R^T, -R^T t]
            rt = [[gm[j][i] for j in range(3)] for i in range(3)]
            it = [-sum(rt[i][k] * gt[k] for k in range(3)) for i in range(3)]
            ibm.append([rt[0][0], rt[1][0], rt[2][0], 0, rt[0][1], rt[1][1], rt[2][1], 0,
                        rt[0][2], rt[1][2], rt[2][2], 0, it[0], it[1], it[2], 1])
        prims = mesh_from(skinned, bones, lambda c: worlds[c['bone']] if c['bone'] is not None else ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0]))
        meshes.append(dict(name='skin', primitives=prims))
        skins.append(dict(joints=bones, inverseBindMatrices=g.floats(ibm, 'MAT4', target=None), name='skin'))
        nodes.append(dict(name='skin', mesh=len(meshes) - 1, skin=0))
        roots.append(len(nodes) - 1)
    # animations: one clip per sequence id that actually differs
    animations = []
    if hmd.anim:
        for sid in range(0, 9):
            samplers, channels = [], []
            for bi, desc in enumerate(hmd.anim['descs']):
                seq = hmd.sequences(sid)[bi]
                if not seq or desc['coord'] is None or desc['coord'] >= len(hmd.coords):
                    continue
                times, trans, rots, scales = [], [], [], []
                t = 0.0
                keys = [(seq[0]['key_from'], seq[0]['code'])] + [(s['key_to'], s['code']) for s in seq]
                durs = [s['frames'] for s in seq]
                for i, (koff, code) in enumerate(keys):
                    k = hmd.key(koff, code)
                    times.append(t)
                    trans.append(to_gl_vec(k['t']))
                    rots.append(mat_to_quat(to_gl_mat(rot_matrix_yxz(k['r']))))
                    scales.append([v / 4096.0 for v in k['s']])
                    if i < len(durs):
                        # a 0-frame key is an instant cut; glTF wants strictly
                        # increasing times, so give it a hundredth of a frame
                        t += (durs[i] or 0.01) / fps
                # keep quaternion continuity for linear interpolation
                for i in range(1, len(rots)):
                    if sum(a * b for a, b in zip(rots[i - 1], rots[i])) < 0:
                        rots[i] = [-x for x in rots[i]]
                ta = g.floats(times, 'SCALAR', target=None, minmax=True)
                for path, vals, atype in (('translation', trans, 'VEC3'), ('rotation', rots, 'VEC4'), ('scale', scales, 'VEC3')):
                    samplers.append(dict(input=ta, output=g.floats(vals, atype, target=None), interpolation='LINEAR'))
                    channels.append(dict(sampler=len(samplers) - 1, target=dict(node=desc['coord'], path=path)))
            if channels:
                animations.append(dict(name='seq%d' % sid, samplers=samplers, channels=channels))
    # drop clips that are byte-identical to an earlier one (sid 0 and 1 usually are)
    uniq = []
    seen = set()
    for a in animations:
        sig = json.dumps(a['channels'], sort_keys=True) + str([s['output'] for s in a['samplers']])
        # outputs are distinct accessors, so compare the underlying data instead
        blob = b''
        for s in a['samplers']:
            acc = g.accessors[s['output']]
            v = g.views[acc['bufferView']]
            blob += bytes(g.bin[v['byteOffset']:v['byteOffset'] + v['byteLength']])
        h = zlib.crc32(blob)
        if h in seen:
            continue
        seen.add(h)
        uniq.append(a)
    doc = dict(asset=dict(version='2.0', generator='model_extract.py'),
               scene=0, scenes=[dict(nodes=roots, name=name)], nodes=nodes, meshes=meshes,
               materials=mat_list, images=images, textures=textures,
               samplers=[dict(magFilter=9728, minFilter=9728, wrapS=10497, wrapT=10497)],
               buffers=[dict(uri=name + '.bin', byteLength=len(g.bin))],
               bufferViews=g.views, accessors=g.accessors)
    if skins:
        doc['skins'] = skins
    if uniq:
        doc['animations'] = uniq
    for k in ('images', 'textures'):
        if not doc[k]:
            del doc[k]
    with open(os.path.join(outdir, name + '.bin'), 'wb') as f:
        f.write(bytes(g.bin))
    with open(os.path.join(outdir, name + '.gltf'), 'w') as f:
        json.dump(doc, f, indent=1)


def export_json(src, outdir):
    hmd = src.hmd
    doc = dict(name=src.name, info=src.info, size=hmd.size, block_count=hmd.nblk,
               coords=hmd.coords, headers={hex(k): [hex(x) for x in v] for k, v in hmd.headers.items()},
               entries=[dict(block=b, chain=c, type='%08x' % t, count=n, words=[hex(x) for x in w]) for b, c, t, n, w in hmd.entries],
               shared_calc=hmd.shared_calc,
               prims=[dict(type='%08x' % p.type, coord=p.coord, verts=[v for _, v in p.verts], norms=[n for _, n in p.norms],
                           uvs=p.uvs, clut=p.clut, tpage=p.tpage, color=p.color, shared=p.shared) for p in hmd.prims])
    if hmd.anim:
        a = hmd.anim
        doc['animation'] = dict(codes=[hex(c) for c in a['codes']], control_stream=[hex(w) for w in a['ctrl']],
                                descriptors=a['descs'],
                                sequences={sid: [s for s in hmd.sequences(sid)] for sid in range(9)},
                                keys_top=a['keys_top'])
    with open(os.path.join(outdir, src.name + '.json'), 'w') as f:
        json.dump(doc, f, indent=1)


def export_raw(src, outdir):
    raw = os.path.join(outdir, 'raw')
    os.makedirs(raw, exist_ok=True)
    for fn, data in src.extras.items():
        with open(os.path.join(raw, fn), 'wb') as f:
            f.write(data)


def export_preview(src, outdir, worlds, rigid, skinned, size=512):
    """A quick painter's-algorithm render of the bind pose, seen from the
    front (-z in glTF space), flat shaded by material color."""
    tris = []
    ident = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])

    def collect(mesh, transform):
        for key, grp in mesh.groups.items():
            pts = []
            for c in grp['verts']:
                m, t = transform(c)
                p = [a + b for a, b in zip(mat_vec(m, c['pos']), t)]
                pts.append(to_gl_vec(p))
            for a, b, cc in grp['tris']:
                tris.append((pts[a], pts[b], pts[cc], key))
    for bone, mesh in rigid.items():
        collect(mesh, lambda c, w=(worlds[bone] if bone is not None else ident): w)
    collect(skinned, lambda c: worlds[c['bone']] if c['bone'] is not None else ident)
    if not tris:
        return
    xs = [p[0] for t in tris for p in t[:3]]
    ys = [p[1] for t in tris for p in t[:3]]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1.0
    sc = (size * 0.9) / span
    img = [[(24, 24, 32, 255)] * size for _ in range(size)]
    tris.sort(key=lambda t: -(t[0][2] + t[1][2] + t[2][2]))
    for a, b, c, key in tris:
        p = [((v[0] - cx) * sc + size / 2, (cy - v[1]) * sc + size / 2) for v in (a, b, c)]
        n = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
        shade = 0.55 + 0.45 * (1 if n > 0 else 0.3)
        if key[0] == 'tex':
            base = (200, 200, 210)
        else:
            base = key[1]
        col = tuple(min(255, int(v * shade)) for v in base) + (255,)
        fill_triangle(img, p, col, size)
    rows = [b''.join(bytes(px) for px in row) for row in img]
    write_png(os.path.join(outdir, src.name + '_preview.png'), size, size, rows)


def fill_triangle(img, p, col, size):
    (x0, y0), (x1, y1), (x2, y2) = p
    ymin = max(int(min(y0, y1, y2)), 0)
    ymax = min(int(max(y0, y1, y2)) + 1, size - 1)
    for y in range(ymin, ymax + 1):
        xs = []
        for (ax, ay), (bx, by) in (((x0, y0), (x1, y1)), ((x1, y1), (x2, y2)), ((x2, y2), (x0, y0))):
            if (ay <= y < by) or (by <= y < ay):
                xs.append(ax + (y - ay) * (bx - ax) / (by - ay))
        if len(xs) >= 2:
            xa, xb = int(max(min(xs), 0)), int(min(max(xs), size - 1))
            row = img[y]
            for x in range(xa, xb + 1):
                row[x] = col


# --- self test ----------------------------------------------------------------
def rot_matrix_xyz(r):
    """PsyQ RotMatrix: M = Rx * Ry * Rz.  The exporter built the bind matrices
    with this one; the runtime animation uses RotMatrixYXZ.  Both were
    checked by running the EXE's own libgte code on sample angles."""
    ax, ay, az = [v * 2 * math.pi / 4096.0 for v in r]
    return mat_mul(rot_axis('x', ax), mat_mul(rot_axis('y', ay), rot_axis('z', az)))


def selftest(src):
    """Check the bind rotation convention against the stored matrices."""
    bad = 0
    for i, c in enumerate(src.hmd.coords):
        if c['rot'] == [0, 0, 0]:
            continue
        m = rot_matrix_xyz(c['rot'])
        stored, _ = coord_local(c)
        err = max(abs(m[r][k] - stored[r][k]) for r in range(3) for k in range(3))
        if err > 0.02:
            bad += 1
    return bad


# --- driver -----------------------------------------------------------------------
def extract(src, outroot, formats, preview=False):
    outdir = os.path.join(outroot, src.name)
    os.makedirs(outdir, exist_ok=True)
    worlds, rigid, skinned = build_mesh(src)
    keys = set()
    for mesh in list(rigid.values()) + [skinned]:
        keys.update(mesh.groups.keys())
    texnames = export_textures(src, outdir, keys) if ('png' in formats or 'gltf' in formats or 'obj' in formats) else {}
    if 'obj' in formats:
        export_obj(src, outdir, worlds, rigid, skinned, texnames)
    if 'gltf' in formats:
        export_gltf(src, outdir, worlds, rigid, skinned, texnames)
    if 'json' in formats:
        export_json(src, outdir)
    if 'raw' in formats:
        export_raw(src, outdir)
    if preview:
        export_preview(src, outdir, worlds, rigid, skinned)
    return outdir


def parse_what(args):
    cards = []
    kinds = set()
    for a in args:
        if a in ('monsters', 'exodia', 'field', 'scenes', 'all'):
            kinds.add(a)
        elif '-' in a:
            lo, hi = a.split('-')
            cards.extend(range(int(lo), int(hi) + 1))
        else:
            cards.append(int(a))
    if 'all' in kinds:
        kinds.update(('monsters', 'exodia', 'field', 'scenes'))
    if 'monsters' in kinds:
        cards = [c for c in range(1, 723) if card_to_model(c) is not None]
    return cards, kinds


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    disc_path, outroot = argv[1], argv[2]
    rest = argv[3:]
    formats = {'gltf', 'obj', 'json', 'png', 'raw'}
    preview = False
    what = []
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == '--formats':
            formats = set(rest[i + 1].split(','))
            i += 2
            continue
        if a == '--preview':
            preview = True
        elif a.startswith('--'):
            print('unknown option', a)
            return 2
        else:
            what.append(a)
        i += 1
    if not what:
        what = ['1']
    cards, kinds = parse_what(what)
    disc = open_disc(disc_path)
    os.makedirs(outroot, exist_ok=True)
    names = {}
    npath = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cards', 'fmr_cards.json')
    if os.path.exists(npath):
        try:
            for n, c in json.load(open(npath)).items():
                names[int(c['printouts']['Card number'][0])] = n.replace(' (FMR)', '')
        except Exception:
            names = {}
    done = []
    for card in cards:
        m = card_to_model(card)
        if m is None:
            print('card %d: no model' % card)
            continue
        src = monster_source(disc, card)
        src.info['name'] = names.get(card)
        out = extract(src, outroot, formats, preview)
        done.append((src, out))
        print('card %3d %-30s %2d bones %4d polys %s' % (card, names.get(card, ''), src.hmd.ncoord, len(src.hmd.prims), out))
    if 'exodia' in kinds:
        src = exodia_source(disc)
        out = extract(src, outroot, formats, preview)
        done.append((src, out))
        print('exodia %d bones %d polys %s' % (src.hmd.ncoord, len(src.hmd.prims), out))
    if 'field' in kinds:
        for t in range(7):
            src = field_source(disc, t)
            out = extract(src, outroot, formats, preview)
            done.append((src, out))
            print('field %s %d polys %s' % (TERRAIN_NAMES[t], len(src.hmd.prims), out))
    if 'scenes' in kinds:
        for k in range(SCENE_COUNT):
            src = scene_source(disc, k)
            out = extract(src, outroot, formats, preview)
            done.append((src, out))
            print('scene %d %d bones %d polys %s' % (k, src.hmd.ncoord, len(src.hmd.prims), out))
    bad = sum(selftest(s) for s, _ in done)
    if bad:
        print('WARNING: %d coordinates disagree with the RotMatrix (XYZ) convention' % bad)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
