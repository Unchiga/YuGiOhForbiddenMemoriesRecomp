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

### Smart first normal drop

`MODS > Smart first drop` is an optional, persisted setting whose default is
Off. For the first normal reward only, it excludes weighted cards the player
already owns at least three of across the 40-card deck and trunk. It uses the
resident opponent/rank table after Drop Table edits and other transforms, then
rescales eligible weights to exactly 2048 with deterministic largest-remainder
rounding and card-ID tie breaking. The guest still performs its one ordinary
RNG call. The original table is restored byte-for-byte before later normal
awards, so their table and RNG stream are unchanged.

A guaranteed campaign reward retains position zero. With two or more total
drops, Smart applies to the first normal reward after it and sees ownership
after the guaranteed card was awarded; with one total drop there is no normal
reward to filter. If every weighted card is already owned at least three times,
the intentional deterministic fallback is one unfiltered roll from the
selected rank table. There is no retry loop.

Seeded coverage used 1, 2, 16, and 99 drops, ownership counts 0, 1, 2, 3 and
255, a deck/trunk split of 1+2, all three rank bands, duplicate weights, an
all-ineligible table, and 100,000-roll distributions. A controlled live Free
Duel used `1:2000, 2:48`: card 1 was present 40 times in the deck and card 2
was absent, so award zero was card 2 while the other 15 awards returned to the
original table (14 card 1, one additional card 2). The results page showed
Mystical Elf x2 and Blue-eyes White Dragon x14; card 2's New state was true and
card 1's false. Screenshots and the exact fixture are under
`/tmp/ygofm-smart-live-filter-EtFaWE/`. A separate live all-ineligible run
under `/tmp/ygofm-smart-live-duel-G29EGf/` took the unfiltered fallback and
rendered 16 distinct rewards across multiple result pages without hanging.

The setting survived save/restart and a MOD-package export, clear, import and
restart/import cycle. The old `full-coverage-2026-09-07.ygomods` fixture, which
has no Smart key, still imports and leaves the additive default Off. During
stock netplay the stored On preference remains configured, but effective state
and row-enabled state are both false; mutation is rejected on both peers and
the hostile fixture hashes remain unchanged after graceful shutdown. Evidence
is `/tmp/ygofm-smart-netplay-policy-SdVaeX/stock-policy.json`.
The consolidated machine-readable result, including exact seeded counts,
package hash, live award order and screenshot hashes, is
`/tmp/ygofm-smart-first-final-evidence/results.json`.

Adding the distribution probe exposed the framework debug-command registry's
silent 64-command ceiling. psxrecomp commit `adbef3b9` raises the fixed registry
to 128 and adds a capacity/dispatch regression, restoring all previously
registered title probes.

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

### Continuation: reward-only exports and equip authoring

The next authoring pass fixed the direct cause of guaranteed-story-only drop
exports being ineffective: the export predicate counted weight overrides but
not a scripted reward. Direct Drop Tables export now reports weight entries and
scripted drops separately, and both card-share and full MOD packages include
`drop_table_edits.ini` when a reward is the only edit. Reward-only, weights-only,
both, and neither were exercised. Card 37 in every-win mode survived inspect,
clear/import, restart/import and both package paths; the old committed
`full-coverage-2026-09-07.ygomods` fixture still imports. Machine-readable
evidence is in `/tmp/ygofm-drop-export-fix-e5qhaE/`. Commit `cb9a286` contains
that isolated fix.

The Fusions page now separates `Clear fusions...` from `Clear equips...`, with
the latter defined as every Equip card and guarded by an explicit modal. Each
Equip also has a searchable pending checklist with filtered mass add/remove,
clear, cancel and apply. It writes the existing `equips` card-pack field; no new
parser or package section was introduced. The guest equip hook now answers
explicit card-ID lists directly, so a 621-monster list cannot overflow the
fixed streamed equip buffer. A fully empty set of groups remains a valid live
Fusion Hint state.

The fresh portable software-rendered evidence in
`/tmp/ygofm-equip-bulk-final-Mp1v8d/` covers stock/empty/two/621 entries,
duplicate refusal, filtered removal cancellation, clear-everything cancellation
and confirmation, restart, direct card and full MOD round trips, the old full
fixture, and preservation of fusion recipes plus unrelated price/password/color
fields. In a live Free Duel, Legendary Sword accepted only configured card 1,
the guest lookup recorded the included pair, and the summoned monster displayed
the +500 ATK/DEF result; configured-excluded card 2 returned no equip result.
Because the shared card-effect hook changed, all 20 executable effect cases
were rerun and passed, including speed, queue saturation, and mid-cast restore;
the new result is `/tmp/ygofm-equip-effects-2026-09-10/results.json`.

