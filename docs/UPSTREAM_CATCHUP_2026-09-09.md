# psxrecomp catch-up — 2026-09-09

The local runtime branch `ygofm-upstream-2026-09-09` starts at upstream
`ed55299be34710a90fc080484a83e8634bd41fa9`, followed only by ten title extension
and regression-fix commits, ending at `c75132a6`. The title branch is `upstream-catchup-2026-09-09`. Both worktrees are
under `/tmp/ygofm-upstream-2026-09-09/title`; the original checkout remains on
`netplay-2p` / `ygofm-netplay`. Nothing was pushed or released.

Evidence root **E** below is `/tmp/ygofm-upstream-2026-09-09/evidence`.
These captures, scratch saves, build logs and investigation failures are local
artifacts, not committed assets. Preserve them before clearing `/tmp`.

| Item | Baseline result | New result | Evidence under E |
| --- | --- | --- | --- |
| 1. Linux builds | Debug, release and setup host link | Debug, release and setup host link | `baseline/build-{debug,release,setup}.log`; `new/build-{debug,release,setup}-final.log` |
| 2. Boot / menus | Title, LOAD, Library and Password 89631139 pass | Same; loaded-menu native capture identical | `baseline/solo-v3`, `new/solo`, `screenshot-comparison.json` |
| 3. Duel / freezes | Two turns and win → drops → Library pass | Same; Library advances 300 frames over 5 seconds after drops | `baseline/duel-fixture`, `baseline/mods-v2`, `new/solo`, `new/win-probe` |
| 4. Managers | Card, Fusion, Drop, CPU open/edit; randomize, card share, package round-trip pass | Same; edited ATK 3100 visible in Library | `baseline/solo-v3`, `new/solo`; manager PPMs and `commands.jsonl` |
| 5. Video / audio | GL dropdowns/navigation pass; extra applied-control probe fails its persistence assertion; Vulkan loads; SDL menu draw missing in source | GL controls pass; software F10 and all seven composed menus pass after follow-up fix; exhaustive row activation remains incomplete | `baseline/video-v2`, `new/video`, `baseline/video-actions-v4`, `new/video-actions-v3`, `{baseline,new}/vulkan`, `new/software-menu-final-v2` |
| 6. Mods | Art, frame color, repacked description, portrait, fill Library and monster/magic effects visible; full package round-trip passes | Same; visual means 0–3.66 | `{baseline,new}/mods`, `baseline/mods-v2`, `new/win-probe`, `mods-screenshot-comparison.json`; `{baseline,new}/magic`, `magic-comparison.json` |
| 7. Loopback netplay | Delay-sync, rollback 35/15, 5% loss boot/P2 cover pass | All pass after CD fix and state-aware route correction; trade carries both cards back with backups | `baseline/netplay-{delay,rollback,loss}`; `new/netplay-delay-fixed`, `new/netplay-rollback-v2`, `new/netplay-loss` and matching `.log` files |
| 8. Save integrity | Trade changes 74 bytes per card; directory unchanged | Final traded cards exactly match baseline; loss run changes zero bytes | `delay-comparison.json`, `rollback-card-comparison.json`, `loss-card-integrity.json` |
| 9. Performance | Duel 59.9565 fps; Card Manager 60.0302 fps | Duel 59.9652 fps; Card Manager 59.9654 fps | `baseline/duel-fixture/duel-perf.json`, `baseline/solo-v3/card-manager-perf.json`, `new/solo/*-perf.json` |
| 10. Linux setup packaging | Stages and configures offline with explicit toolchain | Same, final 31 MB setup archive staged locally | `baseline/package.log`, `baseline/staged-offline-setup-configure.log`, `new/package-final.log`, `new/staged-offline-final.log` |

