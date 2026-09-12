#!/usr/bin/env python3
"""Live Card Shop -> Sell UI capture.

Boots the debug build in a fresh portable profile (a COPY of the given memory
card, never the player's own folder), walks into the real Card Shop with
controller presses, and captures every Sell screen the UI can show: empty
trunk, quantities 1/10/99/255, long names, derived and override prices, zero
value, six-digit prices and totals, cap loss, confirmation, GO BACK, zero
selection, stale transaction, cancellation, a completed sale, the 722-card
scroll case, and a keyboard pass through ydotool. Every capture is a composed
`present_shot` plus an exact 320x240 native downscale of the game area.
Reuses sell_regression's Runner/route helpers so process ownership, ports and
graceful shutdown follow the same rules.

    python3 tools/sell_ui_capture.py --card /path/to/card1.mcd --output /tmp/ygofm-sell-ui-<date>
"""
import argparse, json, os, shutil, struct, subprocess, sys, time, hashlib
from pathlib import Path
from PIL import Image
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import sell_regression as sr

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--card', type=Path, required=True, help='authorized FM card1.mcd; copied, never used in place')
ap.add_argument('--output', type=Path, required=True, help='new evidence directory below /tmp')
ap.add_argument('--exe', type=Path, default=ROOT / 'build-dbg' / 'Yu_Gi_Oh_Forbidden_Memories_Recompiled')
ap.add_argument('--disc', type=Path, default=ROOT / 'disc' / 'Yu-Gi-Oh! Forbidden Memories (USA).cue')
args = ap.parse_args()
OUT = args.output.resolve()
if not OUT.is_relative_to(Path('/tmp')): raise SystemExit('--output must be a new directory below /tmp')
OUT.mkdir(parents=True, exist_ok=False)
SOURCE_CARD = args.card.resolve(); EXE = args.exe.resolve(); DISC = args.disc.resolve()
shots = OUT / 'screenshots'; shots.mkdir()
player = OUT / 'player'; player.mkdir()
shutil.copy2(SOURCE_CARD, player / 'card1.mcd')
(player / 'menu_settings.ini').write_text('card_shop=1\n')
for card, price in {108: 999999, 5: 0, 24: 17, 298: 123456}.items():
    d = player / 'cards' / str(card); d.mkdir(parents=True)
    (d / 'card.ini').write_text(f'sell_price = {price}\n')

ev = sr.Evidence(OUT / 'results.json', {
    'purpose': 'sell-ui-polish-captures', 'title_head': subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
    'executable': str(EXE), 'executable_sha256': sr.sha256_file(EXE), 'disc': str(DISC),
    'source_card': str(SOURCE_CARD), 'source_card_sha256': sr.sha256_file(SOURCE_CARD), 'renderer': 'software'})
ev.data['shots'] = {}
r = sr.Runner(EXE, DISC, player, ev, 'ui', shots)

def shot(tag):
    info = r.composed_shot(f'ui-{tag}')
    im = Image.open(info['path']); w, h = im.size
    # Game area is the window minus the host menu bar; native is an exact 1/3.
    top = h - 720 if (w, h) == (960, 755) else 0
    native = im.crop((0, top, w, h)).resize((320, 240), Image.NEAREST)
    npath = shots / f'ui-{tag}-native.png'; native.save(npath)
    info['native'] = {'path': str(npath), 'sha256': sr.sha256_file(npath), 'size': im.size}
    ev.data['shots'][tag] = info; ev.flush()
    return info

def state(): return r.query('card_shop_sell')
def setq(card, qty): return r.query('card_shop_sell', op='set', card=card, quantity=qty)
def entry(s, card): return next(row for row in s['entries'] if row['id'] == card)
def press(b, frames=12, settle=0.8): r.press(b, frames, settle)
def key(code):
    """Keyboard-equivalent input through ydotool (Linux keycode), if focused."""
    subprocess.run(['ydotool', 'key', '-d', '120', f'{code}:1', f'{code}:0'], check=True); time.sleep(0.8)

