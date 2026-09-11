#!/usr/bin/env python3
"""Repeatable live regression for Card Shop -> Sell Extra Cards.

This is intentionally an end-to-end test, not a model of the implementation.
It starts the current debug executable with a copied, explicitly authorized FM
memory card, drives the public debug protocol and (unless --api-only is used)
drives the real Card Shop and SAVE screens with controller input.  Every run
uses two new player-data directories below a new /tmp evidence root.

The source memory card is never opened by the game.  Only its scratch copy is
mutated.  On exit, each exact process started here receives quit_graceful and
is waited by PID; terminate/kill are bounded last resorts and are recorded.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
import traceback
from typing import Any

from PIL import Image, ImageChops, ImageStat


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client  # noqa: E402

CARD_COUNT = 722
DECK_SLOTS = 40
LIVE_BASE = 0x801D0200
MIRROR_BASE = 0x801D3200
DECK_OFFSET = 0
TRUNK_OFFSET = 0x50
CHIPS_OFFSET = 0x5E0
STARCHIP_CAP = 999_999
BUTTONS = {
    "start": 0x0008,
    "up": 0x0010,
    "right": 0x0020,
    "down": 0x0040,
    "left": 0x0080,
    "triangle": 0x1000,
    "circle": 0x2000,
    "cross": 0x4000,
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def deck_bytes(cards: list[int]) -> bytes:
    if len(cards) != DECK_SLOTS or any(not 1 <= card <= CARD_COUNT for card in cards):
        raise ValueError("deck must contain exactly 40 card IDs in 1..722")
    return b"".join(struct.pack("<H", card) for card in cards)


def deterministic_deck(counts: dict[int, int]) -> list[int]:
    deck: list[int] = []
    for card, count in sorted(counts.items()):
        if count < 0 or count > 3:
            raise ValueError(f"invalid requested deck count {card}={count}")
        deck.extend([card] * count)
    used = set(counts)
    for card in range(1, CARD_COUNT + 1):
        if len(deck) == DECK_SLOTS:
            break
        if card not in used:
            deck.append(card)
    if len(deck) != DECK_SLOTS:
        raise ValueError("could not construct a 40-card deck")
    return deck


def allocate_port() -> int:
    # Keeping the bound socket until immediately before Popen minimizes the
    # allocation race.  The chosen port is probed a second time by Runner.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


class Evidence:
    def __init__(self, path: Path, provenance: dict[str, Any]):
        self.path = path
        self.data: dict[str, Any] = {
            "feature": "sell-extra-cards",
            "schema": 1,
            "status": "running",
            "provenance": provenance,
            "checks": [],
            "cases": {},
            "processes": [],
            "limitations": [],
        }
        self.flush()

    def flush(self) -> None:
        self.path.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n")

    def check(self, label: str, condition: bool, detail: Any = None) -> None:
        row = {"label": label, "pass": bool(condition)}
        if detail is not None:
            row["detail"] = detail
        self.data["checks"].append(row)
        self.flush()
        if not condition:
            raise AssertionError(f"{label}: {detail!r}")


class Runner:
    def __init__(self, exe: Path, disc: Path, player: Path, evidence: Evidence,
                 label: str, shots: Path):
        self.exe = exe
        self.disc = disc
        self.player = player
        self.evidence = evidence
        self.label = label
        self.shots = shots
        self.port = allocate_port()
        self.proc: subprocess.Popen[str] | None = None
        self.command_log = None
        self.runtime_log = None
        self.connected = False
        self.stopped = False
        self.process_row: dict[str, Any] = {}

    def start(self) -> None:
        # Verify the selected debug port is still free directly before launch.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", self.port))
        command = [
            str(self.exe), "--no-launcher", "--renderer", "software",
            "--memcard-dir", str(self.player), "--debug-port", str(self.port),
            "--disc", str(self.disc),
        ]
        env = dict(os.environ)
        env.pop("APPIMAGE", None)
        env.pop("APPDIR", None)
        env["PSX_PORTABLE"] = "1"
        self.command_log = (self.evidence.path.parent / f"{self.label}-commands.jsonl").open("w")
        self.runtime_log = (self.evidence.path.parent / f"{self.label}-runtime.log").open("w")
        self.proc = subprocess.Popen(
            command, cwd=self.exe.parent, env=env, text=True,
            stdout=self.runtime_log, stderr=subprocess.STDOUT,
        )
        self.process_row = {
            "label": self.label,
            "pid": self.proc.pid,
            "debug_port": self.port,
            "command": command,
            "cwd": str(self.exe.parent),
            "PSX_PORTABLE": "1",
            "player_data": str(self.player),
            "graceful_requested": False,
        }
        self.evidence.data["processes"].append(self.process_row)
        self.evidence.flush()
        print(json.dumps({"started": self.label, "pid": self.proc.pid,
                          "port": self.port, "player": str(self.player)}), flush=True)

        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"{self.label} exited during boot: {self.proc.returncode}")
            try:
                self.query("ping")
                self.connected = True
                return
            except (OSError, ConnectionError):
                time.sleep(0.25)
        raise TimeoutError(f"{self.label} debug server did not answer")

    def query(self, cmd: str, expect_ok: bool = True, **kwargs: Any) -> dict[str, Any]:
        request = {"cmd": cmd, **kwargs}
        response = debug_client.query("127.0.0.1", self.port, request)
        if self.command_log:
            self.command_log.write(json.dumps({"request": request, "response": response}) + "\n")
            self.command_log.flush()
        if expect_ok and not response.get("ok"):
            raise AssertionError({"request": request, "response": response})
        return response

    def read(self, address: int, length: int) -> bytes:
        response = self.query("read_ram", addr=f"{address:08X}", len=length)
        value = response.get("data") or response.get("hex") or ""
        data = bytes.fromhex(value)
        if len(data) != length:
            raise AssertionError(f"short read at {address:08X}: {len(data)} != {length}")
        return data

    def write(self, address: int, data: bytes) -> None:
        self.query("write_mem", addr=f"{address:08X}", hex=data.hex())

    def press(self, name: str, frames: int = 12, settle: float = 1.0) -> None:
        self.query("press", buttons=0xFFFF & ~BUTTONS[name], frames=frames)
        time.sleep(0.25)
        self.query("clear_input")
        time.sleep(settle)

    def raw_shot_small(self, name: str) -> Image.Image:
        path = self.shots / f"{self.label}-{name}.png"
        self.query("screenshot", path=str(path))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if path.exists() and path.stat().st_size:
                image = Image.open(path).convert("L").resize((80, 60))
                image.load()
                return image
            time.sleep(0.05)
        raise TimeoutError(f"screenshot not written: {path}")

    def composed_shot(self, name: str) -> dict[str, Any]:
        path = self.shots / f"{self.label}-{name}.png"
        prior = self.query("present_shot_seq")["seq"]
        self.query("present_shot", path=str(path))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            state = self.query("present_shot_seq")
            if state["seq"] > prior:
                if not state.get("wrote") or not path.exists():
                    raise AssertionError({"capture": str(path), "state": state})
                return {"path": str(path), "sha256": sha256_file(path)}
            time.sleep(0.05)
        raise TimeoutError(f"composed screenshot timed out: {path}")

    def stop(self) -> bool:
        if not self.proc or self.stopped:
            return bool(self.process_row.get("graceful_clean"))
        errors = []
        if self.proc.poll() is None and self.connected:
            try:
                self.query("quit_graceful")
                self.process_row["graceful_requested"] = True
            except Exception as exc:  # still drain this exact owned PID
                errors.append(f"quit_graceful: {exc}")
        try:
            returncode = self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            errors.append("graceful wait timed out; sent SIGTERM to exact PID")
            self.proc.terminate()
            try:
                returncode = self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                errors.append("SIGTERM wait timed out; sent SIGKILL to exact PID")
                self.proc.kill()
                returncode = self.proc.wait(timeout=10)
        self.process_row["returncode"] = returncode
        self.process_row["stop_errors"] = errors
        self.process_row["graceful_clean"] = bool(
            self.process_row["graceful_requested"] and not errors
        )
        self.process_row["stopped"] = True
        self.stopped = True
        self.evidence.flush()
        if self.command_log:
            self.command_log.close()
        if self.runtime_log:
            self.runtime_log.close()
        print(json.dumps({"stopped": self.label, "pid": self.proc.pid,
                          "returncode": returncode, "errors": errors}), flush=True)
        return bool(self.process_row["graceful_clean"])


def inventory(runner: Runner) -> dict[str, Any]:
    live_deck_raw = runner.read(LIVE_BASE + DECK_OFFSET, DECK_SLOTS * 2)
    mirror_deck_raw = runner.read(MIRROR_BASE + DECK_OFFSET, DECK_SLOTS * 2)
    live_trunk = runner.read(LIVE_BASE + TRUNK_OFFSET, CARD_COUNT)
    mirror_trunk = runner.read(MIRROR_BASE + TRUNK_OFFSET, CARD_COUNT)
    live_chips_raw = runner.read(LIVE_BASE + CHIPS_OFFSET, 4)
    mirror_chips_raw = runner.read(MIRROR_BASE + CHIPS_OFFSET, 4)
    deck = list(struct.unpack("<40H", live_deck_raw))
    mirror_deck = list(struct.unpack("<40H", mirror_deck_raw))
    live_chips = struct.unpack("<I", live_chips_raw)[0]
    mirror_chips = struct.unpack("<I", mirror_chips_raw)[0]
    combined = live_deck_raw + live_trunk + live_chips_raw
    full = combined + mirror_deck_raw + mirror_trunk + mirror_chips_raw
    return {
        "deck": deck,
        "deck_sha256": sha256_bytes(live_deck_raw),
        "trunk": list(live_trunk),
        "trunk_sha256": sha256_bytes(live_trunk),
        "chips": live_chips,
        "combined_sha256": sha256_bytes(combined),
        "live_and_mirror_sha256": sha256_bytes(full),
        "mirror_deck": mirror_deck,
        "mirror_deck_sha256": sha256_bytes(mirror_deck_raw),
        "mirror_trunk": list(mirror_trunk),
        "mirror_trunk_sha256": sha256_bytes(mirror_trunk),
        "mirror_chips": mirror_chips,
        "live_mirror_equal": (
            live_deck_raw == mirror_deck_raw and live_trunk == mirror_trunk
            and live_chips_raw == mirror_chips_raw
        ),
    }


def install_inventory(runner: Runner, deck: list[int], trunk: bytes, chips: int) -> None:
    if len(trunk) != CARD_COUNT or not 0 <= chips <= STARCHIP_CAP:
        raise ValueError("invalid inventory fixture")
    raw_deck = deck_bytes(deck)
    for base in (LIVE_BASE, MIRROR_BASE):
        runner.write(base + DECK_OFFSET, raw_deck)
        runner.write(base + TRUNK_OFFSET, trunk)
        runner.write(base + CHIPS_OFFSET, u32(chips))
    runner.query("card_shop_sell", op="cancel")


def sale_state(runner: Runner, operation: str, expect_ok: bool = True) -> dict[str, Any]:
    return runner.query("card_shop_sell", expect_ok=expect_ok, op=operation)


def entry_map(state: dict[str, Any]) -> dict[int, dict[str, Any]]:
    return {int(entry["id"]): entry for entry in state["entries"]}


def route_loaded_menu(runner: Runner) -> float:
    runner.query("game_speed", mult=8)
    reference = Image.open(ROOT / "tools" / "refs" / "title_80x60.png").convert("L")
    deadline = time.monotonic() + 240
    last_diff = None
    probe = 0
    while time.monotonic() < deadline:
        if runner.proc and runner.proc.poll() is not None:
            raise RuntimeError("runtime exited while waiting for title")
        try:
            live = runner.raw_shot_small(f"title-probe-{probe}")
        except AssertionError as exc:
            # The debug socket comes up before the BIOS enables the PSX
            # display.  That is a normal boot state, not a failed render.
            if "display disabled" not in str(exc):
                raise
            time.sleep(0.25)
            continue
        probe += 1
        last_diff = ImageStat.Stat(ImageChops.difference(live, reference)).mean[0]
        if last_diff <= 18:
            break
        time.sleep(0.5)
    else:
        raise TimeoutError(f"title screen not found; last difference {last_diff}")
    runner.query("game_speed", mult=1)
    runner.press("start", 60, 3.0)
    runner.composed_shot("route-01-main-menu")
    runner.press("down", 20, 1.5)     # NEW GAME -> LOAD
    runner.composed_shot("route-02-load-selected")
    runner.press("cross", 20, 2.0)    # LOAD -> memory-card prompt
    runner.composed_shot("route-03-load-confirm")
    runner.press("cross", 20, 4.0)    # YES
    runner.composed_shot("route-04-loading")
    runner.press("cross", 20, 3.0)    # dismiss LOAD COMPLETE
    runner.composed_shot("route-05-loaded-menu")
    return float(last_diff)


def controlled_fixture() -> tuple[list[int], bytes, dict[int, dict[str, int]]]:
    # The seven rows deliberately cover deck/trunk splits around total=3,
    # maximum trunk count, and the scratch zero-price card 5.
    rows = {
        4: {"deck": 0, "trunk": 3, "sell": 0, "keep": 3},
        5: {"deck": 0, "trunk": 4, "sell": 1, "keep": 3},
        104: {"deck": 1, "trunk": 2, "sell": 0, "keep": 3},
        114: {"deck": 1, "trunk": 3, "sell": 1, "keep": 3},
        195: {"deck": 2, "trunk": 1, "sell": 0, "keep": 3},
        232: {"deck": 2, "trunk": 2, "sell": 1, "keep": 3},
        229: {"deck": 3, "trunk": 255, "sell": 255, "keep": 3},
    }
    deck = deterministic_deck({card: row["deck"] for card, row in rows.items()})
    trunk = bytearray(CARD_COUNT)
    for card, row in rows.items():
        trunk[card - 1] = row["trunk"]
    return deck, bytes(trunk), rows


def exercise_backend(runner: Runner, evidence: Evidence, player: Path) -> None:
    deck, trunk, rows = controlled_fixture()
    install_inventory(runner, deck, trunk, 100)
    before = inventory(runner)
    evidence.check("controlled fixture live/mirror byte exact", before["live_mirror_equal"])

    preview = sale_state(runner, "preview")
    after_preview = inventory(runner)
    entries = entry_map(preview)
    expected_sold = {card for card, row in rows.items() if row["sell"]}
    evidence.check("preview is read-only", before["live_and_mirror_sha256"] == after_preview["live_and_mirror_sha256"])
    evidence.check("preview lists only cards above three total copies",
                   set(entries) == expected_sold, sorted(entries))
    for card, row in rows.items():
        if row["sell"]:
            entry = entries[card]
            evidence.check(f"card {card} split/sell/keep",
                           (entry["deck"], entry["trunk"], entry["sell"], entry["keep"])
                           == (row["deck"], row["trunk"], row["sell"], row["keep"]), entry)
    evidence.check("zero-value extra is previewed", entries[5]["value"] == 0, entries[5])
    evidence.check("preview arithmetic is internally exact",
                   preview["gross"] == sum(e["subtotal"] for e in entries.values())
                   and all(e["subtotal"] == e["sell"] * e["value"] for e in entries.values()), preview)

    cancelled = sale_state(runner, "cancel")
    after_cancel = inventory(runner)
    evidence.check("cancel makes preview inactive", not cancelled["active"], cancelled)
    evidence.check("cancel is byte-exact", before["live_and_mirror_sha256"] == after_cancel["live_and_mirror_sha256"])

    # A change anywhere in the inventory invalidates the whole preview, even if
    # it is a card that is not itself being sold.
    sale_state(runner, "preview")
    stale = bytearray(trunk)
    stale[721] = 1
    runner.write(LIVE_BASE + TRUNK_OFFSET, bytes(stale))
    runner.write(MIRROR_BASE + TRUNK_OFFSET, bytes(stale))
    stale_before = inventory(runner)
    stale_reply = sale_state(runner, "confirm", expect_ok=False)
    stale_after = inventory(runner)
    evidence.check("stale inventory confirm is rejected", not stale_reply.get("ok"), stale_reply)
    evidence.check("stale inventory rejection does not mutate",
                   stale_before["live_and_mirror_sha256"] == stale_after["live_and_mirror_sha256"])
    evidence.check("stale inventory explains review is required",
                   "REVIEW" in stale_reply.get("msg", "").upper(), stale_reply.get("msg"))

    # Price is part of preview identity.  Change a scratch-only card override
    # after preview and hot reload it; no deck/trunk/chip byte may be committed.
    price_card = 24
    price_deck = deterministic_deck({price_card: 0})
    price_trunk = bytearray(CARD_COUNT)
    price_trunk[price_card - 1] = 4
    install_inventory(runner, price_deck, bytes(price_trunk), 321)
    price_preview = sale_state(runner, "preview")
    old_price = entry_map(price_preview)[price_card]["value"]
    new_price = old_price - 1 if old_price == 999_999 else old_price + 1
    override = player / "cards" / str(price_card) / "card.ini"
    override.parent.mkdir(parents=True, exist_ok=True)
    override.write_text(f"price = {new_price}\n")
    runner.query("card_packs_reload", card=price_card)
    price_before = inventory(runner)
    price_reply = sale_state(runner, "confirm", expect_ok=False)
    price_after = inventory(runner)
    evidence.check("stale price confirm is rejected", not price_reply.get("ok"), price_reply)
    evidence.check("stale price rejection does not mutate",
                   price_before["live_and_mirror_sha256"] == price_after["live_and_mirror_sha256"])
    evidence.check("stale price explains review is required",
                   "REVIEW" in price_reply.get("msg", "").upper(), price_reply.get("msg"))
    override.unlink()
    override.parent.rmdir()
    runner.query("card_packs_reload", card=price_card)

    # Successful controlled commit, including removal of a zero-value extra.
    install_inventory(runner, deck, trunk, 100)
    confirmed_preview = sale_state(runner, "preview")
    confirmed_entries = entry_map(confirmed_preview)
    confirmed = sale_state(runner, "confirm")
    after_confirm = inventory(runner)
    expected_trunk = bytearray(trunk)
    for card, row in rows.items():
        expected_trunk[card - 1] = min(row["trunk"], max(0, 3 - row["deck"]))
    expected_after = min(STARCHIP_CAP, 100 + confirmed_preview["gross"])
    evidence.check("confirm reports sale", "SOLD" in confirmed.get("msg", "").upper(), confirmed)
    evidence.check("confirm never changes the active deck", after_confirm["deck"] == deck)
    evidence.check("confirm writes exact retained trunk", after_confirm["trunk"] == list(expected_trunk))
    evidence.check("confirm writes cap-safe starchips", after_confirm["chips"] == expected_after,
                   {"actual": after_confirm["chips"], "expected": expected_after})
    evidence.check("confirm keeps live and mirror identical", after_confirm["live_mirror_equal"])
    evidence.check("zero-value extras are actually removed", after_confirm["trunk"][4] == 3)
    empty = sale_state(runner, "preview")
    no_extra_before = inventory(runner)
    empty_confirm = sale_state(runner, "confirm", expect_ok=False)
    no_extra_after = inventory(runner)
    evidence.check("no-extras preview is an empty valid preview",
                   empty.get("ok") and empty["types"] == 0 and empty["copies"] == 0, empty)
    evidence.check("no-extras confirm refuses and is read-only",
                   not empty_confirm.get("ok")
                   and no_extra_before["live_and_mirror_sha256"] == no_extra_after["live_and_mirror_sha256"],
                   empty_confirm)

    # Cap boundary uses the same sale but only credits the remaining nine.
    install_inventory(runner, deck, trunk, STARCHIP_CAP - 9)
    cap_preview = sale_state(runner, "preview")
    evidence.check("cap preview credits only available room",
                   cap_preview["credit"] == 9 and cap_preview["after"] == STARCHIP_CAP,
                   cap_preview)
    sale_state(runner, "confirm")
    cap_after = inventory(runner)
    evidence.check("cap confirm cannot overflow", cap_after["chips"] == STARCHIP_CAP)

    # Every card has a maximum trunk.  The 40 unique deck cards retain two
    # trunk copies and sell 253; the other 682 retain three and sell 252.
    all_deck = list(range(1, DECK_SLOTS + 1))
    all_trunk = bytes([255]) * CARD_COUNT
    install_inventory(runner, all_deck, all_trunk, 0)
    all_before = inventory(runner)
    all_preview = sale_state(runner, "preview")
    all_entries = entry_map(all_preview)
    expected_copies = DECK_SLOTS * 253 + (CARD_COUNT - DECK_SLOTS) * 252
    evidence.check("all-722 preview contains every card", all_preview["types"] == CARD_COUNT,
                   all_preview["types"])
    evidence.check("all-722 copy count is exact", all_preview["copies"] == expected_copies,
                   all_preview["copies"])
    evidence.check("all-722 gross uses wider-than-32-bit arithmetic",
                   all_preview["gross"] > 0xFFFFFFFF, all_preview["gross"])
    evidence.check("all-722 entry subtotals sum to gross",
                   sum(entry["subtotal"] for entry in all_entries.values()) == all_preview["gross"])
    evidence.check("all-722 preview is read-only",
                   all_before["live_and_mirror_sha256"] == inventory(runner)["live_and_mirror_sha256"])
    sale_state(runner, "confirm")
    all_after = inventory(runner)
    expected_all_trunk = [2] * DECK_SLOTS + [3] * (CARD_COUNT - DECK_SLOTS)
    evidence.check("all-722 commit preserves deck", all_after["deck"] == all_deck)
    evidence.check("all-722 commit leaves three total of every card",
                   all_after["trunk"] == expected_all_trunk)
    evidence.check("all-722 commit mirrors every byte", all_after["live_mirror_equal"])
    evidence.check("all-722 commit caps starchips", all_after["chips"] == STARCHIP_CAP)

    evidence.data["cases"]["controlled"] = {
        "fixture": {str(card): row for card, row in rows.items()},
        "before": before,
        "preview": preview,
        "entries": confirmed_entries,
        "after_confirm": after_confirm,
        "no_extras": empty,
    }
    evidence.data["cases"]["stale_inventory"] = stale_reply
    evidence.data["cases"]["stale_price"] = {
        "card": price_card, "old": old_price, "new": new_price, "reply": price_reply,
    }
    evidence.data["cases"]["cap"] = {"preview": cap_preview, "after": cap_after}
    evidence.data["cases"]["all_722"] = {
        "before": all_before,
        "preview": all_preview,
        "after": all_after,
    }
    evidence.flush()


def set_actual_shop_fixture(runner: Runner) -> tuple[list[int], bytes, int]:
    deck, trunk, _ = controlled_fixture()
    chips = 77
    install_inventory(runner, deck, trunk, chips)
    return deck, trunk, chips


def exercise_actual_shop_and_save(runner: Runner, evidence: Evidence,
                                  card_path: Path) -> dict[str, Any]:
    expected_deck, _trunk, before_chips = set_actual_shop_fixture(runner)
    expected_preview = sale_state(runner, "preview")
    sale_state(runner, "cancel")
    before = inventory(runner)

    # Loaded menu begins on CAMPAIGN.  Campaign opens the shopkeeper menu;
    # CARD SHOP is its next row.  The greeting and shop question are native
    # modal text and each require Cross before Triangle opens Sell Extras.
    runner.press("cross", 20, 6.0)
    runner.composed_shot("shop-route-01-campaign")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("shop-route-02-greeting-dismissed")
    runner.press("down", 20, 1.5)
    runner.composed_shot("shop-route-03-card-shop-selected")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("shop-route-04-shop-question")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("shop-route-05-pack-panel")
    shop_state = runner.query("card_shop")
    evidence.check("actual Card Shop is open", bool(shop_state.get("open")), shop_state)
    runner.press("triangle", 12, 1.5)
    ui_preview = sale_state(runner, "state")
    evidence.check("Triangle opens the real Sell Extras preview",
                   ui_preview["active"] and ui_preview["types"] == expected_preview["types"], ui_preview)
    preview_shot = runner.composed_shot("shop-preview")

    # First visit cancels with the real Circle handler and must be byte-exact.
    runner.press("circle", 12, 1.0)
    cancelled = sale_state(runner, "state")
    evidence.check("shop Circle cancels preview", not cancelled["active"], cancelled)
    evidence.check("shop Circle is byte-exact",
                   before["live_and_mirror_sha256"] == inventory(runner)["live_and_mirror_sha256"])
    runner.press("triangle", 12, 1.0)
    runner.press("cross", 12, 1.5)
    after = inventory(runner)
    evidence.check("shop Cross preserves deck", after["deck"] == expected_deck)
    evidence.check("shop Cross applies preview starchips",
                   after["chips"] == min(STARCHIP_CAP, before_chips + expected_preview["gross"]), after["chips"])
    sold_shot = runner.composed_shot("shop-sold")

    # Leave the pack panel and shopkeeper, select SAVE on the loaded menu, and
    # accept SAVE? plus OVERWRITE?.  The card hash must change before restart.
    prior_card_hash = sha256_file(card_path)
    runner.press("circle", 12, 2.0)
    runner.press("up", 12, 1.0)       # CARD SHOP -> SAVE
    runner.press("cross", 12, 1.5)
    runner.press("cross", 12, 2.0)
    runner.press("cross", 12, 3.0)
    runner.press("cross", 12, 2.0)
    deadline = time.monotonic() + 10
    saved_hash = sha256_file(card_path)
    while saved_hash == prior_card_hash and time.monotonic() < deadline:
        time.sleep(0.25)
        saved_hash = sha256_file(card_path)
    evidence.check("native SAVE changed scratch memory card", saved_hash != prior_card_hash,
                   {"before": prior_card_hash, "after": saved_hash})
    save_shot = runner.composed_shot("save-complete")
    return {
        "before": before,
        "expected_preview": expected_preview,
        "after": after,
        "screenshots": [preview_shot, sold_shot, save_shot],
        "memory_card_sha256_before": prior_card_hash,
        "memory_card_sha256_after": saved_hash,
    }


def exercise_restart(runner: Runner, evidence: Evidence,
                     expected: dict[str, Any]) -> dict[str, Any]:
    title_difference = route_loaded_menu(runner)
    restarted = inventory(runner)
    after = expected["after"]
    evidence.check("restart reloads sold deck byte-exact", restarted["deck"] == after["deck"])
    evidence.check("restart reloads sold trunk byte-exact", restarted["trunk"] == after["trunk"])
    evidence.check("restart reloads sold starchips", restarted["chips"] == after["chips"])
    evidence.check("restart inventory live/mirror exact", restarted["live_mirror_equal"])
    no_extras = sale_state(runner, "preview")
    evidence.check("restart Sell Extras has nothing left to sell",
                   no_extras["types"] == 0 and no_extras["copies"] == 0, no_extras)
    sale_state(runner, "cancel")

    # Re-enter the actual shop to record the visible no-extras outcome.
    runner.press("cross", 20, 6.0)
    runner.composed_shot("restart-shop-route-01-campaign")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("restart-shop-route-02-greeting-dismissed")
    runner.press("down", 20, 1.5)
    runner.composed_shot("restart-shop-route-03-card-shop-selected")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("restart-shop-route-04-shop-question")
    runner.press("cross", 20, 3.0)
    runner.composed_shot("restart-shop-route-05-pack-panel")
    runner.press("triangle", 12, 1.5)
    visible = sale_state(runner, "state")
    evidence.check("restart shop no-extras preview is visible",
                   visible["active"] and visible["types"] == 0, visible)
    shot = runner.composed_shot("restart-no-extras")
    runner.press("circle", 12, 0.5)
    return {
        "title_mean_difference": title_difference,
        "inventory": restarted,
        "preview": no_extras,
        "visible_state": visible,
        "screenshot": shot,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-dbg" / "Yu_Gi_Oh_Forbidden_Memories_Recompiled")
    parser.add_argument("--disc", type=Path,
                        default=ROOT / "disc" / "Yu-Gi-Oh! Forbidden Memories (USA).cue")
    parser.add_argument("--card", type=Path, required=True,
                        help="authorized valid FM card1.mcd; copied, never used in place")
    parser.add_argument("--output", type=Path, required=True,
                        help="new evidence directory below /tmp")
    parser.add_argument("--api-only", action="store_true",
                        help="skip real shop/SAVE/restart UI (backend qualification only)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    exe = args.exe.resolve()
    disc = args.disc.resolve()
    source_card = args.card.resolve()
    output = args.output.resolve()
    for path, label in ((exe, "--exe"), (disc, "--disc"), (source_card, "--card")):
        if not path.is_file():
            raise SystemExit(f"{label} is not a file: {path}")
    if "USA" not in disc.name.upper() or disc.suffix.lower() != ".cue":
        raise SystemExit(f"--disc must be the explicit USA cue: {disc}")
    if not output.is_relative_to(Path("/tmp")):
        raise SystemExit("--output must be a new directory below /tmp")
    output.mkdir(parents=True, exist_ok=False)
    shots = output / "screenshots"
    player = output / "player"
    restart_player = output / "restart-player"
    shots.mkdir()
    player.mkdir()
    shutil.copy2(source_card, player / "card1.mcd")
    (player / "menu_settings.ini").write_text("card_shop=1\n", encoding="utf-8")

    # This isolated override proves extras with no sale value are still removed.
    zero_override = player / "cards" / "5" / "card.ini"
    zero_override.parent.mkdir(parents=True)
    zero_override.write_text("price = 0\n")

    evidence = Evidence(output / "results.json", {
        "repository": str(ROOT),
        "title_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "executable": str(exe),
        "executable_sha256": sha256_file(exe),
        "disc": str(disc),
        "disc_sha256": sha256_file(disc),
        "source_card": str(source_card),
        "source_card_sha256": sha256_file(source_card),
        "source_card_was_copied": True,
        "output": str(output),
        "renderer": "software",
        "api_only": bool(args.api_only),
    })

    first = Runner(exe, disc, player, evidence, "initial", shots)
    restart: Runner | None = None
    try:
        first.start()
        title_difference = route_loaded_menu(first)
        evidence.data["cases"]["route"] = {"title_mean_difference": title_difference}
        exercise_backend(first, evidence, player)
        if args.api_only:
            evidence.data["limitations"].append(
                "--api-only requested: real Card Shop controller UI, SAVE, and restart were not run."
            )
            first.stop()
            evidence.check("initial process stopped through quit_graceful",
                           first.process_row["graceful_clean"], first.process_row)
        else:
            actual = exercise_actual_shop_and_save(first, evidence, player / "card1.mcd")
            evidence.data["cases"]["actual_shop_save"] = actual
            evidence.flush()
            first.stop()
            evidence.check("initial process stopped through quit_graceful",
                           first.process_row["graceful_clean"], first.process_row)

            restart_player.mkdir()
            shutil.copy2(player / "card1.mcd", restart_player / "card1.mcd")
            (restart_player / "menu_settings.ini").write_text(
                "card_shop=1\n", encoding="utf-8")
            restart = Runner(exe, disc, restart_player, evidence, "restart", shots)
            restart.start()
            restarted = exercise_restart(restart, evidence, actual)
            evidence.data["cases"]["restart"] = restarted
            restart.stop()
            evidence.check("restart process stopped through quit_graceful",
                           restart.process_row["graceful_clean"], restart.process_row)

        source_after = sha256_file(source_card)
        evidence.data["provenance"]["source_card_sha256_after"] = source_after
        evidence.check("source memory card was never mutated",
                       source_after == evidence.data["provenance"]["source_card_sha256"],
                       source_after)

        evidence.data["status"] = "pass"
        evidence.data["summary"] = {
            "checks": len(evidence.data["checks"]),
            "passed": sum(1 for row in evidence.data["checks"] if row["pass"]),
        }
        evidence.flush()
        print(json.dumps(evidence.data["summary"]), flush=True)
        return 0
    except Exception as exc:
        evidence.data["status"] = "fail"
        evidence.data["error"] = str(exc)
        evidence.data["traceback"] = traceback.format_exc()
        evidence.flush()
        traceback.print_exc()
        return 1
    finally:
        if restart is not None:
            restart.stop()
        first.stop()
        evidence.flush()


if __name__ == "__main__":
    raise SystemExit(main())
