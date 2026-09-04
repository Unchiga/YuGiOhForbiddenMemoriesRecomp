#!/usr/bin/env python3
"""duel_turns.py -- play turns in a running Free Duel (debug build) and report board, LP and mod events.

  python3 tools/duel_turns.py <turns> <prefer ids...>
Each turn: summon the first preferred monster in hand (else a magic card from the
list into the magic zone), then START. Prints the field rows, the opponent's
monsters, both LP, and the monster_effects / card_effects events since the
previous turn. See the memory note ygofm-duel-controls for the controls.
"""
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_probe as p

MAGIC_IDS = set(range(301, 351)) | set(range(651, 701)) | {721}

def board():
    rows = p.rd(0x801A7AD8, 0x1C * 30); out = []
    for i in range(30):
        r = rows[i*0x1C:(i+1)*0x1C]; fl = struct.unpack('<H', r[0x16:0x18])[0]
        if fl & 0x8000: out.append((i,) + struct.unpack('<hhhhh', r[0xC:0x16]) + (hex(fl),))
    return out
def lp(): return struct.unpack('<H', p.rd(0x800EA004, 2))[0], struct.unpack('<H', p.rd(0x800EA024, 2))[0]
def side(): return p.rd(0x8009B1D5, 1)[0]
def hand(): return [(i, r[0]) for i, *r in board() if i < 5]
def field(): return [(r[0], r[1], r[4]) for r in board() if 5 <= r[0] < 10]
def opp(): return [(r[0], r[1], r[4]) for r in board() if 20 <= r[0] < 25]
def goto(slot):
    for _ in range(slot): p.press('right', 6, 0.8)
def summon(slot):
    goto(slot); p.press('cross', 6, 1.5); p.press('cross', 6, 3.0); p.press('cross', 6, 3.0); p.press('cross', 6, 5.0)
def set_magic(slot):
    goto(slot); p.press('cross', 6, 1.5); p.press('cross', 6, 2.0); p.press('down', 6, 1.0); p.press('cross', 6, 10.0)
def end_turn():
    p.press('start', 12, 1.0)
    for _ in range(40):
        time.sleep(1)
        if side() == 0: break
    time.sleep(6)

_seen = {'m': 0, 'c': 0}
def events():
    out = []
    m = p.q({'cmd': 'monster_effects'})
    new = [e for e in m['events'] if e['frame'] > _seen['m']]
    if m['events']: _seen['m'] = m['events'][-1]['frame']
    out += [(e['what'], e['a'], e['b'], e['c']) for e in new]
    c = p.q({'cmd': 'card_effects'})
    new = [e for e in c['events'] if e['frame'] > _seen['c']]
    if c['events']: _seen['c'] = c['events'][-1]['frame']
    out += [('fx@' + e['at'], e['a'], e['b'], e['out']) for e in new]
    return out

def report(tag):
    print(tag, 'field', field(), 'opp', opp(), 'LP', lp(), events(), flush=True)

def play_turn(prefer):
    h = hand()
    for want in prefer:
        s = [i for i, c in h if c == want]
        if not s: continue
        if want in MAGIC_IDS: set_magic(s[0]); return ('magic', want)
        if len(field()) < 5: summon(s[0]); return ('summon', want)
    return None

if __name__ == '__main__':
    turns = int(sys.argv[1]); prefer = [int(x) for x in sys.argv[2:]]
    events()
    for t in range(turns):
        print('turn', t, 'hand', hand(), flush=True)
        played = play_turn(prefer)
        if not played: print('  nothing to play'); break
        time.sleep(4); report('  played %s ->' % (played,))
        end_turn(); report('  opp turn ->')