FIX_TRUNK = {1:3, 4:3, 5:4, 11:1, 24:10, 104:2, 108:10, 114:3, 148:99, 195:1, 229:255, 232:2, 298:99}
def fixture(chips, trunk_map=None):
    tm = FIX_TRUNK if trunk_map is None else trunk_map
    deck = sr.deterministic_deck({104:1, 114:1, 195:2, 232:2, 229:3})
    trunk = bytearray(722)
    for c, n in tm.items(): trunk[c-1] = n
    sr.install_inventory(r, deck, bytes(trunk), chips)

try:
    r.start()
    ev.data['cases']['title_difference'] = sr.route_loaded_menu(r)
    # Loaded menu -> CAMPAIGN -> shopkeeper -> CARD SHOP -> pack panel.
    press('cross', 20, 6.0); press('cross', 20, 3.0); press('down', 20, 1.5); press('cross', 20, 3.0); press('cross', 20, 3.0)
    sh = r.query('card_shop'); ev.check('pack panel open', bool(sh.get('open')), sh)
    shot('pack-panel')

    # A. empty trunk
    fixture(77, {})
    press('triangle'); s = state(); ev.check('empty preview', s['active'] == 1 and s['types'] == 0, s)
    shot('empty-trunk'); press('cross'); shot('empty-trunk-cross'); press('circle'); shot('empty-trunk-cancelled')
    ev.check('empty circle closes sell', not state()['active'])

    # B. normal quantity selection with controller-equivalent presses
    fixture(999000)
    press('triangle'); s = state(); ev.check('editor open', s['active'] == 1 and s['types'] == len(FIX_TRUNK), s)
    ev.check('effective resale respects pack and direct-price ceilings',
             (entry(s, 1)['sell_price'], entry(s, 11)['sell_price'], entry(s, 4)['sell_price']) == (250, 25, 6),
             {card: entry(s, card)['sell_price'] for card in (1, 11, 4)})
    shot('editor-initial')
    press('right'); s = state(); ev.check('right = +1 on first row', s['entries'][0]['sell'] == 1, s['entries'][0]); shot('qty-1')
    press('left'); ev.check('left = -1', state()['entries'][0]['sell'] == 0)
    setq(24, 10); shot('qty-10')
    setq(148, 99); shot('qty-99')
    start_sel = state()['selected']; press('r1'); s = state()
    ev.check('R1 skips forward ten cards', s['selected'] == (start_sel + 10) % s['types'], s['selected']); shot('r1-skip-10')
    press('l1'); ev.check('L1 skips back ten cards', state()['selected'] == start_sel)
    setq(229, 0)
    press('square'); s = state(); ev.check('square = max', entry(s, 229)['sell'] == 255, entry(s, 229)); shot('qty-255')
    setq(108, 0); shot('long-name-override')
    setq(298, 5); shot('mixed-prices-six-digit')
    setq(5, 4); shot('zero-value-row')
    press('down'); press('down'); shot('cursor-down-twice')
    press('up'); press('up'); press('up'); shot('cursor-wrap-up')
    press('start'); s = state(); ev.check('start = all', s['copies'] == sum(FIX_TRUNK.values()), s['copies'])
    ev.check('cap loss present', s['gross'] > s['credit'], {'gross': s['gross'], 'credit': s['credit']})
    shot('cap-loss'); press('cross'); ev.check('cross = review', state()['active'] == 2); shot('confirm-cap-loss')
    press('circle'); s = state(); ev.check('circle from confirm keeps quantities', s['active'] == 1 and s['copies'] == sum(FIX_TRUNK.values()), s['copies'])
    shot('back-from-confirm')
    press('l2'); ev.check('L2 = clear', state()['copies'] == 0); press('cross'); ev.check('cross with nothing selected stays in editor', state()['active'] == 1)
    shot('zero-selection')
    press('r2'); ev.check('R2 = all', state()['copies'] == sum(FIX_TRUNK.values())); press('start'); ev.check('start toggles clear', state()['copies'] == 0)
    press('triangle', 12, 3.0); shot('card-viewer'); press('circle', 12, 2.0); shot('after-viewer')
    press('circle'); ev.check('circle cancels editor', not state()['active']); shot('cancelled')

    # C/D. six-digit totals without cap loss, confirm, successful sale
    fixture(100)
    press('triangle'); setq(298, 5); s = state(); ev.check('selected total is exact', s['gross'] == 30 and s['credit'] == 30, s)
    shot('selected-totals'); press('cross'); shot('confirm')
    before = sr.inventory(r); press('cross', 12, 1.5); after = sr.inventory(r)
    ev.check('sale credited', after['chips'] == 130 and after['trunk'][297] == 94, after['chips'])
    ev.check('deck unchanged', after['deck'] == before['deck'])
    shot('sold')

    # F. stale transaction
    press('triangle'); setq(4, 1); press('cross'); ev.check('review for stale', state()['active'] == 2)
    stale = bytearray(sr.inventory(r)['trunk']); stale[721] = 1
    r.write(sr.LIVE_BASE + sr.TRUNK_OFFSET, bytes(stale)); r.write(sr.MIRROR_BASE + sr.TRUNK_OFFSET, bytes(stale))
    press('cross'); s = state(); ev.check('stale confirm rejected back to editor', s['active'] == 1, s['active'])
    shot('stale-transaction'); press('circle')

    # G. zero-value-only sale
    fixture(100); press('triangle'); setq(5, 4); press('cross'); shot('confirm-zero-value'); press('circle'); press('circle')

    # H. scrolling context with all 722 cards, wide sale, wide result message
    fixture(0, {c: 1 for c in range(1, 723)})
    for c in range(600, 618):
        d = player / 'cards' / str(c); d.mkdir(parents=True, exist_ok=True); (d / 'card.ini').write_text('sell_price = 999999\n')
    r.query('card_packs_reload')
    press('triangle'); shot('scroll-top')
    for _ in range(3): press('down', 12, 0.4)
    shot('scroll-mid'); setq(722, 1); shot('scroll-bottom')
    press('start'); s = state(); ev.check('all 722 selected', s['copies'] == 722, s['copies']); shot('all-722-selected')
    all_722_credit = s['credit']
    press('cross'); shot('confirm-722')
    press('cross', 12, 1.5); s = sr.inventory(r); ev.check('722 sale credits the exact reviewed total', s['chips'] == all_722_credit, s['chips']); shot('sold-722')
    for c in range(600, 618): shutil.rmtree(player / 'cards' / str(c))
    r.query('card_packs_reload')

    # I. keyboard input through ydotool (needs window focus; recorded honestly)
    fixture(77); press('triangle'); kb = {'attempted': True}
    try:
        key(45)  # X -> cross -> review is refused (nothing selected) -> message
        s1 = state(); key(106)  # Right -> +1
        s2 = state(); key(45); s3 = state(); key(31); s4 = state()  # X review, S back
        kb.update({'after_x_nothing': s1['active'], 'after_right_sell': s2['entries'][0]['sell'], 'after_x_review': s3['active'], 'after_s_back': s4['active']})
        kb['worked'] = s2['entries'][0]['sell'] == 1 and s3['active'] == 2 and s4['active'] == 1
        shot('keyboard-after-back')
    except Exception as exc:
        kb['error'] = str(exc)
    ev.data['cases']['keyboard'] = kb; ev.flush()
    press('circle'); press('circle', 12, 2.0)
    ev.data['status'] = 'pass'
except Exception as exc:
    import traceback; ev.data['status'] = 'fail'; ev.data['error'] = str(exc); ev.data['traceback'] = traceback.format_exc(); traceback.print_exc()
finally:
    r.stop(); ev.data['provenance']['source_card_sha256_after'] = sr.sha256_file(SOURCE_CARD); ev.flush()
    print(json.dumps({'status': ev.data['status'], 'checks': len(ev.data['checks']), 'failed': [c['label'] for c in ev.data['checks'] if not c['pass']], 'processes': ev.data['processes']}, indent=1))
