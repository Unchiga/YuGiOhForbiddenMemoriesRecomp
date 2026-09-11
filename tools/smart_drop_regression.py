#!/usr/bin/env python3
"""Actual-duel regression for Drop Table Manager Smart Drops.

This intentionally does not call card_drops_test/card_drops_sim: those legacy
commands recursively dispatch guest code and are disabled.  Each case reloads
an authorized near-win duel state, edits only its isolated RAM, wins the duel,
and checks the real roll/award/results paths.  The source fixture is never
modified; its codegen hash is patched only in the fresh output copy.
"""

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


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import debug_client

STATE = "state_800129D8_slot00.pst"
THUMB = "state_800129D8_slot00.thumb"
OLD_CODEGEN = 0xF691F520
NEW_CODEGEN = 0xE3093403

PHASE = 0x8009B23A
FREE_FLAGS = 0x8009B365
UNLOCKS = 0x801D06F4
RESULT_PTR = 0x8009B1E8
RANK_INDEX = 0x8009B165
RNG_SEED = 0x800FE6F8
DROP_TABLE = 0x8017878C
TIER_STRIDE = 1460
TABLE_BYTES = 722 * 2
DECK = 0x801D0200
TRUNK = 0x801D0250
RECENT = 0x801D07BC
CARD_ZERO = 0x801D024F
OPPONENT = 2
DUELIST = OPPONENT - 1
STORY_CARD = 37

CROSS = 0x4000
RIGHT = 0x0020
LEFT = 0x0080


