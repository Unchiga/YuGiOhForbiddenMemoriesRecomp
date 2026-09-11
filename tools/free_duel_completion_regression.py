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
import zipfile

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

        default_boot = q("free_duel_completion")
        assert default_boot["enabled"] == 0 and not default_boot["visible"], default_boot
        mods_menu = q("video_menu", menu=6)
        assert "Free Duel progress" in mods_menu.get("rows", []), mods_menu
        q("game_speed", mult=8)
        enabled_boot = q("free_duel_completion", enabled=1)
        assert enabled_boot["enabled"] == 1, enabled_boot
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

        # The overlay consumes the manager backend's effective tables and its
        # generation counter, not an independent UI copy. Replace all three
        # selected-opponent bands with one card and require 1 immediately;
        # restore while the MODS toggle is Off, then require the stock count
        # as soon as the toggle returns On.
        edited_fixture = scratch / "completion-one-card.ygodrops.ini"
        edited_fixture.write_text(
            "format = 3\n[Duel Master K]\n"
            "pow_table = 1:2048\n"
            "bcd_table = 1:2048\n"
            "tec_table = 1:2048\n", encoding="utf-8")
        blank_fixture = scratch / "completion-stock.ygodrops.ini"
        blank_fixture.write_text("format = 3\n", encoding="utf-8")
        imported_edit = q("drop_viewer_set", **{"import": str(edited_fixture)})
        assert imported_edit["ok"], imported_edit
        deadline = time.monotonic() + 5
        edited_count = None
        while time.monotonic() < deadline:
            edited_count = q("free_duel_completion")
            if (edited_count["obtainable"] == 1 and
                    edited_count["edit_gen"] > selected["edit_gen"]):
                break
            time.sleep(0.1)
        else:
            raise AssertionError(f"drop edit did not update completion: {edited_count}")
        composed_shot("04-drop-edit-live-one-card")

        edit_disabled = q("free_duel_completion", enabled=0)
        assert not edit_disabled["visible"], edit_disabled
        imported_stock = q("drop_viewer_set", **{"import": str(blank_fixture)})
        assert imported_stock["ok"], imported_stock
        q("free_duel_completion", enabled=1)
        deadline = time.monotonic() + 5
        restored_count = None
        while time.monotonic() < deadline:
            restored_count = q("free_duel_completion")
            if restored_count["visible"] and \
                    restored_count["obtainable"] == selected["obtainable"]:
                break
            time.sleep(0.1)
        else:
            raise AssertionError(f"Off-time drop restore was stale: {restored_count}")
        composed_shot("05-drop-edit-cleared-while-off")

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
        first_complete = composed_shot("06-complete-frame-a")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            animated = q("free_duel_completion")
            if animated["frame"] != before_frame:
                break
            time.sleep(0.05)
        else:
            raise AssertionError(f"border frame did not animate: {animated}")
        second_complete = None
        for visual_try in range(8):
            second_complete = composed_shot(
                f"07-complete-frame-b-{visual_try + 1}")
            if ImageChops.difference(first_complete.convert("RGB"),
                                     second_complete.convert("RGB")).getbbox():
                break
            time.sleep(0.1)
        else:
            raise AssertionError("animated composed captures stayed identical")

        scrolled = q("free_duel_completion")
        assert scrolled["top_row"] > 0, scrolled
        assert scrolled["borders"] > 0, scrolled
        composed_shot("08-scrolled-complete")

        # The whole feature is one persisted MODS switch: Off removes both
        # the selected ratio and every portrait frame, then On rebuilds the
        # current screen without requiring a re-entry. A .ygomods bundle owns
        # the same key through the menu's single settings serializer.
        disabled = q("free_duel_completion", enabled=0)
        assert disabled["enabled"] == 0 and not disabled["visible"], disabled
        off_image = composed_shot("09-toggle-off-stock-grid")
        assert ImageChops.difference(second_complete.convert("RGB"),
                                     off_image.convert("RGB")).getbbox(), \
            "turning completion progress off left the composed view unchanged"
        reenabled = q("free_duel_completion", enabled=1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            reenabled = q("free_duel_completion")
            if reenabled["visible"] and reenabled["complete"] and reenabled["borders"]:
                break
            time.sleep(0.1)
        else:
            raise AssertionError(f"toggle did not rebuild the overlay: {reenabled}")
        composed_shot("10-toggle-on-restored")

        q("menu_settings_save")
        settings_paths = [player / "menu_settings.ini", exe.parent / "menu_settings.ini"]
        settings_path = next((p for p in settings_paths if p.exists()), None)
        assert settings_path is not None, settings_paths
        settings_text = settings_path.read_text(encoding="utf-8")
        assert "free_duel_progress=1" in settings_text.replace(" ", ""), settings_text

        package = scratch / "free-duel-progress.ygomods"
        exported = q("mod_package", export=str(package))
        assert package.exists() and package.stat().st_size, exported
        with zipfile.ZipFile(package) as zf:
            package_settings = zf.read("mod_settings.ini").decode("utf-8")
        assert "free_duel_progress=1" in package_settings.replace(" ", ""), package_settings
        q("free_duel_completion", enabled=0)
        imported = q("mod_package", **{"import": str(package)})
        assert imported["ok"], imported
        package_restored = q("free_duel_completion")
        assert package_restored["enabled"] == 1, package_restored

        result = {
            "pid": proc.pid,
            "port": port,
            "scratch": str(scratch),
            "title_mean_difference": title_diff,
            "default_boot": default_boot,
            "mods_menu": mods_menu,
            "build_deck": empty,
            "unavailable_cell": hole_state,
            "selected": selected,
            "edited_count": edited_count,
            "restored_count": restored_count,
            "complete": complete,
            "animated": animated,
            "scrolled": scrolled,
            "disabled": disabled,
            "reenabled": reenabled,
            "settings_path": str(settings_path),
            "package": str(package),
            "package_restored": package_restored,
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
