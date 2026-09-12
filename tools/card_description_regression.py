#!/usr/bin/env python3
"""Live boundary, encoding, reload, round-trip, restart, and Library test.

The process and memory-card directory are always owned by this script.  The
caller supplies an already-isolated save seed and the explicit disc image.
Machine-readable results include the description pointer and bytes decoded
back from guest RAM; screenshots are supporting evidence, not the oracle.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client

DESC_TABLE = 0x801C0200
DESC_SEGMENT = 0x801C0000
CARD = 1
# This exact table mirrors the C encoder's 0x5c entries, with None for glyphs
# that have no ASCII equivalent.
CODE = [' ','e','t','a','o','i','n','s','r','h','l','.','d','u','m','c',
        'g','y','w','f','p','b','k','!','A','v','I',"'",'T','S','M',',',
        'D','O','W','H','Y','E','R',None,None,'G','L','C','N','B','?','P',
        '-','F','z','K','j','U','x','q','0','V','2','J','#','1','Q','Z',
        '"','3','5','&','/','7','X',None,':',None,'4',')','(',None,'6','$',
        '*','>',None,None,'<',None,'+','8',None,'9',None,'%']


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def q(port, value):
    reply = debug_client.query("127.0.0.1", port, value)
    if not reply.get("ok"):
        raise RuntimeError(f"{value}: {reply}")
    return reply


def wait_for(port, value, pred, timeout, label):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = q(port, value)
            if pred(last):
                return last
        except (ConnectionError, OSError, RuntimeError):
            pass
        time.sleep(1)
    raise RuntimeError(f"timeout waiting for {label}: {last}")


def read(port, addr, size):
    r = q(port, {"cmd": "read_ram", "addr": f"{addr:08X}", "len": size})
    return bytes.fromhex(r.get("hex") or r.get("data") or "")


def decode(raw):
    out = []
    i = 0
    while i < len(raw):
        b = raw[i]
        i += 1
        if b == 0xFF:
            return ''.join(out), i
        if b == 0xFE:
            out.append('|')
        elif b >= 0xF0:
            i += 1
        else:
            out.append(CODE[b] if b < len(CODE) and CODE[b] else '?')
    raise RuntimeError("description has no terminator in probe window")


def guest_description(port, card=CARD):
    offset = int.from_bytes(read(port, DESC_TABLE + card * 2, 2), "little")
    addr = DESC_SEGMENT + offset
    raw = read(port, addr, 272)
    try:
        text, used = decode(raw)
    except RuntimeError as exc:
        raise RuntimeError(
            f"{exc}; card={card} offset={offset:04X} address={addr:08X} "
            f"bytes={raw.hex()}"
        ) from exc
    return {"offset": offset, "address": f"{addr:08X}", "bytes": raw[:used].hex(),
            "byte_count": used, "decoded": text}


def stop(proc, port):
    if proc is None or proc.poll() is not None:
        return
    try:
        q(port, {"cmd": "quit_graceful"})
        proc.wait(timeout=12)
        return
    except Exception:
        pass
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait(timeout=5)


def launch(args, port, log_name):
    env = dict(os.environ, PSX_PORTABLE="1", PSX_TOOL_RENDERER="software",
               YGOFM_DEBUG_PORT=str(port), SHOTDIR=str(args.output / "shots"))
    env.pop("APPIMAGE", None); env.pop("APPDIR", None)
    log = (args.output / log_name).open("w")
    cmd = [str(args.exe), "--no-launcher", "--renderer", "software",
           "--memcard-dir", str(args.output / "player"), "--debug-port", str(port),
           "--disc", str(args.disc)]
    proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                            stdout=log, stderr=subprocess.STDOUT)
    wait_for(port, {"cmd": "frame"}, lambda r: r.get("frame", 0) > 8400, 240, "game boot")
    return proc, log


def write_card(player, description):
    path = player / "cards" / str(CARD) / "card.ini"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("description = " + description + "\n", encoding="utf-8")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, default=ROOT / "build-dbg" /
                    "Yu_Gi_Oh_Forbidden_Memories_Recompiled")
    ap.add_argument("--disc", type=Path, default=ROOT / "disc" /
                    "Yu-Gi-Oh! Forbidden Memories (USA).cue")
    ap.add_argument("--seed", type=Path, required=True,
                    help="already-isolated card1.mcd with a loadable save")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    args.exe = args.exe.resolve(); args.disc = args.disc.resolve(); args.seed = args.seed.resolve()
    args.output = args.output.resolve()
    if args.output.exists():
        ap.error("output must be a new directory")
    for p in (args.exe, args.disc, args.seed):
        if not p.is_file(): ap.error(f"missing file: {p}")
    player = args.output / "player"
    (args.output / "shots").mkdir(parents=True)
    player.mkdir()
    shutil.copyfile(args.seed, player / "card1.mcd")

    cases = [
        ("one-line", "Short text.", True, 1),
        ("six-lines", "A|B|C|D|E|F", True, 6),
        ("seven-lines", "A|B|C|D|E|F|G", True, 7),
        ("eight-lines", "A|B|C|D|E|F|G|H", True, 8),
        ("nine-lines", "A|B|C|D|E|F|G|H|I", False, 9),
        ("auto-19", "A" * 19, True, 1),
        ("auto-20", "A" * 20, True, 1),
        ("auto-21", "A" * 21, True, 1),
        ("auto-22-long-word", "A" * 22, True, 2),
        ("explicit-19", "A" * 19 + "|B", True, 2),
        ("explicit-20", "A" * 20 + "|B", True, 2),
        ("explicit-21", "A" * 21 + "|B", True, 2),
        ("explicit-22", "A" * 22 + "|B", False, 1),
        ("melting-red-shadow", "A creature that|melts into the earth,|melds with an|opponent's shadow,|and attacks|from below.", True, 6),
        ("literal-backslash-n", r"First\nSecond", True, 2),
        ("actual-newline", "First\nSecond", True, 2),
        ("automatic-words", "One two three four five six seven", True, 2),
        ("unsupported-glyph", "Cannot show @", False, 1),
        ("generated-spell-effect", "Edited effect.|Foe loses 500 LP.|Then destroy every|Dragon on the field.", True, 4),
        ("generated-monster-effect", "Edited effect.|When summoned, gain|500 ATK per ally.|When flipped, foe|loses 500 LP.|Immune to traps.|An equip adds 1000.|End of effect text.", True, 8),
    ]
    results = {"schema": 1, "validation": [], "reloads": []}
    port = free_port()
    proc = log = None
    try:
        proc, log = launch(args, port, "runtime-first.log")
        results["stock"] = guest_description(port)
        guards = [(0x801CEB80, 0x80), (0x801CFE00, 0x80)]
        guard_before = [hashlib.sha256(read(port, a, n)).hexdigest() for a, n in guards]
        for name, text, valid, lines in cases:
            r = q(port, {"cmd": "card_description_validate", "text": text})
            if bool(r.get("valid")) != valid or r.get("lines") != lines:
                raise AssertionError((name, valid, lines, r))
            results["validation"].append({"case": name, "text": text, "reply": r})

        reload_texts = ["One line.", "A|B|C|D|E|F", "A|B|C|D|E|F|G",
                        "A|B|C|D|E|F|G|H", "A" * 22]
        for text in reload_texts:
            write_card(player, text)
            q(port, {"cmd": "card_packs_reload", "card": CARD})
            time.sleep(.5)
            got = guest_description(port)
            expected = "A" * 21 + "|A" if text == "A" * 22 else text
            if got["decoded"] != expected:
                raise AssertionError((text, expected, got))
            results["reloads"].append({"source": text, "guest": got})

        final_text = cases[-1][1]
        write_card(player, final_text)
        q(port, {"cmd": "card_packs_reload", "card": CARD})
        time.sleep(.5)
        results["hot_reload_final"] = guest_description(port)
        if results["hot_reload_final"]["decoded"] != final_text:
            raise AssertionError(results["hot_reload_final"])

        export = args.output / "card-texts.txt"
        results["export"] = q(port, {"cmd": "card_texts_export", "path": str(export)})
        card_ini = player / "cards" / str(CARD) / "card.ini"
        card_ini.unlink()
        q(port, {"cmd": "card_packs_reload", "card": CARD})
        results["after_remove"] = guest_description(port)
        results["import"] = q(port, {"cmd": "card_texts_import", "path": str(export)})
        q(port, {"cmd": "card_packs_reload", "card": CARD})
        time.sleep(.5)
        results["after_import"] = guest_description(port)
        if results["after_import"]["decoded"] != final_text:
            raise AssertionError(results["after_import"])

        guard_after = [hashlib.sha256(read(port, a, n)).hexdigest() for a, n in guards]
        results["adjacent_guards"] = {"before": guard_before, "after": guard_after,
                                       "unchanged": guard_before == guard_after}
        if guard_before != guard_after:
            raise AssertionError("memory adjacent to description arena changed")
        stop(proc, port); proc = None; log.close(); log = None

        port = free_port()
        proc, log = launch(args, port, "runtime-restart.log")
        results["after_restart"] = guest_description(port)
        if results["after_restart"]["decoded"] != final_text:
            raise AssertionError(results["after_restart"])

        # Drive the real loaded-save Library and capture card 1's detail view.
        os.environ["YGOFM_DEBUG_PORT"] = str(port)
        os.environ["SHOTDIR"] = str(args.output / "shots")
        sys.path.insert(0, str(ROOT / "tools"))
        import game_route
        import live_probe as pad
        game_route.to_library()
        pad.press("cross", 20, 4.0)
        shot = args.output / "shots" / "library-card-1-eight-lines.png"
        q(port, {"cmd": "screenshot", "path": str(shot)})
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and not shot.exists(): time.sleep(.1)
        if not shot.exists(): raise RuntimeError("Library screenshot did not arrive")
        results["library_screenshot"] = str(shot)
        results["passed"] = True
    finally:
        stop(proc, port)
        if log is not None: log.close()
        (args.output / "results.json").write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
    print("PASS card-description regression:", args.output / "results.json")


if __name__ == "__main__":
    main()
