#!/usr/bin/env python3
"""package_roundtrip.py -- prove MOD package export and import agree, part by part.

    python3 tools/package_roundtrip.py [--fixtures DIR] [--keep OUT.ygomods] [--seed-package FILE.ygomods]

Run against the DEBUG build (./Play.sh -dbg --no-launcher), booted past the
title. It drives every manager through the debug server and checks that what
a package carries survives a round trip:

  1. exports the player's current state as a package (kept, and re-imported
     at the end so the player's edits come back);
  2. builds a FULL-COVERAGE state: a seed package (tools/randomizer.py's, or
     --seed-package) for cards, drop tables, CPU decks and AI, fusions and
     settings; a translated dialogue line; a CPU portrait; edited
     drop_missing_cards.ini and card_shop.ini;
  3. exports that as package A;
  4. Revert to Stock, and checks every manager reads empty;
  5. imports A, exports B, and compares A and B part by part (card.ini keys,
     drop vectors, deck pools and AI bytes, portrait bytes, fusion recipes,
     dialogue text, the two inis, the settings rows);
  6. imports every package in --fixtures (packages written by EARLIER
     versions) and checks each part the manifest lists imports "ok".

Exit code 0 when everything agrees. Keep B with --keep as this version's
fixture, so the next version has to import it.
"""
import argparse, io, json, os, re, subprocess, sys, tempfile, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dbg  # noqa: E402

FAILS = []


def q(cmd):
    r = dbg.q(cmd)
    if not r.get('ok'):
        raise SystemExit('debug command failed: %s -> %s' % (cmd, r))
    return r


def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        FAILS.append(what)


def player_dir():
    # the runtime prints it at start; ask the drop viewer's share dir instead, which sits inside it
    r = q({'cmd': 'card_packs'})
    return os.path.dirname(r['dir'].rstrip('/'))


# --- part parsers -------------------------------------------------------------

def parse_kv(text):
    out = {}
    sec = ''
    for ln in text.splitlines():
        s = ln.strip()
        if not s or s[0] in ';#':
            continue
        if s[0] == '[':
            sec = s[1:s.find(']')]
            continue
        if '=' in s:
            k, v = s.split('=', 1)
            out[(sec, k.strip())] = v.split(';')[0].split('#')[0].strip()
    return out


def parse_fusions(text):
    out = set()
    clear = False
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith('clear'):
            clear = True
        m = re.match(r'(\d+)\D+(\d+)\D+(\d+)', s)
        if m:
            out.add(tuple(int(x) for x in m.groups()))
    return clear, out


def parse_dialogue(text):
    return '\n'.join(ln for ln in text.splitlines() if not ln.startswith(';')).strip()


def normalize(name, data):
    if name.startswith('cards/') and name.endswith('card.ini'):
        return ('ini', parse_kv(data.decode('utf-8', 'replace')))
    if name.endswith('.ini') and name not in ('manifest.ini', 'cards-manifest.ini'):
        return ('ini', parse_kv(data.decode('utf-8', 'replace')))
    if name == 'fusion-edits.txt':
        return ('fusions', parse_fusions(data.decode('utf-8', 'replace')))
    if name == 'dialogue.txt':
        return ('dialogue', parse_dialogue(data.decode('utf-8', 'replace')))
    if name == 'cards-manifest.ini':
        kv = parse_kv(data.decode('utf-8', 'replace'))
        return ('cards-manifest', sorted(int(x) for x in kv.get(('', 'cards'), '').split(',') if x.strip()))
    if name == 'manifest.ini':
        kv = parse_kv(data.decode('utf-8', 'replace'))
        kv.pop(('', 'created'), None)
        return ('manifest', kv)
    return ('bytes', data)


def compare_packages(a_path, b_path):
    za, zb = zipfile.ZipFile(a_path), zipfile.ZipFile(b_path)
    na, nb = set(za.namelist()), set(zb.namelist())
    check(na == nb, 'same set of files (%d); only in A: %s; only in B: %s' % (len(na), sorted(na - nb)[:5], sorted(nb - na)[:5]))
    bad = []
    for n in sorted(na & nb):
        if normalize(n, za.read(n)) != normalize(n, zb.read(n)):
            bad.append(n)
    check(not bad, 'every shared part equal after the round trip (differ: %s)' % bad[:8])
    return not bad and na == nb


