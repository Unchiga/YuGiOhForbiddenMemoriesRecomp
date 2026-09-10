#!/usr/bin/env python3
"""netplay_scenario.py -- a whole 2P session on the loopback pair, scripted.

    python3 tools/netplay_scenario.py [rollback] [latency_ms] [jitter_ms]

Seeds the cards, launches both instances (delay-sync by default; `rollback`
switches PSX_NET_MODE and the optional latency/jitter engage recomp-net's
receive-side link simulator on BOTH peers, so the added RTT is twice the
latency), then plays: 2P DUEL with both LP at 1, P1 summons, P2 sets, P1 sets,
P2 summons and attacks P1's monster (P1 loses), the winner leaves the results,
and the host opens TRADE from the menu. Every step checks the duel state it
expects and screenshots both peers, so a hang shows up as a timeout with the
screens beside it. Same hands every run: the game seeds its RNG with a
constant, and the cards are re-seeded from the personal card 1 each time.
"""
import os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netplay_pair as np

LOG = None


def log(*a):
    s = time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a)
    print(s, flush=True)
    if LOG:
        LOG.write(s + '\n'); LOG.flush()


def phase(H):
    return H.h(0x8009B23A) & 0xF


def state(H):
    return 'side %s phase %s sub %02x mode %02x LP %s/%s' % (
        H.b(0x8009B1D5), phase(H), H.b(0x8009B174), H.mode(), H.h(0x800EA004), H.h(0x800EA024))


def wait_for(H, pred, what, limit=60):
    t0 = time.time()
    while time.time() - t0 < limit:
        if pred():
            log('  ok:', what, '|', state(H))
            return True
        time.sleep(1)
    log('  TIMEOUT:', what, '|', state(H))
    return False


def shots(tag):
    return [np.inst(0).shot(tag), np.inst(1).shot(tag)]