### Card-view passwords

Password editing already existed on the Cards page. This pass adds the missing
presentation in the game's shared full-card view: deck building/chest, Library,
and duel inspection show the effective eight-digit password in compact white
digits at the bottom-right of the description panel. The overlay has no label
or background, fits below a complete seven-line description in both panel
layouts, preserves leading zeros, and does not write guest RAM, VRAM, saves, or
RNG state. Animated views expose it only when the front has settled; it is
absent during the incoming flip and from the first Circle exit frame. Library
uses the lifetime of its description object for the same open/close behavior.
It is registered below the netplay privacy cover and is disabled for a stock
netplay session.

The software present path now composites the same guest-overlay registry as
OpenGL and Vulkan, through the actual picture/letterbox rectangle and below
host menus and toasts. This fixed a real backend discrepancy found while
testing the password rather than making the password renderer-specific.

Live deck, Library, and duel lifecycle/geometry results are in
`/tmp/ygofm-password-display-evidence/password-contexts-final-v9.json`; the
seven-line visual montage is `password-white-right-seven-lines-montage-v9.png`.
A fresh restarted software-rendered process preserved `00001234` and recorded
the composed placement in `password-software-restart-v10.json`. Direct
`.ygocards`, full `.ygomods`, reset/import, old format-v1 input without a
password key, and hot reload all pass in the adjacent v9 JSON files.

The stock Password screen resolves duplicate passwords to the first, lowest
card ID: with cards 1 and 2 both set to `00001234`, it displayed CARD NUMBER
001. Existing imported duplicate files remain readable for compatibility, but
the Cards editor now refuses a newly entered duplicate and names its current
owner. Exact eight-decimal-digit validation remains enforced; leading-zero
input saves normally. Evidence is `password-duplicate-v10.json` and
`password-manager-validation-v11.json`.

### Field-spell creature allow-lists

Cards 330 through 335 now expose `Field creatures` on the Cards page. It is an
optional, searchable, batch-editable list of stable card IDs with individual
toggle, Select filtered, Remove filtered, Clear all, Ctrl+A/Delete, Cancel and
Apply. Apply remains pending until the normal Save, making clear-and-save the
intentional two-step path. An absent list preserves the disc's type rules
exactly; `field_targets = none` is a distinct explicit empty list. Existing
files need no migration, and the canonical key is additive to card.ini,
`.ygocards`, and `.ygomods`.

The stock path was traced to the signed 20-type by 6-terrain table at
`0x800909D4` and the dedicated volatile duel-row modifier at `+0x14`. The
runtime filters that modifier symmetrically for player and opponent without
changing base/permanent stats, the other modifier, flags/ownership, field
visuals, or other field spells. The current type's configured/stock amount is
still authoritative, including penalties. Hot removal and save-state load
restore stock values instead of leaving filtered rows stale. Stock netplay
continues to skip the entire card layer and refuses editor/debug mutation while
preserving the on-disk allow-list byte-for-byte.

`/tmp/ygofm-field-targets-ui2-ZmHSco/evidence/` contains the software-rendered
picker captures, empty/46-card save and restart, direct and full-package round
trips, malformed/partial/future cases, old fixture import, save-state removal,
and live duel results. A real Umi activation changed the visible board to SEA;
on both sides included card IDs 60 and 124 received +500 and -500, while
excluded same-type IDs 230 and 275 received zero. Explicit empty yielded all
zeroes and absence restored stock `+500,+500,-500,-500` per side. A current
build also imported and displayed all 722 IDs, preserved them on Cancel after
a pending clear, and rejected mixed `none, 60` before changing files. All 20 shared
effect cases passed in `/tmp/ygofm-field-targets-effects-2026-09-10/results.json`.
The isolated stock-netplay check passed with unchanged card 334 hashes on both
peers in `/tmp/ygofm-field-targets-netplay-policy/stock-policy.json`.

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

### Clear and rebuild one drop table