Manual-launch follow-up: the user reported F10 displaying nothing in software.
The SDL path consumed input and reserved the inset but omitted the menu image;
earlier composed menu checks used GL and missed this. Runtime `c75132a6` adds
SDL composition, seeds the documented 3x window default (zero previously gave
a 1x window and a displayed 0x), and makes `present_shot` capture the entire SDL
window including UI outside the game viewport. Existing valid settings still win.
`tools/software_menu_regression.py` launches a new scratch profile, drives real
F10/arrow events, checks pixels in all seven composed dropdowns and opens Card
Manager. It passes in `new/software-menu-final-v2`; debug, release and setup
rebuilds pass in `new/software-menu-{build,release-build,setup-build}.log`.
The first test attempt queried title hooks before initialization; the harness
now waits for readiness. Baseline source has the same omitted SDL draw; no new
matched baseline run was made for this follow-up. SDL3 code remains untested.
The scratch helper `/tmp/ygofm-upstream-2026-09-09/Try-upstream.sh` now launches
this rebuilt software version. The original checkout's Play.sh still runs the
original fork. No personal cards were accessed for this follow-up.

The custom magic card deals exactly 500 damage on both revisions (opponent LP
8000 → 7500), with matching hook-event sequences and image mean difference
0.1454. Both monster-trigger runs record one completed cast.

The measured Card Manager difference is 0.11%, about two frames over a 30-second
sample. This is a throughput check, not a frame-time distribution or input-lag
benchmark. No freeze occurred in either two-turn or post-drop Library route;
Pause's latency was not separately instrumented.

The extra baseline applied-control probe failed its widescreen persistence
assertion after moving to an isolated portable environment; the earlier baseline
log does record the 16:9 callback. The candidate applied-control probe passes,
including persisted widescreen on/off, Master volume 17, and open Game savestate
menu. A complete matched baseline for those extra controls remains unverified.

The broad suite is **not an exhaustive feature certification**. Fusion and
randomized drop edits were verified in their managers and package exports;
their exact recipes/rewards were not all exercised in game. Dialogue import
rebuilt the bank and the manager opened, but the translated campaign line was
not visited. Story reward configuration round-trips, but a campaign reward was
not awarded. Those require additional campaign/duel fixtures beyond the loaded
scratch seed. Every cheat, fullscreen/window-scale/resolution row, live renderer
switch, and an offline update-check failure was not individually exercised.
Renderer selection was tested at launch. The private menu has a persisted
renderer setting and launch update check, not dedicated visible rows for both.
Native OS pickers were bypassed through the existing debug import/export APIs;
the package import itself and backup-first round trip were exercised.

Vulkan initializes on the RTX 4090 and accepts the loaded-save state on both
revisions. `screenshot_present` queues only the GL capture path on both, so the
Vulkan menu capture checks fail with a missing output image. No plain screenshot
was substituted to claim an overlay pass. Netplay cover checks use composed GL
captures exclusively.

Native solo screenshots all pass mean gray difference <18 at 80×60. Full-window
netplay images initially fail because the baseline's launcher settings select a
1280×1000 window, while the candidate fits a larger display area. The originals
are retained in `screenshot-comparison.json`. Comparing the measured game
viewports gives 31/32 passes (`netplay-viewport-comparison.json`); the remaining
baseline guest menu image catches the sliding menu transition, whereas the new
image has settled. This is an explained capture mismatch, not a silently waived
threshold. Mod art/color/description and portrait screenshots agree closely.

The early baseline inherited shared menu preferences (including the Dev Card
Effects set); later baseline visual-mod tests use a portable copy of the same
executable and explicitly select Own Cards. The editor also exported APPIMAGE,
which made legacy portable sidecars resolve beside the editor under Applications;
the final harness strips APPIMAGE/APPDIR for native child executables. The final
baseline applied-menu check uses that corrected environment. This explains the
different opponent LP after two otherwise successful turns. The port exposed
and fixes the fork's sidecar isolation gap: `--memcard-dir` now also isolates
menu settings and keybinds. Early baseline/menu runs used the old shared sidecar
behavior. Cards and title mod files stayed in scratch throughout; the personal
cards were never written.

The mechanical merge measurement found 34 conflicting paths (30 content,
two submodule and two deleted-file conflicts), 226 text hunks. Main had 53,
GL 77, lobby 14 and netplay 14. `conflicts.md` lists every path;
`merge-tree.txt` retains the experiment. The fork has no shared ancestry with
upstream; its root snapshot corresponds to upstream `1dc58357`, 397 upstream
commits before the pinned tip. The port added 37 standalone private files plus
CMake wiring first, then integrated runtime, netplay, video, debug and setup
hooks as separate commits. `runtime-symbols.txt` records the 95 distinct title
runtime calls measured with the requested prefix pattern.

