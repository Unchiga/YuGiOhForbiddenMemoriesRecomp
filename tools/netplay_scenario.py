#!/usr/bin/env python3
"""netplay_scenario.py -- a whole 2P session on the loopback pair, scripted.

    python3 tools/netplay_scenario.py [rollback] [latency_ms] [jitter_ms]

Seeds the cards from NETPAIR_SEED, launches both instances (delay-sync by default; `rollback`
switches PSX_NET_MODE and the optional latency/jitter engage recomp-net's
receive-side link simulator on BOTH peers, so the added RTT is twice the
latency), then plays: 2P DUEL with both LP at 1, P1 summons, P2 sets, P1 sets,
P2 summons and attacks P1's monster (P1 loses), the winner leaves the results,
and the host opens TRADE from the menu. Every step checks the duel state it
expects and screenshots both peers, so a hang shows up as a timeout with the
screens beside it. Same hands every run: the game seeds its RNG with a
constant, and the cards are re-seeded from the caller's isolated card image.
"""
import json, os, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netplay_pair as np

LOG = None
PRIVACY = []


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
    raise RuntimeError('timeout: ' + what)


def wait_battle(H, limit=90):
    """Wait for duel completion while retaining actionable freeze evidence."""
    t0 = time.time()
    last = None
    entered = False
    while time.time() - t0 < limit:
        now = (phase(H), H.b(0x8009B174), H.h(0x800EA004), H.h(0x800EA024))
        if now[0] == 0xD:
            log('  ok: duel over (phase D) |', state(H))
            return True
        if now[0] == 9:
            entered = True
        elif entered and now[0] == 5 and now[2:] == (1, 1):
            raise RuntimeError('battle completed with zero damage; fixture is not lethal')
        if now != last:
            log('  battle:', now, 'busy=%08x' % (H.w(0x8009B0F4) or 0),
                'stream=%s/%04x/%s' % (H.h(0x8009B100), H.h(0x8009B112) or 0,
                                       H.w(0x8009B0EC)))
            last = now
        time.sleep(1)
    shots('s_battle_timeout')
    log('  battle freeze_check', H.q({'cmd':'freeze_check','window':256}))
    log('  battle cdrom_state', H.q({'cmd':'cdrom_state'}))
    log('  battle dispatch_tail', H.q({'cmd':'dispatch_tail','count':32}))
    log('  battle savestate', H.q({'cmd':'savestate','op':'save','slot':11}))
    log('  TIMEOUT: duel over (phase D) |', state(H))
    raise RuntimeError('timeout: duel over (phase D)')


def shots(tag):
    return [np.inst(0).shot(tag), np.inst(1).shot(tag)]


def check_facedown_privacy(tag, owner):
    """Require only the face-down card's owner to receive its stock label."""
    time.sleep(1)
    states = [np.inst(slot).q({'cmd': 'netplay_privacy'}) for slot in (0, 1)]
    for slot, state_ in enumerate(states):
        expected_hidden = slot != owner
        if not state_.get('ok'):
            raise RuntimeError('%s privacy command failed on slot %d: %r' % (tag, slot, state_))
        if state_.get('phase') != 5 or state_.get('selected_owner') != owner:
            raise RuntimeError('%s cursor did not resolve owner %d on slot %d: %r' %
                               (tag, owner, slot, state_))
        if state_.get('selected_flags', 0) & 0x9000 != 0x9000:
            raise RuntimeError('%s selected card is not occupied face-down: %r' % (tag, state_))
        if bool(state_.get('field_hidden')) != expected_hidden:
            raise RuntimeError('%s wrong hidden policy on slot %d: %r' % (tag, slot, state_))
        if expected_hidden and not state_.get('cover_present'):
            raise RuntimeError('%s missing present-time cover on slot %d: %r' % (tag, slot, state_))
    if states[0].get('selected_card') != states[1].get('selected_card'):
        raise RuntimeError('%s peers disagree on selected card: %r' % (tag, states))
    record = {'tag': tag, 'owner': owner, 'peers': states}
    PRIVACY.append(record)
    log('privacy', json.dumps(record, sort_keys=True))
    shots(tag)


