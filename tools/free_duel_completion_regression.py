#!/usr/bin/env python3
"""Live smoke test for the Free Duel owned/obtainable completion overlay.

The test always uses a newly-created scratch profile under /tmp.  It captures
the composed software-renderer output so the host overlay is present in every
evidence image; the stock ``screenshot`` command intentionally is not enough.
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

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client  # noqa: E402

BUTTONS = {
    "start": 0x0008,
    "up": 0x0010,
    "right": 0x0020,
    "down": 0x0040,
    "left": 0x0080,
    "cross": 0x4000,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--seed", type=Path, required=True,
                        help="authorized test card1.mcd copied into the scratch profile")
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()

    scratch = args.scratch.resolve()
    if not scratch.is_relative_to(Path("/tmp")):
        parser.error("--scratch must be a new directory below /tmp")
    scratch.mkdir(parents=True, exist_ok=False)
    player = scratch / "player"
    shots = scratch / "shots"
    player.mkdir()
    shots.mkdir()
    shutil.copy2(args.seed.resolve(), player / "card1.mcd")

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    exe = args.exe.resolve()
    disc = args.disc.resolve()
    env = dict(os.environ)
    env.pop("APPIMAGE", None)
    env.pop("APPDIR", None)
    env["PSX_PORTABLE"] = "1"
    command_log = (scratch / "commands.jsonl").open("w")
    runtime_log = (scratch / "runtime.log").open("w")
    proc = subprocess.Popen(
        [str(exe), "--no-launcher", "--renderer", "software",
         "--memcard-dir", str(player), "--debug-port", str(port),
         "--disc", str(disc)],
        cwd=exe.parent, env=env, stdout=runtime_log,
        stderr=subprocess.STDOUT,
    )
    print(json.dumps({"pid": proc.pid, "port": port, "scratch": str(scratch)}),
          flush=True)

    def q(cmd, **kwargs):
        request = {"cmd": cmd, **kwargs}
        response = debug_client.query("127.0.0.1", port, request)
        command_log.write(json.dumps({"request": request, "response": response}) + "\n")
        command_log.flush()
        assert response.get("ok"), response
        return response

    def read(addr, length):
        result = q("read_ram", addr=f"{addr:08X}", len=length)
        return bytes.fromhex(result.get("data") or result.get("hex") or "")

    def write(addr, data):
        q("write_mem", addr=f"{addr:08X}", hex=data.hex())

    def press(name, frames=20, settle=0.8):
        q("press", buttons=0xFFFF & ~BUTTONS[name], frames=frames)
        time.sleep(0.25)
        q("clear_input")
        time.sleep(settle)

    def composed_shot(name):
        path = shots / f"{name}.png"
        sequence = q("present_shot_seq")["seq"]
        q("present_shot", path=str(path))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            result = q("present_shot_seq")
            if result["seq"] > sequence:
                assert result["wrote"], result
                image = Image.open(path).convert("RGBA")
                image.load()
                return image
            time.sleep(0.05)
        raise AssertionError(f"composed screenshot timed out: {name}")

    def raw_shot(path):
        q("screenshot", path=str(path))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if path.exists() and path.stat().st_size:
                image = Image.open(path).convert("L").resize((80, 60))
                image.load()
                return image
            time.sleep(0.05)
        raise AssertionError("raw screenshot timed out")

    owned = False
    result = {}
    try:
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            assert proc.poll() is None, "runtime exited during boot"
            try:
                q("ping")
                owned = True
                break
            except OSError:
                time.sleep(0.25)
        else:
            raise AssertionError("debug server did not start")

        q("game_speed", mult=8)
        reference = Image.open(ROOT / "tools/refs/title_80x60.png").convert("L")
        deadline = time.monotonic() + 180
        title_diff = None
        probe_number = 0
        while time.monotonic() < deadline:
            path = scratch / f"title-probe-{probe_number}.png"
            probe_number += 1
            try:
                live = raw_shot(path)
            except AssertionError as exc:
                if "display disabled" in str(exc):
                    time.sleep(0.5)
                    continue
                raise
            title_diff = ImageStat.Stat(ImageChops.difference(live, reference)).mean[0]
            if title_diff <= 18:
                break
            time.sleep(0.5)
        else:
            raise AssertionError(f"title screen not found; last diff {title_diff}")

        # Title -> LOAD -> loaded menu -> FREE DUEL -> dismiss prompt.
        # Return to 1x for UI input. Long START holds can span both the title
        # and main menu at accelerated speed and accidentally choose NEW GAME.
        q("game_speed", mult=1)
        press("start", 60, 3.0)
        composed_shot("route-01-main-menu")
        press("down", 12, 1.5)
        composed_shot("route-02-load-selected")
        press("cross", 12, 2.0)
        composed_shot("route-03-load-confirm")
        press("cross", 12, 3.0)
        composed_shot("route-04-loading")
        press("cross", 12, 3.0)
        composed_shot("route-05-loaded-menu")
        press("down", 12, 1.5)
        composed_shot("route-06-free-duel-selected")
        press("cross", 12, 4.0)
        composed_shot("route-07-free-duel-prompt")
        press("cross", 40, 2.0)
        composed_shot("route-08-free-duel-grid")
        mode = read(0x8009B26C, 1)[0]
        assert mode == 0xC6, f"expected Free Duel mode C6, got {mode:02X}"

        # Cell 0 is Build Deck. It is deliberately not a CPU and has no ratio.
        empty = q("free_duel_completion")
        assert empty["visible"] and empty["selected"] == -1, empty
        composed_shot("01-build-deck-no-progress")

        occupancy = read(0x80169030, 40)
        present_cells = [cell for cell in range(1, 40) if occupancy[cell]]
        assert present_cells, "Free Duel overlay has no present opponents"

        # Exercise an actual blank grid cell with stock input. The known test
        # save has cell 5 empty, directly below Build Deck.
        hole = 5 if not occupancy[5] else None
        if hole is not None:
            press("down", 6, 0.6)
            hole_state = q("free_duel_completion")
            assert hole_state["selected"] == -1, hole_state
            composed_shot("02-unavailable-cell-no-progress")
        else:
            hole_state = {"skipped": "cell below Build Deck is unlocked"}

        # Begin below Build Deck (row 1, column 0), then use only stock d-pad
        # input to reach the lowest unlocked cell. This proves both the cursor
        # identity and the game's scrolling grid agree with the host overlay.
        write(0x801D0250, bytes(722))
        write(0x801D3250, bytes(722))
        target = max(present_cells, key=lambda cell: (cell // 5, cell % 5))
        row, column = divmod(target, 5)
        current_row = 1 if hole is not None else 0
        for _ in range(row - current_row):
            press("down", 6, 0.35)
        for _ in range(column):
            press("right", 6, 0.35)
        raw_selected = read(0x8009B32E, 1)[0]
        assert raw_selected == 40 + target, (raw_selected, target)
        selected = q("free_duel_completion")
        assert selected["selected"] == target - 1, selected
        assert selected["name"], selected
        assert 0 < selected["obtainable"] <= 722, selected
        assert 0 <= selected["owned"] <= selected["obtainable"], selected
        assert not selected["complete"], selected
        assert selected["top_row"] > 0, selected
        composed_shot("03-selected-progress")

        # Scratch-process-only completion fixture: own every card in live and
        # mirror trunks. The source memory card is only a copied seed.
        all_owned = bytes([1]) * 722
        write(0x801D0250, all_owned)
        write(0x801D3250, all_owned)
        deadline = time.monotonic() + 5
        complete = None
        while time.monotonic() < deadline:
            complete = q("free_duel_completion")
            if complete["complete"] and complete["borders"]:
                break
            time.sleep(0.1)
        else:
            raise AssertionError(f"completion state did not update: {complete}")
        before_frame = complete["frame"]
        first_complete = composed_shot("04-complete-frame-a")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            animated = q("free_duel_completion")
            if animated["frame"] != before_frame:
                break
            time.sleep(0.05)
        else:
            raise AssertionError(f"border frame did not animate: {animated}")
        second_complete = composed_shot("05-complete-frame-b")
        assert ImageChops.difference(first_complete.convert("RGB"),
                                     second_complete.convert("RGB")).getbbox(), \
            "animated composed captures are identical"

        scrolled = q("free_duel_completion")
        assert scrolled["top_row"] > 0, scrolled
        assert scrolled["borders"] > 0, scrolled
        composed_shot("06-scrolled-complete")

        result = {
            "pid": proc.pid,
            "port": port,
            "scratch": str(scratch),
            "title_mean_difference": title_diff,
            "build_deck": empty,
            "unavailable_cell": hole_state,
            "selected": selected,
            "complete": complete,
            "animated": animated,
            "scrolled": scrolled,
        }
        (scratch / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)
    finally:
        if owned and proc.poll() is None:
            try:
                q("quit_graceful")
            except OSError:
                pass
        try:
            return_code = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.terminate()
            return_code = proc.wait(timeout=10)
        command_log.close()
        runtime_log.close()
        print(json.dumps({"stopped_pid": proc.pid, "returncode": return_code}), flush=True)


if __name__ == "__main__":
    main()
