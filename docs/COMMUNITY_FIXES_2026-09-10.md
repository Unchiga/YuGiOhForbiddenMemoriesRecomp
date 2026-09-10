# Community fixes qualification, 2026-09-10

This checkpoint continues `netplay-2p` from title commit
`93aba164bf77ec68bedb150bae92cc36b3f2a6c9` and psxrecomp commit
`c4a60fa16345983e555fd04adaba27530388d575`. All runs used the normal
checkout, the explicit USA cue, `PSX_PORTABLE=1`, a fresh explicit scratch
card directory, and a private debug port. Nothing was pushed or released.

## Implemented behavior

### File menu lifecycle

The File menu has two different actions when the in-process launcher exists:
`Quit to Launcher` ends the current game session and returns to that launcher;
`Quit to Desktop` emits the application exit. A `--no-launcher` session shows
only the desktop action. The soft return invokes netplay shutdown and title
stop hooks before destroying video, audio, input, save-state, menu, disc, and
plugin session state. Starting the next game initializes a fresh session.

Menu rows and whole menus now have reusable enabled state, disabled hints,
dim drawing, non-activating hit tests, and keyboard/controller navigation that
skips disabled targets. The menu policy selftest covers launcher and direct
layouts, both quit actions through keyboard/controller semantics and mouse hit
testing, plus disabled row and disabled menu behavior.

The real launcher opened with the verified disc and a scratch card directory,
as shown in
`/tmp/ygofm-community-fixes-2026-09-10/launcher-cycle-final/launcher-active.png`.
The desktop then auto-locked before a complete launcher -> game -> launcher ->
game recording could finish. That full physical desktop cycle, Windows, and
macOS remain unverified. Direct `--no-launcher` desktop exit and netplay's
graceful exit/carry-back paths were exercised.

### Stock netplay and hidden information

Netplay clamps requested and effective speed to 1x before the first simulated
frame. Turbo, accelerated loads, rewind/save-state mutation, custom cards and
effects, altered drops and story rewards, dialogue, fusions, CPU data, cheats,
Library filling, packages, and manager/editor write paths are inactive for the
session. GAME, CHEATS, and MODS are disabled as real input policy, and FM
Editor refuses every legacy and general entry path. Persistent offline files
are neither deleted nor rewritten.

The hand privacy overlay is now also applied to a non-owner's field cursor.
When the selected occupied field record is face-down, the other peer sees an
opaque `FACE-DOWN CARD` detail strip instead of the stock card name and stats.
The owner retains normal details. This is a present-only overlay; guest RAM,
VRAM, rollback state, and hashes are not changed.

Final policy evidence is
`/tmp/ygofm-community-fixes-2026-09-10/netplay-stock-final/stock-policy.json`.
Both peers booted at frames 8435/8436 with requested/effective speed 1,
GAME/CHEATS/MODS disabled, 18 mutation probes rejected, offline edit fixtures
still present, no active custom package, and no persistent fixture hash change.

Face-down evidence for both owners is in each final `privacy-results.json`:

- `/tmp/ygofm-community-fixes-2026-09-10/netplay-final-normal/`
- `/tmp/ygofm-community-fixes-2026-09-10/netplay-final-rollback/`
- `/tmp/ygofm-community-fixes-2026-09-10/netplay-final-loss-5pct/`

Player 2's card 137 is hidden from slot 0 and visible to slot 1; player 1's
card 261 is visible to slot 0 and hidden from slot 1. All three scenarios also
completed the duel, results, trade, graceful shutdown, and scratch-card
carry-back with backups. The rollback 35 ms/15 ms run logged synchronized
startup-FMV hold/desync telemetry at simulation frames 2775 and 7118, then
recovered and completed. The 5 percent loss run reported zero snapshot
overflow and completed with deliberate packet drops.

### Guaranteed story rewards and multi-drop order

An eligible campaign story reward is now a real first award for Card Drops
1 through 99. The remaining `N-1` awards use the configured normal rank table
and original RNG stream. The stock SPOILS card, trunk writes, New markers,
distinct result aggregation, pagination, and machine-readable order all agree.
First-win rewards fire only when the opponent was not already defeated;
every-win rewards repeat; Free Duel never fires or consumes one; no reward is
the stock path.

`/tmp/ygofm-community-fixes-2026-09-10/story-regression-v13/results.json`
contains passing live cases for counts 1, 2, 16, and 99, first/every modes,
Free Duel, already-defeated, and no-reward behavior. Every applicable case has
card 37 as award-order entry zero with `kind: story` and later entries marked
`normal`. A separate self-launch proof is under
`story-self-launch-final-v2/results.json`.

### FM Editor

