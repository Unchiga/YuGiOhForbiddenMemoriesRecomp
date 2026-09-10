#!/usr/bin/env python3
"""Scratch-only live regression capture for an explicitly selected runtime.

Run from either revision with the same seed, disc, and arguments. JSON records
checks separately from screenshots; a screenshot alone is never a pass.
"""
import argparse
import json
import os
from pathlib import Path
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
    ap.add_argument('--groups', default='menus,duel,managers,package')
    args = ap.parse_args()
    root = args.scratch.resolve()
    personal = Path.home() / 'Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled'
    if root == personal or personal in root.parents or root.exists():
        ap.error('scratch must be a NEW directory outside personal player data')
    args.exe = args.exe.resolve(strict=True)
    args.seed = args.seed.resolve(strict=True)
    args.disc = args.disc.resolve(strict=True)
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
        s = q({'cmd': 'card_manager'})
        x,y,w,h = s['geom']['value'][2]
        q({'cmd':'card_manager_click','x':x+w//2,'y':y+h//2,'button':1})
        q({'cmd':'card_manager_key','key':'a','ctrl':1})
        q({'cmd':'card_manager_type','text':'3100'})
        q({'cmd':'card_manager_key','key':'return'})
        x,y = s['geom']['btn'][0]
        q({'cmd':'card_manager_click','x':x,'y':y,'button':1})
        s=q({'cmd':'card_manager'})
        assert int(s['atk']) == 3100, s
        q({'cmd':'card_manager_shot','path':str(shots/'card-manager.ppm')})
        perf('card-manager')
        q({'cmd':'card_share','op':'export','path':str(root/'cards.ygocards')})
        q({'cmd':'card_share','op':'import','path':str(root/'cards.ygocards')})
        q({'cmd':'card_manager_set','open':0})
        q({'cmd':'fusion_manager','open':1})
        time.sleep(2)
        q({'cmd':'fusion_manager','a':1,'b':2,'result':37})
        q({'cmd':'fusion_manager','shot':str(shots/'fusion-manager.ppm')})
        q({'cmd':'fusion_manager','open':0})
        q({'cmd':'drop_viewer_set','open':1})
        time.sleep(2)
        q({'cmd':'drop_viewer_set','randomize':2026})
        q({'cmd':'drop_viewer_shot','path':str(shots/'drop-manager.ppm')})
        q({'cmd':'drop_viewer_set','open':0})
        q({'cmd':'cpu_manager','open':1})
        time.sleep(2)
        q({'cmd':'cpu_data','duelist':1,'name':'Catchup Simon','save':1})
        q({'cmd':'cpu_manager','duelist':1,'shot':str(shots/'cpu-manager.ppm')})
        q({'cmd':'cpu_manager','open':0})
        time.sleep(2)
        for _ in range(3): p.press('down',20,1)
        p.press('cross',20,3)
        p.press('cross',20,3)
        shot('edited-library-card')
        return {'card_attack':3100,'fusion':[1,2,37],'drop_seed':2026}

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
        (card/'card.ini').write_text('attack = 4000\ncolor = purple\ndescription = Catchup description|sector override test\non_summon = damage 500\n')
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
        assert turns.play_turn([1])
        time.sleep(3)
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
        for n in range(12):
            phase=int.from_bytes(p.rd(0x8009B23A,2),'little')&15
            print('win probe',n,phase,turns.lp(),flush=True)
            if phase==13: break
            if phase==5 and p.rd(0x8009B174,1)[0]&0x7f==6 and turns.opp():
                target=turns.opp()[0][1]
                for _ in range(12):
                    if int.from_bytes(p.rd(0x8009B338,2),'little')==target: break
                    p.press('right',6,.8)
                else: raise RuntimeError('no attack target under cursor')
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
        return {'finished_phase':phase,'lp':turns.lp(),'frames_advanced':p.frame()-f,
                'monster_effects':q({'cmd':'monster_effects'})}

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

    cmd = [str(args.exe),'--no-launcher','--renderer',args.renderer,
           '--memcard-dir',str(data),'--disc',str(args.disc),'--debug-port',str(args.port)]
    (root/'launch.json').write_text(json.dumps(cmd,indent=2)+'\n')
    env = dict(os.environ, SDL_FILE_DIALOG_DRIVER='nosuchdriver')
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
                group(name, {'menus':menus,'duel':duel,'managers':managers,'package':package,'video':video,'video_actions':video_actions,'mods':mods,'win':win,'magic':magic}[name])
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