# --- state readers ------------------------------------------------------------

def state():
    cp = q({'cmd': 'card_packs'})
    cards_dir = cp['dir']
    n_cards = sum(1 for d in os.listdir(cards_dir) if d.isdigit() and os.path.exists(os.path.join(cards_dir, d, 'card.ini'))) if os.path.isdir(cards_dir) else 0
    dr = q({'cmd': 'drop_edits'})
    cd = q({'cmd': 'cpu_data'})
    fm = q({'cmd': 'fusion_manager'})
    di = q({'cmd': 'dialogue'})
    return dict(cards=n_cards, drops=dr.get('entries'), decks=cd.get('decks'), ai=cd.get('ai'), names=cd.get('names'),
                portraits=cd.get('portraits'), fusion_edits=fm.get('edits'), fusion_applied=fm.get('applied'),
                dialogue=di.get('translated', di.get('texts_translated', di.get('imported'))),
                smart_drop=dr.get('smart_drop'), starchip_rules=dr.get('starchip_rules'),
                card_drops=q({'cmd': 'card_drops_state'}).get('setting'),
                fill_library=q({'cmd': 'fill_library'}).get('on'),
                drop_missing=q({'cmd': 'drop_missing_state'}).get('enabled'))


def write_mini_package(path, files):
    z = zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED)
    for n, d in files.items():
        z.writestr(n, d)
    z.writestr('manifest.ini', 'format = YGOFM-MOD-PACKAGE\nversion = 1\ngame = SLUS-01411\n')
    z.close()


