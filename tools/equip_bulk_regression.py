#!/usr/bin/env python3
r"""Owned-process regression for bulk Equip usable-monster authoring.

The harness requires a never-used portable player-data directory.  It boots an
explicit title executable/disc, exercises the Fusion page through its debug/UI
injection API, restarts the exact owned process to verify persistence, and
writes a request transcript plus a provenance-rich ``results.json``.

Example:

    python3 -B tools/equip_bulk_regression.py \
      --executable build-dbg/YuGiOhForbiddenMemoriesRecomp \
      --disc /path/to/Yu-Gi-Oh-USA.cue \
      --duel-state /path/to/state_800129D8_slot07.pst \
      --duel-slot 7 \
      --memcard-dir /tmp/ygofm-equip-player-new \
      --output /tmp/ygofm-equip-regression

The current ``fusion_manager`` key injection accepts a keycode but no modifier
mask.  Delete is exercised; Ctrl+A is reported as an explicit unsupported
probe rather than being claimed from a synthetic event that cannot carry Ctrl.
"""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client  # noqa: E402


EQUIP = 301                 # Legendary Sword: 63 stock usable monsters
OLD_CODEGEN = 0xF691F520
NEW_CODEGEN = 0xE3093403
SENTINELS = {
    "price": "123456",
    "password": "00123456",
    "color": "green",
    "name_color": "yellow",
}
BOOT_FRAME = 8400


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def file_info(path):
    path = path.resolve()
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": digest(path)}


def parse_ini(text):
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in ";#[":
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.split(";", 1)[0].strip()
    return values


def equip_ids_from_ini(values):
    value = values.get("equips", "")
    if not value or value.lower() == "none":
        return []
    return [int(piece.strip()) for piece in value.split(",") if piece.strip()]


def same_ids(actual, expected):
    """Compare membership while still detecting duplicate IDs."""
    return (len(actual) == len(expected) and len(actual) == len(set(actual)) and
            set(actual) == set(expected))


