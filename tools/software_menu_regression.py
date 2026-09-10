#!/usr/bin/env python3
"""Exercise real F10 input and composed SDL pixels using a fresh scratch profile.

Example: python3 tools/software_menu_regression.py --exe build-dbg/\
Yu_Gi_Oh_Forbidden_Memories_Recompiled --disc /path/game.bin --scratch /tmp/menu-check
Requires Pillow, a desktop display, and a PSX_DEBUG_TOOLS build.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'psxrecomp/tools'))
import debug_client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--port', type=int, default=4376)
    args = parser.parse_args()
    root = args.scratch.resolve()
    if not root.is_relative_to(Path('/tmp')):
        parser.error('--scratch must be a new directory under /tmp')
    root.mkdir(parents=True, exist_ok=False)
    player = root / 'player'
    player.mkdir()
    exe = args.exe.resolve()
    env = dict(os.environ)
    env.pop('APPIMAGE', None)
    env.pop('APPDIR', None)
    results = []
    with (root / 'runtime.log').open('w') as log, (root / 'commands.jsonl').open('w') as commands:
        proc = subprocess.Popen([str(exe), '--no-launcher', '--renderer', 'software',
                                 '--memcard-dir', str(player), '--disc', str(args.disc.resolve()),
                                 '--debug-port', str(args.port)], cwd=exe.parent, env=env,
                                stdout=log, stderr=subprocess.STDOUT)

        def q(cmd, **kwargs):
            request = dict(cmd=cmd, **kwargs)
            response = debug_client.query('127.0.0.1', args.port, request)
            commands.write(json.dumps(dict(request=request, response=response)) + '\n')
            commands.flush()
            assert response.get('ok'), response
            return response

        def shot(name):
            sequence = q('present_shot_seq')['seq']
            path = root / (name + '.png')
            q('present_shot', path=str(path))
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                result = q('present_shot_seq')
                if result['seq'] > sequence:
                    assert result['wrote'], result
                    return Image.open(path).convert('RGB')
                time.sleep(.1)
            raise AssertionError('Composed SDL capture timed out')

        owned = False
        try:
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'Runtime exited; inspect runtime.log'
                try:
                    packs = q('card_packs')
                    assert packs.get('dir'), 'Title hooks have not initialized yet'
                    break
                except (OSError, AssertionError):
                    time.sleep(.3)
            else:
                raise AssertionError('Debug server did not start')
            assert Path(packs['dir']).resolve().is_relative_to(player), packs
            owned = True
            time.sleep(15)
            state = q('menu_state')
            if state['visible']:
                q('menu_key', key=0x40000043)
                time.sleep(.3)
            assert not q('menu_state')['visible']
            shot('hidden')
            q('menu_key', key=0x40000043)
            time.sleep(.3)
            seen = set()
            names = ['file', 'view', 'video', 'audio', 'game', 'cheats', 'mods']
            for _ in names:
                state = q('menu_state')
                assert state['visible'] and state['expanded'], state
                assert state['logical_w'] >= 640, 'Fresh profile started with a tiny window'
                picture = shot(names[state['menu']])
                assert picture.width == state['logical_w'] * state['ui_scale']
                assert picture.height == state['logical_h'] * state['ui_scale'], 'Capture cropped UI'
                # The selected row must actually be composited, beyond the title
                # bar. A menu_state-only check passed with the missing SDL draw.
                region = picture.crop((0, 35, picture.width, min(300, picture.height)))
                pixels = getattr(region, 'get_flattened_data', region.getdata)()
                blue = sum(25 <= r <= 65 and 35 <= g <= 90 and 75 <= b <= 140
                           and b > r + 20 for r, g, b in pixels)
                assert blue > 300, f'Menu {state["menu"]}: no visible selected row'
                results.append(dict(menu=names[state['menu']], selected_pixels=blue,
                                    size=picture.size, state=state))
                seen.add(state['menu'])
                q('menu_key', key=0x4000004F)  # SDL RIGHT
                time.sleep(.3)
            assert seen == set(range(7)), seen
            q('menu_settings_save')
            settings = (player / 'menu_settings.ini').read_text()
            assert 'windowed_scale=3' in settings.replace(' ', ''), settings
            q('card_manager_set', open=1)
            time.sleep(2)
            manager = q('card_manager')
            assert manager['open'] and manager['rows'] == 722, manager
            q('card_manager_shot', path=str(root / 'card-manager.ppm'))
            q('card_manager_set', open=0)
            (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print('ok: software F10, seven composed menus, full-window captures, 3x default, Card Manager')
        finally:
            if owned and proc.poll() is None:
                q('quit_graceful')
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=10)


if __name__ == '__main__':
    main()
