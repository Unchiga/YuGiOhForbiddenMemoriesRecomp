#!/usr/bin/env python3
"""Repeatable live regression for Drop Table Manager clear semantics.

This test deliberately uses a copied authorized near-results savestate and the
shipping editor/debug paths.  It never calls the disabled re-entrant guest
drop simulators and it never points the game at the normal player directory.

Coverage:

* proves from generated code that the stock roll is bounded at 722 entries and
  returns card 0 for an all-zero table;
* exercises one-band and all-three two-step clear confirmation;
* proves pending empty bands cannot Save or export as .ini, .ygocards, or
  .ygomods, and cannot put an empty table into guest RAM;
* adds the first replacement card through the real editor UI and checks its
  canonical weight is 2048;
* wins the copied duel against that rebuilt table and checks the normal award;
* checks story reward, untouched bands, and another duelist are preserved;
* checks per-duelist restore, seeded Randomize, and global restore;
* checks format-3 save/restart and old, malformed, and future imports.

Example (the output directory must not exist):

  python3 tools/drop_clear_regression.py \
    --fixture /path/to/authorized/player/openbios \
    --output /tmp/ygofm-drop-clear-regression
"""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client


STATE = "state_800129D8_slot00.pst"
THUMB = "state_800129D8_slot00.thumb"
OLD_CODEGEN = 0xF691F520
NEW_CODEGEN = 0xE3093403

OPPONENT = 2
DUELIST = OPPONENT - 1                 # Teana
OTHER_DUELIST = "Simon Muran"
TARGET_DUELIST = "Teana"
STORY_CARD = 92
DROP_TABLE = 0x8017878C
TIER_STRIDE = 1460
TABLE_BYTES = 722 * 2
FREE_FLAGS = 0x8009B365
PHASE = 0x8009B23A
TRUNK = 0x801D0250
CARD_ZERO = 0x801D024F
RECENT = 0x801D07BC
CROSS = 0x4000


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    return sha_bytes(path.read_bytes())


def write_text(path, text):
    path.write_text(text, encoding="utf-8")
    return {"path": str(path), "sha256": sha_file(path)}