def main():
    global LOG
    rollback = 'rollback' in sys.argv[1:]
    nums = [a for a in sys.argv[1:] if a.isdigit()]
    lat = int(nums[0]) if nums else 0
    jit = int(nums[1]) if len(nums) > 1 else 0
    os.makedirs(np.ROOT, exist_ok=True)
    LOG = open(os.path.join(np.ROOT, 'scenario.log'), 'w')
    env = {}
    if rollback:
        env['PSX_NET_MODE'] = 'rollback'
    if lat:
        env['RNET_SIM_LATENCY_MS'] = str(lat)
        env['RNET_SIM_JITTER_MS'] = str(jit)
    log('scenario rollback=%s latency=%s jitter=%s' % (rollback, lat, jit))
    np.cards()
    np.start(env_extra=env)
    log('boot', np.wait_boot())
    H, G = np.inst(0), np.inst(1)

    log('rules mode %02x' % np.to_2p_rules())
    for _ in range(20): H.press('left', 10, 0.6)
    for _ in range(20): G.press('left', 10, 0.6)
    shots('s_rules')
    H.press('start', 20, 2.0)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 0, 'P1 hand up', 90)
    time.sleep(3)
    log('LP', H.h(0x800EA004), H.h(0x800EA024))
    shots('s_duel0')

    # P1: summon hand card 0 face-up
    H.press('cross', 6, 1.5); H.press('right', 6, 1.0); H.press('cross', 6, 3.0); H.press('cross', 6, 3.0); H.press('cross', 6, 5.0)
    wait_for(H, lambda: phase(H) == 5, 'P1 placed', 30)
    shots('s_p1_placed')
    H.press('start', 12, 1.0)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 1, 'P2 hand up', 90)
    time.sleep(3)
    shots('s_p2_turn')
    if os.environ.get('NETPLAY_SCENARIO_STOP') == 'p2turn':
        G.press('triangle', 6, 2.0); shots('s_p2_tri'); G.press('circle', 6, 1.0)
        for i in (H, G):
            i.q({'cmd': 'quit_graceful'})
        log('stopped after the P2 turn shots'); return

    # P2: set hand card 0 face-down (the extra cross leaves the placement view)
    G.press('cross', 6, 1.5); G.press('cross', 6, 3.0); G.press('cross', 6, 5.0)
    wait_for(H, lambda: phase(H) in (5, 8), 'P2 placed', 30)
    if phase(H) == 8:
        G.press('cross', 6, 4.0)
    wait_for(H, lambda: phase(H) == 5, 'P2 field cursor', 30)
    G.press('start', 12, 1.0)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 0, 'P1 hand up (2)', 90)
    time.sleep(3)

    # P1: set hand card 0 face-down
    H.press('cross', 6, 1.5); H.press('cross', 6, 3.0); H.press('cross', 6, 5.0)
    wait_for(H, lambda: phase(H) in (5, 8), 'P1 placed (2)', 30)
    if phase(H) == 8:
        H.press('cross', 6, 4.0)
    wait_for(H, lambda: phase(H) == 5, 'P1 field cursor (2)', 30)
    H.press('start', 12, 1.0)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 1, 'P2 hand up (2)', 90)
    time.sleep(3)

    # P2: summon face-up, then attack P1's face-up monster
    G.press('cross', 6, 1.5); G.press('right', 6, 1.0); G.press('cross', 6, 3.0); G.press('cross', 6, 3.0); G.press('cross', 6, 5.0)
    wait_for(H, lambda: phase(H) == 5, 'P2 placed (2)', 30)
    G.press('cross', 6, 4.0)                       # leave the placement view
    G.press('cross', 6, 2.0)                       # select the monster: attack target mode
    rows = H.rd(0x801A7AD8, 0x1C * 30)
    target = None
    for i in range(5, 15):
        r = rows[i * 0x1C:(i + 1) * 0x1C]
        fl = struct.unpack('<H', r[0x16:0x18])[0]
        if (fl & 0x8000) and not (fl & 0x1000):
            target = struct.unpack('<h', r[0xC:0xE])[0]
    log('attack target card', target, state(H))
    found = False
    for _ in range(12):
        if struct.unpack('<h', H.rd(0x8009B338, 2))[0] == target:
            found = True; break
        G.press('right', 6, 0.8)
    log('target under cursor', found)
    shots('s_target')
    G.press('cross', 6, 1.0)
    wait_for(H, lambda: phase(H) == 0xD, 'duel over (phase D)', 90)
    time.sleep(4)
    shots('s_results')
    # The winner's X leaves the results; which seat won depends on the deal.
    G.press('cross', 12, 3.0)
    if H.mode() != 0xC8:
        H.press('cross', 12, 3.0)
    wait_for(H, lambda: H.mode() == 0xC8, 'back at the main menu', 60)
    shots('s_menu')

    # TRADE right after the duel
    H.press('down', 20, 1.0)
    H.press('cross', 20, 3.0)
    shots('s_trade_prompt')
    H.press('cross', 20, 2.0)
    ok = wait_for(H, lambda: H.mode() == 0xCE, 'trade screen (mode CE)', 60)
    for k in range(4):
        time.sleep(4)
        shots('s_trade%d' % k)
        log('trade watch', k, state(H), 'frames', H.frame(), G.frame())
    if ok:
        H.press('cross', 12, 2.5)                  # dismiss the instruction box
        shots('s_trade_after_hint')
        time.sleep(6)
        shots('s_trade_after_hint2')
        # A full trade: each side marks its first card and settles, the host
        # executes. This writes both cards, which is what the carry-back at
        # shutdown has to bring home.
        # Asymmetric on purpose (P1 gives its card 001, P2 its card 002): a
        # symmetric swap of the same card leaves both saves byte-identical.
        H.press('cross', 12, 1.5); G.press('down', 6, 1.0); G.press('cross', 12, 1.5)
        H.press('square', 12, 2.0); G.press('square', 12, 2.5)
        shots('s_trade_menu')
        H.press('cross', 12, 3.0)                  # EXECUTE TRADE
        time.sleep(12)
        shots('s_trade_done')
        H.press('cross', 12, 3.0)                  # dismiss TRADE COMPLETE
        log('trade executed', state(H))
    # Leave through the runtime's own shutdown (SIGTERM would skip it), so the
    # netplay sandboxes are torn down and the cards carried back; then show
    # what happened to every card file.
    import hashlib
    def digest(path):
        try:
            return hashlib.sha1(open(path, 'rb').read()).hexdigest()[:10]
        except OSError:
            return 'missing'
    before = {n: {f: digest(os.path.join(np.inst(k).dir, f)) for f in ('card1.mcd', 'card2.mcd')}
              for k, n in np.NAMES.items()}
    for i in (H, G):
        i.q({'cmd': 'quit_graceful'})
    time.sleep(8)
    for k, n in np.NAMES.items():
        d = np.inst(k).dir
        after = {f: digest(os.path.join(d, f)) for f in ('card1.mcd', 'card2.mcd')}
        log(n, 'cards before', before[n], 'after', after,
            'backups', sorted(f for f in os.listdir(d) if 'pre-netplay' in f))
        for l in np.inst(k).grep('netplay'):
            if 'card' in l and 'live dig' not in l:
                log('  ', n, l.strip())
    log('done')


if __name__ == '__main__':
    main()
