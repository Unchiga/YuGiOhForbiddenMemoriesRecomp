#!/usr/bin/env python3
"""netplay_pair.py -- two debug builds in one LAN netplay session on this box.

    python3 tools/netplay_pair.py cards      seed host/ and guest/ card dirs (never touches the real card)
    python3 tools/netplay_pair.py start      launch both instances (host seat 0, guest seat 1) and wait for boot
    python3 tools/netplay_pair.py stop
    python3 tools/netplay_pair.py shot TAG   present-time screenshot of both instances
    python3 tools/netplay_pair.py menu       host: title -> main menu (NEW GAME / LOAD / 2P DUEL / TRADE / OPTION)

As a module:

    import netplay_pair as np
    H, G = np.inst(0), np.inst(1)
    H.press('start', 60); H.shot('title'); G.rd(0x8009B26C, 1)

Host = seat 0 = pad 1, guest = seat 1 = pad 2. Each instance's debug server
only drives its own seat's pad (slot 0 on the host, slot 1 on the guest); the
other seat's presses arrive through the session. Screenshots go through
screenshot_present so the guest overlays are in them.

Instance layout (an explicit, per-run NETPAIR_DIR outside player data):
    host/  card1.mcd = copy of required NETPAIR_SEED, card2.mcd = blank
    guest/ card1.mcd = the same save with a different duelist code, card2.mcd = blank
Debug ports default to 4372/4373 and UDP to 7777/7778. Override them with
NETPAIR_DEBUG_HOST/GUEST and NETPAIR_UDP_HOST/GUEST; all four are bind-tested
before launch.
"""
import os, sys, time, subprocess, shutil, socket

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, os.path.join(REPO, 'psxrecomp', 'tools'))
import debug_client as dc

EXE = os.environ.get('NETPAIR_EXE') or os.path.join(REPO, 'build-dbg', 'Yu_Gi_Oh_Forbidden_Memories_Recompiled')
DISC = os.environ.get('NETPAIR_DISC')
SEED = os.environ.get('NETPAIR_SEED')
BLANK = os.environ.get('NETPAIR_BLANK')
DATA = os.path.expanduser('~/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled')
ROOT = os.environ.get('NETPAIR_DIR') or os.path.join(
    os.environ.get('CLAUDE_SCRATCHPAD', '/tmp'), 'netpair')
SHOT = os.environ.get('SHOTDIR', os.path.join(ROOT, 'shots'))
NAMES = {0: 'host', 1: 'guest'}
DBG = {
    0: int(os.environ.get('NETPAIR_DEBUG_HOST', '4372')),
    1: int(os.environ.get('NETPAIR_DEBUG_GUEST', '4373')),
}
UDP = {
    0: int(os.environ.get('NETPAIR_UDP_HOST', '7777')),
    1: int(os.environ.get('NETPAIR_UDP_GUEST', '7778')),
}
SESSION = 7

B = dict(select=0x0001, start=0x0008, up=0x0010, right=0x0020, down=0x0040,
         left=0x0080, l2=0x0100, r2=0x0200, l1=0x0400, r1=0x0800,
         triangle=0x1000, circle=0x2000, cross=0x4000, square=0x8000)