def git_output(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, text=True, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.strip()


def free_port():
    """Bind-probe loopback and return the kernel-selected free port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def q(port, command):
    return debug_client.query("127.0.0.1", port, command)


def must(reply, label):
    if not reply.get("ok"):
        raise AssertionError(f"{label}: {reply}")
    return reply


def wait_q(port, command, predicate, timeout, label):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = q(port, command)
            if predicate(last):
                return last
        except (ConnectionError, OSError, TimeoutError):
            pass
        time.sleep(1.0)
    raise RuntimeError(f"timeout waiting for {label}: {last!r}")


def ram(port, address, size):
    reply = must(q(port, {"cmd": "read_ram", "addr": f"{address:08X}",
                          "len": size}), "read_ram")
    return bytes.fromhex(reply.get("hex") or reply.get("data") or "")


def write_ram(port, address, data):
    return must(q(port, {"cmd": "write_mem", "addr": f"{address:08X}",
                          "hex": bytes(data).hex()}), "write_mem")


def resident_tables(port):
    return [ram(port, DROP_TABLE + tier * TIER_STRIDE, TABLE_BYTES)
            for tier in range(3)]


def table_summary(tables):
    return [{"sha256": sha_bytes(blob),
             "sum": sum(struct.unpack("<722H", blob)),
             "nonzero": sum(value != 0 for value in
                            struct.unpack("<722H", blob))}
            for blob in tables]


def dense_table(pairs):
    if sum(pairs.values()) != 2048:
        raise AssertionError(("replacement total", pairs))
    blob = bytearray(TABLE_BYTES)
    for card, weight in pairs.items():
        if not 1 <= card <= 722 or not 1 <= weight <= 2048:
            raise AssertionError((card, weight))
        struct.pack_into("<H", blob, (card - 1) * 2, weight)
    return bytes(blob)


def parse_stock_table_source(path, duelist):
    text = path.read_text(encoding="utf-8")
    tables = []
    for tier in range(3):
        match = re.search(
            rf"static const PsxDropWeight DB_D{duelist:02d}_T{tier}\[\] = "
            rf"\{{(.*?)\n\}};", text, re.S)
        if not match:
            raise AssertionError(f"stock table D{duelist:02d} T{tier} missing")
        pairs = {int(card): int(weight) for card, weight in
                 re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}",
                            match.group(1))}
        tables.append(dense_table(pairs))
    return tables


def patch_copied_state(source, target):
    source_blob = source.read_bytes()
    data = bytearray(source_blob)
    if len(data) < 20:
        raise AssertionError("savestate header is truncated")
    magic, version, bios, entry, codegen = struct.unpack_from("<5I", data)
    if magic != 0x50535842:
        raise AssertionError(f"unexpected savestate magic {magic:08X}")
    if codegen == OLD_CODEGEN:
        struct.pack_into("<I", data, 16, NEW_CODEGEN)
    elif codegen != NEW_CODEGEN:
        raise AssertionError(
            f"fixture codegen {codegen:08X}; expected {OLD_CODEGEN:08X} "
            f"or {NEW_CODEGEN:08X}")
    target.write_bytes(data)
    # The source must remain byte-identical; only the copy may be patched.
    if source.read_bytes() != source_blob:
        raise AssertionError("source fixture changed")
    return {
        "source": str(source), "source_sha256": sha_bytes(source_blob),
        "copy": str(target), "copy_sha256": sha_file(target),
        "version": version, "bios": f"0x{bios:08X}",
        "entry": f"0x{entry:08X}",
        "source_codegen": f"0x{codegen:08X}",
        "copy_codegen": f"0x{NEW_CODEGEN:08X}",
        "only_header_codegen_changed": (
            source_blob == bytes(data) if codegen == NEW_CODEGEN else
            source_blob[:16] == bytes(data[:16]) and
            source_blob[20:] == bytes(data[20:])),
    }


def stock_loop_analysis(path):
    """Validate the exact generated control flow and model its zero input."""
    text = path.read_text(encoding="utf-8")
    start = text.find("void func_80021810(CPUState* cpu)")
    end = text.find("\nvoid func_", start + 10)
    if start < 0 or end < 0:
        raise AssertionError("generated func_80021810 body missing")
    body = text[start:end]
    expected = {
        "rng_mask_2047": "0x80021848: 0x304207FF",
        "roll_plus_one": "0x8002184C: 0x24450001",
        "weight_load_u16": "0x80021858: 0x96020000",
        "cumulative_add": "0x80021860: 0x00822021",
        "index_increment": "0x80021870: 0x24630001",
        "bounded_722": "0x80021874: 0x286202D2",
        "exhausted_return_zero": "0x80021880: 0x00001021",
    }
    missing = {key: token for key, token in expected.items()
               if token not in body}
    if missing:
        raise AssertionError(("generated stock-loop assertion", missing))

    # A literal model of the proved generated loop: roll is always 1..2048,
    # all 722 zero weights leave cumulative at zero, then exhaustion returns 0.
    roll = (0x13579BDF & 0x7FF) + 1
    cumulative = 0
    result = 0
    visits = 0
    for index, weight in enumerate([0] * 722):
        visits += 1
        cumulative += weight
        if cumulative >= roll:
            result = index + 1
            break
    if visits != 722 or cumulative != 0 or result != 0:
        raise AssertionError((visits, cumulative, result))
    return {
        "pass": True, "generated_source": str(path),
        "generated_source_sha256": sha_file(path),
        "function_body_sha256": sha_bytes(body.encode()),
        "instruction_assertions": expected,
        "all_zero_model": {"roll": roll, "entries_visited": visits,
                           "cumulative": cumulative, "return_card": result},
        "conclusion": (
            "The stock loop is finite, but all-zero returns card 0; award and "
            "results callers require 1..722, so empty must remain authoring-only."),
    }


def launch(args, player_dir, log_path, label):
    port = free_port()
    env = dict(os.environ, PSX_PORTABLE="1")
    env.pop("APPIMAGE", None)
    env.pop("APPDIR", None)
    command = [str(args.exe), "--no-launcher", "--renderer", "software", "--memcard-dir",
               str(player_dir), "--debug-port", str(port), "--disc",
               str(args.disc)]
    log = log_path.open("w", encoding="utf-8")
    try:
        proc = subprocess.Popen(
            command, cwd=args.exe.parent, env=env, stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    except Exception:
        log.close()
        raise
    print(f"START {label}: harness={os.getpid()} game={proc.pid} port={port}",
          flush=True)
    record = {"label": label, "pid": proc.pid, "port": port,
              "port_verification": "exclusive loopback bind succeeded",
              "command": command, "log": str(log_path),
              "started_utc": dt.datetime.now(dt.timezone.utc).isoformat()}
    return proc, log, port, record


def wait_initialized(port, label):
    wait_q(port, {"cmd": "frame"}, lambda row: row.get("ok"), 45,
           f"{label} debug server")
    must(q(port, {"cmd": "game_speed", "mult": 4}), "accelerate boot")
    result = wait_q(port, {"cmd": "frame"},
                    lambda row: row.get("frame", 0) > 8400, 240,
                    f"{label} initialized game")
    must(q(port, {"cmd": "game_speed", "mult": 1}), "restore game speed")
    return result


def stop(proc, log, port, record):
    method = "already-exited"
    if proc.poll() is None:
        try:
            reply = q(port, {"cmd": "quit_graceful"})
            proc.wait(timeout=12)
            method = "debug quit_graceful" if reply.get("ok") else "graceful reply false"
        except Exception:
            proc.terminate()             # exact owned child only
            method = "exact-child SIGTERM fallback"
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()              # exact owned child only
                proc.wait(timeout=5)
                method = "exact-child SIGKILL fallback"
    record.update({"stop_method": method, "returncode": proc.returncode,
                   "stopped_utc": dt.datetime.now(dt.timezone.utc).isoformat()})
    log.close()
    print(f"STOP {record['label']}: game={proc.pid} return={proc.returncode} "
          f"method={method}", flush=True)


def load_fixture(port):
    before = q(port, {"cmd": "frame"}).get("frame", 0)
    must(q(port, {"cmd": "savestate", "op": "load", "slot": 0}),
         "load authorized fixture")
    wait_q(port, {"cmd": "frame"},
           lambda row: row.get("frame", 0) >= before + 3, 20,
           "post-load guest frames")
    state = wait_q(port, {"cmd": "story_rewards"},
                   lambda row: row.get("opponent_now") == OPPONENT, 20,
                   "Teana near-results fixture")
    time.sleep(5.0)
    return state


def press(port, mask=CROSS, settle=1.5):
    must(q(port, {"cmd": "press", "buttons": 0xFFFF & ~mask,
                  "frames": 12, "slot": 0}), "press")
    time.sleep(1.2)
    must(q(port, {"cmd": "clear_input"}), "clear_input")
    time.sleep(settle)


def capture_present(port, path):
    prior = must(q(port, {"cmd": "present_shot_seq"}),
                 "present screenshot sequence")["seq"]
    must(q(port, {"cmd": "present_shot", "path": str(path)}),
         "request software composed screenshot")
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        state = must(q(port, {"cmd": "present_shot_seq"}),
                     "poll software composed screenshot")
        if state["seq"] > prior:
            if not state.get("wrote") or not path.is_file():
                raise RuntimeError(("software screenshot failed", state, path))
            return {"path": str(path), "sha256": sha_file(path)}
        time.sleep(0.05)
    raise RuntimeError(f"timeout waiting for screenshot: {path}")


def open_viewer(port):
    must(q(port, {"cmd": "drop_viewer_set", "open": 1}), "open viewer")
    return wait_q(port, {"cmd": "drop_viewer"},
                  lambda row: row.get("open") and row.get("ready"), 30,
                  "Drop Tables page")


def click_rect(port, rect, button=1):
    x, y, width, height = rect
    return must(q(port, {"cmd": "drop_viewer_click",
                         "x": x + width // 2, "y": y + height // 2,
                         "button": button}), "viewer click")


def capture_viewer(port, path, duelist=DUELIST):
    open_viewer(port)
    must(q(port, {"cmd": "drop_viewer_set", "view": 1,
                  "duelist": duelist}), "select duelist for screenshot")
    state = wait_q(port, {"cmd": "drop_viewer"},
                   lambda row: row.get("view") == "duelists" and
                   row.get("sel_duelist") == duelist, 20,
                   "duelist screenshot state")
    must(q(port, {"cmd": "drop_viewer_shot", "path": str(path)}),
         "viewer screenshot")
    return {"path": str(path), "sha256": sha_file(path), "state": state}


def pixel_units(value, unit):
    return int(value * unit + 0.5)


def ui_add_first_card(port, evidence_dir):
    """Right-click card 1 and choose its first (S/A POW) Add action."""
    open_viewer(port)
    must(q(port, {"cmd": "drop_viewer_set", "view": 0, "sort": 0,
                  "desc": 0, "card": 1, "duelist": DUELIST,
                  "search": ""}), "show card 1")
    state = wait_q(port, {"cmd": "drop_viewer"},
                   lambda row: row.get("view") == "cards" and
                   row.get("sel_card") == 1 and row.get("scroll", [99])[0] == 0,
                   20, "card 1 editor row")
    rows = state["geom"]["left_rows"]
    row_h = state["geom"]["row_h"]
    right_x = rows[0] + max(8, row_h // 2)
    right_y = rows[1] + row_h // 2
    must(q(port, {"cmd": "drop_viewer_click", "x": right_x,
                  "y": right_y, "button": 3}), "open card Add menu")
    menu = wait_q(port, {"cmd": "drop_viewer"},
                  lambda row: row.get("menu") == 3, 20, "three Add choices")
    unit = menu["unit"]
    menu_row_h = row_h + pixel_units(2.0, unit)
    item_y = right_y + pixel_units(3.0, unit) + menu_row_h // 2
    must(q(port, {"cmd": "drop_viewer_click", "x": right_x + 8,
                  "y": item_y, "button": 1}), "Add card 1 to S/A POW")
    final = wait_q(port, {"cmd": "drop_viewer"},
                   lambda row: row.get("empty_bands") == 0, 20,
                   "first-card rebuild")
    shot = evidence_dir / "first-card-2048.ppm"
    must(q(port, {"cmd": "drop_viewer_shot", "path": str(shot)}),
         "first-card screenshot")
    return {"right_click": [right_x, right_y], "menu_items": menu["menu"],
            "selected_menu_item": 0, "state": final,
            "screenshot": {"path": str(shot), "sha256": sha_file(shot)}}


def ui_restore_duelist(port, evidence_dir):
    """Use Clear... > Restore this duelist to defaults through SDL events."""
    open_viewer(port)
    must(q(port, {"cmd": "drop_viewer_set", "view": 1,
                  "duelist": DUELIST}), "select Teana")
    state = wait_q(port, {"cmd": "drop_viewer"},
                   lambda row: row.get("view") == "duelists" and
                   row.get("sel_duelist") == DUELIST, 20, "Teana selection")
    click_rect(port, state["geom"]["third"])
    menu = wait_q(port, {"cmd": "drop_viewer"},
                  lambda row: row.get("menu") == 5, 20, "Clear menu")
    unit = menu["unit"]
    third = menu["geom"]["third"]
    px2 = pixel_units(2.0, unit)
    px3 = pixel_units(3.0, unit)
    px4 = pixel_units(4.0, unit)
    px6 = pixel_units(6.0, unit)
    menu_row_h = menu["geom"]["row_h"] + px2
    menu_h = 5 * menu_row_h + px6
    menu_y = third[1] + third[3] + px2
    if menu_y + menu_h > menu["canvas"][1] - px4:
        menu_y = menu["canvas"][1] - px4 - menu_h
    item_y = menu_y + px3 + 4 * menu_row_h + menu_row_h // 2
    item_x = third[0] + max(8, pixel_units(8.0, unit))
    must(q(port, {"cmd": "drop_viewer_click", "x": item_x,
                  "y": item_y, "button": 1}), "Restore this duelist")
    final = wait_q(port, {"cmd": "drop_viewer"},
                   lambda row: row.get("empty_bands") == 0 and
                   "restored to defaults" in row.get("msg", ""), 20,
                   "per-duelist restore")
    shot = evidence_dir / "restore-this-duelist.ppm"
    must(q(port, {"cmd": "drop_viewer_shot", "path": str(shot)}),
         "restore screenshot")
    return {"menu_items": menu["menu"], "selected_menu_item": 4,
            "click": [item_x, item_y], "state": final,
            "screenshot": {"path": str(shot), "sha256": sha_file(shot)}}


def export_drop(port, path):
    reply = q(port, {"cmd": "drop_viewer_set", "export": str(path)})
    if reply.get("ok") and not path.is_file():
        raise AssertionError(("export reported success without file", reply))
    return reply


def section(text, name):
    match = re.search(rf"(?ms)^\[{re.escape(name)}\]\n(.*?)(?=^\[|\Z)", text)
    return match.group(1) if match else ""


def verify_rebuilt_text(text):
    simon = section(text, OTHER_DUELIST)
    teana = section(text, TARGET_DUELIST)
    checks = {
        "format3": bool(re.search(r"(?m)^format = 3$", text)),
        "other_pow": "pow_table = 11:2048" in simon,
        "other_bcd": "bcd_table = 12:2048" in simon,
        "other_tec": "tec_table = 13:2048" in simon,
        "story_card": bool(re.search(r"(?m)^card = 92$", teana)),
        "story_every": bool(re.search(r"(?m)^when = every$", teana)),
        "first_card_2048": "pow_table = 1:2048" in teana,
        "unrelated_bands_absent": (
            "bcd_table" not in teana and "tec_table" not in teana),
    }
    if not all(checks.values()):
        raise AssertionError(("rebuilt export mismatch", checks, text))
    return checks


def assert_refused_exports(port, evidence_dir, label):
    targets = {
        "drop_ini": evidence_dir / f"{label}-refused.ini",
        "cards": evidence_dir / f"{label}-refused.ygocards",
        "package": evidence_dir / f"{label}-refused.ygomods",
    }
    for target in targets.values():
        if target.exists():
            raise AssertionError(f"refusal target already exists: {target}")
    viewer = must(q(port, {"cmd": "drop_viewer"}), "viewer before Save")
    if not viewer.get("open"):
        viewer = open_viewer(port)
    click_rect(port, viewer["geom"]["save"])
    ui_save = wait_q(port, {"cmd": "drop_viewer"},
                     lambda row: "empty" in row.get("msg", "").lower(), 20,
                     "shipping Save refusal")
    save_reply = q(port, {"cmd": "story_rewards", "save": 1})
    save_state = must(q(port, {"cmd": "drop_edits"}),
                      "state after refused Save")
    replies = {
        "save": save_reply,
        "drop_ini": q(port, {"cmd": "drop_viewer_set",
                              "export": str(targets["drop_ini"])}),
        "cards": q(port, {"cmd": "card_share", "op": "export",
                           "path": str(targets["cards"])}),
        "package": q(port, {"cmd": "mod_package",
                             "export": str(targets["package"])}),
    }
    for kind, reply in replies.items():
        detail = json.dumps(reply).lower()
        if kind == "save":
            detail += " " + save_state.get("status", "").lower()
        if reply.get("ok") or "empty" not in detail:
            raise AssertionError((f"{kind} did not clearly refuse empty", reply))
    created = {key: path.exists() for key, path in targets.items()}
    if any(created.values()):
        raise AssertionError(("refused export created output", created))
    return {"ui_save": ui_save, "replies": replies,
            "save_state": save_state,
            "output_created": created}


def import_file(port, path):
    return q(port, {"cmd": "drop_viewer_set", "import": str(path)})


def clear_one_case(port, evidence_dir, stock_tables):
    before_state = must(q(port, {"cmd": "drop_edits"}), "drop state")
    before_ram = resident_tables(port)
    if before_ram != stock_tables:
        raise AssertionError(("authorized fixture is not stock Teana",
                              table_summary(before_ram),
                              table_summary(stock_tables)))
    arm = must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                        "clear_band": 0, "confirm": 0}), "arm one-band clear")
    armed_state = must(q(port, {"cmd": "drop_edits"}), "armed state")
    if armed_state["empty_bands"] != 0:
        raise AssertionError(("arming mutated backend", armed_state))
    confirm = must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                            "clear_band": 0, "confirm": 1}),
                   "confirm one-band clear")
    empty = wait_q(port, {"cmd": "drop_edits"},
                   lambda row: row.get("empty_bands") == 1, 20,
                   "one pending empty band")
    time.sleep(2.0)
    after_ram = resident_tables(port)
    # Starting from stock, reconciliation may rewrite stock, but must be byte
    # identical. In particular, the authoring zero vector is never installed.
    if after_ram != before_ram or any(row["sum"] != 2048 for row in
                                      table_summary(after_ram)):
        raise AssertionError(("clear changed/emptied guest table",
                              table_summary(before_ram),
                              table_summary(after_ram)))
    empty_shot = capture_viewer(port, evidence_dir / "one-band-empty.ppm")
    refused = assert_refused_exports(port, evidence_dir, "one-band")
    ui = ui_add_first_card(port, evidence_dir)
    rebuilt_path = evidence_dir / "rebuilt-format3.ini"
    exported = must(export_drop(port, rebuilt_path), "rebuilt export")
    rebuilt_text = rebuilt_path.read_text(encoding="utf-8")
    checks = verify_rebuilt_text(rebuilt_text)
    saved = must(q(port, {"cmd": "story_rewards", "save": 1}),
                 "save rebuilt table")
    saved_state = must(q(port, {"cmd": "drop_edits"}), "saved state")
    if saved_state["dirty"] or saved_state["empty_bands"]:
        raise AssertionError(("save did not settle", saved_state))
    cards_dir = Path(must(q(port, {"cmd": "card_packs"}),
                          "player data location")["dir"])
    persistent_path = cards_dir.parent / "drop_table_edits.ini"
    if (not persistent_path.is_file() or
            persistent_path.read_bytes() != rebuilt_path.read_bytes()):
        raise AssertionError(("persistent Save differs from export",
                              persistent_path, rebuilt_path))
    return {
        "before_state": before_state, "arm": arm,
        "armed_state": armed_state, "confirm": confirm, "empty": empty,
        "guest_before": table_summary(before_ram),
        "guest_after": table_summary(after_ram),
        "guest_byte_identical": after_ram == before_ram,
        "empty_screenshot": empty_shot,
        "refusals": refused, "ui_first_card": ui,
        "export": {"reply": exported, "path": str(rebuilt_path),
                   "sha256": sha_file(rebuilt_path), "checks": checks},
        "save": saved, "saved_state": saved_state,
        "persistent_save": {"path": str(persistent_path),
                            "sha256": sha_file(persistent_path),
                            "byte_exact_with_export": True},
    }


def remaining_individual_bands(port, evidence_dir, baseline_path,
                               stock_tables):
    """Exercise the two individual-band choices not rebuilt by the live case."""
    names = ("S/A POW", "B/C/D", "S/A TEC")
    results = []
    for tier in (1, 2):
        must(import_file(port, baseline_path), f"baseline before {names[tier]}")
        wait_q(port, {"cmd": "read_ram",
                      "addr": f"{DROP_TABLE + tier * TIER_STRIDE:08X}",
                      "len": TABLE_BYTES},
               lambda row, expected=stock_tables[tier]: row.get("ok") and
               bytes.fromhex(row.get("hex") or row.get("data") or "") == expected,
               30, f"stock {names[tier]} before clear")
        before = must(q(port, {"cmd": "drop_edits"}),
                      f"state before {names[tier]}")
        arm = must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                            "clear_band": tier, "confirm": 0}),
                   f"arm {names[tier]}")
        armed = must(q(port, {"cmd": "drop_edits"}),
                     f"armed {names[tier]}")
        if armed["empty_bands"] != before["empty_bands"]:
            raise AssertionError(("individual arm mutated state", tier,
                                  before, armed))
        confirm = must(q(port, {"cmd": "drop_viewer_set",
                                "duelist": DUELIST, "clear_band": tier,
                                "confirm": 1}), f"confirm {names[tier]}")
        empty = wait_q(port, {"cmd": "drop_edits"},
                       lambda row: row.get("empty_bands") == 1, 20,
                       f"pending empty {names[tier]}")
        time.sleep(2.0)
        after = resident_tables(port)
        if after != stock_tables:
            raise AssertionError(("individual clear changed guest stock", tier,
                                  table_summary(stock_tables),
                                  table_summary(after)))
        results.append({"tier": tier, "name": names[tier], "arm": arm,
                        "armed_state": armed, "confirm": confirm,
                        "empty_state": empty,
                        "guest_byte_identical_to_stock": True,
                        "guest_tables": table_summary(after)})
    must(import_file(port, baseline_path), "baseline after individual bands")
    return results


def install_stock_tables(port, stock_tables):
    """Normalize only the copied fixture's resident drop rows to baked stock."""
    for tier, table in enumerate(stock_tables):
        write_ram(port, DROP_TABLE + tier * TIER_STRIDE, table)
    actual = resident_tables(port)
    if actual != stock_tables:
        raise AssertionError(("stock fixture normalization failed",
                              table_summary(actual)))
    return table_summary(actual)


def actual_duel_case(port, evidence_dir, rebuilt_path, stock_tables):
    # This copied fixture is switched to Free Duel so the preserved campaign
    # story reward remains separate from the one normal award under test.
    flags = bytearray(ram(port, FREE_FLAGS, 1))
    flags[0] |= 0x80
    write_ram(port, FREE_FLAGS, flags)
    must(q(port, {"cmd": "card_drops_set", "drops": 1, "smart": 0}),
         "one normal drop")
    write_ram(port, TRUNK, bytes([0]) + ram(port, TRUNK + 1, 721))
    trunk_before = ram(port, TRUNK, 722)
    card_zero_before = ram(port, CARD_ZERO, 1)[0]
    recent_before = ram(port, RECENT, 32)

    press(port, CROSS, 6.0)
    # The duel transition can reload its resident record. A hot import through
    # the real backend after the first confirmation forces reconciliation; the
    # fixture setup first restores the copied state's resident rows to the
    # exact baked stock bytes; no edited/replacement table is written directly.
    stock_after_transition = install_stock_tables(port, stock_tables)
    must(import_file(port, rebuilt_path), "hot import rebuilt table")
    expected = dense_table({1: 2048})
    wait_q(port, {"cmd": "drop_missing_state"},
           lambda row: row.get("edit_result", [-9])[0] == 1, 20,
           "rebuilt live tier")
    wait_q(port, {"cmd": "read_ram", "addr": f"{DROP_TABLE:08X}",
                  "len": TABLE_BYTES},
           lambda row: row.get("ok") and
           bytes.fromhex(row.get("hex") or row.get("data") or "") == expected,
           20, "resident card-1-only table")
    resident_before_roll = ram(port, DROP_TABLE, TABLE_BYTES)
    press(port, CROSS, 6.0)
    wait_q(port, {"cmd": "read_ram", "addr": f"{PHASE:08X}", "len": 2},
           lambda row: row.get("ok") and
           (int.from_bytes(bytes.fromhex(row.get("hex") or
                                         row.get("data") or ""), "little")
            & 0xF) == 0xD, 45, "duel results")
    visible = wait_q(port, {"cmd": "card_drops_list"},
                     lambda row: row.get("total") == 1, 20,
                     "one visible normal award")
    if visible.get("award_order", [{}])[0].get("id") != 1:
        raise AssertionError(("rebuilt table awarded wrong card", visible))
    shot = evidence_dir / "live-rebuilt-result.png"
    screenshot = capture_present(port, shot)
    press(port, CROSS, 6.0)
    final = wait_q(port, {"cmd": "card_drops_list"},
                   lambda row: row.get("order_n") == 1 and
                   all(item.get("committed") for item in
                       row.get("award_order", [])), 30, "committed card 1")
    trunk_after = ram(port, TRUNK, 722)
    if (trunk_after[0] != 1 or trunk_after[1:] != trunk_before[1:] or
            ram(port, CARD_ZERO, 1)[0] != card_zero_before):
        raise AssertionError(("live award inventory mutation", trunk_before[0],
                              trunk_after[0], card_zero_before,
                              ram(port, CARD_ZERO, 1)[0]))
    return {
        "pass": True, "mode": "free_duel", "normal_drop_count": 1,
        "stock_fixture_after_transition": stock_after_transition,
        "resident_before_roll": table_summary([resident_before_roll])[0],
        "visible": visible, "final": final,
        "card_zero_before": card_zero_before,
        "card_zero_after": ram(port, CARD_ZERO, 1)[0],
        "trunk_before_sha256": sha_bytes(trunk_before),
        "trunk_after_sha256": sha_bytes(trunk_after),
        "target_before": trunk_before[0], "target_after": trunk_after[0],
        "recent_before_sha256": sha_bytes(recent_before),
        "recent_after_sha256": sha_bytes(ram(port, RECENT, 32)),
        "screenshot": screenshot,
    }


def all_three_and_restore(port, evidence_dir, stock_tables):
    before = must(q(port, {"cmd": "drop_edits"}), "pre-clear state")
    arm = must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                        "clear_all_bands": 1, "confirm": 0}),
               "arm all-three clear")
    armed = must(q(port, {"cmd": "drop_edits"}), "all-three armed state")
    if armed["empty_bands"] != before["empty_bands"]:
        raise AssertionError(("all-three arm mutated state", before, armed))
    confirm = must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                            "clear_all_bands": 1, "confirm": 1}),
                   "confirm all-three clear")
    empty = wait_q(port, {"cmd": "drop_edits"},
                   lambda row: row.get("empty_bands") == 3, 20,
                   "three pending empty bands")
    time.sleep(2.0)
    tables = resident_tables(port)
    if tables != stock_tables or any(row["sum"] != 2048 for row in
                                     table_summary(tables)):
        raise AssertionError(("all-three clear reached guest", table_summary(tables)))
    empty_shot = capture_viewer(port, evidence_dir / "all-three-empty.ppm")
    refused = assert_refused_exports(port, evidence_dir, "all-three")
    restored_ui = ui_restore_duelist(port, evidence_dir)
    restored_path = evidence_dir / "restored-this-duelist.ini"
    must(export_drop(port, restored_path), "export per-duelist restore")
    text = restored_path.read_text(encoding="utf-8")
    teana = section(text, TARGET_DUELIST)
    simon = section(text, OTHER_DUELIST)
    checks = {
        "story_preserved": "card = 92" in teana and "when = every" in teana,
        "target_weight_edits_removed": "_table" not in teana,
        "other_duelist_preserved": all(token in simon for token in
            ("pow_table = 11:2048", "bcd_table = 12:2048",
             "tec_table = 13:2048")),
    }
    if not all(checks.values()):
        raise AssertionError(("per-duelist restore preservation", checks, text))
    return {"before": before, "arm": arm, "armed": armed,
            "confirm": confirm, "empty": empty,
            "guest_safe": table_summary(tables),
            "empty_screenshot": empty_shot, "refusals": refused,
            "restore_ui": restored_ui,
            "export": {"path": str(restored_path),
                       "sha256": sha_file(restored_path), "checks": checks}}


