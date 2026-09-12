#!/usr/bin/env python3
"""Live stock-policy probe for an already-seeded netplay_pair scratch tree.

The caller deliberately places non-stock settings/files in both peer roots.
This launches the normal pair, proves the session is clamped to 1x, checks
the three guest-mutating menus and editor paths are disabled, attacks debug
backdoors (including current mod-authoring features), and verifies every
persistent input file is byte-identical after a graceful two-peer shutdown.
Results are written as JSON under NETPAIR_DIR.
"""

import hashlib
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import netplay_pair as np


def hashes(root):
    result = {}
    for path in sorted(Path(root).rglob("*")):
        if path.is_file() and "runtime.log" not in path.name and "/netplay/" not in str(path):
            result[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def rejected(reply):
    return reply.get("ok") is False and "netplay" in (reply.get("error") or "").lower()


def main():
    Path(np.ROOT).mkdir(parents=True, exist_ok=True)
    before = {np.NAMES[s]: hashes(np.inst(s).dir) for s in (0, 1)}
    report = {
        "schema": 2,
        "binary_sha256": hashlib.sha256(Path(np.EXE).read_bytes()).hexdigest(),
        "disc_sha256": hashlib.sha256(Path(np.DISC).read_bytes()).hexdigest(),
        "ports": {"debug": np.DBG, "udp": np.UDP},
        "before": before,
        "peers": {},
    }
    procs = np.start()
    report["owned_pids"] = [proc.pid for proc in procs]
    try:
        report["boot_frames"] = np.wait_boot()
        for slot in (0, 1):
            peer = np.inst(slot)
            speed = peer.q({"cmd": "game_speed"})
            menus = {str(m): peer.q({"cmd": "video_menu", "menu": m})
                     for m in (4, 5, 6)}
            packs = peer.q({"cmd": "card_packs"})
            drops = peer.q({"cmd": "drop_edits"})
            story = peer.q({"cmd": "story_rewards"})
            starchips = peer.q({"cmd": "starchip_rewards", "op": "state"})
            drop_mode = peer.q({"cmd": "card_drops_state"})
            completion = peer.q({"cmd": "free_duel_completion"})
            password_view = peer.q({"cmd": "card_password_view"})
            sell_state = peer.q({"cmd": "card_shop_sell", "op": "state"})
            probes = {
                "speed_4x": peer.q({"cmd": "game_speed", "mult": 4}),
                "turbo": peer.q({"cmd": "turbo", "enabled": 1}),
                "turbo_loads": peer.q({"cmd": "turbo_loads", "n": 1}),
                "savestate": peer.q({"cmd": "savestate", "op": "save", "slot": 11}),
                "fm_editor": peer.q({"cmd": "fm_editor", "open": 1}),
                "card_manager": peer.q({"cmd": "card_manager_set", "open": 1}),
                "drop_manager": peer.q({"cmd": "drop_viewer_set", "open": 1}),
                "fusion_manager": peer.q({"cmd": "fusion_manager", "open": 1}),
                "dialogue_manager": peer.q({"cmd": "dialogue_manager", "open": 1}),
                "cpu_manager": peer.q({"cmd": "cpu_manager", "open": 1}),
                "story_edit": peer.q({"cmd": "story_rewards", "duelist": 1,
                                      "card": 1, "every": 1}),
                "starchip_edit": peer.q({"cmd": "starchip_rewards", "op": "set",
                                          "index": 0, "amount": 777777}),
                "drop_count": peer.q({"cmd": "card_drops_set", "drops": 99}),
                "drop_zero": peer.q({"cmd": "card_drops_set", "drops": 0}),
                "smart_drop": peer.q({"cmd": "card_drops_set", "smart": 0}),
                "drop_clear": peer.q({"cmd": "drop_viewer_set", "clear_band": 0,
                                       "confirm": 1}),
                "fill_library": peer.q({"cmd": "fill_library", "on": 1}),
                "card_set": peer.q({"cmd": "card_packs", "dev": 1}),
                "card_reload": peer.q({"cmd": "card_packs_reload", "card": 1}),
                "card_import": peer.q({"cmd": "card_share", "op": "import",
                                        "path": str(Path(np.ROOT) / "blocked.ygocard")}),
                "card_texts_import": peer.q({"cmd": "card_texts_import",
                                              "path": str(Path(np.ROOT) / "blocked.txt")}),
                "monster_effect": peer.q({"cmd": "monster_effects", "fx": 1,
                                          "card": 1, "side": 0}),
                "sell": peer.q({"cmd": "card_shop_sell", "op": "confirm"}),
                "dialogue_clear": peer.q({"cmd": "dialogue_clear"}),
                "cpu_edit": peer.q({"cmd": "cpu_data", "duelist": 2,
                                    "name": "NETPLAY LEAK"}),
                "mod_reset": peer.q({"cmd": "mod_package", "reset": 1}),
            }
            # Runtime speed requests are deliberately answered with the
            # effective 1x state; every title mutation path returns an error.
            if any(speed.get(k) != 1 for k in ("speed_mult", "requested", "effective")):
                raise AssertionError((slot, "initial speed", speed))
            if any(menus[str(m)].get("menu_enabled") != 0 for m in (4, 5, 6)):
                raise AssertionError((slot, "menus not disabled", menus))
            if any(probes["speed_4x"].get(k) != 1 for k in
                   ("speed_mult", "requested", "effective")):
                raise AssertionError((slot, "speed backdoor", probes["speed_4x"]))
            for name, reply in probes.items():
                if name == "speed_4x":
                    continue
                if not rejected(reply):
                    raise AssertionError((slot, name, reply))
            # The non-stock files/settings remain loaded as configuration but
            # no card pack was installed into guest RAM/CD state in netplay.
            if packs.get("packs"):
                raise AssertionError((slot, "custom card pack active", packs))
            if drops.get("entries", 0) < 1 or story.get("pairs", 0) < 1:
                raise AssertionError((slot, "offline edit fixtures missing", drops, story))
            if starchips.get("configured_rules", 0) < 1:
                raise AssertionError((slot, "offline starchip fixture missing", starchips))
            if starchips.get("active") or starchips.get("visible") or starchips.get("applies"):
                raise AssertionError((slot, "starchip override active in netplay", starchips))
            smart = drop_mode.get("smart", {})
            if (smart.get("configured") != 1 or smart.get("effective") != 0 or
                    smart.get("row_enabled") != 0):
                raise AssertionError((slot, "smart drop netplay policy", drop_mode))
            if drop_mode.get("setting") != 1:
                raise AssertionError((slot, "normal drop count not clamped to stock", drop_mode))
            if completion.get("netplay") != 1 or completion.get("visible") != 0:
                raise AssertionError((slot, "Free Duel completion visible in netplay", completion))
            if password_view.get("visible") != 0:
                raise AssertionError((slot, "password overlay visible in netplay", password_view))
            report["peers"][np.NAMES[slot]] = {
                "speed": speed, "menus": menus, "card_packs": packs,
                "drop_edits": drops, "story_rewards": story,
                "starchip_rewards": starchips, "card_drops": drop_mode,
                "free_duel_completion": completion,
                "card_password_view": password_view,
                "sell": sell_state,
                "blocked": probes,
            }
    finally:
        shutdown_errors = []
        for slot in (0, 1):
            try:
                np.inst(slot).q({"cmd": "quit_graceful"})
            except Exception as exc:
                shutdown_errors.append(f"{np.NAMES[slot]}: {exc}")
        deadline = time.monotonic() + 15
        while np.pids() and time.monotonic() < deadline:
            time.sleep(0.5)
        if np.pids():
            raise RuntimeError(f"owned peers did not stop gracefully: {np.pids()}")
        report["exit_codes"] = [proc.poll() for proc in procs]
        if shutdown_errors:
            raise RuntimeError("graceful shutdown request failed: " +
                               "; ".join(shutdown_errors))

    after = {np.NAMES[s]: hashes(np.inst(s).dir) for s in (0, 1)}
    report["after"] = after
    # Ignore normal generated card2/input/cache files by comparing every
    # fixture that existed at launch, not the directory's generated superset.
    changed = {}
    for peer, entries in before.items():
        changed[peer] = {name: [value, after[peer].get(name)]
                         for name, value in entries.items()
                         if after[peer].get(name) != value}
    report["persistent_changes"] = changed
    if any(changed.values()):
        raise AssertionError(("persistent settings changed", changed))
    out = Path(np.ROOT) / "stock-policy.json"
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS stock netplay policy; evidence: {out}")


if __name__ == "__main__":
    main()
