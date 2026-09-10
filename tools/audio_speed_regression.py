#!/usr/bin/env python3
"""Measure menu game speed independently from real-time audio production.

Use a menu state from the SAME recompiler/runtime revision. All data is copied
to a new /tmp profile. Requires a desktop, Pillow and PSX_DEBUG_TOOLS.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'disc', 'seed', 'menu-state', 'menu-reference', 'scratch'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--port', type=int, default=4386)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--speeds', type=int, nargs='+', default=[1, 2, 1])
    parser.add_argument('--borderless', action='store_true')
    parser.add_argument('--renderer', choices=('software', 'opengl'), default='software')
    args = parser.parse_args()
    root = args.scratch.resolve()
    if not root.is_relative_to(Path('/tmp')) or root.exists():
        parser.error('--scratch must be a new directory under /tmp')
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            parser.error('test port is occupied')
    root.mkdir(parents=True)
    player = root / 'player'
    (player / 'openbios').mkdir(parents=True)
    shutil.copyfile(args.seed, player / 'card1.mcd')
    shutil.copyfile(args.menu_state, player / 'openbios' / args.menu_state.name)
    if args.borderless:
        (player / 'menu_settings.ini').write_text('screen=1\nsupersampling=2\n')
    os.environ['YGOFM_DEBUG_PORT'] = str(args.port)
    os.environ['SHOTDIR'] = str(root)
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    import dbg
    import game_route
    env = dict(os.environ)
    env.pop('APPIMAGE', None)
    env.pop('APPDIR', None)
    env['PSX_RUNTIME_PERF_DIAG'] = '1'
    results = []
    owned = False
    with (root / 'runtime.log').open('w') as log, (root / 'commands.jsonl').open('w') as commands:
        exe = args.exe.resolve()
        proc = subprocess.Popen([str(exe), '--no-launcher', '--renderer', args.renderer,
                                 '--memcard-dir', str(player), '--disc', str(args.disc.resolve()),
                                 '--debug-port', str(args.port)], cwd=exe.parent,
                                env=env, stdout=log, stderr=subprocess.STDOUT)

        def q(cmd, **params):
            request = dict(cmd=cmd, **params)
            result = dbg.q(request)
            commands.write(json.dumps(dict(request=request, response=result)) + '\n')
            commands.flush()
            assert result.get('ok'), result
            return result

        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'Test process exited'
                state = dbg.q({'cmd': 'card_packs'})
                if state.get('dir'):
                    assert Path(state['dir']).resolve().is_relative_to(player), state
                    owned = True
                    break
                time.sleep(.3)
            assert owned, 'Title did not initialize'
            time.sleep(3)
            q('savestate', op='load', slot=7)
            q('game_speed', mult=1)
            time.sleep(5)
            assert game_route.screen_matches(str(args.menu_reference), 'loaded-menu')[0]
            # A collapsed bar reproduces the player's normal menu view.
            if q('menu_state')['expanded']:
                q('menu_key', key=27)
            for index, speed in enumerate(args.speeds):
                q('game_speed', mult=speed)
                time.sleep(8)
                before = q('audio_stats')
                frame = q('frame')['frame']
                start = time.monotonic()
                time.sleep(args.seconds)
                after = q('audio_stats')
                end_frame = q('frame')['frame']
                elapsed = time.monotonic() - start
                state = q('game_speed')
                fps = (end_frame - frame) / elapsed
                spu_rate = (after['taps'][0]['frames'] - before['taps'][0]['frames']) / elapsed
                underruns = after['out']['underruns'] - before['out']['underruns']
                overflow = after['out']['overflow_drops'] - before['out']['overflow_drops']
                for tap in (0, 2):
                    q('audio_wav', tap=tap, count=str(44100 * 10),
                      path=str(root / f'{index}-{speed}x-tap{tap}.wav'))
                result = dict(speed=speed, seconds=elapsed, fps=fps, spu_rate=spu_rate,
                              underruns=underruns, overflow=overflow, state=state,
                              before=before, after=after, latency=q('latency'))
                results.append(result)
                (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                assert state['effective'] == speed and not state['eased'], state
                assert abs(fps / (59.94 * speed) - 1) < .015, result
                assert abs(spu_rate / 44100 - 1) < .015, result
                assert underruns == 0 and overflow == 0, result
                print(f'ok: {speed}x: {fps:.2f} fps, SPU {spu_rate:.0f} Hz, no underruns/overflow', flush=True)
            assert game_route.screen_matches(str(args.menu_reference), 'final-menu')[0]
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