Upstream replacements retained: single-context GL interpolation scheduling,
HiDPI/window handling, lobby chat/auth/session handling, guest-card transfer,
spectator/rematch resets, dirty-rectangle/readback work, mod catalog and setup
toolchain stamps. The old private GL presenter thread/fence scheme and duplicate
lobby implementation were dropped. Private title menus, UI fonts/drawing,
guest overlays, input/audio services, savestate host UI, debug commands and
performance probes remain. Title provenance counters coexist with upstream's
texture-correction counters under distinct names. Pause's 131072 idle latency,
card guards/mirrors, `memcard_mirror_to`, seat names, `quit_graceful`, and portable
setup download/unpack fallbacks remain fork-only.

Two additional fixes were validated during the port:

- Upstream accelerates mode A0 using a 0x48 realtime mask but its consumer guard
  still used 0x68. This overwrote unread CD sectors and stalled at Konami even
  with baseline generated C. One shared predicate now drives both decisions.
  `runtime/tests/test_cdrom_accelerated_consumer.c` passes; restoring the old
  guard fails six mode/divisor cases. See `cdrom-consumer-ctest.log` and
  `cdrom-consumer-negative.log`.
- `--memcard-dir` now resolves sidecars before the cached user-data default and
  suppresses legacy shared-settings fallback. Debug/release/setup all build;
  new live runs write `menu_settings.ini` and `keybinds.ini` in their own player
  directories.

The basic-block, decoder and seed differences from the fork were line endings,
not semantic recompiler changes. Upstream's versions are used. The private
guest-card default config option and Windows UTF-8 executable manifest remain.
Game and OpenBIOS C were regenerated, then CMake was reconfigured to discover the
new shard list: game C 67 → 70 files, 100,930,361 → 101,454,214 bytes; BIOS C two
files, 9,420,172 → 9,468,913 bytes. See `generated-{before,after}.json` and
`regenerate-{game,bios}.log`. Generated sources remain ignored, as before.

`recomp-net` is upstream `46ef6ed` plus the retransmit fix and its new deterministic
initial-loss regression, local tip `4952fae`. The test drops BEGIN or chunk zero
at clocks 0 and 1000, pumps real sessions, and compares the entire payload.
All four cases pass; unpatched upstream fails. CTest is 15/16: the existing
`rollback_episode_test` has the same four failures on pristine upstream, verified
separately (`net-ctest.log`, `net-initial-loss*.log`). The fix was already on the
fork's remote, so the requested upstream PR was opened without pushing:
https://github.com/RetroPortingToolKit/recomp-net/pull/12 . The new regression
commit is local and is not yet in that PR.

`retcomm-rbengine` is current upstream `a7b9850`; `recomp-ui` remains current
`8bf4738`. The nine runtime structural checks pass after adapting the setup-host
catalog guard to the private setup option (`runtime-tests.json` records the
initial guard failure; `mod-catalog-final.log` records its passing rerun).

The setup stage uses `RETCOMM_TOOLCHAIN_DIR` pointing at the locally installed
cmake-clang-v1/1.0.14 pack, disconnected FetchContent, and existing local libjuice
and libchdr sources. `PSXRECOMP_ALLOW_NO_BIOS=ON` permits the deliberately omitted
generated BIOS C in a setup-host-only stage. No dependency download occurred in
the staged configure. This machine lacked zip/7z, so packaging used a local
Python zipfile shim under the task scratch directory; no system installation or
release upload was performed.

Reproduction tools are `tools/upstream_regression.py` (explicit executable,
new scratch directory, seed, disc, menu reference and groups),
`tools/compare_upstream_regression.py` (80×60 image differences and card ranges),
and the existing pair/scenario with `NETPAIR_EXE`, `NETPAIR_DISC`, `NETPAIR_SEED`,
`NETPAIR_DIR`. Loopback routes now fail on timeouts, require composed captures,
select an occupied attack target, and leave attack selection before START.
The Free Duel route reads the current save's availability grid instead of
blindly selecting an empty tile. Diagnostic failed runs are retained rather
than overwritten. A same-revision slot-7 menu state can shorten follow-up
checks; do not reuse a state across regeneration.

An optional final hash read of the protected personal card was rejected by
automatic approval review and was not retried. Save-integrity evidence uses
only scratch seeds, final cards and their pre-netplay backups.
