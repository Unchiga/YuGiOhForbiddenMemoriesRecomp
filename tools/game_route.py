#!/usr/bin/env python3
"""game_route.py -- drive the DEBUG build (./Play.sh -dbg --no-launcher) from a
cold boot to the screens the mods are checked on. Everything here was learned
the hard way; read tools/LIVE_TESTING.md before changing the hold lengths.

    python3 tools/game_route.py menu        boot -> loaded-save menu (CAMPAIGN row)
    python3 tools/game_route.py library     ... -> LIBRARY, first card shown
    python3 tools/game_route.py password    ... -> PASSWORD entry screen
    python3 tools/game_route.py shot <tag>  screenshot to $SHOTDIR/shot_<tag>.png

The intro movie is NOT skipped by START while it plays; START only works once
the title (PUSH START BUTTON) is up, and a second START there opens the main
menu. The title is detected from the screen, not from the frame counter: the
game may run at 2x-3x (VIDEO > Game speed), which makes frame counts useless
for timing the movie. Pressing START one time too many on the main menu
selects NEW GAME and lands on name entry -- restart the game if that happens.
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import live_probe as p
from PIL import Image, ImageChops, ImageStat

SHOT = os.environ.get('SHOTDIR', '/tmp')
REF_TITLE = os.path.join(HERE, 'refs', 'title_80x60.png')

def wait_port(timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if p.frame() is not None: return True
        except Exception: pass
        time.sleep(1)
    raise SystemExit('debug server never answered on 127.0.0.1:4370')

def screen_matches(ref_path, tag='probe', tol=18.0):
    """Mean absolute difference between the live screen and a reference, both
    reduced to 80x60 grey. Title screen vs. the reference is ~5; the movie is
    30+; the main menu is ~25."""
    f = p.shot(tag)
    a = Image.open(f).convert('L').resize((80, 60))
    b = Image.open(ref_path).convert('L').resize((80, 60))
    d = ImageStat.Stat(ImageChops.difference(a, b)).mean[0]
    return d <= tol, d

def wait_title(timeout=300):
    """Block until the title screen (PUSH START BUTTON) is up."""
    wait_port()
    t0 = time.time()
    while time.time() - t0 < timeout:
        ok, d = screen_matches(REF_TITLE, 'title_probe')
        if ok: return d
        time.sleep(3)
    raise SystemExit('title screen never appeared')

def to_main_menu():
    wait_title()
    time.sleep(1.0)
    p.press('start', 60, 3.0)          # title -> NEW GAME / LOAD / 2P DUEL / TRADE / OPTION

def to_loaded_menu():
    """Main menu -> LOAD -> loaded-save menu (CAMPAIGN / FREE DUEL / BUILD DECK /
    LIBRARY / PASSWORD / SAVE), cursor on CAMPAIGN."""
    to_main_menu()
    p.press('down', 20, 1.5)           # NEW GAME -> LOAD
    p.press('cross', 20, 2.0)          # LOAD -> memory card prompt
    p.press('cross', 20, 4.0)          # YES
    p.press('cross', 20, 3.0)          # dismiss LOAD COMPLETE
    return p.shot('loaded_menu')

def to_library():
    to_loaded_menu()
    for _ in range(3): p.press('down', 20, 1.0)     # CAMPAIGN -> FREE DUEL -> BUILD DECK -> LIBRARY
    p.press('cross', 20, 4.0)
    return p.shot('library')

def to_password():
    to_loaded_menu()
    for _ in range(4): p.press('down', 20, 1.0)     # -> PASSWORD
    p.press('cross', 20, 4.0)
    return p.shot('password')

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'menu'
    if cmd == 'menu': print(to_loaded_menu())
    elif cmd == 'library': print(to_library())
    elif cmd == 'password': print(to_password())
    elif cmd == 'title': print('title diff', wait_title())
    elif cmd == 'shot': print(p.shot(sys.argv[2] if len(sys.argv) > 2 else 'now'))
    elif cmd == 'match': print(screen_matches(sys.argv[2], 'match'))
    else: raise SystemExit(__doc__)
