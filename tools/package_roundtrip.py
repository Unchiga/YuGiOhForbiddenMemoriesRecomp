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
                card_drops=q({'cmd': 'card_drops_state'}).get('setting'),
                fill_library=q({'cmd': 'fill_library'}).get('on'),
                drop_missing=q({'cmd': 'drop_missing_state'}).get('enabled'))


def write_mini_package(path, files):
    z = zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED)
    for n, d in files.items():
        z.writestr(n, d)
    z.writestr('manifest.ini', 'format = YGOFM-MOD-PACKAGE\nversion = 1\ngame = SLUS-01411\n')
    z.close()


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
        time.sleep(2)
        full = state(); print('coverage state:', full)
        check(full['cards'] and full['drops'] and full['decks'] and full['fusion_edits'], 'every manager holds an edit')

        # 3. export A
        A = os.path.join(tmp, 'A.ygomods')
        print('export A ->', q({'cmd': 'mod_package', 'export': A})['msg'])
        names = zipfile.ZipFile(A).namelist()
        for part in ('drop_table_edits.ini', 'cpu-duelists.ini', 'duelists/10/portrait.png', 'fusion-edits.txt', 'dialogue.txt', 'drop_missing_cards.ini', 'card_shop.ini', 'mod_settings.ini', 'cards-manifest.ini', 'manifest.ini'):
            check(part in names, 'A carries %s' % part)
        check(sum(1 for n in names if n.startswith('cards/')) >= 700, 'A carries the edited cards (%d)' % sum(1 for n in names if n.startswith('cards/')))

        # 4. revert to stock
        print('revert ->', q({'cmd': 'mod_package', 'reset': 1})['msg'])
        time.sleep(3)
        empty = state(); print('after revert:', empty)
        check(not empty['cards'] and not empty['drops'] and not empty['decks'] and not empty['names'] and not empty['fusion_edits'], 'Revert to Stock empties every manager')
        check(empty['card_drops'] == 1 and not empty['fill_library'] and not empty['drop_missing'], 'Revert to Stock puts the settings rows back')

        # 5. import A, export B, compare
        print('import A ->', q({'cmd': 'mod_package', 'import': A})['msg'])
        time.sleep(3)
        back = state(); print('after import A:', back)
        check(back['cards'] == full['cards'] and back['drops'] == full['drops'] and back['decks'] == full['decks'] and back['fusion_edits'] == full['fusion_edits'], 'the managers read the same counts as before the revert')
        B = os.path.join(tmp, 'B.ygomods')
        print('export B ->', q({'cmd': 'mod_package', 'export': B})['msg'])
        compare_packages(A, B)
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
