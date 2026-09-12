#!/usr/bin/env python3
"""Scratch-only live regression capture for an explicitly selected runtime.

Run from either revision with the same seed, disc, and arguments. JSON records
checks separately from screenshots; a screenshot alone is never a pass.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import traceback


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--menu-reference', type=Path, required=True, help='known loaded-save menu screenshot')
    ap.add_argument('--port', type=int, default=4370)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--scratch', required=True, type=Path)
    ap.add_argument('--seed', required=True, type=Path)
    ap.add_argument('--disc', required=True, type=Path)
    ap.add_argument('--renderer', choices=['software','opengl','vulkan'], default='software')
    ap.add_argument('--resume-menu', type=Path, help='slot 7 state from this same executable revision')
    ap.add_argument('--load-package', type=Path,
                    help='import this .ygomods package for the stress_free_duel group')
    ap.add_argument('--near-win-state', type=Path,
                    help='slot-0 near-win state for deterministic stress reward results')
    ap.add_argument('--groups', default='menus,duel,managers,package')
    args = ap.parse_args()
    root = args.scratch.resolve()
    personal = Path.home() / 'Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled'
    if root == personal or personal in root.parents or root.exists():
        ap.error('scratch must be a NEW directory outside personal player data')
    args.exe = args.exe.resolve(strict=True)
    args.seed = args.seed.resolve(strict=True)
    args.disc = args.disc.resolve(strict=True)
    if args.load_package:
        args.load_package = args.load_package.resolve(strict=True)
    if args.near_win_state:
        args.near_win_state = args.near_win_state.resolve(strict=True)
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            ap.error(f'debug port {args.port} is occupied; stop its owner before running')
    root.mkdir(parents=True)
    data = root / 'player'
    data.mkdir()
    shutil.copyfile(args.seed, data / 'card1.mcd')
    if args.resume_menu:
        (data/'openbios').mkdir()
        shutil.copyfile(args.resume_menu, data/'openbios'/args.resume_menu.name)
    if args.near_win_state:
        (data/'openbios').mkdir(exist_ok=True)
        shutil.copyfile(args.near_win_state,
                        data/'openbios'/'state_800129D8_slot00.pst')
    shots = root / 'shots'
    shots.mkdir()
    os.environ['SHOTDIR'] = str(shots)
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    os.environ['YGOFM_DEBUG_PORT'] = str(args.port)
    import dbg
    import live_probe as p
    import game_route as route
    import goto_duel
    import duel_turns as turns
    results = {}
    transcript = (root / 'commands.jsonl').open('w')

    def q(cmd):
        r = dbg.q(cmd)
        transcript.write(json.dumps({'request': cmd, 'response': r}) + '\n')
        transcript.flush()
        if not r.get('ok'):
            raise RuntimeError(f'{cmd}: {r}')
        return r

    def shot(tag):
        path = shots / (tag + '.png')
        q({'cmd': 'screenshot', 'path': str(path)})
        for _ in range(50):
            if path.exists() and path.stat().st_size:
                from PIL import Image
                try:
                    with Image.open(path) as im:
                        im.load()
                    return str(path)
                except OSError:
                    pass
            time.sleep(.1)
        raise RuntimeError('native game screenshot did not complete: ' + str(path))

    def present_shot(tag):
        path = shots / (tag + '-present.png')
        before = q({'cmd':'present_shot_seq'})['seq']
        q({'cmd':'present_shot','path':str(path)})
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = q({'cmd':'present_shot_seq'})
            if state['seq'] != before:
                assert state['wrote'], state
                assert path.exists() and path.stat().st_size, path
                return str(path)
            time.sleep(.02)
        raise RuntimeError('composed screenshot did not complete: ' + str(path))

    def group(name, fn):
        print('RUN', name, flush=True)
        try:
            detail = fn()
            results[name] = {'result': 'pass', 'detail': detail}
        except Exception:
            results[name] = {'result': 'fail', 'error': traceback.format_exc()}
        (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(name, results[name], flush=True)

    def menus():
        goto_duel.wait_boot()
        route.to_loaded_menu()
        assert route.screen_matches(str(args.menu_reference), 'loaded-check')[0]
        shot('loaded-menu')
        q({'cmd': 'savestate', 'op': 'save', 'slot': 7})
        time.sleep(2)
        for _ in range(3):
            p.press('down', 20, 1)
        p.press('cross', 20, 3)
        shot('library-grid')
        p.press('cross', 20, 3)
        shot('library-card')
        restore_menu()
        for _ in range(4):
            p.press('down', 20, 1)
        p.press('cross', 20, 3)
        shot('password-entry')
        # Blue-Eyes White Dragon: 89631139. Verify the card prompt, then quit.
        for digit in '89631139':
            for _ in range(int(digit)):
                p.press('up', 6, .15)
            p.press('right', 6, .15)
        p.press('cross', 12, 3)
        shot('password-card')
        restore_menu()
        return {'loaded_mode': p.mode(), 'password': '89631139'}

    def restore_menu():
        q({'cmd': 'savestate', 'op': 'load', 'slot': 7})
        time.sleep(4)
        assert route.screen_matches(str(args.menu_reference), 'restore-check')[0]

    def perf(tag):
        start = time.monotonic()
        f0 = p.frame()
        time.sleep(30)
        elapsed = time.monotonic() - start
        f1 = p.frame()
        r = {'frames': f1 - f0, 'seconds': elapsed, 'fps': (f1-f0)/elapsed}
        (root / (tag + '-perf.json')).write_text(json.dumps(r, indent=2)+'\n')
        assert f1 > f0, 'frame counter stopped'
        return r

    def duel():
        restore_menu()
        # Controlled scratch fixture: mark Simon as met in RAM only. The
        # loopback seed need not have campaign progress. Never save this patch.
        for addr in (0x801D06F4, 0x801D36F4):
            q({'cmd':'write_mem','addr':f'{addr:08X}',
               'hex':bytes([p.rd(addr,1)[0] | 0x40]).hex()})
        p.press('down', 20, 1)
        p.press('cross', 20, 4)
        p.press('cross', 40, 2)
        q({'cmd': 'read_ram', 'addr': '8009B26C', 'len': 1})
        shot('duel-grid')
        goto_duel.duel([1])
        assert turns.lp() == (8000, 8000), turns.lp()
        shot('duel-start')
        for n in range(2):
            assert turns.play_turn([1]), f'no Blue-Eyes in turn {n}'
            turns.end_turn()
            shot('duel-turn-' + str(n+1))
        q({'cmd': 'cdrom_state'})
        return {'turns': 2, 'lp': turns.lp(), 'performance': perf('duel')}

    def managers():
        restore_menu()
        # Assert isolation before any mod write, independently of the CLI.
        packs = q({'cmd': 'card_packs'})
        assert Path(packs['dir']).resolve().is_relative_to(data), packs
        q({'cmd': 'mod_package', 'export': str(root/'before-managers.ygomods')})
        q({'cmd': 'card_manager_set', 'open': 1})
        time.sleep(2)
        q({'cmd': 'card_manager_set', 'card': 1})
        q({'cmd': 'card_manager_set', 'search': 'Blue'})
        s = q({'cmd': 'card_manager'})
        editor = q({'cmd': 'fm_editor'})
        window_id = editor['window_id']
        page_ids = {'cards': window_id}
        x,y,w,h = s['geom']['value'][2]
        q({'cmd':'card_manager_click','x':x+w//2,'y':y+h//2,'button':1})
        q({'cmd':'card_manager_key','key':'a','ctrl':1})
        q({'cmd':'card_manager_type','text':'3100'})
        q({'cmd':'card_manager_key','key':'return'})
        time.sleep(.5)
        s=q({'cmd':'card_manager'})
        assert int(s['atk']) == 3100 and s['changed'] and s['search'] == 'Blue', s
        q({'cmd':'card_manager_shot','path':str(shots/'card-manager.ppm')})
        perf('card-manager')
        q({'cmd':'card_share','op':'export','path':str(root/'cards.ygocards')})
        q({'cmd':'card_share','op':'import','path':str(root/'cards.ygocards')})
        q({'cmd':'fusion_manager','open':1})
        time.sleep(2)
        e=q({'cmd':'fm_editor'});page_ids['fusions']=e['window_id']
        assert e['window_id'] == window_id and e['page_name'] == 'Fusions',e
        q({'cmd':'fusion_manager','a':1,'b':2,'result':37})
        q({'cmd':'fusion_manager','shot':str(shots/'fusion-manager.ppm')})
        q({'cmd':'drop_viewer_set','open':1})
        time.sleep(2)
        e=q({'cmd':'fm_editor'});page_ids['drop_tables']=e['window_id']
        assert e['window_id'] == window_id and e['page_name'] == 'Drop Tables',e
        q({'cmd':'drop_viewer_set','randomize':2026})
        q({'cmd':'drop_viewer_shot','path':str(shots/'drop-manager.ppm')})
        q({'cmd':'dialogue_manager','open':1})
        time.sleep(2)
        e=q({'cmd':'fm_editor'});page_ids['dialogue']=e['window_id']
        assert e['window_id'] == window_id and e['page_name'] == 'Dialogue',e
        q({'cmd':'dialogue_manager','shot':str(shots/'dialogue-manager.ppm')})
        q({'cmd':'cpu_manager','open':1})
        time.sleep(2)
        e=q({'cmd':'fm_editor'});page_ids['cpu']=e['window_id']
        assert e['window_id'] == window_id and e['page_name'] == 'CPU',e
        renamed = 'Catchup "Teana"'
        q({'cmd':'cpu_data','duelist':1,'name':renamed,'save':1})
        cpu = q({'cmd':'cpu_data'})
        assert next(row for row in cpu['duelists'] if row['d'] == 1)['shown'] == renamed, cpu
        q({'cmd':'cpu_manager','duelist':1,'shot':str(shots/'cpu-manager.ppm')})
        q({'cmd':'starchip_rewards','op':'clear'})
        chips = q({'cmd':'starchip_rewards','op':'set','index':0,
                   'mode':1,'opponent':1,'outcome':1,'rank':9,'amount':25})
        assert chips['rules'][0]['opponent'] == 1, chips
        assert chips['rules'][0]['opponent_name'] == renamed, chips
        q({'cmd':'drop_viewer_set','open':1})
        time.sleep(.5)
        q({'cmd':'drop_viewer_set','view':1,'duelist':1})
        drops = q({'cmd':'drop_viewer'})
        assert drops['sel_duelist_name'] == renamed, drops
        x,y,w,h = drops['geom']['starchip_open']
        q({'cmd':'drop_viewer_click','x':x+w//2,'y':y+h//2,'button':1})
        # Debug clicks are queued onto the SDL/game thread. Do not race the
        # immediate debug reply against the next frame on a busy editor.
        deadline = time.monotonic() + 2
        while True:
            drops = q({'cmd':'drop_viewer'})
            if drops['starchip_editor']['open'] or time.monotonic() >= deadline:
                break
            time.sleep(.02)
        assert drops['starchip_editor']['opponent_name'] == renamed, drops
        drop_ini = root/'rename-drop-tables.ini'
        q({'cmd':'drop_viewer_set','export':str(drop_ini)})
        chip_text = drop_ini.read_text()
        assert 'opponent = 2' in chip_text and renamed not in chip_text, chip_text
        q({'cmd':'card_manager_set','open':1})
        time.sleep(2)
        e=q({'cmd':'fm_editor'});page_ids['cards_return']=e['window_id']
        assert e['window_id'] == window_id and e['page_name'] == 'Cards',e
        s=q({'cmd':'card_manager'})
        assert int(s['atk']) == 3100 and s['changed'] and s['search'] == 'Blue',s
        x,y = s['geom']['btn'][0]
        q({'cmd':'card_manager_click','x':x,'y':y,'button':1})
        time.sleep(.5)
        assert int(q({'cmd':'card_manager'})['atk']) == 3100
        q({'cmd':'fm_editor','open':0})
        time.sleep(1)
        assert not q({'cmd':'fm_editor'})['open']
        for _ in range(3): p.press('down',20,1)
        p.press('cross',20,3)
        p.press('cross',20,3)
        shot('edited-library-card')
        return {'card_attack':3100,'fusion':[1,2,37],'drop_seed':2026,
                'shared_window_id':window_id,'page_window_ids':page_ids}

    def video():
        def key(k):
            q({'cmd':'menu_key','key':k})
            time.sleep(.15)
        q({'cmd':'video_menu','hide':1})
        q({'cmd':'video_menu','toggle':1})
        time.sleep(1)
        for _ in range(q({'cmd':'menu_state'})['menu']): key(1073741904)
        snapshots=[]
        for n,name in enumerate(('file','view','video','audio','game','cheats','mods')):
            m=q({'cmd':'menu_state'})
            assert m['expanded'] and m['menu']==n, m
            path=shots/('menu-'+name+'.png')
            q({'cmd':'screenshot_present','path':str(path)})
            time.sleep(.6)
            assert path.exists(), path
            snapshots.append(m)
            # Walk all rows without executing actions such as Quit or disc picker.
            for _ in range(m['rows']): key(1073741905)
            key(1073741903)
        key(27)
        q({'cmd':'menu_settings_save'})
        return {'menus': snapshots}

    def package():
        q({'cmd':'card_packs','dev':0})
        time.sleep(2)
        r = subprocess.run([sys.executable,str(Path(__file__).with_name('package_roundtrip.py')),
                            '--keep',str(root/'full-coverage.ygomods')],
                           stdout=(root/'package-roundtrip.log').open('w'),stderr=subprocess.STDOUT,
                           timeout=300)
        assert r.returncode == 0, f'package_roundtrip exit {r.returncode}'
        return {'fixture': str(root/'full-coverage.ygomods')}

    def video_actions():
        def key(k):
            q({'cmd':'menu_key','key':k});time.sleep(.3)
        def select(menu,row):
            q({'cmd':'video_menu','hide':1});q({'cmd':'video_menu','toggle':1})
            for _ in range(q({'cmd':'menu_state'})['menu']): key(1073741904)
            for _ in range(menu): key(1073741903)
            m=q({'cmd':'menu_state'})
            for _ in range((row-m['item'])%m['rows']): key(1073741905)
        rows=q({'cmd':'video_menu','menu':1})['rows']
        select(1,1+rows.index('Widescreen'))  # View's built-in Menu bar row is first.
        key(13);q({'cmd':'video_menu','collapse':1});time.sleep(2)
        q({'cmd':'screenshot_present','path':str(shots/'widescreen.png')})
        q({'cmd':'menu_settings_save'})
        settings=data/'menu_settings.ini'
        if not settings.exists(): settings=args.exe.parent/'menu_settings.ini'
        wide=settings.read_text()
        assert 'widescreen=1' in wide
        select(1,1+rows.index('Widescreen'));key(13)
        q({'cmd':'menu_settings_save'})
        native=settings.read_text()
        assert 'widescreen=0' in native
        # Exercise every non-disruptive Video option and restore it by cycling
        # its complete choice range. Screen/Windowed scale are left for a
        # desktop run because resizing can move a window off the captured area.
        for row,count in ((0,2),(1,2),(2,2),(5,3)):
            select(2,row)
            for _ in range(count):key(13)
        select(3,0)
        before=q({'cmd':'menu_state'})['vol_master']
        key(13)
        for ch in '17':key(ord(ch))
        key(13)
        after=q({'cmd':'menu_state'})['vol_master']
        assert after==17,(before,after)
        select(4,1)
        for _ in range(3):key(13)
        select(4,2);key(13);time.sleep(1)
        state=q({'cmd':'savestate_menu_state'})
        key(27)
        q({'cmd':'video_menu','hide':1})
        q({'cmd':'menu_settings_save'})
        return {'wide':wide,'native':native,'volume_before':before,'volume_after':after,
                'savestate_menu':state,'menu_rows':rows}

    def mods():
        restore_menu()
        q({'cmd':'mod_package','export':str(root/'before-mods.ygomods')})
        q({'cmd':'card_packs','dev':0})
        time.sleep(2)
        packs=q({'cmd':'card_packs'})
        folder=Path(packs['dir']).resolve()
        assert folder.is_relative_to(data) and not packs['dev'], packs
        card=folder/'1'
        card.mkdir(parents=True,exist_ok=True)
        (card/'card.ini').write_text('attack = 4000\ncolor = purple\ndescription = Catchup description|sector override test\nbattle = slayer\non_summon = damage 500\non_flip = damage 500\n')
        from PIL import Image, ImageDraw
        art=Image.new('RGB',(102,96),(180,40,120))
        ImageDraw.Draw(art).rectangle((15,15,85,80),fill=(20,180,220))
        art.save(card/'art.png')
        q({'cmd':'card_packs_reload','card':1})
        q({'cmd':'fill_library','on':1})
        time.sleep(3)
        for _ in range(3): p.press('down',20,1)
        p.press('cross',20,3)
        shot('mods-fill-library')
        p.press('cross',20,3)
        shot('mods-card-art-color-description')
        q({'cmd':'card_colors'})
        q({'cmd':'card_effects'})
        q({'cmd':'monster_effects'})
        restore_menu()
        portrait=root/'portrait.png'
        Image.new('RGB',(48,48),(200,40,120)).save(portrait)
        q({'cmd':'cpu_data','duelist':0,'portrait':str(portrait)})
        q({'cmd':'cpu_data','duelist':0,'name':'Catchup Simon','save':1})
        time.sleep(.5)
        cpu = q({'cmd':'cpu_data'})
        simon = next(row for row in cpu['duelists'] if row['d'] == 0)
        assert simon['shown'] == 'Catchup Simon', simon
        # The native FREE DUEL caption looks up global string entry 809.
        # Its edited offset must point at Simon's fixed name slot before the
        # screenshot below is accepted as evidence of the game-side rename.
        assert int.from_bytes(p.rd(0x801D5E52,2),'little') == 0x9CC0
        q({'cmd':'story_rewards','duelist':0,'card':37,'every':1,'save':1})
        q({'cmd':'dialogue_export','path':str(root/'dialogue.txt')})
        dialogue=root/'dialogue.txt'
        text=dialogue.read_text().replace("I've found it at last...",'Catchup dialogue test...',1)
        dialogue.write_text(text)
        q({'cmd':'dialogue_import','path':str(dialogue)})
        q({'cmd':'dialogue_manager','open':1})
        time.sleep(2)
        q({'cmd':'dialogue_manager','open':0})
        for addr in (0x801D06F4,0x801D36F4):
            q({'cmd':'write_mem','addr':f'{addr:08X}','hex':bytes([p.rd(addr,1)[0]|0x40]).hex()})
        p.press('down',20,1);p.press('cross',20,4);p.press('cross',40,2)
        shot('mods-cpu-portrait-grid')
        goto_duel.duel([1])
        shot('mods-duel-hand')
        done0 = q({'cmd':'monster_effects'})['casts_done']
        played = turns.play_turn([1])
        assert played == ('summon', 1), (played, turns.hand(), turns.board())
        placed = None
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            board = turns.board()
            placed = next((row for row in board
                           if row[0] in range(5, 10) and row[1] == 1), None)
            if placed is not None:
                break
            time.sleep(.025)
        assert placed is not None, {'played': played, 'board': board}
        # The scripted helper may set the monster face-down. That correctly
        # forfeits on_summon, so reveal that guest-authored row and exercise
        # the production on_flip path instead of silently passing no effect.
        for row in range(5, 10):
            at = 0x801A7AD8 + row * 0x1c
            raw = p.rd(at, 0x1c)
            if int.from_bytes(raw[0xc:0xe], 'little') != 1: continue
            flags = int.from_bytes(raw[0x16:0x18], 'little')
            if flags & 0x1000:
                q({'cmd':'write_mem','addr':f'{at+0x16:08X}',
                   'hex':(flags & ~0x1000).to_bytes(2,'little').hex()})
            break
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            monster=q({'cmd':'monster_effects'})
            if monster['casts_done'] == done0 + 1 and monster['idle']: break
            time.sleep(.025)
        else: raise AssertionError(monster)
        assert turns.lp()[1] == 7500, (turns.lp(), monster)
        shot('mods-summon')
        return {'monster_effects':q({'cmd':'monster_effects'}),'card_effects':q({'cmd':'card_effects'}),
                'lp':turns.lp(),'story_rewards':q({'cmd':'story_rewards'}),'dialogue':q({'cmd':'dialogue'})}

    def win():
        # Run after mods: one scripted Blue-Eyes has already been summoned.
        # The 1-LP opponent is a RAM-only fixture, like the loopback scenario.
        q({'cmd':'write_mem','addr':'800EA024','hex':'0100'})
        # Player 1 cannot attack on the opening turn. Finish placement first,
        # let the CPU play, then place the second scripted monster.
        for _ in range(3): p.press('cross',6,1.5)
        turns.end_turn()
        assert turns.play_turn([1]), 'second scripted monster missing'
        for n in range(30):
            phase=int.from_bytes(p.rd(0x8009B23A,2),'little')&15
            print('win probe',n,phase,turns.lp(),flush=True)
            if phase==13: break
            if phase==5 and p.rd(0x8009B174,1)[0]&0x7f==6 and turns.opp():
                target=turns.opp()[0][1]
                for _ in range(12):
                    if int.from_bytes(p.rd(0x8009B338,2),'little')==target: break
                    p.press('right',6,.8)
                else: raise RuntimeError('no attack target under cursor')
                # Square confirms an attack with the stock 3D monster scene;
                # Cross takes the direct 2D path. The scene transition must
                # preserve s_bat until action 11 rewrites its result rows.
                p.press('square',6,3)
                continue
            p.press('cross',6,3)
        else: raise RuntimeError('scripted attack did not finish the duel')
        time.sleep(4)
        shot('win-drops')
        for _ in range(6):
            p.press('cross',12,2)
            if p.mode()==0xC6: break
        shot('after-win')
        # Exit the Free Duel grid; its menu cursor remains on FREE DUEL.
        p.press('circle',12,3)
        p.press('down',20,1);p.press('down',20,1);p.press('cross',20,4)
        shot('library-after-drops')
        p.press('cross',20,3)
        shot('library-card-after-drops')
        f=p.frame();time.sleep(3)
        assert p.frame()>f,'Library froze after drops'
        monster = q({'cmd':'monster_effects'})
        battle = next((e for e in monster['events'] if e['what'] == 'battle'), None)
        assert battle is not None and battle['c'] == 1, monster
        assert any(e['what'] == 'scene_rows' for e in monster['events']), monster
        return {'finished_phase':phase,'lp':turns.lp(),'frames_advanced':p.frame()-f,
                'monster_effects':monster}

    def magic():
        restore_menu()
        q({'cmd':'mod_package','export':str(root/'before-magic.ygomods')})
        q({'cmd':'card_packs','dev':0});time.sleep(2)
        folder=Path(q({'cmd':'card_packs'})['dir']).resolve()
        assert folder.is_relative_to(data)
        card=folder/'301';card.mkdir(parents=True,exist_ok=True)
        (card/'card.ini').write_text('type = Magic\neffect = damage\namount = 500\n')
        q({'cmd':'card_packs_reload','card':301})
        for addr in (0x801D06F4,0x801D36F4):
            q({'cmd':'write_mem','addr':f'{addr:08X}','hex':bytes([p.rd(addr,1)[0]|0x40]).hex()})
        p.press('down',20,1);p.press('cross',20,4);p.press('cross',40,2)
        goto_duel.duel([301])
        assert turns.play_turn([301])
        time.sleep(4)
        shot('magic-effect')
        effect=q({'cmd':'card_effects'})
        assert turns.lp()[1]==7500, (turns.lp(),effect)
        return {'lp':turns.lp(),'card_effects':effect}

    def stress_free_duel():
        """Import the full stress package and win a real Free Duel with it."""
        if not args.load_package:
            raise RuntimeError('stress_free_duel requires --load-package')
        restore_menu()
        imported = q({'cmd':'mod_package','import':str(args.load_package)})
        time.sleep(3)
        packs = q({'cmd':'card_packs'})
        assert packs['generation'] == 722 and len(packs['packs']) == 722, packs
        shop_audit = q({'cmd':'card_shop'})
        assert shop_audit['economy']['direct_violations'] == 0, shop_audit
        assert shop_audit['economy']['pack_violations'] == 0, shop_audit
        assert shop_audit['rarity_audit']['atk2500_violations'] == 0, shop_audit
        assert shop_audit['rarity_audit']['atk3000_violations'] == 0, shop_audit
        assert shop_audit['rarity_audit']['megamorph_min_tier'] == 3, shop_audit
        assert shop_audit['rarity_audit']['ultimate_min_tier'] == 3, shop_audit
        rewards = q({'cmd':'starchip_rewards','op':'state'})
        assert rewards['configured_rules'] >= 1, rewards
        authored = rewards['rules'][0]
        assert authored['mode'] == 1 and authored['opponent'] == 9, authored
        assert authored['outcome'] == 1 and authored['rank'] == 4, authored
        assert authored['amount'] == 123456, authored

        # Card 7 is the package's deliberately strong no-effect control. Card
        # 1 exercises a real authored on-summon effect before card 7 wins.
        q({'cmd':'card_manager_set','open':1})
        time.sleep(1)
        q({'cmd':'card_manager_set','card':7})
        card7 = q({'cmd':'card_manager'})
        assert int(card7['atk']) == 3250 and int(card7['def']) == 540, card7
        assert str(card7['password']) == '44034117', card7
        assert card7['effective_sell_price'] <= int(card7['price']), card7
        assert card7['effective_sell_source'] == 2, card7
        q({'cmd':'card_manager_set','card':1})
        card1 = q({'cmd':'card_manager'})
        assert int(card1['atk']) == 2820 and str(card1['password']) == '28755651', card1
        assert 'gain 500' in card1.get('desc','').lower(), card1
        assert card1['effective_sell_price'] <= int(card1['price']), card1
        q({'cmd':'card_manager_set','card':125})
        card125 = q({'cmd':'card_manager'})
        monster125 = card125['geom']['monster']
        assert monster125['on_summon'] == 'destroy_strongest', monster125
        assert monster125['on_flip'] == 'destroy_own_lp', monster125
        q({'cmd':'card_manager_set','card':366})
        card366 = q({'cmd':'card_manager'})
        expected_long_desc = ('Effect: When it|attacks: destroy|your own monsters.|'
                              'When flipped up: the|foe loses 1000 LP.|Cannot be destroyed|'
                              'in battle. ATK and|DEF +300 per ally.')
        assert card366['desc'] == expected_long_desc, card366
        assert len(card366['desc']) == 149, card366['desc']
        q({'cmd':'fm_editor','open':0})
        time.sleep(1)

        # Keep the imported amount but broaden the selector in this disposable
        # scratch copy so any genuine Free Duel win reaches the visual oracle.
        rewards = q({'cmd':'starchip_rewards','op':'set','index':0,
                     'mode':1,'opponent':-1,'outcome':1,'rank':-1,
                     'amount':123456})
        assert rewards['rules'][0]['amount'] == 123456, rewards
        for addr in (0x801D06F4, 0x801D36F4):
            q({'cmd':'write_mem','addr':f'{addr:08X}',
               'hex':bytes([p.rd(addr,1)[0] | 0x40]).hex()})
        p.press('down',20,1); p.press('cross',20,4); p.press('cross',40,2)
        assert p.mode() == 0xC6, hex(p.mode())
        shot('stress-free-duel-grid')
        q({'cmd':'savestate','op':'save','slot':8})
        time.sleep(2)

        def wait_cast(before, label):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                state = q({'cmd':'monster_effects'})
                if (state['idle'] and not state['queue'] and not state['casting'] and
                    state['casts_done'] > before['casts_done']):
                    assert state['casts_stalled'] == before['casts_stalled'], (label,state)
                    assert p.frame() > 0, label
                    return state
                time.sleep(.025)
            raise AssertionError((label,state))

        def fresh_duel(card):
            q({'cmd':'savestate','op':'load','slot':8})
            time.sleep(3)
            assert p.mode() == 0xC6, hex(p.mode())
            goto_duel.duel([card])

        # These are genuine imported monster triggers, not synthetic queue
        # calls. Both paths historically crashed when attached to monsters.
        fresh_duel(227)
        fx0 = q({'cmd':'monster_effects'})
        assert turns.play_turn([227]), 'on-summon Dark Hole monster missing'
        fx_dark = wait_cast(fx0, 'monster Dark Hole')
        shot('stress-monster-dark-hole')
        fresh_duel(123)
        fx_before_jar = q({'cmd':'monster_effects'})
        assert turns.play_turn([123]), 'on-summon Dragon Capture Jar monster missing'
        fx_jar = wait_cast(fx_before_jar, 'monster Dragon Capture Jar')
        shot('stress-monster-dragon-jar')

        # Card 125 carries two independent authored trigger definitions. Its
        # compound flip rule itself queues both destruction and LP loss; this
        # is the high-risk multi-effect resolution path.
        fresh_duel(125)
        slot = next(slot for slot,card in turns.hand() if card == 125)
        turns.summon(slot, face_up=False)
        facedown = None
        for row in range(5,10):
            at = 0x801A7AD8 + row * 0x1c
            raw = p.rd(at,0x1c)
            if int.from_bytes(raw[0xc:0xe],'little') != 125: continue
            flags = int.from_bytes(raw[0x16:0x18],'little')
            if flags & 0x1000:
                facedown = (at,flags)
                break
        assert facedown is not None, turns.field()
        fx_before_flip = q({'cmd':'monster_effects'})
        q({'cmd':'write_mem','addr':f'{facedown[0]+0x16:08X}',
           'hex':(facedown[1] & ~0x1000).to_bytes(2,'little').hex()})
        fx_multi_flip = wait_cast(fx_before_flip, 'multi-effect on_flip')
        assert fx_multi_flip['casts_done'] - fx_before_flip['casts_done'] == 2, fx_multi_flip
        shot('stress-multi-effect-monster')

        # The three menu-started Free Duels above exercised the authored
        # effects. Use the established real near-win duel fixture for a
        # deterministic stock result boundary, marking its persisted mode byte
        # as Free Duel before the win so the reward selector takes that path.
        if not args.near_win_state:
            raise RuntimeError('stress reward test requires --near-win-state')
        q({'cmd':'savestate','op':'load','slot':0})
        time.sleep(4)
        flags = p.rd(0x8009B365,1)[0] | 0x80
        q({'cmd':'write_mem','addr':'8009B365','hex':bytes([flags]).hex()})
        live_rule = q({'cmd':'starchip_rewards','op':'state'})['rules'][0]
        if (live_rule['mode'],live_rule['opponent'],live_rule['outcome'],
            live_rule['rank'],live_rule['amount']) != (1,-1,1,-1,123456):
            q({'cmd':'starchip_rewards','op':'set','index':0,
               'mode':1,'opponent':-1,'outcome':1,'rank':-1,'amount':123456})
        for _ in range(5):
            p.press('cross',12,4)
            reward_probe = q({'cmd':'starchip_rewards','op':'state'})
            if reward_probe['matched']: break
        else: raise RuntimeError(('near-win result did not match reward',reward_probe))

        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            reward0 = q({'cmd':'starchip_rewards','op':'state'})
            if reward0['matched'] and reward0['page'] == 0 and reward0['visible']:
                break
            time.sleep(.05)
        else: raise AssertionError(reward0)
        assert reward0['mode'] == 1 and reward0['outcome'] == 1, reward0
        assert reward0['amount'] == 123456 and reward0['size'] == [100,17], reward0
        assert reward0['background_alpha'] == 255, reward0
        assert reward0['after'] == min(999999, reward0['before'] + 123456), reward0
        result0 = present_shot('stress-reward-page-0')

        # Right leaves the summary for the mod's card-results page. Telemetry
        # retains how many composed frames covered that transition, so it can
        # be asserted after a normal human-length press.
        p.press('right', 6, 0)
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            reward1 = q({'cmd':'starchip_rewards','op':'state'})
            if reward1['page_exit_events'] > reward0['page_exit_events']:
                break
            time.sleep(.01)
        else: raise AssertionError(reward1)
        assert reward1['page_exit_frames'] - reward0['page_exit_frames'] >= 2, (reward0,reward1)
        result1 = present_shot('stress-reward-next-page')
        f0 = p.frame(); time.sleep(.5); assert p.frame() > f0
        return {'import':imported,'card_packs':len(packs['packs']),
                'override_fields':packs['overrides'],
                'shop_audit':shop_audit,
                'authored_rule':authored,'card1':{'atk':card1['atk'],'password':card1['password']},
                'card7':{'atk':card7['atk'],'def':card7['def'],'password':card7['password']},
                'multi_effect_card':{'id':125,'on_summon':monster125['on_summon'],
                                     'on_flip':monster125['on_flip']},
                'long_description':{'card':366,'characters':len(card366['desc']),
                                    'text':card366['desc']},
                'effect_casts':1 + 1 + (fx_multi_flip['casts_done'] - fx_before_flip['casts_done']),
                'effect_checkpoints':{'dark_hole':fx_dark['casts_done']-fx0['casts_done'],
                                      'dragon_jar':fx_jar['casts_done']-fx_before_jar['casts_done'],
                                      'multi_flip':fx_multi_flip['casts_done']-fx_before_flip['casts_done']},
                'reward_page0':reward0,'reward_after_transition':reward1,
                'screenshots':[result0,result1]}

    def effects():
        """Every synthetic effect class, queue bounds, and mid-cast restore."""
        restore_menu()
        for addr in (0x801D06F4, 0x801D36F4):
            q({'cmd':'write_mem','addr':f'{addr:08X}',
               'hex':bytes([p.rd(addr,1)[0] | 0x40]).hex()})
        p.press('down',20,1);p.press('cross',20,4);p.press('cross',40,2)
        goto_duel.duel([1])
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            baseline=q({'cmd':'monster_effects'})
            if baseline['idle']: break
            time.sleep(.025)
        else: raise AssertionError(baseline)
        q({'cmd':'savestate','op':'save','slot':9})

        # Names and ids mirror PSX_CARD_FX_*; ritual (15) intentionally has no
        # synthetic handler. The last five are host-composed effects.
        inventory = [
            (1,'heal'),(2,'damage'),(3,'destroy_type'),(4,'destroy_atk'),
            (5,'raigeki'),(6,'dark_hole'),(7,'dragon_jar'),
            (8,'stop_defense'),(9,'flip'),(10,'weaken'),(11,'swords'),
            (12,'cursebreaker'),(13,'harpie'),(14,'field'),
            (16,'destroy_strongest'),(17,'lose_lp'),(18,'gamble_lp'),
            (19,'gamble'),(20,'destroy_own'),(21,'destroy_own_lp')]
        header = (Path(__file__).resolve().parents[1] / 'src' / 'psx_card_packs.h').read_text()
        enum_body = re.search(r'enum\s*\{\s*PSX_CARD_FX_NONE\b(.*?)PSX_CARD_FX_COUNT',
                              header, re.S)
        assert enum_body, 'PSX_CARD_FX enum not found'
        declared = ['NONE'] + re.findall(r'PSX_CARD_FX_([A-Z_]+)', enum_body.group(1))
        covered = [(i, name.lower()) for i, name in enumerate(declared)
                   if name not in ('NONE', 'RITUAL')]
        assert covered == inventory, ('effect inventory is stale', covered, inventory)
        rows=[]
        for n,(fx,name) in enumerate(inventory):
            q({'cmd':'savestate','op':'load','slot':9})
            time.sleep(.1)
            # Give LP effects room and preserve a simple semantic oracle.
            q({'cmd':'write_mem','addr':'800EA004','hex':(7000 if fx==1 else 8000).to_bytes(2,'little').hex()})
            q({'cmd':'write_mem','addr':'800EA024','hex':(8000).to_bytes(2,'little').hex()})
            before=q({'cmd':'monster_effects'})
            side=n & 1
            t0=time.monotonic()
            q({'cmd':'monster_effects','side':side,'card':1,'fx':fx,
               'amount':500,'target':3,'terrain':1})
            deadline=t0+8
            while time.monotonic()<deadline:
                after=q({'cmd':'monster_effects'})
                if not after['queue'] and not after['casting'] and after['idle']: break
                time.sleep(.02)
            else: raise AssertionError((name,after,q({'cmd':'card_effects'})))
            elapsed=time.monotonic()-t0
            expected_casts = 0 if fx in (17,18) else 1
            assert after['casts_done']==before['casts_done']+expected_casts,(name,before,after)
            assert after['casts_stalled']==before['casts_stalled'],(name,after)
            assert after['casts_cancelled']==before['casts_cancelled'],(name,after)
            assert not after['flipped'],(name,after)
            lp=turns.lp()
            if fx==1: assert lp[side] > 7000,(name,side,lp)
            if fx==2: assert lp[side ^ 1] == 7500,(name,side,lp)
            if fx==17: assert lp[side] == 7500,(name,side,lp)
            rows.append({'id':fx,'name':name,'side':side,'seconds':elapsed,
                         'lp':lp,'audio_skips':after['audio_skips']})

        # The repaired class-4 path is frame-driven. Exercise it at every
        # supported user-facing speed and require genuine completion at each
        # rate; a moving frame counter or watchdog event is not a pass.
        speed_rows=[]
        for speed in (1, 2, 3, 4):
            q({'cmd':'savestate','op':'load','slot':9});time.sleep(.1)
            q({'cmd':'game_speed','mult':speed})
            speed_state=q({'cmd':'game_speed'})
            assert speed_state['effective'] == speed,(speed,speed_state)
            before=q({'cmd':'monster_effects'}); f0=p.frame(); t0=time.monotonic()
            q({'cmd':'monster_effects','side':speed & 1,'card':1,'fx':7})
            deadline=t0+8
            while time.monotonic()<deadline:
                after=q({'cmd':'monster_effects'})
                if after['casts_done']==before['casts_done']+1 and after['idle']: break
                time.sleep(.02)
            else: raise AssertionError((speed,after))
            assert after['casts_stalled']==before['casts_stalled'] and not after['flipped'],after
            speed_rows.append({'speed':speed,'seconds':time.monotonic()-t0,
                               'frames':p.frame()-f0,'audio_skips':after['audio_skips']})
        q({'cmd':'game_speed','mult':1})

        # Saturate the bounded queue. Rejection must be explicit telemetry,
        # never an invisible overwrite; restore immediately so the test does
        # not spend a minute draining deliberate duplicates.
        q({'cmd':'savestate','op':'load','slot':9});time.sleep(.1)
        for _ in range(40):
            try: q({'cmd':'monster_effects','side':0,'card':1,'fx':7})
            except RuntimeError: pass
        saturated=q({'cmd':'monster_effects'})
        assert saturated['queue_dropped'] > baseline['queue_dropped'],saturated
        q({'cmd':'savestate','op':'load','slot':9});time.sleep(.1)

        # Save while the acting side is flipped and the parameter hold is
        # live, finish once, then restore and finish the same timeline again.
        base=q({'cmd':'monster_effects'})
        q({'cmd':'monster_effects','side':1,'card':1,'fx':7})
        deadline=time.monotonic()+4
        while time.monotonic()<deadline:
            active=q({'cmd':'monster_effects'})
            if active['casting'] and active['flipped'] and active['audio_skips']>base['audio_skips']: break
            time.sleep(.01)
        else: raise AssertionError(active)
        q({'cmd':'savestate','op':'save','slot':10})
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            first=q({'cmd':'monster_effects'})
            if first['casts_done']==base['casts_done']+1 and first['idle']: break
            time.sleep(.02)
        else: raise AssertionError(first)
        q({'cmd':'savestate','op':'load','slot':10})
        time.sleep(.1)
        restored=q({'cmd':'monster_effects'})
        assert restored['casting'] and restored['flipped'],restored
        assert restored['casts_done']==base['casts_done'],(base,restored)
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            second=q({'cmd':'monster_effects'})
            if second['casts_done']==base['casts_done']+1 and second['idle']: break
            time.sleep(.02)
        else: raise AssertionError(second)
        assert not second['flipped'] and not second['casts_stalled'],second
        card=q({'cmd':'card_effects'})
        assert not card['hold']['active'] and not card['holds_stalled'],card
        f=p.frame();time.sleep(.25)
        assert p.frame()>f,'frames stopped after synthetic effects'
        return {'inventory':rows,'speeds':speed_rows,'queue_saturation':saturated,
                'midcast':{'active':active,'restored':restored,'finished':second},
                'card_effects':card,'frames_after':p.frame()-f}

    cmd = [str(args.exe),'--no-launcher','--renderer',args.renderer,
           '--memcard-dir',str(data),'--disc',str(args.disc),'--debug-port',str(args.port)]
    (root/'launch.json').write_text(json.dumps(cmd,indent=2)+'\n')
    env = dict(os.environ, SDL_FILE_DIALOG_DRIVER='nosuchdriver', PSX_PORTABLE='1')
    # A native runtime launched from an AppImage-hosted editor must not use
    # the editor's executable path for its own sidecars.
    env.pop('APPIMAGE', None)
    env.pop('APPDIR', None)
    with (root/'runtime.log').open('w') as log:
        child = subprocess.Popen(cmd,cwd=args.exe.parent,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            route.wait_port()
            if args.resume_menu:
                time.sleep(12)
                restore_menu()
            for name in args.groups.split(','):
                group(name, {'menus':menus,'duel':duel,'managers':managers,'package':package,'video':video,'video_actions':video_actions,'mods':mods,'win':win,'magic':magic,'effects':effects,'stress_free_duel':stress_free_duel}[name])
        finally:
            dbg.q({'cmd':'quit_graceful'})
            try: child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=10)
            transcript.close()
    return int(any(r['result']!='pass' for r in results.values()))


if __name__ == '__main__':
    sys.exit(main())