def randomize_and_global_restore(port, evidence_dir, baseline_path):
    must(import_file(port, baseline_path), "restore baseline before Randomize")
    must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                  "clear_all_bands": 1, "confirm": 0}), "arm Randomize setup")
    must(q(port, {"cmd": "drop_viewer_set", "duelist": DUELIST,
                  "clear_all_bands": 1, "confirm": 1}), "clear Randomize setup")
    seed = 9534
    randomized = must(q(port, {"cmd": "drop_viewer_set", "randomize": seed}),
                      "Randomize after clear")
    state = must(q(port, {"cmd": "drop_edits"}), "randomized state")
    if state["empty_bands"] or randomized.get("entries", 0) <= 0:
        raise AssertionError(("Randomize did not rebuild", randomized, state))
    random_path = evidence_dir / "randomized-format3.ini"
    must(export_drop(port, random_path), "randomized export")
    random_text = random_path.read_text(encoding="utf-8")
    if (not re.search(r"(?m)^format = 3$", random_text) or
            "card = 92" not in section(random_text, TARGET_DUELIST)):
        raise AssertionError("Randomize lost format/story reward")

    arm = must(q(port, {"cmd": "drop_viewer_set", "restore_all": 1}),
               "arm global restore")
    armed_state = must(q(port, {"cmd": "drop_edits"}),
                       "global restore armed state")
    if armed_state["entries"] != state["entries"]:
        raise AssertionError(("restore arm mutated weights", state, armed_state))
    confirm = must(q(port, {"cmd": "drop_viewer_set", "restore_all": 2}),
                   "confirm global restore")
    final_state = must(q(port, {"cmd": "drop_edits"}),
                       "global restored state")
    if (final_state["entries"] or final_state["replacements"] or
            final_state["empty_bands"]):
        raise AssertionError(("global restore retained weights", final_state))
    final_path = evidence_dir / "global-restored-format3.ini"
    must(export_drop(port, final_path), "global restored export")
    final_text = final_path.read_text(encoding="utf-8")
    if ("card = 92" not in section(final_text, TARGET_DUELIST) or
            re.search(r"(?m)^(?:pow|bcd|tec)_table\s*=", final_text)):
        raise AssertionError(("global restore lost story/kept weights", final_text))
    return {
        "seed": seed, "randomize": randomized, "randomized_state": state,
        "randomized_export": {"path": str(random_path),
                              "sha256": sha_file(random_path)},
        "restore_arm": arm, "armed_state": armed_state,
        "restore_confirm": confirm, "final_state": final_state,
        "restored_export": {"path": str(final_path),
                            "sha256": sha_file(final_path)},
        "story_preserved": True,
    }