def end_turn(player, host):
    # A confirmation may already have selected the summoned monster by the
    # time its animation settles. START is ignored in attack-target mode.
    if phase(host) == 5 and host.b(0x8009B174) & 0x7f == 6:
        player.press('circle', 6, 1.5)
    player.press('start', 12, 1.0)


def place_selected_hand_card(player, host, face_direction=None, tag='placement'):
    """Drive the placement state machine without wall-clock assumptions.

    Selecting a hand card remains in phase 4 but advances substate 81 -> 83.
    Only then does left/right choose face-down/up. The first confirmation
    advances to stable substate 84; the next passes through the transient
    phase-7 animation and settles at phase 8, then the last returns to the
    phase-5 field cursor. Waiting for stable edges keeps loss/rollback stalls
    from redirecting an input into the preceding screen.
    """
    player.press('cross', 6, 1.0)
    wait_for(host, lambda: phase(host) == 4 and
             (host.b(0x8009B174) & 0x7f) == 3,
             tag + ' hand selected', 30)
    if face_direction:
        player.press(face_direction, 6, 1.0)
    player.press('cross', 6, 1.0)
    wait_for(host, lambda: phase(host) == 4 and
             (host.b(0x8009B174) & 0x7f) == 4,
             tag + ' choice confirmed', 30)
    player.press('cross', 6, 1.0)
    wait_for(host, lambda: phase(host) == 8, tag + ' confirm', 30)
    player.press('cross', 6, 1.0)
    wait_for(host, lambda: phase(host) == 5, tag + ' field cursor', 30)


# The two Guardian Star wheels. A star beats the value to its right here.
# These ids are the packed card-stat ids documented in psx_card_extend.c.
GS_BEATS = {8: 9, 9: 10, 10: 7, 7: 8,
            1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 6: 1}


