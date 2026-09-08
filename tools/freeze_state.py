#!/usr/bin/env python3
"""freeze_state.py -- read a freeze savestate without launching the game.

    python3 tools/freeze_state.py <state_800129D8_slotN.pst> [more.pst ...]

Prints, for each .pst a tester posted beside freeze_report.txt: which build
marks the RAM carries, the duel's blocker words, the CD transfer descriptor
the effect scripts wait on, and the CD controller's own state (the pending
and queued commands, the response FIFO, what it was reading). Everything
here is what tools/LIVE_TESTING.md section 16 tabulates, so a new report
can be classified in a minute and the game only has to be launched for the
states that need a live look.

The file is psxrecomp's boot_state v5: a 36-byte header, then sections
(tag, flags, u64 length, payload; flag bit 0 = u32 raw length + zlib).
"""
import struct, sys, zlib

SEC_RAM, SEC_IRQ, SEC_CLOCK, SEC_CDROM = 2, 4, 6, 0x0B
WHOLE_SECTOR = 2340   # RAW_SECTOR_SIZE - WHOLE_SECTOR_OFFSET


def load(path):
    d = open(path, 'rb').read()
    magic, ver, bios, entry, cg, abi, cgv, nsec, _ = struct.unpack('<9I', d[:36])
    if magic != 0x50535842:
        raise SystemExit('%s: not a psxrecomp state (magic %08x)' % (path, magic))
    off = 36
    secs = {}
    for _ in range(nsec):
        tag, flags, ln = struct.unpack('<IIQ', d[off:off + 16])
        off += 16
        pl = d[off:off + ln]
        off += ln
        if flags & 1:
            pl = zlib.decompress(pl[4:])
        secs[tag] = pl
    return dict(version=ver, bios=bios, entry=entry, codegen=cg, abi=abi, cgv=cgv), secs


class Rd:
    def __init__(self, b): self.b, self.o = b, 0
    def u8(self): v = self.b[self.o]; self.o += 1; return v
    def i32(self): v = struct.unpack_from('<i', self.b, self.o)[0]; self.o += 4; return v
    def u32(self): v = struct.unpack_from('<I', self.b, self.o)[0]; self.o += 4; return v
    def u64(self): v = struct.unpack_from('<Q', self.b, self.o)[0]; self.o += 8; return v
    def skip(self, n): self.o += n
    def bytes(self, n): v = self.b[self.o:self.o + n]; self.o += n; return v


def cd_state(cd):
    """Mirror of cdrom_snap_emit() in psxrecomp/runtime/src/cdrom.c (v5 wire)."""
    r = Rd(cd); d = {}
    d['index'], d['stat'], d['request'], d['irq_enable'], d['irq_flag'] = (r.u8() for _ in range(5))
    d['intc_latched'] = r.i32(); d['irq_gen'] = r.u32(); d['latched_gen'] = r.u32(); d['present_rem'] = r.i32()
    r.skip(16); d['param_count'] = r.i32()
    d['response'] = r.bytes(16); d['response_read'] = r.i32(); d['response_count'] = r.i32()
    r.skip(WHOLE_SECTOR); d['sector_read_pos'] = r.i32(); d['sector_available'] = r.i32(); d['sector_size'] = r.i32()
    r.skip(WHOLE_SECTOR); d['last_sector_lba'] = r.i32(); d['last_sector_size'] = r.i32()
    r.skip(12); d['subq_valid'] = r.i32()
    r.u32(); d['last_sector_mode'] = r.u8(); r.skip(6)
    d['seek'] = (r.u8(), r.u8(), r.u8()); d['setloc_lba'] = r.i32(); d['seek_far'] = r.i32()
    d['reading'] = r.i32(); m, s, f = r.i32(), r.i32(), r.i32(); d['read_lba'] = (m * 60 + s) * 75 + f - 150
    d['mode'] = r.u8(); d['read_cmd'] = r.u8(); d['read_delay'] = r.i32()
    d['filter'] = (r.u8(), r.u8()); d['muted'] = r.u8()
    r.i32(); r.i32(); r.u32(); r.i32(); r.i32(); r.u64()
    r.skip(16); r.skip(3); d['xa_active'] = r.i32()
    d['divisors'] = (r.i32(), r.i32(), r.i32())
    d['pending'] = dict(cmd=r.u8(), active=r.i32(), remaining=r.i32(), phase=r.i32())
    d['queued'] = dict(cmd=r.u8(), params=r.bytes(16), n=r.i32(), pending=r.i32())
    d['dataready_pended'] = r.u8()
    return d