VIEW exposes one `FM Editor` action. Cards, Drop Tables, Fusions, Dialogue,
and CPU are tabs in one SDL native window, not five OS windows. Mouse tab
clicks and Ctrl+1 through Ctrl+5 use the same page switch. The legacy debug
commands remain compatible by opening the shared window on their page. Page
selection, search, scroll, pending fields, modal/import state, and unsaved
edits survive a tab round trip. The editor closes and is unavailable during
netplay.

`/tmp/ygofm-community-fixes-2026-09-10/fm-editor-package-final-v2/results.json`
records all five pages and the return to Cards using window ID 6. An unsaved
3100 ATK edit and search survived the round trip and then saved. Software
rendering was exercised live for every page, including resize, synthetic
mouse/keyboard input, reopen, and package operations. Native tool-window
rendering remains the supported qualification path; the existing OpenGL
tool-window limitation was not represented as fixed. A 1920x1080 containment
audit is intentionally left for the next task.

### Restore all drops to stock

The Drop Tables page has `Restore All Drops to Stock`. It requires two
activations within ten seconds, states that Save is required, and removes only
randomized/manual weights for all 39 duelists, all three bands, and all 722
cards. Story rewards and card, CPU, fusion, dialogue, and other settings are
preserved.

The live scratch evidence under
`/tmp/ygofm-community-fixes-2026-09-10/drop-restore/` includes the randomized
export/round trip and restored exports. Seed 9534 changed every one of the 39
duelists while keeping each table valid; restore returned every weight to the
disc-derived stock hash, expired confirmation did nothing, save/restart stayed
stock, and story reward 37 plus unrelated CPU/fusion edits survived.

### Card descriptions

Generated code, stock strings, and live Library rendering establish an eight
line by 20 column display. Validation, preview, encoding, import, and save now
share one planner. `|`, actual newlines, and literal `\\n` are explicit line
breaks and do not silently wrap; text without explicit breaks uses greedy
20-column word wrapping, splitting a long word at the boundary. More than
eight lines, an explicit line over 20 columns, unsupported glyphs, or text
that cannot fit is rejected with a specific error instead of clipped.

The complete description bank retains exact stock byte strings and compacts
all 722 offsets into the proven primary string region plus the isolated
overflow arena. It skips the relocated card tables and reasserts an end marker.
This preserves large existing MOD packages while guarding adjacent memory.

`/tmp/ygofm-community-fixes-2026-09-10/card-description-final-v9/results.json`
passes 1/6/7/8-line acceptance, 9-line rejection, 19/20/21-column boundaries,
explicit and automatic wrapping, long words, unsupported glyphs, stock/custom
text, generated spell and monster wording, hot reload, text export/import,
restart, decoded guest bytes, and unchanged adjacent-region hashes. The live
eight-line Library image is
`card-description-final-v9/shots/library-card-1-eight-lines.png`.

## Build and regression summary

- Current recompiler tools, debug title, and release title build successfully.
  No generator source changed, so game/OpenBIOS regeneration was not required.
- `build-dbg/menu_preview --selftest`: PASS.
- Description boundary/live suite: PASS.
- Story reward suite: 7/7 PASS plus independent self-launch PASS.
- FM Editor and full manager/package round trip: PASS. The final full package
  after the description allocator correction is
  `/tmp/ygofm-community-fixes-2026-09-10/package-final-v3/full-coverage.ygomods`.
- All 20 executable effect regressions: PASS in
  `/tmp/ygofm-community-fixes-2026-09-10/effects-final/results.json`.
- Offline audio speed: PASS at 1x, 2x, 3x, 4x with 59.94, 119.87, 179.81,
  and 239.73 game fps and approximately 44.05 kHz SPU production, with no
  queue underflow/overflow. Evidence is `audio-speed-final/results.json`.
- Netplay normal, rollback 35/15, and 5 percent loss: PASS, including both
  face-down owners, result/trade continuation, and graceful carry-back.
- `Play.sh` resolves `build-dbg` normally and `build` with `-rel`; both files
  were rebuilt at this checkpoint.

The complete psxrecomp CTest run has 56/62 enabled tests passing and three
performance tests disabled. Six failures also reproduce from an archive of
the unmodified starting psxrecomp commit: dirty-text continuation source-shape,
reachable-discovery `/tmp/test.exe` fixture, AOT candidate-capacity expectation,
hybrid-input source-shape, launcher pad-mode source-shape, and launcher Vulkan
source-shape. They are baseline test debt, not claimed passes. The independent
static overlay compilation suite passes 10/10. Only the existing third-party
`stb_image.h` GCC string-overflow diagnostic appears in title compilation; no
new title-source warning was introduced.

## Known unverified platform behavior

The current Linux software-rendered live paths and Linux OpenGL gameplay paths
were tested. A physical controller was not available; controller activation is
covered by the menu's shared keyboard/controller handler selftest, not a real
device. Windows, macOS, Vulkan, real HiDPI hardware behavior, and a completed
post-lock in-process launcher cycle were not tested in this checkpoint.