def compatibility_cases(port, evidence_dir, baseline_path):
    old2 = evidence_dir / "old-format2.ini"
    write_text(old2, "format = 2\n\n[Weevil Underwood]\ncard = 37\n"
                     "when = every\npow_table = 1:1500, 2:548\n")
    old_unversioned = evidence_dir / "old-unversioned-reward.ini"
    write_text(old_unversioned, "[Weevil Underwood]\ncard = 92\n")
    accepted = []
    for source in (old2, old_unversioned):
        reply = must(import_file(port, source), f"import {source.name}")
        canonical = evidence_dir / f"{source.stem}-canonical.ini"
        must(export_drop(port, canonical), f"export {source.name}")
        text = canonical.read_text(encoding="utf-8")
        if not re.search(r"(?m)^format = 3$", text):
            raise AssertionError(("old import did not migrate to format 3", text))
        accepted.append({"source": {"path": str(source),
                                    "sha256": sha_file(source)},
                         "reply": reply,
                         "canonical": {"path": str(canonical),
                                       "sha256": sha_file(canonical)}})

    must(import_file(port, baseline_path), "install transactional baseline")
    before_path = evidence_dir / "before-invalid.ini"
    must(export_drop(port, before_path), "export transactional baseline")
    before = before_path.read_bytes()
    invalid = {
        "empty": "format = 3\n[Teana]\npow_table = none\n",
        "bad-total": "format = 3\n[Teana]\npow_table = 1:2047\n",
        "duplicate": "format = 3\n[Teana]\npow_table = 1:1024, 1:1024\n",
        "malformed": "format = 3\n[Teana]\npow_table = 1:2048,\n",
        "future": "format = 999\n[Teana]\npow_table = 1:2048\n",
    }
    rejected = []
    for name, body in invalid.items():
        source = evidence_dir / f"invalid-{name}.ini"
        write_text(source, body)
        reply = import_file(port, source)
        if reply.get("ok"):
            raise AssertionError(("invalid import accepted", name, reply))
        after_path = evidence_dir / f"after-invalid-{name}.ini"
        must(export_drop(port, after_path), f"export after invalid {name}")
        after = after_path.read_bytes()
        if after != before:
            raise AssertionError(("invalid import was not transactional", name,
                                  sha_bytes(before), sha_bytes(after)))
        rejected.append({"case": name, "source": str(source),
                         "source_sha256": sha_file(source), "reply": reply,
                         "state_unchanged_sha256": sha_bytes(after)})
    return {"accepted_old": accepted, "rejected": rejected,
            "transactional_baseline": {"path": str(before_path),
                                       "sha256": sha_bytes(before)}}