class Inst:
    def __init__(self, slot):
        self.slot = slot
        self.name = NAMES[slot]
        self.port = DBG[slot]
        self.dir = os.path.join(ROOT, self.name)
        self.log = os.path.join(self.dir, 'runtime.log')

    def q(self, c):
        try:
            return dc.query('127.0.0.1', self.port, c)
        except Exception as e:
            return {'err': str(e)}

    def frame(self):
        return self.q({'cmd': 'frame'}).get('frame')

    def rd(self, addr, n=4):
        r = self.q({'cmd': 'read_ram', 'addr': '%08X' % addr, 'len': n})
        return bytes.fromhex(r.get('data') or r.get('hex') or '')

    def b(self, addr):
        d = self.rd(addr, 1)
        return d[0] if d else None

    def h(self, addr):
        d = self.rd(addr, 2)
        return int.from_bytes(d, 'little') if len(d) == 2 else None

    def w(self, addr):
        d = self.rd(addr, 4)
        return int.from_bytes(d, 'little') if len(d) == 4 else None

    def wr(self, addr, data):
        return self.q({'cmd': 'write_mem', 'addr': '%08X' % addr, 'hex': bytes(data).hex()})

    def mode(self):
        return self.b(0x8009B26C)

    def press(self, name, frames=12, settle=1.2, pad=None):
        pad = self.slot if pad is None else pad
        mask = 0
        for n in name.split('+'):
            mask |= B[n]
        self.q({'cmd': 'press', 'buttons': 0xFFFF & ~mask, 'frames': frames, 'slot': pad})
        time.sleep(0.5)
        self.q({'cmd': 'clear_input'})
        time.sleep(settle)

    def shot(self, tag, wait=4.0):
        os.makedirs(SHOT, exist_ok=True)
        p = os.path.join(SHOT, '%s_%s.png' % (tag, self.name))
        try:
            os.remove(p)
        except OSError:
            pass
        r = self.q({'cmd': 'screenshot_present', 'path': p})
        if not r.get('ok'):
            raise RuntimeError('composed capture unavailable: %r' % r)
        t0 = time.time()
        while time.time() - t0 < wait:
            if os.path.exists(p) and os.path.getsize(p) > 0:
                time.sleep(0.2)
                return p
            time.sleep(0.1)
        raise RuntimeError('composed capture timed out: ' + p)

    def shot_fb(self, tag):
        """Plain framebuffer screenshot (no overlays): for screen detection."""
        os.makedirs(SHOT, exist_ok=True)
        p = os.path.join(SHOT, '%s_%s_fb.png' % (tag, self.name))
        self.q({'cmd': 'screenshot', 'path': p})
        time.sleep(0.3)
        return p

    def screen_diff(self, ref_path, tag='probe'):
        """Mean grey difference (80x60) between the live screen and a reference."""
        from PIL import Image, ImageChops, ImageStat
        f = self.shot_fb(tag)
        a = Image.open(f).convert('L').resize((80, 60))
        b = Image.open(ref_path).convert('L').resize((80, 60))
        return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]

    def is_title(self, tol=18.0):
        return self.screen_diff(os.path.join(TOOLS, 'refs', 'title_80x60.png'), 'title_probe') <= tol

    def burst(self, tag, seconds=6.0, interval=0.35):
        """Present-time screenshots in a row: catches transient leaks."""
        os.makedirs(SHOT, exist_ok=True)
        out = []
        t0 = time.time()
        k = 0
        while time.time() - t0 < seconds:
            p = os.path.join(SHOT, '%s_%s_%02d.png' % (tag, self.name, k))
            self.q({'cmd': 'screenshot_present', 'path': p})
            out.append(p)
            k += 1
            time.sleep(interval)
        time.sleep(0.5)
        return [f for f in out if os.path.exists(f)]

    def tail(self, n=20):
        try:
            with open(self.log, 'r', errors='replace') as f:
                return ''.join(f.readlines()[-n:])
        except OSError:
            return ''

    def grep(self, pat):
        try:
            with open(self.log, 'r', errors='replace') as f:
                return [l.rstrip() for l in f if pat in l]
        except OSError:
            return []


def inst(slot):
    return Inst(slot)


def cards(code=None):
    """Seed both isolated card dirs. NETPAIR_SEED never consults DATA."""
    validate_root()
    if not SEED:
        raise ValueError('NETPAIR_SEED is required; personal player data is never a default')
    src = SEED
    blank = BLANK
    if blank and not os.path.exists(blank):
        raise FileNotFoundError('NETPAIR_BLANK does not exist: ' + blank)
    for s in (0, 1):
        d = os.path.join(ROOT, NAMES[s])
        os.makedirs(d, exist_ok=True)
        if s == 0:
            shutil.copyfile(src, os.path.join(d, 'card1.mcd'))
        else:
            args = [sys.executable, os.path.join(TOOLS, 'save_clone.py'), src,
                    os.path.join(d, 'card1.mcd')]
            if code is not None:
                args += ['--code', str(code)]
            subprocess.check_call(args)
        c2 = os.path.join(d, 'card2.mcd')
        if blank:
            shutil.copyfile(blank, c2)
        elif os.path.exists(c2):
            os.remove(c2)
    print('cards seeded under', ROOT)


