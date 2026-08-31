"""Pad driving + state peeks for the crash repro. Active-low buttons."""
import sys, os, time
sys.path.insert(0, r'C:\dev\memories\YuGiOhForbiddenMemoriesRecomp\tools')
import dbg

B = dict(select=0x0001, start=0x0008, up=0x0010, right=0x0020, down=0x0040,
         left=0x0080, l2=0x0100, r2=0x0200, l1=0x0400, r1=0x0800,
         triangle=0x1000, circle=0x2000, cross=0x4000, square=0x8000)

def press(*names, frames=8, settle=0.5):
    mask = 0
    for n in names:
        mask |= B[n]
    word = 0xFFFF & ~mask
    r = dbg.q({'cmd': 'press', 'buttons': word, 'frames': frames})
    time.sleep(settle)
    return r

ARENA = 0x80100000
N = 790
D = N - 722
S = 16 * D
SH_TAIL = 2 * S + 3 * D
TAIL = ARENA + 0x633E + SH_TAIL          # u16 state
NEW_STRIDE = (0x6344 + SH_TAIL + 15) & ~15

def u16(a):
    h = dbg.rd(a, 2)
    return int.from_bytes(bytes.fromhex(h[:4]), 'little') if h else None

def tail():
    return (u16(TAIL), u16(TAIL + 2))

def shot(name):
    p = r'C:\Users\Unchiga\AppData\Local\Temp\claude\C--dev-tools\fd616076-7221-4139-80d4-ddbc7c1c3ee3\scratchpad' + '\\' + name
    print(dbg.q({'cmd': 'screenshot_present', 'path': p}))
    return p

if __name__ == '__main__':
    for arg in sys.argv[1:]:
        if arg == 'tail':
            print('tail', tail())
        elif arg == 'frame':
            print(dbg.frame())
        elif arg.startswith('shot:'):
            shot(arg[5:])
        else:
            press(*arg.split('+'))
            print(arg, '->', 'tail', tail())
