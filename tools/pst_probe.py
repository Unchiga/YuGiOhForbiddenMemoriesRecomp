#!/usr/bin/env python3
"""Offline savestate probe: read guest RAM and walk the guest stack from a .pst.

The mid-duel freeze work needs to compare a frozen capture against a healthy one
without launching the game. A .pst carries full RAM (BS_SEC_RAM) and the CPU
registers (BS_SEC_CPU), which is everything needed to answer "where is the guest
stuck" offline and repeatably.

  python tools/pst_probe.py regs   <state.pst>
  python tools/pst_probe.py stack  <state.pst>
  python tools/pst_probe.py read   <state.pst> 0x8009B0F4 [len]
  python tools/pst_probe.py freeze <state.pst> [more.pst ...]   # the discriminators

`stack` scans upward from $sp and reports every word that is a plausible return
address (the word 8 bytes before it is a jal/jalr). Stale slots from earlier,
deeper calls show up too -- read the chain by call-site plausibility, not by
taking every line as a live frame.
"""
import struct, sys, zlib, os

BS_SEC_CPU, BS_SEC_RAM = 0x01, 0x02
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'disc', 'SLUS_014.11')

REGN = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
        's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']


def load(path):
    d = open(path, 'rb').read()
    magic, ver, bios, entry, cgh, abi, cgv, nsec, _ = struct.unpack_from('<9I', d, 0)
    if magic != 0x50535842:
        raise SystemExit('%s: not a .pst (magic %08X)' % (path, magic))
    secs, off = {}, 36
    for _ in range(nsec):
        tag, flags, ln = struct.unpack_from('<IIQ', d, off)
        off += 16
        pay = d[off:off + ln]
        off += ln
        if flags & 1:                       # BOOT_STATE_SEC_ZLIB
            pay = zlib.decompress(pay[4:])
        secs[tag] = pay
    return {'bios': bios, 'entry': entry, 'ver': ver, 'secs': secs}


def ram(st):  return st['secs'][BS_SEC_RAM]
def rd(r, a, n): return r[(a & 0x1FFFFF):(a & 0x1FFFFF) + n]
def u8(r, a):  return rd(r, a, 1)[0]
def u16(r, a): return struct.unpack('<H', rd(r, a, 2))[0]
def i16(r, a): return struct.unpack('<h', rd(r, a, 2))[0]
def u32(r, a): return struct.unpack('<I', rd(r, a, 4))[0]


def regs(st):
    b = st['secs'][BS_SEC_CPU]
    return list(struct.unpack_from('<32I', b, 0)), struct.unpack_from('<I', b, 128)[0]


class Exe:
    """The PS-EXE text, for deciding whether a stack word is a return address."""
    def __init__(self, path=EXE):
        d = open(path, 'rb').read()
        self.base, size = struct.unpack_from('<II', d, 0x18)
        self.text = d[0x800:0x800 + size]

    def word(self, a):
        o = a - self.base
        return None if o < 0 or o + 4 > len(self.text) else struct.unpack_from('<I', self.text, o)[0]

    def ret_kind(self, a):
        if not (self.base <= a < self.base + len(self.text)) or (a & 3):
            return None
        w = self.word(a - 8)
        if w is None:
            return None
        if (w >> 26) == 0x03:                       # jal
            return 'jal 0x%08X' % (((a - 8) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
        if (w & 0xFC00003F) == 0x00000009:          # jalr
            return 'jalr $%s' % REGN[(w >> 21) & 31]
        return None


def cmd_regs(path):
    st = load(path); g, pc = regs(st)
    print('%s  bios=%08X' % (os.path.basename(path), st['bios']))
    print('PC=%08X' % pc)
    for i in range(0, 32, 4):
        print('  ' + '  '.join('%-4s=%08X' % (REGN[i + j], g[i + j]) for j in range(4)))


def cmd_stack(path):
    st = load(path); r = ram(st); g, pc = regs(st); e = Exe()
    sp = g[29]
    print('%s\nPC=%08X RA=%08X SP=%08X' % (os.path.basename(path), pc, g[31], sp))
    for a in range(sp, 0x80200000, 4):
        v = u32(r, a)
        k = e.ret_kind(v)
        if k:
            print('  %08X (+%03X)  %08X  <- %s' % (a, a - sp, v, k))


def cmd_read(path, addr, ln=16):
    r = ram(load(path))
    b = rd(r, addr, ln)
    for i in range(0, len(b), 16):
        print('%08X  %s' % (addr + i, ' '.join('%02X' % x for x in b[i:i + 16])))


# The addresses that separate a frozen duel from a healthy one. See
# FREEZE_INVESTIGATION.md; 0x8009B0F4 bit 4 is the blocker, bit 10 is the gate
# that stops the release from ever running.
def cmd_freeze(paths):
    hdr = ('state', 'bios', 'B0F4', 'b4', 'b10', 'b19', 'B134', 'phase', 'mode', 'sub')
    print('%-34s %-8s %-8s %-3s %-3s %-3s %-8s %-5s %-4s %s' % hdr)
    for p in paths:
        st = load(p); r = ram(st)
        w = u32(r, 0x8009B0F4)
        print('%-34s %08X %08X %-3d %-3d %-3d %08X %-5d %02X   %02X %s' % (
            os.path.basename(p)[-34:], st['bios'], w,
            bool(w & 0x10), bool(w & 0x400), bool(w & 0x80000),
            u32(r, 0x8009B134), i16(r, 0x800EADA0),
            u8(r, 0x8009B26C), u8(r, 0x8009B26E),
            '  <-- FROZEN' if (w & 0x02000030) | u32(r, 0x8009B134) else ''))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    c = sys.argv[1]
    if   c == 'regs':   cmd_regs(sys.argv[2])
    elif c == 'stack':  cmd_stack(sys.argv[2])
    elif c == 'read':   cmd_read(sys.argv[2], int(sys.argv[3], 0),
                                 int(sys.argv[4], 0) if len(sys.argv) > 4 else 16)
    elif c == 'freeze': cmd_freeze(sys.argv[2:])
    else: raise SystemExit(__doc__)