def write_crc_damaged_zip(path, files, damaged_name, damaged_payload):
    """Write stored members, then flip one payload byte without fixing CRC."""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
        for name, data in files.items():
            z.writestr(name, data)
        z.writestr(damaged_name, damaged_payload)
    blob = bytearray(open(path, 'rb').read())
    at = blob.find(damaged_payload)
    if at < 0:
        raise RuntimeError('could not locate CRC test payload')
    blob[at + len(damaged_payload) // 2] ^= 0x40
    open(path, 'wb').write(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fixtures', default=os.path.join(HERE, 'fixtures'))
    ap.add_argument('--keep', default=None, help='where to keep package B (this version\'s fixture)')
    ap.add_argument('--seed-package', default=None, help='a full package to build the coverage state from (default: tools/randomizer.py --seed 1)')
    a = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix='ygomods-roundtrip-')
    P = player_dir()
    print('player data:', P)

    # 1. the player's own state, to come back to
    mine = os.path.join(tmp, 'mine.ygomods')
    print('export the current state ->', q({'cmd': 'mod_package', 'export': mine})['msg'])

    try:
        # 2. full coverage
        seed = a.seed_package
        if not seed:
            seed = os.path.join(tmp, 'seed.ygomods')
            subprocess.run([sys.executable, os.path.join(HERE, 'randomizer.py'), '--seed', '1', '--out', seed], check=True, cwd=tmp)
        print('import seed ->', q({'cmd': 'mod_package', 'import': seed})['msg'])
        # Make the coverage package carry the additive authoring fields that
        # predate neither the stock card schema nor the randomizer fixture.
        # Card 334 is Umi, a field spell in every stock-compatible set.
        field_ini = os.path.join(q({'cmd': 'card_packs'})['dir'], '334', 'card.ini')
        with open(field_ini, 'a', encoding='utf-8') as f:
            f.write('field_targets = 1, 2, 3\n')
        q({'cmd': 'card_packs_reload', 'card': 334})
        q({'cmd': 'story_rewards', 'duelist': 0, 'card': 37,
           'every': 1, 'save': 1})
        # a translated line
        dlg = os.path.join(tmp, 'dialogue.txt')
        q({'cmd': 'dialogue_export', 'path': dlg})
        text = open(dlg, encoding='utf-8').read().replace("I've found it at last...", "Round trip test line...", 1)
        open(dlg, 'w', encoding='utf-8').write(text)
        print('import dialogue ->', q({'cmd': 'dialogue_import', 'path': dlg})['msg'])
        # a portrait through the CPU Manager's own zip
        from PIL import Image
        png = io.BytesIO(); Image.new('RGB', (48, 48), (200, 40, 120)).save(png, 'PNG')
        # a CPU import replaces the whole layer, so the zip carries the seed's decks and AI too
        quoted_name = 'Round "Trip"'
        cpu_ini = zipfile.ZipFile(seed).read('cpu-duelists.ini').decode().replace(
            '[Weevil Underwood]\n', '[Weevil Underwood]\nname = %s\n' % quoted_name, 1)
        duel = os.path.join(tmp, 'portrait.ygoduelists')
        z = zipfile.ZipFile(duel, 'w'); z.writestr('cpu_manager.ini', cpu_ini); z.writestr('duelists/10/portrait.png', png.getvalue()); z.close()
        print('import portrait ->', q({'cmd': 'cpu_data', 'import': duel})['msg'])
        cpu_after_name = q({'cmd': 'cpu_data'})
        weevil = next(row for row in cpu_after_name['duelists'] if row['d'] == 9)
        check(weevil['shown'] == quoted_name,
              'CPU rename accepts the full documented glyph set and returns valid JSON')
        # the two inis, through a mini package so the managers reload them
        mini = os.path.join(tmp, 'inis.ygomods')
        write_mini_package(mini, {
            'drop_missing_cards.ini': '[Weevil Underwood]\n52 = 30, 20, 0\n278 = 30, 20, 0\n',
            'card_shop.ini': '[prices]\ncommon = 25\nuncommon = 90\nrare = 250\nlegendary = 900\n[packs]\ncards = 2\n',
        })
        print('import inis ->', q({'cmd': 'mod_package', 'import': mini})['msg'])
        q({'cmd': 'card_drops_set', 'drops': 0})
        q({'cmd': 'card_drops_set', 'smart': 1})
        q({'cmd': 'starchip_rewards', 'op': 'set', 'index': 0,
           'mode': 1, 'opponent': 9, 'outcome': 1, 'rank': 4,
           'amount': 123456})
        q({'cmd': 'starchip_rewards', 'op': 'save'})
        time.sleep(2)
        full = state(); print('coverage state:', full)
        check(full['cards'] and full['drops'] and full['decks'] and full['names'] and full['fusion_edits'], 'every manager holds an edit')
        check(full['card_drops'] == 0,
              'Card Drops 0 is a live persisted menu-row value')
        check(full['smart_drop'] == 1 and full['starchip_rules'] == 1,
              'Smart Drops and an ordered starchip rule share the drop backend')

        # 3. export A
        A = os.path.join(tmp, 'A.ygomods')
        print('export A ->', q({'cmd': 'mod_package', 'export': A})['msg'])
        names = zipfile.ZipFile(A).namelist()
        for part in ('drop_table_edits.ini', 'cpu-duelists.ini', 'duelists/10/portrait.png', 'fusion-edits.txt', 'dialogue.txt', 'drop_missing_cards.ini', 'card_shop.ini', 'mod_settings.ini', 'cards-manifest.ini', 'manifest.ini'):
            check(part in names, 'A carries %s' % part)
        check(sum(1 for n in names if n.startswith('cards/')) >= 700, 'A carries the edited cards (%d)' % sum(1 for n in names if n.startswith('cards/')))
        with zipfile.ZipFile(A) as za:
            field_text = za.read('cards/334/card.ini').decode('utf-8', 'replace')
            drop_text = za.read('drop_table_edits.ini').decode('utf-8', 'replace')
            card_texts = [za.read(n).decode('utf-8', 'replace') for n in names
                          if n.startswith('cards/') and n.endswith('/card.ini')]
        check('field_targets = 1, 2, 3' in field_text,
              'A carries an explicit field-spell card-ID allow-list')
        check(any(re.search(r'^password = \d{8}$', text, re.M) for text in card_texts),
              'A carries editable eight-digit card passwords')
        check(any(re.search(r'^equips = ', text, re.M) for text in card_texts),
              'A carries equip usable-monster lists')
        check('smart_drop = on' in drop_text and '[Starchip Reward 1]' in drop_text and
              re.search(r'(?m)^card = 37$', drop_text),
              'A carries Smart Drops, starchip rules and a guaranteed story reward together')

        # 4. revert to stock
        print('revert ->', q({'cmd': 'mod_package', 'reset': 1})['msg'])
        time.sleep(3)
        empty = state(); print('after revert:', empty)
        check(not empty['cards'] and not empty['drops'] and not empty['decks'] and not empty['names'] and not empty['fusion_edits'], 'Revert to Stock empties every manager')
        check(empty['card_drops'] == 1 and not empty['fill_library'] and not empty['drop_missing'], 'Revert to Stock puts the settings rows back')
        check(not empty['smart_drop'] and not empty['starchip_rules'],
              'Revert to Stock clears Smart Drops and starchip rules')

        # 5. import A, export B, compare
        print('import A ->', q({'cmd': 'mod_package', 'import': A})['msg'])
        time.sleep(3)
        back = state(); print('after import A:', back)
        check(back['cards'] == full['cards'] and back['drops'] == full['drops'] and
              back['decks'] == full['decks'] and back['names'] == full['names'] and
              back['fusion_edits'] == full['fusion_edits'],
              'the managers read the same counts as before the revert')
        check(back['card_drops'] == 0,
              'Card Drops 0 survives MOD package export/revert/import')
        check(back['smart_drop'] == 1 and back['starchip_rules'] == 1,
              'Smart Drops and starchip rules survive MOD package export/revert/import')
        cpu_after_roundtrip = q({'cmd': 'cpu_data'})
        weevil = next(row for row in cpu_after_roundtrip['duelists'] if row['d'] == 9)
        check(weevil['shown'] == quoted_name,
              'CPU display name survives MOD package inspect/import/export round trip')
        B = os.path.join(tmp, 'B.ygomods')
        print('export B ->', q({'cmd': 'mod_package', 'export': B})['msg'])
        compare_packages(A, B)

        # A valid archive can still contain a later manager payload that its
        # single-sourced parser rejects. Cards are deliberately first in this
        # package: the package layer must restore the complete prior state.
        malformed = os.path.join(tmp, 'malformed-late-fusion.ygomods')
        with zipfile.ZipFile(B) as src, zipfile.ZipFile(malformed, 'w', zipfile.ZIP_DEFLATED) as dst:
            for name in src.namelist():
                data = src.read(name)
                if name == 'fusion-edits.txt':
                    data = ('clear\n' + ''.join('1 %d 2\n' % b for b in range(2, 603))).encode()
                dst.writestr(name, data)
        failed = dbg.q({'cmd': 'mod_package', 'import': malformed})
        check(not failed.get('ok') and 'restored' in failed.get('msg', '').lower(),
              'a rejected later manager restores all earlier package changes')
        after_failed = os.path.join(tmp, 'after-failed.ygomods')
        print('export after failed import ->', q({'cmd': 'mod_package', 'export': after_failed})['msg'])
        compare_packages(B, after_failed)

        # CRC/stream validation covers unknown additive members too and runs
        # before any manager import. The same policy is enforced by direct
        # edited-card archives.
        with zipfile.ZipFile(B) as src:
            package_files = {name: src.read(name) for name in src.namelist()}
        damaged_pkg = os.path.join(tmp, 'damaged-late-entry.ygomods')
        write_crc_damaged_zip(damaged_pkg, package_files, 'zz-late.bin',
                              b'late CRC payload sentinel 0123456789')
        failed = dbg.q({'cmd': 'mod_package', 'import': damaged_pkg})
        check(not failed.get('ok') and 'damaged archive entry' in failed.get('msg', '').lower(),
              'MOD package rejects a damaged late member before manager mutation')
        check(state() == back, 'damaged MOD package leaves every managed count and setting unchanged')

        card_name = next(n for n in package_files
                         if n.startswith('cards/') and n.endswith('/card.ini'))
        card_id = int(card_name.split('/')[1])
        cards_files = {
            'manifest.ini': ('format = YGOFM-EDITED-CARDS\nversion = 1\n'
                             'game = SLUS-01411\ncards = %d\ndrop_table_edits = 0\n' % card_id),
            card_name: package_files[card_name],
        }
        damaged_cards = os.path.join(tmp, 'damaged-late-entry.ygocards')
        write_crc_damaged_zip(damaged_cards, cards_files, 'zz-late.bin',
                              b'edited cards CRC payload sentinel abcdef')
        failed = dbg.q({'cmd': 'card_share', 'op': 'import', 'path': damaged_cards})
        check(not failed.get('ok') and 'damaged archive entry' in failed.get('msg', '').lower(),
              'edited-card import rejects a damaged late member before replacing a card')
        check(state() == back, 'damaged edited-card archive leaves managed state unchanged')

        for label, drop_text in (
                ('unterminated section', 'format = 3\n[Simon Muran]\ncard = 37\n[broken\ncard = 99\n'),
                ('junk card suffix', 'format = 3\n[Simon Muran]\ncard = 37oops\n'),
                ('junk mode suffix', 'format = 3\n[Simon Muran]\ncard = 37\nwhen = every later\n'),
                ('future format', 'format = 999\n[Simon Muran]\ncard = 37\n')):
            bad_drop = os.path.join(tmp, 'bad-drop-%s.ygocards' % label.replace(' ', '-'))
            with zipfile.ZipFile(bad_drop, 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr('manifest.ini',
                           'format = YGOFM-EDITED-CARDS\nversion = 1\n'
                           'game = SLUS-01411\ncards =\ndrop_table_edits = 1\n')
                z.writestr('drop_table_edits.ini', drop_text)
            failed = dbg.q({'cmd': 'card_share', 'op': 'import', 'path': bad_drop})
            check(not failed.get('ok'), '%s drop input is rejected' % label)
            check(state() == back, '%s rejection leaves the prior drop layer intact' % label)

        # Transitional packages used a MODS row key. It migrates only when a
        # format-3 drop file does not carry the authoritative new key.
        precedence = os.path.join(tmp, 'smart-precedence.ygomods')
        write_mini_package(precedence, {
            'cards-manifest.ini': 'format = YGOFM-EDITED-CARDS\nversion = 1\ngame = SLUS-01411\ncards =\ndrop_table_edits = 1\n',
            'drop_table_edits.ini': 'format = 3\nsmart_drop = off\n',
            'mod_settings.ini': 'smart_first_drop = 1\n',
        })
        print('import Smart precedence fixture ->', q({'cmd': 'mod_package', 'import': precedence})['msg'])
        precedence_state = q({'cmd': 'drop_edits'})
        check(precedence_state.get('smart_drop_present') == 1 and
              precedence_state.get('smart_drop') == 0,
              'format-3 Smart key wins over a stale transitional MODS-row key')
        legacy_smart = os.path.join(tmp, 'legacy-smart-row.ygomods')
        write_mini_package(legacy_smart, {
            'mod_settings.ini': 'smart_first_drop = 1\n',
        })
        print('import legacy Smart fixture ->', q({'cmd': 'mod_package', 'import': legacy_smart})['msg'])
        legacy_state = q({'cmd': 'drop_edits'})
        check(legacy_state.get('smart_drop_present') == 1 and
              legacy_state.get('smart_drop') == 1,
              'transitional smart_first_drop packages migrate to Smart Drops')
        print('restore full package after precedence fixture ->', q({'cmd': 'mod_package', 'import': B})['msg'])
        back = state()
        if a.keep:
            import shutil; shutil.copy(B, a.keep); print('kept', a.keep)

        # 6. packages from earlier versions
        if os.path.isdir(a.fixtures):
            for fn in sorted(os.listdir(a.fixtures)):
                if not fn.endswith('.ygomods'):
                    continue
                path = os.path.join(a.fixtures, fn)
                ins = q({'cmd': 'mod_package', 'inspect': path})['msg']
                r = q({'cmd': 'mod_package', 'import': path})['msg']
                check('damaged' not in r and 'could not' not in r.lower() and ': ' in r, 'fixture %s imports (%s -> %s)' % (fn, ins, r))
    finally:
        # back to the player's own state
        print('revert ->', q({'cmd': 'mod_package', 'reset': 1})['msg'])
        time.sleep(2)
        print('re-import the player\'s package ->', q({'cmd': 'mod_package', 'import': mine})['msg'])
    print('\n%d check%s failed' % (len(FAILS), '' if len(FAILS) == 1 else 's'))
    for f in FAILS:
        print('  -', f)
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