def validate_root():
    from pathlib import Path
    if not os.environ.get('NETPAIR_DIR'):
        raise ValueError('NETPAIR_DIR must be explicit and unique to this run')
    root, personal = Path(ROOT).resolve(), Path(DATA).resolve()
    if root == personal or personal in root.parents:
        raise ValueError('NETPAIR_DIR must be outside personal player data')


def pids():
    # Only this pair's children; never kill another test using the same build.
    from pathlib import Path
    dirs = {str(Path(ROOT, n).resolve()) for n in NAMES.values()}
    result = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            args = (proc/'cmdline').read_bytes().split(b'\0')
            args = [a.decode() for a in args if a]
            if not args or Path(args[0]).resolve() != Path(EXE).resolve(): continue
            ix = args.index('--memcard-dir')
            if str(Path(args[ix+1]).resolve()) in dirs: result.append(int(proc.name))
        except (OSError, ValueError, IndexError): pass
    return result


def verify_ports_free():
    """Bind-test every exact port before either peer is started."""
    ports = [(socket.SOCK_STREAM, DBG[s], 'debug') for s in (0, 1)]
    ports += [(socket.SOCK_DGRAM, UDP[s], 'netplay') for s in (0, 1)]
    if len({(kind, port) for kind, port, _ in ports}) != len(ports):
        raise RuntimeError('netplay pair ports must be distinct per protocol')
    held = []
    try:
        for kind, port, label in ports:
            if port < 1 or port > 65535:
                raise RuntimeError('%s port is out of range: %d' % (label, port))
            sock = socket.socket(socket.AF_INET, kind)
            try:
                sock.bind(('127.0.0.1', port))
                if kind == socket.SOCK_STREAM:
                    sock.listen(1)
            except OSError as exc:
                sock.close()
                raise RuntimeError('%s port is not free: %d (%s)' %
                                   (label, port, exc)) from exc
            held.append(sock)
    finally:
        for sock in held:
            sock.close()
    print('ports verified free: debug %d/%d, UDP %d/%d' %
          (DBG[0], DBG[1], UDP[0], UDP[1]))


def stop(limit=20):
    """Gracefully stop only peers owned by this exact executable/root pair."""
    owned = pids()
    if not owned:
        return []
    replies = {}
    for s in (0, 1):
        try:
            replies[s] = inst(s).q({'cmd': 'quit_graceful'})
        except Exception as exc:
            replies[s] = {'error': repr(exc)}
    deadline = time.monotonic() + limit
    remaining = pids()
    while remaining and time.monotonic() < deadline:
        time.sleep(0.25)
        remaining = pids()
    if remaining:
        raise RuntimeError('owned peers did not stop gracefully: %r; replies=%r' %
                           (remaining, replies))
    return owned