def guardian_attack(atk, attacker, defender):
    if GS_BEATS.get(attacker) == defender:
        return atk + 500
    if GS_BEATS.get(defender) == attacker:
        return max(0, atk - 500)
    return atk


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
    place_selected_hand_card(H, H, 'right', 'P1 placement')
    shots('s_p1_placed')
    end_turn(H, H)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 1, 'P2 hand up', 90)
    time.sleep(3)
    shots('s_p2_turn')
    if os.environ.get('NETPLAY_SCENARIO_STOP') == 'p2turn':
        G.press('triangle', 6, 2.0); shots('s_p2_tri'); G.press('circle', 6, 1.0)
        for i in (H, G):
            i.q({'cmd': 'quit_graceful'})
        log('stopped after the P2 turn shots'); return

    # P2: set hand card 0 face-down. This player's untouched placement choice
    # starts on face-down; the extra cross leaves the placement view.
    place_selected_hand_card(G, H, None, 'P2 placement')
    check_facedown_privacy('s_p2_facedown', 1)
    end_turn(G, H)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 0, 'P1 hand up (2)', 90)
    time.sleep(3)

    # P1: likewise choose face-down explicitly; this menu can retain the
    # face-up side selected by P1's previous summon.
    place_selected_hand_card(H, H, 'left', 'P1 placement (2)')
    check_facedown_privacy('s_p1_facedown', 0)
    with open(os.path.join(np.ROOT, 'privacy-results.json'), 'w') as f:
        json.dump({'passed': True, 'checks': PRIVACY}, f, indent=2, sort_keys=True)
        f.write('\n')
    if os.environ.get('NETPLAY_SCENARIO_STOP') == 'privacy':
        for i in (H, G):
            i.q({'cmd': 'quit_graceful'})
        log('stopped after face-down privacy checks'); return
    end_turn(H, H)
    wait_for(H, lambda: phase(H) == 4 and H.b(0x8009B1D5) == 1, 'P2 hand up (2)', 90)
    time.sleep(3)

    # P2: summon face-up. Pick a hand card whose default Guardian Star is
    # guaranteed to deal battle damage to P1's face-up monster. Always using
    # hand slot 0 can create a perfectly legal zero-damage tie for some seeds.
    rows = H.rd(0x801A7AD8, 0x1C * 30)
    defender_row = next((i for i in range(5, 10)
                         if struct.unpack('<H', rows[i * 0x1C + 0x16:i * 0x1C + 0x18])[0] & 0x8000
                         and not struct.unpack('<H', rows[i * 0x1C + 0x16:i * 0x1C + 0x18])[0] & 0x1000), None)
    if defender_row is None:
        raise RuntimeError('missing face-up defender')
    dr = rows[defender_row * 0x1C:(defender_row + 1) * 0x1C]
    defender, datk, ddef, _, _, dflags = struct.unpack('<hhhhhH', dr[0xC:0x18])
    dstat = H.w(0x801D4244 + (defender - 1) * 4)
    dgs = (dstat >> 22) & 15
    plans=[]
    for slot in range(5):
        hr = rows[(15 + slot) * 0x1C:(16 + slot) * 0x1C]
        attacker, _, _, _, _, hflags = struct.unpack('<hhhhhH', hr[0xC:0x18])
        if not (hflags & 0x8000) or attacker <= 0:
            continue
        astat = H.w(0x801D4244 + (attacker - 1) * 4)
        atk, ags = (astat & 0x1FF) * 10, (astat >> 22) & 15
        effective = guardian_attack(atk, ags, dgs)
        defended = bool(dflags & 0x0800)
        damage = max(0, ddef - effective) if defended else abs(effective - datk)
        plans.append((damage, slot, attacker, atk, ags, effective))
    damage, hand_slot, attacker, atk, ags, effective = max(plans, default=(0, 0, 0, 0, 0, 0))
    if damage <= 0:
        raise RuntimeError('isolated deal has no damaging default-star attacker')
    log('battle plan attacker', attacker, 'slot', hand_slot, 'defender', defender,
        'ATK', atk, 'star', ags, 'vs', dgs, 'effective', effective, 'damage', damage)
    for _ in range(hand_slot):
        G.press('right', 6, .8)
    place_selected_hand_card(G, H, 'right', 'P2 placement (2)')
    placed = H.rd(0x801A7AD8, 0x1C * 30)
    log('battle rows', [tuple(struct.unpack('<hhhhhH',
         placed[i * 0x1C + 0xC:i * 0x1C + 0x18])) for i in range(30)
         if struct.unpack('<H', placed[i * 0x1C + 0x16:i * 0x1C + 0x18])[0] & 0x8000])
    if not any(struct.unpack('<h', placed[i * 0x1C + 0xC:i * 0x1C + 0xE])[0] == attacker
               and (struct.unpack('<H', placed[i * 0x1C + 0x16:i * 0x1C + 0x18])[0]
                    & 0x9000) == 0x8000 for i in range(20, 25)):
        raise RuntimeError('planned attacker was not summoned')
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
    if not found:
        raise RuntimeError('attack target was not selected')
    shots('s_target')
    G.press('cross', 6, 1.0)
    wait_battle(H, 90)
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
        backups = sorted(f for f in os.listdir(d) if 'pre-netplay' in f)
        log(n, 'cards before', before[n], 'after', after,
            'backups', backups)
        for l in np.inst(k).grep('netplay'):
            if 'card' in l and 'live dig' not in l:
                log('  ', n, l.strip())
        if after['card1.mcd'] == before[n]['card1.mcd'] or not backups:
            raise RuntimeError('%s trade was not carried back with a backup' % n)
    log('done')


if __name__ == '__main__':
    try:
        main()
    finally:
        np.stop()
