#!/usr/bin/env python3
"""goto_duel.py -- drive the DEBUG build from a cold boot into a Free Duel against Simon.

  python3 tools/goto_duel.py boot            cold boot -> loaded save -> FREE DUEL grid, savestate slot 8
  python3 tools/goto_duel.py duel [ids...]   from the grid (or slot 8): write the 40-card deck, start the duel
  python3 tools/goto_duel.py grid            load savestate slot 8 (the grid)

The deck is the 40 u16 at gDuel_awPlayerDeck (0x801D0200); it is written at the
pre-duel deck screen and again right after the duel is confirmed. Only RAM is
touched, never the save.
"""
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_probe as p

GRID_SLOT = 8

def wait_boot(limit=300):
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            f = p.frame()
            if f and f > 8400: return f
        except Exception:
            pass
        time.sleep(2)
    raise SystemExit('never booted')

def boot():
    wait_boot(); time.sleep(3)
    p.press('start', 60, 3.0)
    p.press('down', 20, 1.0)
    p.press('cross', 20, 2.0)
    p.press('cross', 20, 4.0)
    p.press('cross', 20, 3.0)          # CAMPAIGN menu
    p.press('down', 20, 1.0)
    p.press('cross', 20, 4.0)          # FREE DUEL
    p.press('cross', 40, 2.0)          # clear the prompt -> grid, cursor on Build Deck
    print('grid frame', p.frame(), 'mode 0x%02X' % p.mode(), flush=True)
    p.q({'cmd': 'savestate', 'op': 'save', 'slot': GRID_SLOT}); time.sleep(3)

def grid():
    p.q({'cmd': 'savestate', 'op': 'load', 'slot': GRID_SLOT}); time.sleep(5)
    print('grid frame', p.frame(), 'mode 0x%02X' % p.mode(), flush=True)

def write_deck(ids):
    ids = (list(ids) * 40)[:40] if ids else None
    if not ids: return
    data = b''.join(struct.pack('<H', i) for i in ids)
    p.q({'cmd': 'write_mem', 'addr': '%08X' % 0x801D0200, 'hex': data.hex()})

def duel(ids):
    write_deck(ids)                    # at the grid: the deck view stages from here and commits on exit
    p.press('right', 6, 1.0)           # Build Deck -> Simon Muran (6 frames = one step)
    p.press('cross', 12, 3.0)          # deck view, mode 0xC3
    write_deck(ids)
    p.q({'cmd': 'press', 'buttons': 0xFFFF & ~p.B['circle'], 'frames': 12}); time.sleep(0.3)
    write_deck(ids)
    p.q({'cmd': 'clear_input'})
    for _ in range(40):
        time.sleep(1)
        if p.mode() == 0xC3 and struct.unpack('<H', p.rd(0x800EA004, 2))[0] == 8000:
            break
    time.sleep(6)
    print('duel frame', p.frame(), 'mode 0x%02X' % p.mode(), 'LP', struct.unpack('<H', p.rd(0x800EA004, 2))[0], flush=True)

if __name__ == '__main__':
    cmd = sys.argv[1]
    ids = [int(x) for x in sys.argv[2:]]
    if cmd == 'boot': boot()
    elif cmd == 'grid': grid()
    elif cmd == 'duel': duel(ids)