def start(extra=(), guest_memcard=True, env_extra=None):
    validate_root()
    if not DISC:
        raise ValueError('NETPAIR_DISC is required')
    if os.path.basename(DISC) != 'Yu-Gi-Oh! Forbidden Memories (USA).cue':
        raise ValueError('NETPAIR_DISC must be the explicit USA cue')
    owned = pids()
    if owned:
        raise RuntimeError('refusing to replace an existing owned pair: %r' % owned)
    verify_ports_free()
    procs = []
    for s in (0, 1):
        i = Inst(s)
        os.makedirs(i.dir, exist_ok=True)
        env = dict(os.environ)
        env.pop('APPIMAGE', None)
        env.pop('APPDIR', None)
        env['PSX_NET_TRANSPORT'] = 'lan'
        env['PSX_NET_GUEST_MEMCARD'] = '1' if guest_memcard else '0'
        env['PSX_PORTABLE'] = '1'
        if env_extra:
            env.update(env_extra)
        args = [EXE, '--no-launcher', '--netplay', '--net-slot', str(s),
                '--net-bind', '127.0.0.1:%d' % UDP[s],
                '--net-peer', '127.0.0.1:%d' % UDP[1 - s],
                '--net-session-id', str(SESSION),
                '--memcard-dir', i.dir, '--debug-port', str(i.port),
                '--renderer', 'opengl', '--disc', DISC] + list(extra)
        log = open(i.log, 'w')
        procs.append(subprocess.Popen(args, cwd=os.path.dirname(EXE), env=env,
                                      stdout=log, stderr=subprocess.STDOUT))
        time.sleep(1.0)
    return procs


def wait_boot(limit=240):
    t0 = time.time()
    while time.time() - t0 < limit:
        fs = [Inst(s).frame() for s in (0, 1)]
        if all(f and f > 8400 for f in fs):
            return fs
        time.sleep(2)
    raise SystemExit('never booted: %r' % ([Inst(s).frame() for s in (0, 1)],))


def wait_change(i, addr, was, limit=30):
    t0 = time.time()
    while time.time() - t0 < limit:
        v = i.b(addr)
        if v != was:
            return v
        time.sleep(0.5)
    return None


def menu(limit=20):
    """Title -> main menu (NEW GAME / LOAD / 2P DUEL / TRADE / OPTION, cursor on
    NEW GAME) on the host. The first title after boot may ignore START until
    the attract loop has cycled, so press until the title is gone. Never
    press twice past the title: a second START picks NEW GAME."""
    H = Inst(0)
    seen_title = False
    for k in range(limit):
        if H.is_title():
            seen_title = True
            H.press('start', 60, 3.0)
            if not H.is_title():
                return k + 1            # title -> menu
        elif seen_title:
            return k                    # already left the title: the menu is up
        else:
            H.press('start', 60, 3.0)   # intro movie: START skips to the title
    raise SystemExit('never left the title')


def to_2p_rules(limit=40):
    """Main menu -> 2P DUEL -> BEGIN LOAD -> the REGULATION OF 2P-DUEL RULES
    screen (scene 0x10, mode byte 0xD0). Returns the mode byte."""
    H = Inst(0)
    menu()
    H.press('down', 20, 1.0)
    H.press('down', 20, 1.0)
    H.press('cross', 20, 3.0)           # CAUTION: 1P slot 1 / 2P slot 2
    H.press('cross', 20, 2.0)           # BEGIN LOAD
    for _ in range(limit):
        if H.mode() == 0xD0:
            return 0xD0
        time.sleep(1)
    return H.mode()


def to_trade(limit=40):
    """Main menu -> TRADE -> BEGIN LOAD -> the trade screen (mode 0xCE)."""
    H = Inst(0)
    menu()
    for _ in range(3):
        H.press('down', 20, 1.0)
    H.press('cross', 20, 3.0)
    H.press('cross', 20, 2.0)
    for _ in range(limit):
        if H.mode() == 0xCE:
            return 0xCE
        time.sleep(1)
    return H.mode()


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'help'
    if cmd == 'cards':
        cards()
    elif cmd == 'start':
        start()
        print('booted frames', wait_boot())
    elif cmd == 'stop':
        stop()
    elif cmd == 'shot':
        for s in (0, 1):
            print(Inst(s).shot(sys.argv[2] if len(sys.argv) > 2 else 'shot'))
    elif cmd == 'menu':
        print('presses', menu())
        for s_ in (0, 1):
            print(Inst(s_).shot('menu'))
    elif cmd == 'rules':
        print('mode %02x' % to_2p_rules())
    elif cmd == 'trade':
        print('mode %02x' % to_trade())
    elif cmd == 'frames':
        print([Inst(s).frame() for s in (0, 1)])
    else:
        print(__doc__)
