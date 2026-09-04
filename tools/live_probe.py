#!/usr/bin/env python3
"""Live probe for the card-replacement survey. Drives the debug build over the
TCP debug server and decodes CD DMA transfers into card records."""
import os, sys, time, json
# live_probe.py: drive the DEBUG build (./Play.sh -dbg) over the TCP debug server and decode CD reads.
# Usage: python3 tools/live_probe.py wait|frame|press <btn[:frames]>...|shot <tag>|log|rd <hex> <n>|raw <json>
sys.path.insert(0, os.path.expanduser('~/YuGiOhForbiddenMemoriesRecomp/psxrecomp/tools'))
import debug_client as dc
H, P = '127.0.0.1', 4370
SHOT = os.environ.get('SHOTDIR', '/tmp')
B = dict(select=0x0001, start=0x0008, up=0x0010, right=0x0020, down=0x0040,
         left=0x0080, l2=0x0100, r2=0x0200, l1=0x0400, r1=0x0800,
         triangle=0x1000, circle=0x2000, cross=0x4000, square=0x8000)

def q(c):
    return dc.query(H, P, c)

def frame():
    return q({'cmd': 'frame'}).get('frame')

def press(name, frames=12, settle=1.2):
    q({'cmd': 'press', 'buttons': 0xFFFF & ~B[name], 'frames': frames})
    time.sleep(0.5)
    q({'cmd': 'clear_input'})
    time.sleep(settle)

def rd(addr, n):
    r = q({'cmd': 'read_ram', 'addr': '%08X' % addr, 'len': n})
    h = r.get('data') or r.get('hex') or ''
    return bytes.fromhex(h)

def mode():
    return rd(0x8009B26C, 1)[0]

def shot(tag):
    p = '%s/shot_%s.png' % (SHOT, tag)
    q({'cmd': 'screenshot', 'path': p})
    time.sleep(0.3)
    return p

WA = 10102
def decode_lba(lba):
    s = lba - WA
    if 0 <= s < 722:
        return 'WA sec %d = THUMB card %d' % (s, s + 1)
    if 722 <= s < 722 + 7 * 722:
        rec = (s - 722) // 7
        return 'WA sec %d = ART REC card %d +%d' % (s, rec + 1, (s - 722) % 7)
    if lba >= 28534:
        k = (lba - 28534) // 276
        if k < 722:
            return 'MODEL block card %d +%d' % (k + 1, (lba - 28534) % 276)
    if s >= 0:
        return 'WA sec 0x%X (%d)' % (s, s)
    return 'disc'

def readlog(tail=64, since=0):
    r = q({'cmd': 'cd_read_log', 'tail': tail})
    total = r.get('total', 0)
    ents = r.get('entries', [])
    base = total - len(ents)
    out = []
    for i, e in enumerate(ents):
        idx = base + i
        if idx < since:
            continue
        out.append('%6d lba %6d -> %s size %6d  %s' % (idx, e['lba'], e['dest'], e['size'], decode_lba(e['lba'])))
    return total, out

def sechist(tail=80):
    r = q({'cmd': 'cdrom_sector_history', 'tail': tail})
    return r

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'wait':
        t0 = time.time()
        while time.time() - t0 < 90:
            try:
                f = frame()
                if f and f > 300:
                    print('booted frame', f); break
            except Exception:
                pass
            time.sleep(0.5)
    elif cmd == 'frame':
        print(frame(), 'mode 0x%02X' % mode())
    elif cmd == 'press':
        for spec in sys.argv[2:]:
            name, _, fr = spec.partition(':')
            press(name, int(fr) if fr else 12)
            print(name, frame(), 'mode 0x%02X' % mode(), flush=True)
    elif cmd == 'shot':
        print(shot(sys.argv[2]))
    elif cmd == 'log':
        tail = int(sys.argv[2]) if len(sys.argv) > 2 else 64
        since = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        total, out = readlog(tail, since)
        print('total', total)
        print('\n'.join(out))
    elif cmd == 'total':
        print(q({'cmd': 'cd_read_log', 'tail': 0}).get('total'))
    elif cmd == 'digits':
        # write the 8 password digits then press X
        digs = [int(c) for c in sys.argv[2]]
        q({'cmd': 'write_mem', 'addr': '%08X' % 0x8016D410, 'hex': bytes(digs).hex()})
        print(rd(0x8016D410, 8).hex())
    elif cmd == 'rd':
        print(rd(int(sys.argv[2], 16), int(sys.argv[3])).hex())
    elif cmd == 'wr':
        # wr <hexaddr> <hexbytes>
        print(q({'cmd': 'write_mem', 'addr': sys.argv[2], 'hex': sys.argv[3]}))
    elif cmd == 'raw':
        print(json.dumps(q(json.loads(sys.argv[2])))[:4000])

def ranges(h0, limit=1500):
    """New sector reads since history total h0, compressed into LBA runs."""
    r = q({'cmd':'cdrom_sector_history','count':limit})
    ents = sorted([e for e in r['entries'] if e['seq'] >= h0], key=lambda e: e['seq'])
    runs = []
    for e in ents:
        if runs and e['lba'] == runs[-1][1] + 1:
            runs[-1][1] = e['lba']; runs[-1][3] = e['frame']
        else:
            runs.append([e['lba'], e['lba'], e['frame'], e['frame']])
    for a, b, f0, f1 in runs:
        print('  lba %6d..%6d (%3d)  frames %d..%d  %s -> %s' % (a, b, b - a + 1, f0, f1, decode_lba(a), decode_lba(b)))
    print('  (total %d, new %d)' % (r['total'], len(ents)))
    return r['total']