def center(rect):
    return rect[0] + max(rect[2] // 2, 1), rect[1] + max(rect[3] // 2, 1)


def git_provenance():
    def run(*args):
        return subprocess.run(args, cwd=ROOT, check=True, text=True,
                              stdout=subprocess.PIPE).stdout.strip()
    try:
        return {
            "head": run("git", "rev-parse", "HEAD"),
            "status_porcelain": run("git", "status", "--porcelain").splitlines(),
        }
    except (OSError, subprocess.CalledProcessError) as exc:
        return {"error": str(exc)}


class Regression:
    def __init__(self, args):
        self.args = args
        self.port = args.port or self.find_free_port()
        self.duel_state_target = (
            args.memcard_dir / "openbios" /
            f"state_800129D8_slot{args.duel_slot:02d}.pst"
        )
        self.duel_state_target.parent.mkdir(parents=True)
        shutil.copy2(args.duel_state, self.duel_state_target)
        state_data = bytearray(self.duel_state_target.read_bytes())
        if len(state_data) < 20:
            raise ValueError("duel savestate header is truncated")
        source_codegen = struct.unpack_from("<I", state_data, 16)[0]
        if source_codegen == OLD_CODEGEN:
            struct.pack_into("<I", state_data, 16, NEW_CODEGEN)
            self.duel_state_target.write_bytes(state_data)
        elif source_codegen != NEW_CODEGEN:
            raise ValueError(f"unsupported duel savestate codegen {source_codegen:08X}")
        (args.memcard_dir / ".pst_bios_isolated").write_text(
            "equip bulk regression fixture installed explicitly\n",
            encoding="utf-8",
        )
        self.transcript_path = args.output / "commands.jsonl"
        self.transcript = self.transcript_path.open("w", encoding="utf-8")
        self.sequence = 0
        self.proc = None
        self.log = None
        self.card_path = None
        self.launch_number = 0
        self.failures = []
        self.result = {
            "schema": 1,
            "started_utc": utc_now(),
            "provenance": {
                "root": str(ROOT),
                "python": sys.version,
                "platform": platform.platform(),
                "portable": True,
                "renderer": args.renderer,
                "debug_host": "127.0.0.1",
                "debug_port": self.port,
                "memcard_dir": str(args.memcard_dir),
                "output_dir": str(args.output),
                "executable": file_info(args.executable),
                "disc_cue": file_info(args.disc),
                "duel_state_source": file_info(args.duel_state),
                "duel_state_installed": file_info(self.duel_state_target),
                "duel_state_slot": args.duel_slot,
                "duel_state_source_codegen": f"0x{source_codegen:08X}",
                "duel_state_copy_codegen": f"0x{NEW_CODEGEN:08X}",
                "old_fixture": file_info(args.old_fixture),
                "git": git_provenance(),
            },
            "processes": [],
            "checks": {},
            "observations": {},
            "artifacts": {},
            "limitations": {
                "picker_ctrl_a": (
                    "not executable through fusion_manager: keycode injection "
                    "does not expose an SDL modifier mask"
                )
            },
        }

    @staticmethod
    def find_free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def port_is_free(self):
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", self.port))
                return True
            except OSError:
                return False

    def wait_port_free(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.port_is_free():
                return True
            time.sleep(0.1)
        return self.port_is_free()

    def check(self, name, condition, detail=None):
        passed = bool(condition)
        row = {"pass": passed}
        if detail is not None:
            row["detail"] = detail
        self.result["checks"][name] = row
        if not passed:
            self.failures.append(name)
            raise AssertionError(f"{name}: {detail!r}")

    def skip(self, name, reason):
        self.result["checks"][name] = {"pass": None, "status": "unsupported",
                                       "reason": reason}

    def raw_query(self, payload):
        return debug_client.query("127.0.0.1", self.port, payload)

    def query(self, payload, require_ok=True):
        started = time.monotonic()
        response = self.raw_query(payload)
        self.sequence += 1
        self.transcript.write(json.dumps({
            "sequence": self.sequence,
            "utc": utc_now(),
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "request": payload,
            "response": response,
        }, sort_keys=True) + "\n")
        self.transcript.flush()
        if require_ok and not response.get("ok"):
            raise RuntimeError(f"debug command failed: {payload!r}: {response!r}")
        return response

    def wait(self, payload, predicate, timeout, label, interval=0.25):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError(
                    f"owned process {self.proc.pid} exited while waiting for {label}: "
                    f"{self.proc.returncode}"
                )
            try:
                last = self.raw_query(payload)
                if predicate(last):
                    return self.query(payload)
            except (ConnectionError, OSError, TimeoutError):
                pass
            time.sleep(interval)
        raise RuntimeError(f"timeout waiting for {label}: {last!r}")

    def launch(self):
        self.launch_number += 1
        if self.launch_number > 1 and self.args.port is None:
            self.port = self.find_free_port()
        self.check(f"launch_{self.launch_number}_port_was_free",
                   self.wait_port_free(), {"port": self.port})
        log_path = self.args.output / f"runtime-{self.launch_number}.log"
        self.log = log_path.open("w", encoding="utf-8")
        env = dict(os.environ, PSX_PORTABLE="1")
        env.pop("APPIMAGE", None)
        env.pop("APPDIR", None)
        command = [
            str(self.args.executable), "--no-launcher",
            "--renderer", self.args.renderer,
            "--memcard-dir", str(self.args.memcard_dir),
            "--debug-port", str(self.port),
            "--disc", str(self.args.disc),
        ]
        self.proc = subprocess.Popen(
            command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        process = {
            "launch": self.launch_number,
            "pid": self.proc.pid,
            "port": self.port,
            "command": command,
            "runtime_log": str(log_path),
            "started_utc": utc_now(),
        }
        self.result["processes"].append(process)
        self.wait({"cmd": "frame"}, lambda r: r.get("ok"), 45,
                  "debug server")
        self.query({"cmd": "game_speed", "mult": 4})
        self.wait({"cmd": "frame"},
                  lambda r: r.get("frame", 0) > BOOT_FRAME, 240,
                  "fully initialized game", interval=1.0)
        self.query({"cmd": "game_speed", "mult": 1})

    def stop(self):
        if self.proc is None:
            return
        process = self.result["processes"][-1]
        method = "already_exited"
        if self.proc.poll() is None:
            try:
                self.query({"cmd": "quit_graceful"})
                self.proc.wait(timeout=15)
                method = "quit_graceful"
            except Exception as exc:  # exact owned PID fallback
                process["graceful_error"] = repr(exc)
                self.proc.terminate()
                method = "terminate"
                try:
                    self.proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=8)
                    method = "kill"
        process.update({
            "stopped_utc": utc_now(),
            "stop_method": method,
            "exit_code": self.proc.returncode,
        })
        stop_check = f"launch_{self.launch_number}_graceful_exact_owned_stop"
        stop_passed = method == "quit_graceful" and self.proc.returncode == 0
        self.result["checks"][stop_check] = {
            "pass": stop_passed,
            "detail": {"pid": self.proc.pid, "method": method,
                       "exit_code": self.proc.returncode},
        }
        if not stop_passed and stop_check not in self.failures:
            self.failures.append(stop_check)
        if self.log is not None:
            self.log.close()
            self.log = None
        self.proc = None

    def fusion_state(self, open_window=False):
        payload = {"cmd": "fusion_manager"}
        if open_window:
            payload.update({"open": 1, "card": EQUIP})
            self.query(payload)
            return self.wait(
                {"cmd": "fusion_manager"},
                lambda r: r.get("open") and r.get("table_ready"),
                15, "Fusion page",
            )
        return self.query(payload)

    def equip_ids(self):
        state = self.query({"cmd": "fusion_manager", "card_json": 1,
                            "card": EQUIP})
        return [row["partner"] for row in state.get("makes", [])
                if row.get("equip")]

    def set_equip_ids(self, ids, require_ok=True):
        return self.query({
            "cmd": "fusion_manager", "equip": EQUIP,
            "equip_ids": ",".join(str(value) for value in ids),
        }, require_ok=require_ok)

    def picker_open(self):
        self.query({"cmd": "fusion_manager", "equip_batch": EQUIP})
        return self.wait({"cmd": "fusion_manager"},
                         lambda r: r.get("picker") == 5, 5,
                         "Equip batch picker")

    def click_rect(self, state, name):
        x, y = center(state["geom"][name])
        self.query({"cmd": "fusion_manager", "x": x, "y": y})
        time.sleep(0.15)
        return self.query({"cmd": "fusion_manager"})

    def picker_shot(self, name):
        path = self.args.output / name
        self.query({"cmd": "fusion_manager", "shot": str(path)})
        self.check(name + "_created", path.is_file() and path.stat().st_size > 0,
                   str(path))
        self.result["artifacts"][name] = file_info(path)

    def card_file(self):
        state = self.query({"cmd": "card_packs"})
        self.card_path = Path(state["dir"]) / str(EQUIP) / "card.ini"
        return self.card_path

    def card_values(self):
        return parse_ini(self.card_file().read_text(encoding="utf-8"))

    def check_sentinels(self, name):
        values = self.card_values()
        self.check(name, all(values.get(k) == v for k, v in SENTINELS.items()),
                   {k: values.get(k) for k in SENTINELS})

    def seed_unrelated_fields(self):
        card = self.card_file()
        card.parent.mkdir(parents=True, exist_ok=True)
        card.write_text(
            "; equip bulk regression sentinels\n"
            + "".join(f"{key} = {value}\n" for key, value in SENTINELS.items()),
            encoding="utf-8",
        )
        self.query({"cmd": "card_packs_reload", "card": EQUIP})
        self.check_sentinels("unrelated_fields_seeded")
        self.result["observations"]["sentinel_card_initial"] = file_info(card)

    def stock_and_api(self):
        manager = self.fusion_state(open_window=True)
        stock = self.equip_ids()
        self.check("stock_count_63", len(stock) == 63, len(stock))
        recipes = manager["recipes"]
        self.result["observations"].update({
            "stock_equip_count": len(stock),
            "recipes_before": recipes,
            "stock_equip_cards": manager["equip_cards"],
            "stock_equip_links": manager["equip_links"],
        })
        recipe_snapshot = self.args.output / "fusion-recipes-before.tsv"
        self.query({"cmd": "fusion_manager", "export": str(recipe_snapshot)})
        recipe_sha256 = digest(recipe_snapshot)

        stock_json = self.args.output / "stock.json"
        exported = self.query({"cmd": "randomizer_stock", "path": str(stock_json)})
        data = json.loads(stock_json.read_text(encoding="utf-8"))
        monsters = sorted(int(card) for card, row in data["cards"].items()
                          if row["type"] < 20)
        self.check("stock_export_complete", exported.get("cards") == 722,
                   exported)
        self.check("monster_count_621", len(monsters) == 621, len(monsters))
        self.result["artifacts"]["stock.json"] = file_info(stock_json)

        self.set_equip_ids([])
        self.check("api_empty", same_ids(self.equip_ids(), []), self.equip_ids())
        self.check_sentinels("api_empty_preserves_unrelated_fields")
        two = monsters[:2]
        self.set_equip_ids(two)
        self.check("api_two", same_ids(self.equip_ids(), two), self.equip_ids())
        duplicate = self.set_equip_ids([two[0], two[0]], require_ok=False)
        self.check("api_duplicate_rejected",
                   not duplicate.get("ok") and same_ids(self.equip_ids(), two),
                   duplicate)
        self.set_equip_ids(monsters)
        self.check("api_all_621", same_ids(self.equip_ids(), monsters),
                   len(self.equip_ids()))
        self.check_sentinels("api_621_preserves_unrelated_fields")
        effects = self.query({"cmd": "card_effects"})
        self.check("runtime_621_no_drop",
                   effects.get("equip_override") == 1 and
                   effects.get("equip_dropped") == 0,
                   {k: effects.get(k) for k in
                    ("equip_override", "equip_bytes", "equip_dropped")})
        return recipes, recipe_sha256, monsters, two, stock

    def picker_actions(self, monsters, two):
        # Begin from two so a single visible row toggle has an exact delta.
        self.set_equip_ids(two)
        state = self.picker_open()
        self.check("picker_lists_621_monsters", state.get("pick_listed") == 621,
                   state.get("pick_listed"))
        self.picker_shot("picker-initial.ppm")
        rows = state["geom"]["pick_rows"]
        x = rows[0] + 6
        y = rows[1] + max(state["geom"]["row_h"] // 2, 1)
        before = state["pick_selected"]
        self.query({"cmd": "fusion_manager", "x": x, "y": y})
        time.sleep(0.15)
        toggled = self.query({"cmd": "fusion_manager"})
        self.check("picker_row_toggle",
                   abs(toggled["pick_selected"] - before) == 1,
                   {"before": before, "after": toggled["pick_selected"]})
        self.query({"cmd": "fusion_manager", "x": x, "y": y})
        time.sleep(0.15)
        state = self.query({"cmd": "fusion_manager"})
        self.check("picker_row_toggle_reversible",
                   state["pick_selected"] == before, state["pick_selected"])

        state = self.click_rect(state, "pick_clear")
        self.check("picker_clear_all_pending", state["pick_selected"] == 0,
                   state["pick_selected"])
        state = self.click_rect(state, "pick_cancel")
        self.check("picker_clear_cancelled", same_ids(self.equip_ids(), two),
                   self.equip_ids())

        state = self.picker_open()
        self.query({"cmd": "fusion_manager", "text": "Dragon"})
        time.sleep(0.15)
        state = self.query({"cmd": "fusion_manager"})
        filtered = state["pick_listed"]
        self.check("picker_search_filters", 0 < filtered < 621,
                   {"search": state["pick_search"], "listed": filtered})
        before = state["pick_selected"]
        state = self.click_rect(state, "pick_remove")
        self.check("picker_remove_filtered",
                   state["pick_selected"] < before,
                   {"before": before, "after": state["pick_selected"]})
        self.click_rect(state, "pick_cancel")
        self.check("picker_remove_cancelled", same_ids(self.equip_ids(), two),
                   self.equip_ids())

        state = self.picker_open()
        state = self.click_rect(state, "pick_clear")
        self.query({"cmd": "fusion_manager", "text": "Dragon"})
        time.sleep(0.15)
        state = self.query({"cmd": "fusion_manager"})
        filtered = state["pick_listed"]
        state = self.click_rect(state, "pick_select")
        self.check("picker_select_filtered",
                   state["pick_selected"] == filtered,
                   {"filtered": filtered, "selected": state["pick_selected"]})
        self.query({"cmd": "fusion_manager", "keycode": 127})
        time.sleep(0.15)
        state = self.query({"cmd": "fusion_manager"})
        self.check("picker_delete_removes_filtered",
                   state["pick_selected"] == 0, state["pick_selected"])
        self.click_rect(state, "pick_cancel")
        self.check("picker_delete_cancelled", same_ids(self.equip_ids(), two),
                   self.equip_ids())

        self.skip("picker_ctrl_a", self.result["limitations"]["picker_ctrl_a"])

        state = self.picker_open()
        state = self.click_rect(state, "pick_clear")
        state = self.click_rect(state, "pick_select")
        self.check("picker_select_all_621", state["pick_selected"] == 621,
                   state["pick_selected"])
        self.picker_shot("picker-all-selected.ppm")
        self.click_rect(state, "pick_apply")
        self.check("picker_apply_persists_621", same_ids(self.equip_ids(), monsters),
                   len(self.equip_ids()))

    def package_roundtrips(self, monsters, two):
        cards = self.args.output / "equip-large.ygocards"
        mods = self.args.output / "equip-large.ygomods"
        self.query({"cmd": "card_share", "op": "export", "path": str(cards)})
        inspect_cards = self.query({"cmd": "card_share", "op": "inspect",
                                    "path": str(cards)})
        with zipfile.ZipFile(cards) as archive:
            member_bytes = archive.read(f"cards/{EQUIP}/card.ini")
            member = member_bytes.decode("utf-8")
        values = parse_ini(member)
        self.check("ygocards_contains_621_and_sentinels",
                   equip_ids_from_ini(values) == monsters and
                   all(values.get(k) == v for k, v in SENTINELS.items()),
                   {"inspect": inspect_cards,
                    "equip_count": len(equip_ids_from_ini(values))})
        self.result["observations"]["ygocards_card301"] = {
            "bytes": len(member_bytes),
            "sha256": hashlib.sha256(member_bytes).hexdigest(),
        }
        self.set_equip_ids([])
        self.query({"cmd": "card_share", "op": "import", "path": str(cards)})
        self.check("ygocards_roundtrip_621", same_ids(self.equip_ids(), monsters),
                   len(self.equip_ids()))
        self.check_sentinels("ygocards_roundtrip_preserves_unrelated_fields")

        self.query({"cmd": "mod_package", "export": str(mods)})
        inspect_mod = self.query({"cmd": "mod_package", "inspect": str(mods)})
        with zipfile.ZipFile(mods) as archive:
            member_bytes = archive.read(f"cards/{EQUIP}/card.ini")
            member = member_bytes.decode("utf-8")
        values = parse_ini(member)
        self.check("ygomods_contains_621_and_sentinels",
                   equip_ids_from_ini(values) == monsters and
                   all(values.get(k) == v for k, v in SENTINELS.items()),
                   {"inspect": inspect_mod,
                    "equip_count": len(equip_ids_from_ini(values))})
        self.result["observations"]["ygomods_card301"] = {
            "bytes": len(member_bytes),
            "sha256": hashlib.sha256(member_bytes).hexdigest(),
        }
        self.set_equip_ids(two)
        self.query({"cmd": "mod_package", "import": str(mods)})
        self.check("ygomods_roundtrip_621", same_ids(self.equip_ids(), monsters),
                   len(self.equip_ids()))
        self.check_sentinels("ygomods_roundtrip_preserves_unrelated_fields")
        self.result["artifacts"][cards.name] = file_info(cards)
        self.result["artifacts"][mods.name] = file_info(mods)

    def restore_stock_action(self, stock, recipe_sha256):
        """The one Restore stock action must cover fusions and Equip lists."""
        self.set_equip_ids([])
        self.query({"cmd": "fusion_manager", "a": 1, "b": 2,
                    "result": 3})
        before = self.fusion_state()
        self.check("restore_stock_fixture_has_both_override_kinds",
                   before.get("edits", 0) > 0 and not self.equip_ids(),
                   {"edits": before.get("edits"),
                    "equip_ids": self.equip_ids()})

        self.query({"cmd": "fusion_manager", "confirm": 1})
        armed = self.query({"cmd": "fusion_manager"})
        self.check("restore_stock_combined_modal_armed",
                   armed.get("dialog") == 1, armed.get("dialog"))
        self.picker_shot("restore-stock-confirmation.ppm")
        self.query({"cmd": "fusion_manager", "confirm": 0})
        cancelled = self.fusion_state()
        self.check("restore_stock_combined_cancel_preserves_both",
                   cancelled.get("edits", 0) > 0 and not self.equip_ids(),
                   {"edits": cancelled.get("edits"),
                    "equip_ids": self.equip_ids()})

        self.query({"cmd": "fusion_manager", "confirm": 1})
        self.query({"cmd": "fusion_manager", "confirm": 2})
        restored = self.fusion_state()
        restored_equips = self.equip_ids()
        self.check("restore_stock_combined_restores_equip_fusions",
                   same_ids(restored_equips, stock),
                   {"expected": len(stock), "actual": len(restored_equips)})
        self.check("restore_stock_combined_restores_fusions",
                   restored.get("edits") == 0 and not restored.get("cleared"),
                   {"edits": restored.get("edits"),
                    "cleared": restored.get("cleared")})
        restored_recipes = self.args.output / "fusion-recipes-after-restore-stock.tsv"
        self.query({"cmd": "fusion_manager", "export": str(restored_recipes)})
        self.check("restore_stock_combined_exact_fusion_table",
                   digest(restored_recipes) == recipe_sha256,
                   {"stock_sha256": recipe_sha256,
                    "restored_sha256": digest(restored_recipes)})
        self.check_sentinels("restore_stock_preserves_unrelated_card_fields")

    def clear_and_restart(self, recipes, recipe_sha256):
        before = self.fusion_state()
        self.query({"cmd": "fusion_manager", "confirm_equips": 1})
        armed = self.query({"cmd": "fusion_manager"})
        self.check("clear_every_equip_modal_armed",
                   armed.get("dialog") == 4 and
                   armed.get("equip_links") == before.get("equip_links"), armed)
        self.picker_shot("clear-equips-confirmation.ppm")
        self.query({"cmd": "fusion_manager", "confirm_equips": 0})
        cancelled = self.query({"cmd": "fusion_manager"})
        self.check("clear_every_equip_cancelled",
                   cancelled.get("equip_links") == before.get("equip_links"),
                   cancelled.get("equip_links"))
        self.query({"cmd": "fusion_manager", "confirm_equips": 1})
        self.query({"cmd": "fusion_manager", "confirm_equips": 2})
        cleared = self.query({"cmd": "fusion_manager"})
        self.check("clear_every_equip_confirmed",
                   cleared.get("equip_cards") == 0 and
                   cleared.get("equip_links") == 0, cleared)
        self.check("clear_preserves_fusion_recipes",
                   cleared.get("recipes") == recipes,
                   {"before": recipes, "after": cleared.get("recipes")})
        after_clear = self.args.output / "fusion-recipes-after-clear.tsv"
        self.query({"cmd": "fusion_manager", "export": str(after_clear)})
        self.check("clear_preserves_exact_fusion_recipe_table",
                   digest(after_clear) == recipe_sha256,
                   {"before_sha256": recipe_sha256,
                    "after_sha256": digest(after_clear)})
        self.check_sentinels("clear_preserves_unrelated_fields")
        db = self.query({"cmd": "fusion_db"})
        listing = self.query({"cmd": "fusion_list"})
        self.result["observations"]["empty_hint_before_duel"] = {
            "fusion_db_ready": db.get("ready"),
            "fusion_list_ready": listing.get("ready"),
            "note": "duel tables are intentionally absent outside a duel",
        }

        self.stop()
        self.launch()
        restarted = self.fusion_state(open_window=True)
        self.check("clear_survives_restart",
                   restarted.get("equip_cards") == 0 and
                   restarted.get("equip_links") == 0,
                   {"equip_cards": restarted.get("equip_cards"),
                    "equip_links": restarted.get("equip_links")})
        self.check("fusion_recipes_survive_restart",
                   restarted.get("recipes") == recipes,
                   {"before": recipes, "after": restarted.get("recipes")})
        after_restart = self.args.output / "fusion-recipes-after-restart.tsv"
        self.query({"cmd": "fusion_manager", "export": str(after_restart)})
        self.check("exact_fusion_recipe_table_survives_restart",
                   digest(after_restart) == recipe_sha256,
                   {"before_sha256": recipe_sha256,
                    "after_sha256": digest(after_restart)})
        self.check_sentinels("unrelated_fields_survive_restart")
        self.query({"cmd": "fusion_manager", "open": 0})
        loaded = self.query({"cmd": "savestate", "op": "load",
                             "slot": self.args.duel_slot})
        self.check("duel_fixture_loads", loaded.get("ok"), loaded)
        time.sleep(2.0)
        db = self.query({"cmd": "fusion_db"})
        if db.get("ready") != 1:
            reason = (
                "the authorized historical slot does not contain initialized "
                "in-duel fusion tables; use the existing live equip-resolution "
                "artifact for eligibility and this harness for restart/roundtrip"
            )
            self.skip("empty_fusion_hint_ready_after_restart", reason)
            self.result["limitations"]["duel_fixture_fusion_tables"] = reason
            self.result["observations"]["duel_fixture_after_restart"] = db
            return
        self.check("empty_duel_tables_have_no_equip_members",
                   db.get("equip_groups") == 0 and db.get("equip_members") == 0,
                   db)
        listing = self.query({"cmd": "fusion_list"})
        no_equip = self.query({"cmd": "fusion_try", "a": EQUIP, "b": 1})
        self.check("empty_fusion_hint_ready_after_restart",
                   db.get("ready") == 1 and db.get("equip_groups") == 0 and
                   db.get("equip_members") == 0 and
                   listing.get("ready") == 1 and
                   no_equip.get("ready") == 1 and
                   no_equip.get("result") == 0 and
                   no_equip.get("kind") == 0,
                   {"fusion_db": db, "fusion_list": listing,
                    "fusion_try_301_1": no_equip})

    def old_fixture(self):
        inspected = self.query({"cmd": "mod_package",
                                "inspect": str(self.args.old_fixture)})
        imported = self.query({"cmd": "mod_package",
                               "import": str(self.args.old_fixture)})
        self.check("old_fixture_inspects_and_imports",
                   inspected.get("ok") and imported.get("ok"),
                   {"inspect": inspected.get("msg"),
                    "import": imported.get("msg")})
        with zipfile.ZipFile(self.args.old_fixture) as archive:
            expected = parse_ini(
                archive.read(f"cards/{EQUIP}/card.ini").decode("utf-8")
            )
        actual = self.card_values()
        fields = ("price", "password", "color", "name_color", "equips")
        self.check("old_fixture_card301_fields_import",
                   all(actual.get(key) == expected.get(key) for key in fields),
                   {key: {"expected": expected.get(key),
                          "actual": actual.get(key)} for key in fields})
        expected_equips = equip_ids_from_ini(expected)
        self.check("old_fixture_card301_equips_are_live",
                   same_ids(self.equip_ids(), expected_equips),
                   {"expected": len(expected_equips),
                    "actual": len(self.equip_ids())})

    def run(self):
        self.launch()
        self.seed_unrelated_fields()
        recipes, recipe_sha256, monsters, two, stock = self.stock_and_api()
        self.picker_actions(monsters, two)
        self.package_roundtrips(monsters, two)
        self.restore_stock_action(stock, recipe_sha256)
        self.clear_and_restart(recipes, recipe_sha256)
        self.old_fixture()

    def finish(self, error=None):
        if error is not None:
            self.result["error"] = repr(error)
        try:
            self.stop()
        finally:
            self.transcript.close()
        self.result["finished_utc"] = utc_now()
        self.result["status"] = "pass" if error is None and not self.failures else "fail"
        self.result["failures"] = self.failures
        portable_artifacts = {
            "portable/disc_verified.cfg": self.args.memcard_dir /
            "disc_verified.cfg",
            "portable/cards/301/card.ini": self.card_path,
        }
        for name, path in portable_artifacts.items():
            if path is not None and path.is_file():
                self.result["artifacts"][name] = file_info(path)
        for path in sorted(self.args.output.iterdir()):
            if path.is_file() and path.name != "results.json":
                self.result["artifacts"][path.name] = file_info(path)
        result_path = self.args.output / "results.json"
        result_path.write_text(json.dumps(self.result, indent=2, sort_keys=True)
                               + "\n", encoding="utf-8")
        return result_path


def arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--disc", required=True, type=Path,
                        help="explicit USA .cue path")
    parser.add_argument("--duel-state", required=True, type=Path,
                        help="authorized OpenBIOS in-duel .pst fixture")
    parser.add_argument("--duel-slot", required=True, type=int,
                        help="slot encoded by the supplied in-duel state")
    parser.add_argument("--memcard-dir", required=True, type=Path,
                        help="must not exist; the harness creates this portable profile")
    parser.add_argument("--output", required=True, type=Path,
                        help="must not exist; machine evidence is written here")
    parser.add_argument("--old-fixture", type=Path,
                        default=ROOT / "tools" / "fixtures" /
                        "full-coverage-2026-09-07.ygomods")
    parser.add_argument("--renderer", choices=("software", "opengl", "vulkan"),
                        default="software")
    parser.add_argument("--port", type=int,
                        help="specific verified-free port (default: allocate one)")
    args = parser.parse_args()
    args.executable = args.executable.resolve(strict=True)
    args.disc = args.disc.resolve(strict=True)
    args.duel_state = args.duel_state.resolve(strict=True)
    args.old_fixture = args.old_fixture.resolve(strict=True)
    args.memcard_dir = args.memcard_dir.resolve()
    args.output = args.output.resolve()
    if not args.executable.is_file() or not os.access(args.executable, os.X_OK):
        parser.error("--executable must be an executable regular file")
    if not args.disc.is_file() or args.disc.suffix.lower() != ".cue":
        parser.error("--disc must be an explicit .cue file")
    if not args.duel_state.is_file() or args.duel_state.suffix.lower() != ".pst":
        parser.error("--duel-state must be an authorized .pst fixture")
    if not 0 <= args.duel_slot <= 99:
        parser.error("--duel-slot must be in the range 0..99")
    state_slot = f"slot{args.duel_slot:02d}"
    if "slot" in args.duel_state.stem and state_slot not in args.duel_state.stem:
        parser.error("--duel-slot does not match the supplied state filename")
    if not args.old_fixture.is_file():
        parser.error("--old-fixture must be a regular file")
    if args.port is not None and not 1 <= args.port <= 65535:
        parser.error("--port must be in the range 1..65535")
    if args.memcard_dir.exists():
        parser.error("--memcard-dir must be a never-used, nonexistent path")
    if args.output.exists():
        parser.error("--output must be a nonexistent path")
    if args.memcard_dir == args.output:
        parser.error("--memcard-dir and --output must differ")
    args.memcard_dir.mkdir(parents=True)
    args.output.mkdir(parents=True)
    return args


def main():
    args = arguments()
    regression = Regression(args)
    error = None
    try:
        regression.run()
    except Exception as exc:  # preserve partial machine evidence before failing
        error = exc
    result = regression.finish(error)
    print(result)
    if error is not None:
        raise error
    return 1 if regression.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