CASES = (
    {
        "name": "count-1-owned-0-tier-0",
        "count": 1, "tier": 0,
        "weights": {101: 1024, 102: 1024},
        "owned": {}, "expected_normal": 1,
        "expect_new": True,
    },
    {
        "name": "count-2-owned-1-tier-1",
        "count": 2, "tier": 1,
        "weights": {111: 1024, 112: 1024},
        "owned": {111: (0, 1)}, "expected_normal": 2,
    },
    {
        "name": "count-16-owned-2-tier-2-pagination",
        "count": 16, "tier": 2,
        "weights": {card: 128 for card in range(201, 217)},
        "owned": {card: (0, 2) for card in range(201, 217)},
        "expected_normal": 16, "pagination": True,
    },
    {
        "name": "count-99-split-deck-trunk-tier-0",
        "count": 99, "tier": 0,
        "weights": {card: (59 if card == 301 else 51)
                    for card in range(301, 341)},
        "owned": {card: (1, 1) for card in range(301, 341)},
        "expected_normal": 40, "pagination": True,
    },
    {
        "name": "owned-3-and-many-excluded-duplicate-weights",
        "count": 2, "tier": 1,
        "weights": {401: 682, 402: 683, 403: 683},
        "owned": {401: (1, 2), 402: (3, 5)},
        "expected_normal": 2, "only": {403},
    },
    {
        "name": "all-ineligible-no-fallback",
        "count": 1, "tier": 2,
        "weights": {411: 1024, 412: 1024},
        "owned": {411: (1, 2), 412: (3, 5)},
        "expected_normal": 0, "all_ineligible": True,
    },
    {
        "name": "story-every-plus-smart-normals",
        "count": 16, "tier": 0,
        "weights": {card: (144 if card == 501 else 136)
                    for card in range(501, 516)},
        "owned": {**{card: (0, 2) for card in range(501, 516)},
                  STORY_CARD: (0, 3)},
        "expected_normal": 15, "story": True, "pagination": True,
    },
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def q(port, value):
    return debug_client.query("127.0.0.1", port, value)


def wait_q(port, value, pred, timeout, label):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = q(port, value)
            if pred(last):
                return last
        except (ConnectionError, OSError, TimeoutError):
            pass
        time.sleep(1.0)
    raise RuntimeError(f"timeout waiting for {label}: {last!r}")


def capture(port, path):
    """Request a screenshot and wait for the asynchronous writer to finish."""
    reply = q(port, {"cmd": "screenshot_present", "path": str(path)})
    if not reply.get("ok"):
        raise RuntimeError(reply)
    deadline = time.monotonic() + 10.0
    previous = -1
    stable = 0
    while time.monotonic() < deadline:
        try:
            size = path.stat().st_size
        except FileNotFoundError:
            size = -1
        if size > 0 and size == previous:
            stable += 1
            if stable >= 2:
                return
        else:
            stable = 0
        previous = size
        time.sleep(0.1)
    raise RuntimeError(f"timeout waiting for screenshot: {path}")


def ram(port, addr, size):
    reply = q(port, {"cmd": "read_ram", "addr": f"{addr:08X}",
                     "len": size})
    if not reply.get("ok"):
        raise RuntimeError(reply)
    return bytes.fromhex(reply.get("hex") or reply.get("data") or "")


def write(port, addr, data):
    reply = q(port, {"cmd": "write_mem", "addr": f"{addr:08X}",
                     "hex": bytes(data).hex()})
    if not reply.get("ok"):
        raise RuntimeError(reply)


def press(port, mask, settle=1.5):
    q(port, {"cmd": "press", "buttons": 0xFFFF & ~mask,
             "frames": 12, "slot": 0})
    time.sleep(1.2)
    q(port, {"cmd": "clear_input"})
    time.sleep(settle)


def win_inputs(port, before_final=None):
    press(port, CROSS, 6.0)
    if before_final:
        before_final()
    press(port, CROSS, 6.0)


def patch_copied_state(path):
    data = bytearray(path.read_bytes())
    magic, version, bios, entry, codegen = struct.unpack_from("<5I", data, 0)
    if magic != 0x50535842:
        raise RuntimeError(f"bad savestate magic: {magic:08X}")
    if codegen == OLD_CODEGEN:
        struct.pack_into("<I", data, 16, NEW_CODEGEN)
        path.write_bytes(data)
    elif codegen != NEW_CODEGEN:
        raise RuntimeError(
            f"fixture codegen {codegen:08X}, expected {OLD_CODEGEN:08X} "
            f"or {NEW_CODEGEN:08X}")
    return {"version": version, "bios": f"{bios:08X}",
            "entry": f"{entry:08X}", "source_codegen": f"{codegen:08X}",
            "copied_codegen": f"{NEW_CODEGEN:08X}",
            "sha256": sha(path.read_bytes())}


def table_blob(weights):
    if sum(weights.values()) != 2048:
        raise AssertionError(("weight total", sum(weights.values()), weights))
    out = bytearray(TABLE_BYTES)
    for card, weight in weights.items():
        if not 1 <= card <= 722 or not 0 <= weight <= 2048:
            raise AssertionError((card, weight))
        struct.pack_into("<H", out, (card - 1) * 2, weight)
    return bytes(out)


def set_inventory(port, cards, owned):
    """Remove target cards, then install exact (deck,trunk) ownership."""
    deck = list(struct.unpack("<40H", ram(port, DECK, 80)))
    filler = next(card for card in range(722, 0, -1) if card not in cards)
    deck = [filler if card in cards else card for card in deck]
    required = []
    for card, (deck_n, trunk_n) in owned.items():
        if deck_n < 0 or trunk_n < 0:
            raise AssertionError((card, deck_n, trunk_n))
        required.extend([card] * deck_n)
    if len(required) > 40:
        raise AssertionError("deck setup exceeds 40 slots")
    for index, card in enumerate(required):
        deck[index] = card
    write(port, DECK, struct.pack("<40H", *deck))

    trunk = bytearray(ram(port, TRUNK, 722))
    for card in cards:
        trunk[card - 1] = 0
    for card, (_, trunk_n) in owned.items():
        trunk[card - 1] = trunk_n
    write(port, TRUNK, trunk)
    return deck, bytes(trunk)


def owned_totals(deck, trunk, cards):
    return {card: deck.count(card) + trunk[card - 1] for card in cards}


def set_rank_selector(port, tier):
    result = struct.unpack("<I", ram(port, RESULT_PTR, 4))[0]
    if not result:
        raise RuntimeError("null duel-result pointer")
    # Caller increments byte +56 once, then: stored <3 => tier 1;
    # otherwise byte +57 false/true => tier 0/2.
    if tier == 1:
        values = bytes((0, 0))
    elif tier == 0:
        values = bytes((2, 0))
    elif tier == 2:
        values = bytes((2, 1))
    else:
        raise AssertionError(tier)
    write(port, result + 56, values)


def set_rank_source(port, tier):
    """Set the duel-score source from which stock computes the rank bytes."""
    result = struct.unpack("<I", ram(port, RESULT_PTR, 4))[0]
    index = ram(port, RANK_INDEX, 1)[0]
    if not result or index > 15:
        raise RuntimeError(("invalid rank source", result, index))
    # score 99 -> A/S POW band, 50 -> B/C/D band, 0 -> A/S TEC band.
    score = (99, 50, 0)[tier]
    address = result + index * 4 + 44
    write(port, address, struct.pack("<I", score))
    readback = struct.unpack("<I", ram(port, address, 4))[0]
    if readback != score:
        raise AssertionError(("rank source write", address, score, readback))
    return {"index": index, "score": score,
            "address": f"0x{address:08X}", "readback": readback}


def lcg(seed, steps):
    for _ in range(steps):
        seed = (seed * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
    return seed


def lcg_back(seed, steps):
    inverse = pow(0x41C64E6D, -1, 1 << 32)
    for _ in range(steps):
        seed = ((seed - 0x3039) * inverse) & 0xFFFFFFFF
    return seed


def rng_trace(port, count, story, expected_normal, raw_path):
    fn = q(port, {"cmd": "fntrace_dump", "target_lo": "0x80021810",
                  "target_hi": "0x80021811", "count": 256})
    fn_entries = sorted(fn.get("entries", []), key=lambda row: row["seq"])
    expected_function_calls = expected_normal + 1 if expected_normal else 1
    if len(fn_entries) != expected_function_calls:
        raw_path.write_text(json.dumps({"fntrace": fn}, indent=2) + "\n")
        raise AssertionError(("drop-roll function count", len(fn_entries),
                              expected_function_calls, fn_entries))
    frames = [entry["frame"] for entry in fn_entries]
    reply = q(port, {"cmd": "wtrace_dump", "addr_lo": "0x000FE6F8",
                     "addr_hi": "0x000FE6FC", "frame_lo": min(frames),
                     "frame_hi": max(frames), "count": 2048})
    raw_path.write_text(json.dumps({"fntrace": fn, "wtrace": reply},
                                   indent=2) + "\n")
    # func attribution stays at the interrupted outer dispatcher during a
    # nested call. The exact rand() store PC and the drop-roll callsite RA are
    # stable and distinguish these calls from results animation RNG traffic.
    entries = sorted((entry for entry in reply.get("entries", [])
                      if ((int(entry.get("pc", "0"), 16) & 0x1FFFFFFF) ==
                          0x0008E5B0 and
                          (int(entry.get("ra", "0"), 16) & 0x1FFFFFFF) ==
                          0x00021848)), key=lambda row: row["seq"])
    if not entries:
        raise AssertionError(("drop RNG trace empty", frames,
                              reply.get("total"), reply.get("emitted")))
    decoded = []
    for entry in entries:
        old = int(entry["old"], 16)
        new = int(entry["new"], 16)
        if lcg(old, 1) != new:
            raise AssertionError(("unexpected RNG transition", entry))
        decoded.append({"seq": entry["seq"], "old": old, "new": new,
                        "steps": 1, "pc": entry["pc"],
                        "func": entry["func"]})
    normal_positions = count - int(story)
    stream_positions = 1 if count == 1 else 1 + 7 * normal_positions
    if len(decoded) != expected_function_calls:
        raise AssertionError(("RNG write count", len(decoded),
                              expected_function_calls,
                              decoded[:8], decoded[-8:]))
    # At 2+, the first retained normal comes after the discarded call and its
    # six-call burn, so derive the hook-entry seed seven positions backward.
    # At 1, the first call starts at the hook-entry seed. The final duplicate
    # is previewed then restored before the in-flight stock call.
    seed_origin = (decoded[0]["old"] if count == 1 else
                   lcg_back(decoded[0]["old"], 7))
    expected_seed = lcg(seed_origin, stream_positions)
    if decoded[-1]["new"] != expected_seed:
        raise AssertionError(("RNG stream position", seed_origin,
                              stream_positions, expected_seed,
                              decoded[-1], decoded))
    if expected_normal and (decoded[-2]["old"], decoded[-2]["new"]) != \
            (decoded[-1]["old"], decoded[-1]["new"]):
        raise AssertionError(("final preview/in-flight mismatch", decoded[-2:]))
    return {"generated_function_calls": len(decoded),
            "expected_function_calls": expected_function_calls,
            "stream_positions": stream_positions,
            "seed_origin": seed_origin,
            "seed_expected": expected_seed,
            "seed_after": decoded[-1]["new"], "entries": decoded}


def case_ini(case):
    """Build the real Drop Tables import consumed by the runtime backend."""
    pairs = ", ".join(f"{card}:{weight}"
                      for card, weight in sorted(case["weights"].items()))
    lines = ["format = 3", "smart_drop = on", "", "[Teana]"]
    if case.get("story"):
        lines.extend((f"card = {STORY_CARD}", "when = every"))
    # The authorized near-results fixture always calls the roll with a0=0,
    # even though its displayed rank is S-TEC. Mirror the target row into all
    # bands so these behavior cases test Smart itself without claiming that
    # the fixture provides synthetic live rank-band coverage.
    lines.extend((f"pow_table = {pairs}", f"bcd_table = {pairs}",
                  f"tec_table = {pairs}", ""))
    return "\n".join(lines)


def install_case_layer(port, case, case_dir):
    """Install through the single-sourced importer, not transient guest RAM."""
    path = case_dir / "drop_table_edits.ini"
    path.write_text(case_ini(case))
    reply = q(port, {"cmd": "drop_viewer_set", "import": str(path)})
    if not reply.get("ok"):
        raise RuntimeError(("drop import", reply, path.read_text()))
    exported = case_dir / "drop_table_edits.export.ini"
    saved = q(port, {"cmd": "drop_viewer_set", "export": str(exported)})
    if not saved.get("ok") or "smart_drop = on" not in exported.read_text():
        raise RuntimeError(("drop export", saved, exported))
    return reply, exported


def configure_case(port, case, case_dir):
    before_frame = q(port, {"cmd": "frame"}).get("frame", 0)
    loaded = q(port, {"cmd": "savestate", "op": "load", "slot": 0})
    if not loaded.get("ok"):
        raise RuntimeError(("savestate load", loaded))
    wait_q(port, {"cmd": "frame"},
           lambda r: r.get("frame", 0) >= before_frame + 3,
           20, "post-savestate guest frames")
    wait_q(port, {"cmd": "story_rewards"},
           lambda r: r.get("opponent_now") == OPPONENT, 20, "Teana state")
    time.sleep(5.0)
    phase = int.from_bytes(ram(port, PHASE, 2), "little") & 0xF
    if phase == 0xD:
        raise RuntimeError("fresh savestate unexpectedly loaded in results phase")

    # Campaign, with the first-win bit cleared unless the story-every case
    # deliberately proves an already-defeated opponent still grants it.
    flags = bytearray(ram(port, FREE_FLAGS, 1))
    flags[0] &= 0x7F
    write(port, FREE_FLAGS, flags)
    unlock_addr = UNLOCKS + (OPPONENT >> 3)
    unlock = bytearray(ram(port, unlock_addr, 1))
    mask = 0x80 >> (OPPONENT & 7)
    if case.get("story"):
        unlock[0] |= mask
    else:
        unlock[0] &= ~mask
    write(port, unlock_addr, unlock)
    imported, exported = install_case_layer(port, case, case_dir)
    setting = q(port, {"cmd": "card_drops_set", "drops": case["count"],
                       "smart": 1})
    if not setting.get("ok"):
        raise RuntimeError(setting)

    weights = case["weights"]
    blob = table_blob(weights)
    cards = set(weights) | set(case["owned"])
    deck, trunk = set_inventory(port, cards, case["owned"])
    time.sleep(2.0)
    return blob, deck, trunk, cards, imported, exported


def run_case(port, output, case):
    case_dir = output / case["name"]
    case_dir.mkdir()
    (table_before, deck_before, trunk_before, cards, imported,
     exported) = configure_case(port, case, case_dir)
    live_tier = 0
    rank_source = {"natural": True, "tier": live_tier}
    initial = owned_totals(deck_before, trunk_before, cards)
    card_zero_before = ram(port, CARD_ZERO, 1)[0]
    recent_before = ram(port, RECENT, 32)
    story_before = q(port, {"cmd": "story_rewards"}).get("fired", 0)
    smart_before = q(port, {"cmd": "card_drops_state"})["smart"]

    q(port, {"cmd": "wtrace_disarm_all"})
    armed = q(port, {"cmd": "wtrace_add", "lo": "0x000FE6F8",
                     "hi": "0x000FE6FC"})
    if not armed.get("ok"):
        raise RuntimeError(armed)
    q(port, {"cmd": "fntrace_arm_clear"})
    fn_armed = q(port, {"cmd": "fntrace_arm", "target": "0x80021810"})
    if not fn_armed.get("ok"):
        raise RuntimeError(fn_armed)

    def final_setup():
        # This authorized near-win state can contain a prior run's non-stock
        # resident row.  The real import above remains the persistent source
        # of truth, while this immediate pre-roll write makes the copied
        # savestate's one-shot test setup unambiguous after its duel transition
        # has finished reloading guest tables.
        for tier in range(3):
            row_addr = DROP_TABLE + tier * TIER_STRIDE
            write(port, row_addr, table_before)
            if ram(port, row_addr, TABLE_BYTES) != table_before:
                raise AssertionError("pre-roll resident row write did not stick")
        q(port, {"cmd": "wtrace_clear"})
        q(port, {"cmd": "fntrace_clear"})

    # Lower-rank result animations can consume an additional confirmation
    # before reaching the stock roll. Advance one screen at a time, reapplying
    # the intended selector/table after any screen that recomputed them, and
    # stop as soon as the one real outer drop-roll hook has fired.
    press(port, CROSS, 6.0)
    calls_before = q(port, {"cmd": "card_drops_state"})["calls"]
    result_confirms = 0
    for _ in range(4):
        final_setup()
        press(port, CROSS, 4.0)
        result_confirms += 1
        if q(port, {"cmd": "card_drops_state"})["calls"] > calls_before:
            break
    else:
        raise RuntimeError("drop roll did not fire after four result confirmations")
    wait_q(port, {"cmd": "read_ram", "addr": f"{PHASE:08X}", "len": 2},
           lambda r: r.get("ok") and
           (int.from_bytes(bytes.fromhex(r.get("hex") or r.get("data")),
                           "little") & 0xF) == 0xD,
           45, "duel results")
    expected_total = case["expected_normal"] + int(case.get("story", False))
    precommit = wait_q(port, {"cmd": "card_drops_list"},
                       lambda r: r.get("total") == expected_total,
                       20, "Smart award list")
    state_visible = q(port, {"cmd": "card_drops_state"})
    if state_visible["smart"]["last_tier"] != live_tier:
        raise AssertionError(("live tier", live_tier, state_visible))

    summary = case_dir / "summary.png"
    capture(port, summary)
    pages = []
    if case.get("pagination"):
        press(port, RIGHT, 2.0)
        first = case_dir / "cards-page-1.png"
        capture(port, first)
        page1 = q(port, {"cmd": "card_drops_state"})
        press(port, RIGHT, 2.0)
        second = case_dir / "cards-page-2.png"
        capture(port, second)
        page2 = q(port, {"cmd": "card_drops_state"})
        if (page1["suppression"]["page"] != 3 or
                page2["suppression"]["page"] != 3 or
                sha(first.read_bytes()) == sha(second.read_bytes())):
            raise AssertionError(("pagination", page1, page2))
        pages = [{"path": str(first), "sha256": sha(first.read_bytes())},
                 {"path": str(second), "sha256": sha(second.read_bytes())}]
        press(port, LEFT, 1.0)
        press(port, LEFT, 2.0)
        returned = q(port, {"cmd": "card_drops_state"})
        if returned["suppression"]["page"] != 0:
            raise AssertionError(("pagination return", returned))

    # Cross commits or deliberately suppresses the in-flight final card from
    # any results subpage.
    skipped_before = smart_before.get("skipped", 0)
    press(port, CROSS, 6.0)
    if case.get("all_ineligible"):
        wait_q(port, {"cmd": "card_drops_state"},
               lambda r: r["smart"].get("skipped", 0) > skipped_before,
               20, "Smart exhausted suppression")
    else:
        wait_q(port, {"cmd": "card_drops_list"},
               lambda r: r.get("order_n") == expected_total and
               all(item.get("committed") for item in
                   r.get("award_order", [])),
               30, "committed Smart awards")
    final = q(port, {"cmd": "card_drops_list"})
    state_final = q(port, {"cmd": "card_drops_state"})
    story_final = q(port, {"cmd": "story_rewards"})
    table_after = ram(port, DROP_TABLE + live_tier * TIER_STRIDE,
                      TABLE_BYTES)
    deck_after = list(struct.unpack("<40H", ram(port, DECK, 80)))
    trunk_after = ram(port, TRUNK, 722)
    totals = owned_totals(deck_after, trunk_after, cards)

    if table_after != table_before or state_final["smart"]["table_active"]:
        raise AssertionError(("resident table not restored", state_final))
    if final["order_n"] != expected_total:
        raise AssertionError(("award count", expected_total, final))
    order = final["award_order"]
    normal = [item["id"] for item in order if item["kind"] == "normal"]
    if len(normal) != case["expected_normal"]:
        raise AssertionError(("normal count", case, order))
    if case.get("story"):
        if order[0] != {"id": STORY_CARD, "kind": "story",
                         "committed": True}:
            raise AssertionError(("story ordering", order))
        if story_final.get("fired", 0) - story_before != 1:
            raise AssertionError(("story fired count", story_before,
                                  story_final))
        if totals[STORY_CARD] != initial[STORY_CARD] + 1:
            raise AssertionError(("story was Smart-filtered", initial, totals))
    elif any(item["kind"] != "normal" for item in order):
        raise AssertionError(("unexpected story", order))
    if case.get("only") and not set(normal).issubset(case["only"]):
        raise AssertionError(("excluded card awarded", case, normal))
    for card in case["weights"]:
        if totals[card] - initial[card] != normal.count(card):
            raise AssertionError(("inventory/award delta mismatch", card,
                                  initial[card], totals[card], normal))
        if initial[card] < 3 and totals[card] > 3:
            raise AssertionError(("Smart exceeded three", card,
                                  initial[card], totals[card], order))
        if (initial[card] >= 3 and
                (totals[card] != initial[card] or card in normal)):
            raise AssertionError(("ineligible changed", card,
                                  initial[card], totals[card]))
    if case.get("expect_new") and not any(row.get("new") for row in
                                           final.get("cards", [])):
        raise AssertionError(("New marker missing", final))
    if case.get("all_ineligible"):
        if (state_visible["suppression"]["reason"] != "smart_exhausted" or
                ram(port, CARD_ZERO, 1)[0] != card_zero_before or
                trunk_after != trunk_before or
                ram(port, RECENT, 32) != recent_before):
            raise AssertionError(("all-ineligible fallback/mutation",
                                  state_visible, final))

    trace = rng_trace(port, case["count"], case.get("story", False),
                      case["expected_normal"],
                      case_dir / "rng-wtrace.json")
    return {
        "case": case["name"], "pass": True,
        "result_confirmations": result_confirms,
        "rank_source": rank_source,
        "configured": {key: (sorted(value) if isinstance(value, set) else value)
                       for key, value in case.items()},
        "initial_owned": initial,
        "final_owned": totals, "precommit": precommit, "final": final,
        "smart_visible": state_visible["smart"],
        "suppression_visible": state_visible["suppression"],
        "smart_final": state_final["smart"],
        "suppression_final": state_final["suppression"],
        "table_before_sha256": sha(table_before),
        "table_after_sha256": sha(table_after),
        "import": imported,
        "export": {"path": str(exported),
                   "sha256": sha(exported.read_bytes())},
        "trunk_before_sha256": sha(trunk_before),
        "trunk_after_sha256": sha(trunk_after),
        "card_zero_before": card_zero_before,
        "card_zero_after": ram(port, CARD_ZERO, 1)[0],
        "rng": trace,
        "summary": {"path": str(summary), "sha256": sha(summary.read_bytes())},
        "pages": pages,
    }


def run_distribution(port, output):
    loaded = q(port, {"cmd": "savestate", "op": "load", "slot": 0})
    if not loaded.get("ok"):
        raise RuntimeError(loaded)
    wait_q(port, {"cmd": "story_rewards"},
           lambda r: r.get("opponent_now") == OPPONENT, 20,
           "distribution live save")
    time.sleep(3.0)
    weights = {601: 1024, 602: 512, 603: 512}
    distribution_case = {"tier": 0, "weights": weights}
    distribution_dir = output / "distribution-100k"
    distribution_dir.mkdir()
    imported, exported = install_case_layer(port, distribution_case,
                                             distribution_dir)
    blob = table_blob(weights)
    set_inventory(port, set(weights), {})
    tiers = []
    for tier in range(3):
        address = DROP_TABLE + tier * TIER_STRIDE
        write(port, address, blob)
        if ram(port, address, TABLE_BYTES) != blob:
            raise AssertionError(("distribution resident row write", tier))
        reply = q(port, {"cmd": "card_drops_smart_sim", "tier": tier,
                         "seed": 0x13579BDF, "rolls": 100000})
        if (not reply.get("ok") or reply.get("count_sum") != 100000 or
                reply.get("weight_sum") != 2048 or reply.get("fallback") or
                reply.get("tier") != tier):
            raise AssertionError(("distribution totals", tier, reply))
        rows = {row["id"]: row for row in reply["entries"]}
        if set(rows) != set(weights):
            raise AssertionError(("distribution entries", tier, rows))
        for card, weight in weights.items():
            observed = rows[card]["count"] / 100000.0
            expected = weight / 2048.0
            if abs(observed - expected) > 0.01:
                raise AssertionError(("distribution drift", tier, card,
                                      observed, expected, reply))
        tiers.append(reply)
    if any(row["entries"] != tiers[0]["entries"] or
           row["final_seed"] != tiers[0]["final_seed"] for row in tiers[1:]):
        raise AssertionError(("rank-band deterministic mismatch", tiers))
    return {"pass": True, "seed": 0x13579BDF,
            "rolls_per_tier": 100000, "weights": weights,
            "tiers": tiers, "import": imported,
            "export": {"path": str(exported),
                       "sha256": sha(exported.read_bytes())}}


def run_tier_probe(port, output, tier):
    """Minimal actual results-path probe: select a tier and award no card."""
    case = {"name": f"tier-{tier}-no-award-probe", "count": 0,
            "tier": tier, "weights": {101: 2048}, "owned": {},
            "expected_normal": 0}
    case_dir = output / case["name"]
    case_dir.mkdir()
    table, _, _, _, imported, exported = configure_case(
        port, case, case_dir)
    rank_source = {}
    card_zero_before = ram(port, CARD_ZERO, 1)[0]
    q(port, {"cmd": "fntrace_arm_clear"})
    q(port, {"cmd": "fntrace_arm", "target": "0x80021810"})
    press(port, CROSS, 6.0)
    calls_before = q(port, {"cmd": "card_drops_state"})["calls"]
    confirms = 0
    for _ in range(4):
        rank_source.clear()
        rank_source.update(set_rank_source(port, tier))
        write(port, DROP_TABLE + tier * TIER_STRIDE, table)
        q(port, {"cmd": "fntrace_clear"})
        press(port, CROSS, 4.0)
        confirms += 1
        if q(port, {"cmd": "card_drops_state"})["calls"] > calls_before:
            break
    else:
        raise RuntimeError("tier probe did not reach the drop roll")
    state = q(port, {"cmd": "card_drops_state"})
    awards = q(port, {"cmd": "card_drops_list"})
    trace = q(port, {"cmd": "fntrace_dump", "target_lo": "0x80021810",
                     "target_hi": "0x80021811", "count": 16})
    shot = case_dir / "no-award-results.png"
    capture(port, shot)
    if (state["last_tier"] != tier or len(trace.get("entries", [])) != 1 or
            awards.get("total") != 0 or
            state["suppression"]["reason"] != "zero_normal" or
            ram(port, CARD_ZERO, 1)[0] != card_zero_before):
        raise AssertionError(("tier no-award probe", tier, state, awards,
                              trace, card_zero_before,
                              ram(port, CARD_ZERO, 1)[0]))
    return {"pass": True, "tier": tier, "rank_source": rank_source,
            "result_confirmations": confirms, "state": state,
            "awards": awards, "fntrace": trace, "import": imported,
            "export": str(exported), "card_zero": card_zero_before,
            "screenshot": {"path": str(shot),
                           "sha256": sha(shot.read_bytes())}}


def stop(proc, port):
    if proc.poll() is not None:
        return
    try:
        q(port, {"cmd": "quit_graceful"})
        proc.wait(timeout=15)
        return
    except Exception:
        proc.terminate()  # exact owned child only
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exe", type=Path,
                        default=ROOT / "build-dbg" /
                        "Yu_Gi_Oh_Forbidden_Memories_Recompiled")
    parser.add_argument("--disc", type=Path,
                        default=ROOT / "disc" /
                        "Yu-Gi-Oh! Forbidden Memories (USA).cue")
    parser.add_argument("--case", choices=[row["name"] for row in CASES])
    parser.add_argument("--skip-distribution", action="store_true")
    parser.add_argument("--distribution-only", action="store_true")
    parser.add_argument("--tier-probe", type=int, choices=range(3))
    parser.add_argument("--aggregate-from", type=Path, action="append",
                        help="combine independent results.json files and exit")
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.aggregate_from:
        if args.output.exists():
            raise SystemExit(f"refusing to reuse output directory: {args.output}")
        args.output.mkdir(parents=True)
        aggregate = {"schema": 1, "independent_processes": [],
                     "cases": [], "distribution": None}
        for source in args.aggregate_from:
            path = source.resolve()
            if path.is_dir():
                path = path / "results.json"
            data = json.loads(path.read_text())
            aggregate["independent_processes"].append({
                "path": str(path), "pid": data.get("pid"),
                "port": data.get("port"), "fixture": data.get("fixture")})
            aggregate["cases"].extend(data.get("cases", []))
            if data.get("distribution") is not None:
                aggregate["distribution"] = data["distribution"]
        names = [row["case"] for row in aggregate["cases"]]
        expected = [row["name"] for row in CASES]
        if sorted(names) != sorted(expected) or aggregate["distribution"] is None:
            raise SystemExit(("incomplete aggregate", names,
                              aggregate["distribution"] is not None))
        ownership = set()
        exported_bands = set()
        for row in aggregate["cases"]:
            if not row.get("pass"):
                raise SystemExit(("failed case in aggregate", row["case"]))
            if row["table_before_sha256"] != row["table_after_sha256"]:
                raise SystemExit(("table not restored", row["case"]))
            if row["card_zero_before"] != row["card_zero_after"]:
                raise SystemExit(("card zero changed", row["case"]))
            rng = row["rng"]
            if (rng["generated_function_calls"] !=
                    rng["expected_function_calls"] or
                    rng["seed_after"] != rng["seed_expected"] or
                    not rng["entries"]):
                raise SystemExit(("RNG provenance mismatch", row["case"]))
            initial = {int(k): v for k, v in row["initial_owned"].items()}
            final = {int(k): v for k, v in row["final_owned"].items()}
            configured_cards = {int(k) for k in
                                row["configured"]["weights"]}
            normal = [item["id"] for item in row["final"]["award_order"]
                      if item["kind"] == "normal"]
            for card, value in initial.items():
                ownership.add(value if value <= 3 else "many")
                if card in configured_cards and (
                        final[card] - value != normal.count(card)):
                    raise SystemExit(("inventory delta mismatch", row["case"],
                                      card, value, final[card], normal))
            export_text = Path(row["export"]["path"]).read_text()
            if "smart_drop = on" not in export_text:
                raise SystemExit(("Smart export missing", row["case"]))
            for tier, token in enumerate(("pow_table =", "bcd_table =",
                                          "tec_table =")):
                if token in export_text:
                    exported_bands.add(tier)
        required_ownership = {0, 1, 2, 3, "many"}
        if ownership != required_ownership:
            raise SystemExit(("ownership coverage", ownership))
        if exported_bands != {0, 1, 2}:
            raise SystemExit(("rank-band export coverage", exported_bands))
        by_name = {row["case"]: row for row in aggregate["cases"]}
        new_case = by_name["count-1-owned-0-tier-0"]
        owned_case = by_name["count-2-owned-1-tier-1"]
        if (not any(card["new"] for card in new_case["final"]["cards"]) or
                any(card["new"] for card in owned_case["final"]["cards"])):
            raise SystemExit("New-marker ownership policy mismatch")
        for name in ("count-16-owned-2-tier-2-pagination",
                     "count-99-split-deck-trunk-tier-0",
                     "story-every-plus-smart-normals"):
            pages = by_name[name]["pages"]
            if len(pages) != 2 or pages[0]["sha256"] == pages[1]["sha256"]:
                raise SystemExit(("pagination evidence", name, pages))
        exhausted = by_name["all-ineligible-no-fallback"]
        if (exhausted["final"]["order_n"] != 0 or
                exhausted["suppression_visible"]["reason"] !=
                "smart_exhausted" or
                exhausted["trunk_before_sha256"] !=
                exhausted["trunk_after_sha256"]):
            raise SystemExit(("all-ineligible semantics", exhausted))
        story = by_name["story-every-plus-smart-normals"]
        if (story["final"]["award_order"][0]["kind"] != "story" or
                any(item["kind"] != "normal" for item in
                    story["final"]["award_order"][1:])):
            raise SystemExit(("story/normal ordering", story["final"]))
        distribution = aggregate["distribution"]
        tiers = distribution.get("tiers", [])
        if ([row.get("tier") for row in tiers] != [0, 1, 2] or
                any(row.get("count_sum") != 100000 for row in tiers)):
            raise SystemExit(("rank-band distribution incomplete", tiers))
        aggregate["coverage"] = {
            "configured_normal_drop_counts": [1, 2, 16, 99],
            "observed_normal_award_counts": sorted({
                sum(item["kind"] == "normal" for item in
                    row["final"]["award_order"])
                for row in aggregate["cases"]}),
            "ownership_totals": [0, 1, 2, 3, "many"],
            "rank_band_import_export": [0, 1, 2],
            "rank_band_seeded_host_simulation": [0, 1, 2],
            "actual_duel_drop_hook_tiers": [0],
            "new_marker": "new only for initially-zero ownership",
            "pagination_cases": [16, 40],
            "rng_policy": "all traced drop RNG writes match the LCG; Smart performs no retries",
        }
        aggregate["limitations"] = [
            "The authorized near-results fixture displays S-TEC but the real "
            "drop hook receives a0/tier 0. Actual-duel behavior is therefore "
            "proven at tier 0 only; each case mirrors its row across all three "
            "bands.",
            "Tier 1 and 2 are covered through format-3 import/export and the "
            "seeded host-only Smart simulation, not a claimed live rank "
            "selection.",
        ]
        aggregate["pass"] = True
        target = args.output / "aggregate.json"
        target.write_text(json.dumps(aggregate, indent=2) + "\n")
        print(f"PASS aggregate: {target}")
        return
    if args.fixture is None:
        raise SystemExit("--fixture is required unless --aggregate-from is used")
    args.fixture = args.fixture.resolve()
    args.exe = args.exe.resolve()
    args.disc = args.disc.resolve()
    if args.output.exists():
        raise SystemExit(f"refusing to reuse output directory: {args.output}")
    for path in (args.fixture / STATE, args.fixture / THUMB,
                 args.exe, args.disc):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    state_dir = args.output / "player" / "openbios"
    state_dir.mkdir(parents=True)
    shutil.copyfile(args.fixture / STATE, state_dir / STATE)
    shutil.copyfile(args.fixture / THUMB, state_dir / THUMB)
    fixture_meta = patch_copied_state(state_dir / STATE)
    fixture_meta["thumb_sha256"] = sha((state_dir / THUMB).read_bytes())

    port = free_port()
    env = dict(os.environ, PSX_PORTABLE="1")
    env.pop("APPIMAGE", None)
    env.pop("APPDIR", None)
    log = (args.output / "runtime.log").open("w")
    cmd = [str(args.exe), "--no-launcher", "--memcard-dir",
           str(args.output / "player"), "--debug-port", str(port),
           "--disc", str(args.disc)]
    proc = subprocess.Popen(cmd, cwd=args.exe.parent, env=env,
                            stdin=subprocess.DEVNULL, stdout=log,
                            stderr=subprocess.STDOUT, start_new_session=True)
    print(f"START harness={os.getpid()} game={proc.pid} port={port}",
          flush=True)
    report = {"schema": 1, "pid": proc.pid, "port": port,
              "executable_sha256": sha(args.exe.read_bytes()),
              "disc_sha256": sha(args.disc.read_bytes()),
              "fixture": fixture_meta, "command": cmd,
              "cases": [], "tier_probe": None, "distribution": None}
    try:
        wait_q(port, {"cmd": "frame"}, lambda r: r.get("ok"), 45,
               "debug server")
        wait_q(port, {"cmd": "frame"},
               lambda r: r.get("frame", 0) > 8400, 240,
               "fully initialized game")
        if args.tier_probe is not None:
            report["tier_probe"] = run_tier_probe(
                port, args.output, args.tier_probe)
            print(f"PASS no-award tier {args.tier_probe} probe", flush=True)
        selected = ([] if args.distribution_only or args.tier_probe is not None else
                    [row for row in CASES
                     if not args.case or row["name"] == args.case])
        for case in selected:
            result = run_case(port, args.output, case)
            report["cases"].append(result)
            print(f"PASS {case['name']}: {result['final']['order_n']} awards",
                  flush=True)
        if not args.skip_distribution:
            report["distribution"] = run_distribution(port, args.output)
            print("PASS seeded 100000-roll distribution", flush=True)
    finally:
        (args.output / "results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        stop(proc, port)
        log.close()
    print(f"PASS {len(report['cases'])} actual-duel cases; "
          f"evidence: {args.output / 'results.json'}")


if __name__ == "__main__":
    main()
