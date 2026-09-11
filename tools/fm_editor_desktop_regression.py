#!/usr/bin/env python3
"""Capture and verify FM Editor desktop-containment evidence.

Run this against an already-running title process on a deliberately selected
desktop scale.  The title's ``fm_editor`` debug command is the geometry oracle;
optional Spectacle captures include the compositor's native decorations.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client  # noqa: E402


PAGES = ("cards", "drops", "fusions", "dialogue", "cpu")


def query(port, command):
    reply = debug_client.query("127.0.0.1", port, command)
    if reply.get("ok") is False or "err" in reply:
        raise RuntimeError(f"debug command failed: {command!r}: {reply!r}")
    return reply


def wait_for_editor(port, timeout=3.0):
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        state = query(port, {"cmd": "fm_editor"})
        if state.get("open") and state.get("fit_count", 0) >= 2:
            return state
        time.sleep(0.05)
    raise RuntimeError(f"FM Editor did not finish delayed placement: {state!r}")


def native_capture(path, activation_script):
    if activation_script:
        subprocess.run(
            ["qdbus6", "org.kde.KWin", activation_script, "run"],
            check=True,
        )
        time.sleep(0.15)
    subprocess.run(
        ["spectacle", "-a", "-b", "-n", "-o", str(path)],
        check=True,
    )
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"native screenshot was not created: {path}")


def assert_contained(state, expected_display):
    if not state.get("fits_usable"):
        raise AssertionError(f"native window exceeds usable desktop: {state!r}")
    if expected_display:
        actual = state.get("display_bounds", [])[2:]
        expected = [int(piece) for piece in expected_display.lower().split("x")]
        if actual != expected:
            raise AssertionError(
                f"display bounds {actual!r} do not match expected {expected!r}"
            )
    outer = state.get("outer")
    usable = state.get("usable")
    if not outer or not usable:
        raise AssertionError(f"geometry fields missing: {state!r}")
    ox, oy, ow, oh = outer
    ux, uy, uw, uh = usable
    if ox < ux or oy < uy or ox + ow > ux + uw or oy + oh > uy + uh:
        raise AssertionError(f"outer rect is not contained in usable rect: {state!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--expected-display",
        help="expected logical display bounds, for example 1536x864",
    )
    parser.add_argument(
        "--kwin-script-object",
        help="optional loaded KWin script object, for example /Scripting/Script0",
    )
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--executable", type=Path,
                        help="running executable, recorded and SHA-256 bound")
    parser.add_argument("--physical-display",
                        help="physical mode represented by this run, e.g. 1920x1080")
    parser.add_argument("--desktop-scale", type=float,
                        help="desktop scale represented by this run, e.g. 2.0")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    query(args.port, {"cmd": "fm_editor", "open": 1, "page_name": "cards"})
    initial = wait_for_editor(args.port)
    assert_contained(initial, args.expected_display)
    window_id = initial["window_id"]

    pages = {}
    for index, page in enumerate(PAGES):
        canvas = args.output / f"canvas-{page}.ppm"
        state = query(
            args.port,
            {"cmd": "fm_editor", "click_tab": index, "shot": str(canvas)},
        )
        time.sleep(0.12)
        state = query(args.port, {"cmd": "fm_editor"})
        assert_contained(state, args.expected_display)
        if state["window_id"] != window_id:
            raise AssertionError("tab switch created a second native window")
        if not canvas.is_file() or canvas.stat().st_size == 0:
            raise AssertionError(f"canvas capture missing: {canvas}")
        if args.native:
            native_capture(
                args.output / f"native-{page}.png", args.kwin_script_object
            )
        pages[page] = state

    oversize = query(
        args.port,
        {
            "cmd": "fm_editor",
            "restore": 1,
            "w": 10000,
            "h": 10000,
            "fit": 1,
        },
    )
    # Wayland geometry changes are asynchronous; allow KWin enough time to
    # finish placement so a delayed cross-output migration cannot pass.
    time.sleep(0.75)
    oversize = query(args.port, {"cmd": "fm_editor"})
    assert_contained(oversize, args.expected_display)
    if args.native:
        native_capture(args.output / "oversize-fitted.png", args.kwin_script_object)

    result = {
        "schema": 1,
        "port": args.port,
        "expected_display": args.expected_display,
        "video_driver": os.environ.get("SDL_VIDEODRIVER", "auto"),
        "initial": initial,
        "pages": pages,
        "oversize_fitted": oversize,
        "checks": {
            "one_window_across_tabs": True,
            "all_pages_fit_usable_desktop": True,
            "oversize_request_refitted": True,
            "canvas_captures_nonempty": True,
            "native_captures_nonempty": bool(args.native),
        },
    }
    if args.executable:
        executable = args.executable.resolve()
        payload = executable.read_bytes()
        result["executable"] = {
            "path": str(executable),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "bytes": len(payload),
        }
    if args.physical_display:
        result["physical_display"] = args.physical_display
    if args.desktop_scale is not None:
        result["desktop_scale"] = args.desktop_scale
    (args.output / "results.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(result["checks"], sort_keys=True))


if __name__ == "__main__":
    main()
