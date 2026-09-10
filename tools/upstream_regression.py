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
                group(name, {'menus':menus,'duel':duel,'managers':managers,'package':package,'video':video,'video_actions':video_actions,'mods':mods,'win':win,'magic':magic,'effects':effects}[name])
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