def msf(b):
    """BCD mm:ss:ff -> LBA, the way CdPosToInt (0x8007E710) does it."""
    bcd = lambda x: (x >> 4) * 10 + (x & 15)
    return (bcd(b[0]) * 60 + bcd(b[1])) * 75 + bcd(b[2]) - 150


def report(path):
    hdr, secs = load(path)
    ram = secs[SEC_RAM]
    w = lambda a: struct.unpack_from('<I', ram, a & 0x1FFFFF)[0]
    h = lambda a: struct.unpack_from('<H', ram, a & 0x1FFFFF)[0]
    b = lambda a: ram[a & 0x1FFFFF]
    cyc = struct.unpack('<Q', secs[SEC_CLOCK])[0]
    i_stat, i_mask, csv = struct.unpack('<III', secs[SEC_IRQ][:12])
    print('== %s' % path)
    print('  header: state v%d bios %08X entry %08X codegen %08X' % (hdr['version'], hdr['bios'], hdr['entry'], hdr['codegen']))
    # build marks: 0.5.7 rewrote the duel's heal cap to addiu 9999 (psx_ygo_cheats.c)
    heal = w(0x80025188)
    print('  build marks: heal cap word %08X -> %s' % (heal, '0.5.7 or later (addiu 9999)' if heal == 0x2402270F else 'before 0.5.7 (stock lh)'))
    ours = sum(1 for cid in range(1, 723) if 0xEC00 <= h(0x801C0200 + cid * 2) < 0xFE00)
    print('  description table entries in the old 4608-byte gap: %d (a 0.5.7 whole-game package shows 68)' % ours)
    print('  guest clock: %d cycles = %.1f stock vblanks (compare with the report\'s guest frame: half means game speed 2x)' % (cyc, cyc / 564480.0))
    print('  blocker: busy %08X busy2 %08X  state index %u  flags %04X  phase %d  mode %02X sub %02X turn %u sel %u' % (
        w(0x8009B0F4), w(0x8009B134), h(0x8009B100), h(0x8009B112), struct.unpack('<h', ram[0xEADA0:0xEADA2])[0],
        b(0x8009B26C), b(0x8009B26E), b(0x8009B1D5), b(0x800E9F25)))
    d = 0x800E9E60
    print('  transfer: descriptor sector %d  polled position %d  end %d  state %u/%u  file/ch %u/%u  countdown %u  retries %u' % (
        w(d + 0x24), struct.unpack_from('<i', ram, (d + 0x30) & 0x1FFFFF)[0], struct.unpack_from('<i', ram, (d + 0x34) & 0x1FFFFF)[0],
        b(d + 0x46), b(d + 0x47), b(d + 0x39), b(d + 0x38), h(0x8009B0EC), w(0x8009B130)))
    cd = cd_state(secs[SEC_CDROM])
    print('  drive: stat %02X irq enable %02X flag %02X mode %02X i_stat %08X i_mask %08X present_rem %d' % (
        cd['stat'], cd['irq_enable'], cd['irq_flag'], cd['mode'], i_stat, i_mask, cd['present_rem']))
    print('         reading %d cmd %02X at %d (last sector %d, setloc %d, filter %u/%u, read delay %d, xa %d)' % (
        cd['reading'], cd['read_cmd'], cd['read_lba'], cd['last_sector_lba'], cd['setloc_lba'], cd['filter'][0], cd['filter'][1], cd['read_delay'], cd['xa_active']))
    p, q = cd['pending'], cd['queued']
    print('         pending %02X active %d remaining %d phase %d   queued %02X pending %d' % (p['cmd'], p['active'], p['remaining'], p['phase'], q['cmd'], q['pending']))
    resp = cd['response']
    print('         response fifo read %d of %d: %s   (bytes 0..2 as mm:ss:ff -> LBA %d; a later one-byte stat answer overwrites byte 0)' % (
        cd['response_read'], cd['response_count'], resp.hex(), msf(resp)))
    print('         sector buffer: available %d size %d pos %d' % (cd['sector_available'], cd['sector_size'], cd['sector_read_pos']))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for a in sys.argv[1:]:
        report(a)