def baseline_ini():
    # Target Teana deliberately has only a story reward: its resident tables
    # are stock before Clear. Simon supplies an unrelated duelist sentinel.
    return (
        "format = 3\n\n"
        "[Simon Muran]\n"
        "pow_table = 11:2048\n"
        "bcd_table = 12:2048\n"
        "tec_table = 13:2048\n\n"
        "[Teana]\n"
        "card = 92\n"
        "when = every\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True,
                        help=f"authorized directory containing {STATE} and {THUMB}")
    parser.add_argument("--output", type=Path, required=True,
                        help="fresh evidence directory; must not already exist")
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-dbg" /
                        "Yu_Gi_Oh_Forbidden_Memories_Recompiled")
    parser.add_argument("--disc", type=Path,
                        default=ROOT / "disc" /
                        "Yu-Gi-Oh! Forbidden Memories (USA).cue")
    parser.add_argument("--generated-source", type=Path,
                        default=ROOT / "generated" /
                        "SLUS_014.11_full_06.c")
    parser.add_argument("--drop-db-source", type=Path,
                        default=ROOT / "build-dbg" / "game-assets" /
                        "psx_drop_db.c")
    args = parser.parse_args()
    args.fixture = args.fixture.resolve()
    args.output = args.output.resolve()
    args.exe = args.exe.resolve()
    args.disc = args.disc.resolve()
    args.generated_source = args.generated_source.resolve()
    args.drop_db_source = args.drop_db_source.resolve()
    if args.output.exists():
        raise SystemExit(f"refusing to reuse output directory: {args.output}")
    required = (args.fixture / STATE, args.fixture / THUMB, args.exe,
                args.disc, args.generated_source, args.drop_db_source)
    for path in required:
        if not path.is_file():
            raise SystemExit(f"missing required file: {path}")
    if args.disc.suffix.lower() != ".cue" or "(USA)" not in args.disc.name:
        raise SystemExit(
            "--disc must be the explicit Yu-Gi-Oh! Forbidden Memories (USA) .cue")

    evidence_dir = args.output / "evidence"
    state_dir = args.output / "player" / "openbios"
    evidence_dir.mkdir(parents=True)
    state_dir.mkdir(parents=True)
    fixture = patch_copied_state(args.fixture / STATE, state_dir / STATE)
    shutil.copyfile(args.fixture / THUMB, state_dir / THUMB)
    fixture["thumb_source"] = str(args.fixture / THUMB)
    fixture["thumb_source_sha256"] = sha_file(args.fixture / THUMB)
    fixture["thumb_copy_sha256"] = sha_file(state_dir / THUMB)

    report = {
        "schema": 1, "pass": False,
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "provenance": {
            "argv": sys.argv, "cwd": str(Path.cwd()),
            "root": str(ROOT), "git_head": git_output("rev-parse", "HEAD"),
            "git_branch": git_output("branch", "--show-current"),
            "git_status": git_output("status", "--short"),
            "python": sys.version, "platform": platform.platform(),
            "executable": str(args.exe),
            "executable_sha256": sha_file(args.exe),
            "explicit_usa_cue": str(args.disc),
            "cue_sha256": sha_file(args.disc),
            "portable": True,
            "explicit_memcard_dir": str(args.output / "player"),
            "fixture": fixture,
            "drop_db_source": {"path": str(args.drop_db_source),
                               "sha256": sha_file(args.drop_db_source)},
        },
        "processes": [], "stock_loop": None, "single_band": None,
        "remaining_individual_bands": None,
        "live_duel": None, "restart": None, "all_three": None,
        "randomize_restore": None, "compatibility": None,
        "limitations": [
            "The generated-code assertion requires the current generated "
            "title source and baked drop database to be present.",
            "The authorized near-results fixture exercises the actual live "
            "duel at its natural tier-0 drop call; all three authoring bands "
            "are covered through editor state, export, and guest-safety checks.",
            "The copied historical near-results fixture carries non-stock "
            "resident weights. The harness writes the exact current baked "
            "stock bytes as fixture setup before testing any editor action; "
            "replacement tables are applied only by the production backend.",
            "Native file-picker interaction is outside this deterministic "
            "harness. Direct Drop Tables, edited-card, and MOD-package debug "
            "commands invoke the same single-sourced export backends.",
        ],
    }
    results_path = args.output / "results.json"
    report["stock_loop"] = stock_loop_analysis(args.generated_source)
    write_text(evidence_dir / "stock-loop-analysis.json",
               json.dumps(report["stock_loop"], indent=2) + "\n")
    stock_tables = parse_stock_table_source(args.drop_db_source, DUELIST)
    report["stock_tables"] = table_summary(stock_tables)
    baseline_path = evidence_dir / "baseline-format3.ini"
    write_text(baseline_path, baseline_ini())

    proc = log = None
    port = None
    first_process = None
    try:
        proc, log, port, first_process = launch(
            args, args.output / "player", args.output / "runtime-first.log",
            "clear-rebuild-live")
        report["processes"].append(first_process)
        wait_initialized(port, first_process["label"])
        load_fixture(port)
        must(q(port, {"cmd": "mod_package", "reset": 1}), "isolated reset")
        # Reload after reset so the current opponent's resident record is the
        # copied fixture's known stock data, while the host edit layer is empty.
        load_fixture(port)
        report["fixture_stock_normalization"] = install_stock_tables(
            port, stock_tables)
        must(import_file(port, baseline_path), "baseline import")
        wait_q(port, {"cmd": "drop_missing_state"},
               lambda row: row.get("last_duelist") == TARGET_DUELIST, 30,
               "stock Teana resident match")
        report["single_band"] = clear_one_case(
            port, evidence_dir, stock_tables)
        rebuilt_path = Path(report["single_band"]["export"]["path"])
        report["live_duel"] = actual_duel_case(port, evidence_dir,
                                               rebuilt_path, stock_tables)
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if proc is not None:
            stop(proc, log, port, first_process)
        results_path.write_text(json.dumps(report, indent=2) + "\n")

    proc = log = None
    port = None
    restart_process = None
    try:
        proc, log, port, restart_process = launch(
            args, args.output / "player", args.output / "runtime-restart.log",
            "restart-and-remaining-cases")
        report["processes"].append(restart_process)
        wait_initialized(port, restart_process["label"])
        load_fixture(port)
        report["restart_fixture_stock_normalization"] = install_stock_tables(
            port, stock_tables)
        wait_q(port, {"cmd": "drop_missing_state"},
               lambda row: row.get("last_duelist") == TARGET_DUELIST, 30,
               "restart Teana resident match")
        restart_path = evidence_dir / "restart-format3.ini"
        must(export_drop(port, restart_path), "restart export")
        restart_text = restart_path.read_text(encoding="utf-8")
        restart_checks = verify_rebuilt_text(restart_text)
        original = Path(report["single_band"]["export"]["path"]).read_bytes()
        if restart_path.read_bytes() != original:
            raise AssertionError(("restart changed canonical edit file",
                                  sha_bytes(original), sha_file(restart_path)))
        restart_state = must(q(port, {"cmd": "drop_edits"}), "restart state")
        if restart_state["dirty"] or restart_state["empty_bands"]:
            raise AssertionError(("restart state not saved/valid", restart_state))
        report["restart"] = {
            "pass": True, "state": restart_state, "checks": restart_checks,
            "byte_exact": True, "path": str(restart_path),
            "sha256": sha_file(restart_path),
        }
        report["remaining_individual_bands"] = remaining_individual_bands(
            port, evidence_dir, baseline_path, stock_tables)
        report["all_three"] = all_three_and_restore(
            port, evidence_dir, stock_tables)
        report["randomize_restore"] = randomize_and_global_restore(
            port, evidence_dir, baseline_path)
        report["compatibility"] = compatibility_cases(
            port, evidence_dir, baseline_path)
        report["pass"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if proc is not None:
            stop(proc, log, port, restart_process)
        report["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        results_path.write_text(json.dumps(report, indent=2) + "\n")

    print(f"PASS clear-drop regression: {results_path}", flush=True)


if __name__ == "__main__":
    main()