The Drop Tables page's `Clear...` menu clears the selected duelist's S/A POW,
B/C/D, S/A TEC, or all three bands. It does not mean every duelist, and each
choice requires selecting the identical action twice within ten seconds. The
selected bands become a truthful empty authoring canvas while story rewards,
other bands, and other duelists remain unchanged. The first added card receives
the whole 2048 pool; later adds and weight edits rebalance the remaining cards
proportionally. `Restore this duelist to defaults`, the existing global restore,
or Randomize exits the empty/replacement state.

An all-zero stock table was not safe to apply. The disc roll loop is bounded at
722 entries and returns card ID 0 when none has weight, but its award/results
callers assume IDs 1 through 722. Empty is therefore a pending editor state:
Save, direct Drop Tables export, `.ygocards`, and `.ygomods` export all reject it
with the duelist and band named. The running duel retains or reconstructs the
last safe stock/mod table, never normalizes the editor back to stock, and hot
import, clear, restore, and removal now reconcile the resident table without
requiring a new duel.

Saved rebuilt bands use additive drop-table `format = 2` keys such as
`pow_table = 1:2028, 2:20`. IDs must be unique, weights positive, and the exact
sparse list must total 2048. Old unversioned vector/reward files remain valid;
future versions, empty exact lists, malformed numeric rows, duplicates, and
wrong totals are rejected transactionally.

Software UI, restart, invalid/old/direct round trips, and menu captures are in
`/tmp/ygofm-clear-drop-live-zyg2yP/evidence/`. Package, randomize/restore, old
full-fixture, and real-duel evidence is in
`/tmp/ygofm-clear-drop-duel-nT1Vp9/evidence/`. In the live duel a rebuilt
Duel Master K POW table was exactly `{1:1500, 2:548}` in guest RAM and awarded
card 2; clearing that same band during a duel restored the safe 136-entry stock
table, preserved story reward 92, and completed with a valid stock card rather
than card 0 or a hang. The final direct `.ygocards` and malformed embedded-file
preflight (with unchanged save/config hashes) are in
`/tmp/ygofm-clear-drop-cardshare-Grh9IH/evidence/`. A final shipping-event-path
test selected the toolbar menu twice and then rebuilt POW with card 1 at 2048;
its export contained only the exact POW key and no redundant pins for the other
bands. That state and software capture are under
`/tmp/ygofm-clear-drop-final-WRiHso/evidence/`.

### Sell Extra Cards

The optional Card Shop now exposes `Triangle: Sell Extras`. Its preview lists
every affected card, the number sold, the number retained, the effective
per-card password-screen value, gross value, actual credit, final starchips,
and any value lost to the 999,999 cap. Up/Down browses the complete list,
Triangle opens the selected card's ordinary full-card view, Circle cancels
without mutation, and Cross performs the confirmed batch. A fresh preview is
required if deck, trunk, starchips, or a listed price changed while it was open.

Forbidden Memories removes deck copies from the trunk, so the shared inventory
backend counts the 40 live deck slots plus all 722 trunk bytes. For each card,
the sale retains `min(3, deck + trunk)` total copies and removes only surplus
trunk copies; it never edits or reconstructs the active deck. If the deck
already contains three copies, all trunk copies are extra. Sale value is the
current Cards-page/password price (including a live card.ini override), and a
zero-value extra is still removed after being shown as zero. Gross arithmetic
is 64-bit; credited and stored starchips are capped at 999,999 with the lost
amount stated before confirmation. The player must use the game's normal Save
flow afterward. The feature has no persistent setting and every mutation path
is rejected during stock netplay.

Fresh portable software-rendered tests used isolated card images under
`/tmp/ygofm-sell-extras-ui-lRVRCb/`,
`/tmp/ygofm-sell-extras-restart-gYdOgb/`, and
`/tmp/ygofm-sell-extras-shipping-ui-C1iu9x/`. They cover cancellation, stale
preview rejection, a zero-price override, deck/trunk splits of 0/3 through
3/255, cap saturation, no extras, card viewing, the actual Card Shop controls,
the two game Save confirmations, and restart. The restarted save retained the
original 40 deck slots, all 722 cards had exactly three total copies, the shop
showed 999,999 starchips, and a new preview reported no extras. An all-722-card
stress confirmation sold 181,984 trunk copies from 722 card types with gross
value 30,816,957,390, capped safely, preserved the deck, and again left exactly
three total copies of every card.

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
