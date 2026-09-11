#!/usr/bin/env python3
"""Live regression for guaranteed story-card ordering with 0..99 drops.

The fixture is the authorized slot-0 (UI slot 1) Teana duel state.  Each case
copies only its .pst/.thumb pair into a new writable directory, launches one
owned process with PSX_PORTABLE=1 and an explicit disc/debug port, wins by the
fixture's two Cross presses, and writes machine-readable award-order evidence.
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

STATE = "state_800129D8_slot00.pst"
THUMB = "state_800129D8_slot00.thumb"
OPPONENT = 2
DUELIST = OPPONENT - 1
REWARD = 37
UNLOCKS = 0x801D06F4
FREE_FLAGS = 0x8009B365
PHASE = 0x8009B23A
RNG_SEED = 0x800FE6F8
ZERO_TEST_SEED = 0x13579BDF

CASES = (
    ("count-0-first", 0, 0, False, False, True),
    ("count-0-every", 0, 1, False, True, True),
    ("count-0-no-reward", 0, -1, False, False, False),
    ("count-0-free-duel", 0, 1, True, False, False),
    ("count-1-first", 1, 0, False, False, True),
    ("count-2-every", 2, 1, False, True, True),
    ("count-16-first", 16, 0, False, False, True),
    ("count-99-every", 99, 1, False, True, True),
    ("free-duel", 2, 1, True, False, False),
    ("already-defeated", 2, 0, False, True, False),
    ("no-reward", 2, -1, False, False, False),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def query(port, value):
    return debug_client.query("127.0.0.1", port, value)


def wait_query(port, value, predicate, timeout, label):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = query(port, value)
            if predicate(last):
                return last
        except (ConnectionError, OSError, TimeoutError):
            pass
        # Debug commands are pumped by the emulation thread.  Fast polling can
        # starve the very transition being observed, especially during the
        # duel's battle animation.
        time.sleep(1.0)
    raise RuntimeError(f"timeout waiting for {label}: {last!r}")


def ram(port, addr, size=1):
    reply = query(port, {"cmd": "read_ram", "addr": f"{addr:08X}", "len": size})
    return bytes.fromhex(reply.get("hex") or reply.get("data") or "")


def write(port, addr, data):
    reply = query(port, {"cmd": "write_mem", "addr": f"{addr:08X}", "hex": bytes(data).hex()})
    if not reply.get("ok"):
        raise RuntimeError(reply)


def press_cross(port):
    # Pad bits are active low; Cross is 0x4000.
    query(port, {"cmd": "press", "buttons": 0xBFFF, "frames": 12, "slot": 0})
    time.sleep(1.2)
    query(port, {"cmd": "clear_input"})
    # This fixture's first Cross selects the attack and starts a camera/UI
    # transition.  A second press inside that transition is intentionally
    # ignored by the game, so wait through it just as a player must.
    time.sleep(6.0)


def stop(proc, port):
    if proc.poll() is not None:
        return
    try:
        query(port, {"cmd": "quit_graceful"})
        proc.wait(timeout=12)
        return
    except Exception:
        pass
    proc.terminate()                 # exact owned PID; offline fallback only
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def run_case(args, case):
    name, count, every, free_duel, beaten, expect_story = case
    # Card Drops normally names the total award count. Zero is the deliberate
    # exception: it means no normal-table award, while a guaranteed campaign
    # reward remains a separate one-card outcome.
    expected_total = count if count > 0 else int(expect_story)
    case_dir = args.output / name
    fixture_state = args.fixture / STATE
    fixture_thumb = args.fixture / THUMB
    proc = None
    log = None
    if args.connect_port:
        case_dir.mkdir(parents=True, exist_ok=False)
        port = args.connect_port
    else:
        state_dir = case_dir / "openbios"
        state_dir.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(fixture_state, state_dir / STATE)
        shutil.copyfile(fixture_thumb, state_dir / THUMB)
        if args.card:
            shutil.copyfile(args.card, case_dir / "card1.mcd")
        port = free_port()          # bound probe proves it was free
        env = dict(os.environ, PSX_PORTABLE="1")
        env.pop("APPIMAGE", None)
        env.pop("APPDIR", None)
        log = (case_dir / "runtime.log").open("w")
        cmd = [str(args.exe), "--no-launcher", "--memcard-dir", str(case_dir),
               "--debug-port", str(port), "--disc", str(args.disc)]
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env,
                                stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
    try:
        wait_query(port, {"cmd": "frame"}, lambda r: r.get("ok"), 45, "debug server")
        if proc is not None:
            # Loading while the cold boot is still establishing BIOS/game
            # state lets later boot work overwrite the restored duel and can
            # eventually return through PC 0.  An attached diagnosis process
            # is already initialized; an owned process must reach the normal
            # title/menu era before the authorized state is applied.
            wait_query(port, {"cmd": "frame"},
                       lambda r: r.get("frame", 0) > 8400,
                       240, "fully initialized game")
        loaded = query(port, {"cmd": "savestate", "op": "load", "slot": 0})
        if not loaded.get("ok"):
            raise RuntimeError(f"savestate load failed: {loaded}")
        wait_query(port, {"cmd": "story_rewards"},
                   lambda r: r.get("opponent_now") == OPPONENT, 20, "Teana state")
        # The load acknowledgement precedes the first fully presented frame.
        # Let audio/GPU/event state settle before touching the saved battle;
        # driving it immediately can follow the state's stale host-frame edge.
        time.sleep(5.0)

        flags = bytearray(ram(port, FREE_FLAGS, 1))
        flags[0] = (flags[0] | 0x80) if free_duel else (flags[0] & 0x7F)
        if flags != ram(port, FREE_FLAGS, 1):
            write(port, FREE_FLAGS, flags)
        unlock_addr = UNLOCKS + (OPPONENT >> 3)
        unlock = bytearray(ram(port, unlock_addr, 1))
        mask = 0x80 >> (OPPONENT & 7)
        unlock[0] = (unlock[0] | mask) if beaten else (unlock[0] & ~mask)
        if unlock != ram(port, unlock_addr, 1):
            write(port, unlock_addr, unlock)

        fired_before = query(port, {"cmd": "story_rewards"}).get("fired", 0)
        card = 0 if every < 0 else REWARD
        set_reward = query(port, {"cmd": "story_rewards", "duelist": DUELIST,
                                  "card": card, "every": max(every, 0)})
        set_drops = query(port, {"cmd": "card_drops_set", "drops": count})
        if not set_reward.get("ok") or not set_drops.get("ok"):
            raise RuntimeError((set_reward, set_drops))
        # These debug setters are synchronous, but the saved duel's host-side
        # per-frame hooks must observe the restored setting before battle is
        # resumed.  The interactive reproduction naturally has this pause.
        time.sleep(5.0)

        before = ram(port, 0x801D024F + REWARD, 1)[0]
        card_zero_before = ram(port, 0x801D024F, 1)[0]
        trunk_before = ram(port, 0x801D0250, 722)
        ring_before = ram(port, 0x801D07BC, 32)
        suppressed_before = query(
            port, {"cmd": "card_drops_state"})["suppression"][
                "zero_suppressed_awards"]
        print(name, "before inputs", ram(port, PHASE, 8).hex(), flush=True)
        press_cross(port)
        print(name, "after first Cross", ram(port, PHASE, 8).hex(), flush=True)
        seed_before = None
        if count == 0:
            # Use a recognizable seed immediately before the winning input.
            # Battle resolution may consume it before the drop hook; the
            # hook's own seed_before/seed_after fields below isolate the one
            # stock carrier roll and prove that it did not retry.
            write(port, RNG_SEED, ZERO_TEST_SEED.to_bytes(4, "little"))
            seed_before = int.from_bytes(ram(port, RNG_SEED, 4), "little")
        press_cross(port)
        print(name, "after second Cross", ram(port, PHASE, 8).hex(), flush=True)
        wait_query(port, {"cmd": "read_ram", "addr": f"{PHASE:08X}", "len": 2},
                   lambda r: r.get("ok") and
                   (int.from_bytes(bytes.fromhex(r.get("hex") or r.get("data")), "little") & 0xF) == 0xD,
                   45, "results phase")
        precommit = wait_query(port, {"cmd": "card_drops_list"},
                               lambda r: r.get("total") == expected_total,
                               20, "award list")
        zero_visible = query(port, {"cmd": "card_drops_state"})["suppression"]
        shot_path = case_dir / "results.png"
        shot = query(port, {"cmd": "screenshot_present",
                            "path": str(shot_path)})
        if not shot.get("ok"):
            raise RuntimeError((name, "results screenshot failed", shot))
        press_cross(port)            # stock award commits as results advances
        if expected_total:
            final = wait_query(port, {"cmd": "card_drops_list"},
                               lambda r: r.get("order_n") == expected_total and
                               all(x.get("committed") for x in r.get("award_order", [])),
                               30, "committed award order")
        else:
            wait_query(port, {"cmd": "card_drops_state"},
                       lambda r: r.get("suppression", {}).get("zero_suppressed_awards", 0) ==
                       suppressed_before + 1, 30, "suppressed zero-card award")
            final = query(port, {"cmd": "card_drops_list"})
        story = query(port, {"cmd": "story_rewards"})
        zero_final = query(port, {"cmd": "card_drops_state"})["suppression"]
        after = ram(port, 0x801D024F + REWARD, 1)[0]
        card_zero_after = ram(port, 0x801D024F, 1)[0]
        trunk_after = ram(port, 0x801D0250, 722)
        ring_after = ram(port, 0x801D07BC, 32)

        if count == 0:
            hook_seed_before = zero_final.get("rng_seed_before")
            hook_seed_after = zero_final.get("rng_seed_after")
            want_seed = ((hook_seed_before * 0x41C64E6D + 0x3039)
                         & 0xFFFFFFFF)
            if (hook_seed_after != want_seed or
                    zero_final.get("rng_calls_last") != 1 or
                    not zero_final.get("rng_exact_one")):
                raise AssertionError((name, "zero RNG policy", seed_before,
                                      want_seed, zero_final))

        order = final["award_order"]
        if len(order) != expected_total:
            raise AssertionError(f"{name}: expected {expected_total} awards, got {len(order)}")
        if expect_story:
            if order[0] != {"id": REWARD, "kind": "story", "committed": True}:
                raise AssertionError(f"{name}: story reward is not first: {order[:2]}")
            if any(x["kind"] != "normal" for x in order[1:]):
                raise AssertionError(f"{name}: non-normal remainder")
            if story.get("fired") - fired_before != 1:
                raise AssertionError(f"{name}: reward fired {story.get('fired') - fired_before} times")
        else:
            if any(x["kind"] == "story" for x in order):
                raise AssertionError(f"{name}: campaign reward escaped its gate")
            if story.get("fired") - fired_before != 0:
                raise AssertionError(f"{name}: gated reward fired")
        if count == 0 and not expect_story:
            if (not zero_visible.get("visible") or
                    zero_visible.get("reason") != "zero_normal"):
                raise AssertionError((name, "truthful zero-card result missing", zero_visible))
            if (trunk_after != trunk_before or
                    card_zero_after != card_zero_before or
                    ring_after != ring_before or
                    final.get("total") != 0 or final.get("order_n") != 0):
                raise AssertionError((name, "zero live award mutated state", final,
                                      zero_final))

        return {"case": name, "drops": count, "mode": "none" if every < 0 else
                ("every" if every else "first"), "free_duel": free_duel,
                "already_defeated": beaten, "expected_story": expect_story,
                "fixture": {"state_sha256": digest(fixture_state),
                            "thumb_sha256": digest(fixture_thumb)},
                "results_png": {"path": str(shot_path),
                                "sha256": digest(shot_path)},
                "port": port, "precommit": precommit, "final": final,
                "story": story, "reward_trunk_before": before,
                "reward_trunk_after": after,
                "card_zero_before": card_zero_before,
                "card_zero_after": card_zero_after,
                "memory": {
                    "trunk_before_sha256":
                        hashlib.sha256(trunk_before).hexdigest(),
                    "trunk_after_sha256":
                        hashlib.sha256(trunk_after).hexdigest(),
                    "ring_before_sha256":
                        hashlib.sha256(ring_before).hexdigest(),
                    "ring_after_sha256":
                        hashlib.sha256(ring_after).hexdigest()},
                "zero_visible": zero_visible,
                "zero_final": zero_final,
                "zero_rng": {"winning_input_seed": seed_before,
                             "hook_seed_before": zero_final.get("rng_seed_before"),
                             "hook_seed_after": zero_final.get("rng_seed_after")},
                "pass": True}
    finally:
        if proc is not None:
            print(name, "process status before cleanup", proc.poll(), flush=True)
            stop(proc, port)
        if log is not None:
            log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, required=True,
                        help="directory containing only the authorized slot-0 .pst/.thumb")
    parser.add_argument("--card", type=Path,
                        help="optional already-isolated card1.mcd seed")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exe", type=Path, default=ROOT / "build-dbg" /
                        "Yu_Gi_Oh_Forbidden_Memories_Recompiled")
    parser.add_argument("--disc", type=Path, default=ROOT / "disc" /
                        "Yu-Gi-Oh! Forbidden Memories (USA).cue")
    parser.add_argument("--case", choices=[case[0] for case in CASES],
                        help="run one named case while diagnosing a fixture")
    parser.add_argument("--connect-port", type=int,
                        help="drive an already-running isolated process; do not launch or stop it")
    args = parser.parse_args()
    args.fixture = args.fixture.resolve()
    args.output = args.output.resolve()
    args.exe = args.exe.resolve()
    args.disc = args.disc.resolve()
    if args.output.exists():
        raise SystemExit(f"refusing to reuse output directory: {args.output}")
    for path in (args.fixture / STATE, args.fixture / THUMB, args.exe, args.disc):
        if not path.is_file():
            raise SystemExit(f"missing required file: {path}")
    if args.card and not args.card.resolve().is_file():
        raise SystemExit(f"missing card seed: {args.card}")
    args.output.mkdir(parents=True)
    report = {"schema": 1, "cases": []}
    try:
        selected = [case for case in CASES if not args.case or case[0] == args.case]
        for case in selected:
            result = run_case(args, case)
            report["cases"].append(result)
            print(f"PASS {result['case']}: {result['final']['order_n']} committed awards")
    finally:
        (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS all {len(report['cases'])} cases; evidence: {args.output / 'results.json'}")


if __name__ == "__main__":
    main()
