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

Current-binary policy evidence is
`/tmp/ygofm-netplay-sell-final-20260911/stock-policy.json` (SHA-256
`de9c0dc7accd00ee74165ba5642120eaf0eae63ba00685db0843ce0c26513de9`).
It binds final debug binary SHA-256 `5ed5fa5805fa2b3eb91f76689585ba994d2debcaa78b172f261e58e143cbfc4a`;
the Sell confirmation probe was rejected on both peers during stock netplay.
Both peers exited 0 with requested/effective speed 1. Mutation probes were
rejected, except that a 4x request was accepted only as a clamp back to 1x.
Offline Smart/zero-drop/starchip/CPU/package fixtures stayed loaded but
inactive, with no persistent fixture hash change. The owned PIDs were
3270986/3271034 on verified debug ports 54401/54402 and UDP ports
54403/54404.

Face-down evidence for both owners is in each current `privacy-results.json`:

- `/tmp/ygofm-final-netplay-20260911-normal-root3/`
- `/tmp/ygofm-final-netplay-20260911-rollback-root2/`
- `/tmp/ygofm-final-netplay-20260911-loss5-root6/`

All three scenarios completed both owner directions, the duel, results, trade,
graceful shutdown, and scratch-card carry-back with backups. Normal scenario
and privacy SHA-256 values are
`1a77c6c8cd03624ab2c440cc117392872ea6d9da2057a28f3d1d9e64c7a076d3` and
`70170567ade9dbe3b7c6e4aa4155a825b10f7dc8e770feb7338d3a501150cd27`;
rollback 35/15 values are
`da37f85fa52baae1397dfea72af589a4b1ef463b0db1531f250e21f07d9fdabc` and
`8b5b64aab09a6f479f423c232e2a48ced3ae711ac51ef31adad8ff48756f11bb`;
seeded 5 percent loss values are
`dda29dd07a8b8bd97b47acda5ed979eb60271a22382bbc265afb191d877179c0` and
`8b5b64aab09a6f479f423c232e2a48ced3ae711ac51ef31adad8ff48756f11bb`.
The rollback simulator held 54,944/54,079 packets with zero overflow and peaks
120/123. The loss run deliberately dropped 5,847/5,861 packets with zero
overflow and peaks 12/15.

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

### Zero normal-card drops

`MODS > Card drops` accepts 0 through 99, with a stock notch at 1. Zero is an
intentional no-normal-card outcome, not an empty-table roll: the results
routine performs its one unavoidable in-flight stock roll (exactly one LCG
advance, with no retry), then the function-entry replacement skips the award.
No card 0 is invented, the trunk and recent-card ring stay unchanged, and the
per-duel New/result lists are cleared. A present-only panel covers both the
unearned stock card number and name with `NO NORMAL CARD DROP`, so SPOILS does
not claim that the rolled-but-discarded card was earned.

Guaranteed campaign rewards remain separate. If one is eligible while Card
Drops is 0, the same in-flight roll is steered to that card and it is awarded
once; Free Duel and a gated/already-consumed reward still award no card. Smart
Drops remains saved but reports ineffective at zero because there is no normal
award to filter. The zero/no-reward, zero/Free-Duel, zero/first-win, and
zero/every-win cases are part of `tools/story_reward_regression.py`; each live
zero-count case checks the hook-scoped pre/post seed for the exact single-call
RNG result, while no-reward cases require a byte-identical 722-card trunk,
recent-card ring, and card-0 sentinel.
The shared no-card presentation also covers a Smart Drops final-roll
suppression with `NO ELIGIBLE CARD DROP`; an already-awarded guaranteed story
card remains visible because its SPOILS entry is truthful.

Live zero-count evidence was recorded under
`/tmp/ygofm-zero-opaque-live-MD6dL1/evidence-no-reward/` and
`/tmp/ygofm-zero-final-live-oCdvRb/evidence-{first,every,free-duel}/`.
All four cases passed. The final no-reward screenshot hash is
`9dd67c74ae9b815d9a6af4c2eab285ed6b1fb3b0ce078fda9a1e1b841ed0f44e`;
its opaque panel covers the complete stock card number/name area. Its card-0
sentinel stayed `2`, and the before/after trunk and recent-ring hashes were
identical. Both guaranteed modes committed card 37 exactly once; Free Duel
and no-reward modes committed no card.

### Smart Drops

`Smart drops` is now the Drop Table Manager's optional, persisted authoring
toggle, default Off. It applies to every normal reward, not only the first. At
each position it excludes weighted cards already owned three times across the
40-card deck and trunk, including rewards committed earlier in the same duel.
Surviving weights are rescaled to exactly 2048 with deterministic
largest-remainder rounding and card-ID tie breaking, then the resident band is
restored byte-for-byte. The selected rank table and one normal RNG call per
position remain authoritative.

A guaranteed campaign reward is separate: it retains position zero and is not
Smart-filtered; every following normal reward is. Once no eligible weighted
card remains, that position consumes its ordinary RNG call but awards nothing.
There is no retry loop and no unfiltered duplicate fallback. The results page
uses `NO ELIGIBLE CARD DROP` only when the final suppressed stock carrier would
otherwise display a card that was not earned; a truthful guaranteed story card
remains visible.

The setting is additive `smart_drop = on|off` in drop-table format 3, so direct
Drop Tables exports and `.ygomods` packages share it even when there are no
weight edits. An absent key keeps the stock-compatible default Off; explicit
Off can replace a recipient's earlier On. Old packages whose transient setting
was named `smart_first_drop` migrate on import. Stock netplay preserves the
offline file but disables the filter and rejects mutation.

The final aggregate is
`/tmp/ygofm-smart-independent-aggregate-final4-20260911/aggregate.json`
(SHA-256 `8d7c483b9f985b624d00274794d6c84a2803509ad5ec79093f80c95879240948`).
Independent real-duel processes cover counts 1, 2, 16 and 99; ownership 0, 1,
2, 3 and many with deck/trunk splits; duplicate weights; all-ineligible pools;
story-first ordering; exact resident-table restore; New markers; and result
pagination. The 99-drop case exhausted its 40 available copy slots and safely
skipped the remaining positions. Seeded 100,000-roll checks on each imported
rank band produced `49780 / 25185 / 25035` for weights `1024 / 512 / 512`.
The same three-band distribution was repeated against the final debug binary
in `/tmp/ygofm-smart-distribution-provenance-20260911/results.json` (SHA-256
`42eb6501c0e545e2cb029989326405d7e882888e3a97e3c3ff822dd1b4a0f966`).
It records executable SHA-256 `419cb795...`, the explicit USA cue hash, PID
3162412, port 52677, and a successful `quit_graceful` exit code 0.

The live fixture's displayed S-TEC result nevertheless reached the guest hook
as effective tier 0. Real-duel filtering is therefore demonstrated at tier 0;
tiers 1 and 2 are covered by format-3 import/export and the same deterministic
host-side distribution implementation, not claimed as separate live rolls.

Adding the distribution probe exposed the framework debug-command registry's
silent 64-command ceiling. psxrecomp commit `adbef3b9` raises the fixed registry
to 128 and adds a capacity/dispatch regression, restoring all previously
registered title probes. Follow-up psxrecomp commit `7d5f0205` adds the opt-in
generated/interpreted function-replacement hook used to skip a suppressed
award without patching guest text or code.

The legacy `card_drops_test` and `card_drops_sim` commands were also retired:
they recursively ran guest code from the debug callback and could consume
interrupt/SIO event state, eventually stranding the BIOS. They now return a
clear error. Real-duel fixtures cover award behavior; the Smart distribution
probe is host-only and non-mutating.

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
rendering remains the supported qualification path.

The follow-up 1920x1080 audit exercised the complete compositor-managed FM
Editor at 100, 125, 150 and 200 percent desktop scale. Initial placement,
minimum and deliberately oversized resize, maximized state, close/reopen, all
five tab canvases, native KDE file pickers, long validation text, confirmations
and the field/equip batch pickers remained reachable. The minimum client is
720x458; on the 200 percent 960x540 logical desktop it is reduced only as far
as the actual work area permits. The Cards layout can scale below one design
unit so its Password, Frame, Name color, actions, status and help do not clip.
Tab changes retain one native window ID and preserve unsaved page state.

SDL/KWin Wayland reports neither its server-side Breeze decoration size nor
the bottom-panel work area. The fitter therefore reserves measured
conservative logical extents (32 top, 4 on the other decoration edges, and 48
for system UI when SDL reports bounds == usable bounds), plus a horizontal
placement gutter. The debug state reports both SDL's raw values and these
effective estimates. An oversize request also exposed KWin moving a full-width
window to the larger adjacent output; sizes are now clamped against the
pre-resize display before they reach the compositor.

The page windows use a software SDL renderer by default, including when the
main game uses OpenGL. Testing found that renderer creation can still disturb
the current main GL context; all five pages now restore it. This removes the
previous black tool-window failure, and both software-main/software-tool and
OpenGL-main/software-tool combinations rendered live. Current OpenGL caveat:
the editor itself is still intentionally a software tool renderer rather than
a native OpenGL tool surface.

Machine-readable geometry and canvas/native captures are under
`/tmp/ygofm-fm-editor-1080-final/`. Final repeated page/oversize runs are in
`scale-125-final/results.json` and `scale-150-final/results.json`; 100 percent
minimum/maximized coverage is in `scale-100-v2/results.json`, and the tightest
200 percent coverage plus all modal/file-dialog/lifecycle cases is split
between `scale-200-final/results.json` and the JSON files in `scale-200/`.
`kscreen-original.txt` and `kscreen-restored.txt` are byte-identical, proving
the exact pre-test monitor modes, scale, priority and placement were restored.
`tools/fm_editor_desktop_regression.py` makes the five-page containment and
oversize check repeatable without changing display configuration itself.

A later evidence audit caught a real false positive in the retained 200%
five-page result: Fusions reasserted a 458-pixel client minimum, leaving its
decorated outer height (and the following tabs) at 494 against a 492-pixel work
area, while `fits_usable` omitted the top/left borders. Both defects are fixed.
The provenance-bound rerun used an isolated KWin virtual output rather than
changing the restored CRT and is under
`/tmp/ygofm-fm-editor-virtual-200-provenance-20260911/`: all five pages and the
oversize case report outer 928x492 within usable 960x492, one window ID, and
nonempty native captures. `geometry/results.json` SHA-256 is
`ce3ec23fe57b82e86ca394350e541cd192cce564acd54711cfca5f3f26831553`;
it binds debug executable SHA-256
`419cb795feee79d2574f5715565198852c0f3d8774826c8d892eaab661e6c454`,
and the owned process exited 0 through `quit_graceful`.

The navigation/audio follow-up instrumented each page with cumulative tick,
SDL-event, present, present-time/max, tab-layout, tab-upload and failure
counters, alongside the runtime's guest work, renderer presents, SPU
production and underruns. The main cause was host work on the emulation thread:
the shared tab strip was laid out and uploaded on every otherwise necessary
page present, inactive editor pages continued blinking their carets, and the
CPU page repainted every 500 ms even when the 39 records had not changed.
Tabs are now cached by renderer/width/page, caret animation stops when the
editor lacks input focus, and the CPU poll dirties the page only when a hash of
the live records changes. Existing generation guards continue to prevent list,
preview, file and texture rebuilds when their inputs are unchanged.

At 2x after the change, idle per-page samples recorded Cards 10 presents in
420 ticks, Drop Tables 8/425, Fusions 1/433, Dialogue 1/433 and CPU 8/409.
Every page performed exactly one tab rebuild/upload and reported zero renderer
failures. Controlled Cards and CPU navigation ran at about 120 fps on OpenGL
with zero underruns during those intervals. The forced OpenGL tool renderer
also sustained requested/effective 1x, 2x, 3x and 4x at 60, 120, 180 and
239.7 fps without speed easing; short boundary samples still saw isolated
device underrun bursts (754 at closed 2x, 434 at closed 3x, and 839 at open
4x), so the result is not presented as universally underrun-free.

The software main renderer remains the honest limitation on this i7-13700K /
RTX 4090 host: it holds 1x, but the measured 2x-4x samples reached only about
98-121 fps while SPU production fell below 44.1 kHz and audio underruns
continued, whether the editor was closed or open. No added latency, silent
speed reduction, or fallback easing was introduced. The editor still defaults
to its broadly compatible software renderer; `PSX_TOOL_RENDERER=opengl` is a
tested diagnostic/optional path, not a new default.

Machine-readable results are
`/tmp/ygofm-editor-after-P2CAoY/software-after-2x.json` (SHA-256
`b604c6a95f5124a4fb0761ef9a544f50ebc3b0ba314e5289c72cb9629bc04bcc`),
`software-speed-sweep.json` (`898901e37b5cb854d23d814b99a888dee592815b6b4b501930a45aaa51c35e52`),
`/tmp/ygofm-editor-gl-CZ6mDa/opengl-after-2x.json`
(`a25d589dfcc68c25a6255cabac13fc578b1663548adcd9e22e9d062f3465bbd6`)
and `opengl-speed-sweep.json`
(`1b3c9931efcda6d50921113e3db68d5a4ca0fb2ef3e4c40f6183ff0da4716bd5`).
Both owned processes used copied slot-0/card fixtures, explicit portable data
directories and verified ports, and exited cleanly.

The final all-page harness rerun is
`/tmp/ygofm-editor-final3-20260911/profile/results.json` (SHA-256
`3c911b8a146a65adf1cc88c352b89fe5758ecbacfc0fe4ed81003a92672d1694`).
It binds debug executable SHA-256 `5ed5fa5805fa2b3eb91f76689585ba994d2debcaa78b172f261e58e143cbfc4a`,
one editor window ID, nonempty captures for every page, and at most one tab
upload per steady-state page sample. At requested 2x, closed, Cards and CPU
held 119.99 fps with zero underruns and Drop Tables held 119.50 fps with zero
underruns. The same sequential run caught transient host/audio shortfalls on
Fusions (111.50 fps, 5,092 underruns) and Dialogue (106.00 fps, 10,361
underruns), despite only one 1.8/1.6 ms editor present in each interval. That
distinction is recorded rather than masking the remaining runtime/device
variability. The owned process on port 4395 exited 0 through `quit_graceful`;
the copied state hash is `989d1d280887fe8c683fda50999bcc50f7a21f8f62c9720d00d0d4b71499e560`.

### Free Duel collection progress

`MODS > Free Duel progress` controls the entire addition: both the selected
opponent's fraction and every animated portrait frame. It defaults Off for a
stock-compatible launch. The `free_duel_progress` value persists in
`menu_settings.ini` and travels through the existing `mod_settings.ini` entry
in `.ygomods`; turning it Off never deletes the preference or any Drop Table
edit. Turning it On while the grid is already open rebuilds immediately.

The upper-right of the native `FREE DUEL` title now shows the selected CPU's
unique owned / obtainable cards. Obtainable is the union of nonzero card IDs
from all three effective rank bands after Drop Missing Cards and valid saved
Drop Table overrides. Owned intersects that set with the live deck and trunk;
the game has no per-opponent acquisition history, so this is current collection
progress rather than drop provenance. A genuinely empty effective union is
`0/0` and is never complete.

Owning the full set overlays the project owner's eight supplied 48x48 glow
frames on the stock-visible portrait. The bake is lossless ARGB, every original
PNG hash and all 2,304 pixels per frame are verified by
`tools/verify_portrait_borders.py`, and the animation is present-only. Build
Deck/empty cells show no ratio, scrolling follows real D-pad navigation, and
stock netplay hides the complete overlay because local inventory and authoring
state are intentionally unavailable there.

The fraction is now eight guest pixels farther right beside the FREE DUEL
title. Empty/unavailable cells and Build Deck clear the prior CPU name,
fraction and completion state immediately; returning to an opponent restores
its current backend-derived fraction on that accepted cursor move.

The supplied slot-0 state exposed that the old overlay used byte
`0x8009B32E`, which is the last valid duelist rather than the live cursor, and
reconstructed a three-row window from that stale selection. The stock grid
instead publishes pending cursor column/row at `0x8009B36C/D` and its exact
pixel scroll tween at signed halfword `0x8009B148`. The overlay now uses
those values, draws each completed global cell at
`40 + global_row * 52 - scroll_y`, and clips to the stock grid viewport.
Thus a border travels with its portrait during a tween, disappears when the
portrait leaves the viewport, and returns at its proper coordinate. A debug
bitmask records every independently visible completed cell.

The completion cache follows the Drop Table backend's generation counter. In
the final isolated live regression, Duel Master K changed from stock `12/157`
at generation 2 to `0/1` at generation 3 immediately after importing three
one-card bands. Stock was restored while the toggle was Off; switching it back
On immediately returned `12/157` at generation 4. A later ownership fill made
all 39 opponents complete. Off reported no visible overlay, zero borders and
no stale border coordinates; On rebuilt the current ratio/frame. The same run
saved `free_duel_progress=1`, found it in an exported `.ygomods`, changed Off,
and restored On by importing that package.

Evidence is `/tmp/ygofm-fd-toggle-20260911-d/results.json` (SHA-256
`d8b446a48f351e5dcad85781644bcba364782ef0e934cb18d1d08a545d72aa4d`);
the package SHA-256 is
`ce684ac397f89fcbd6f9f999ef69933342c9601566315720a5ec51ab52af1fdd`.
The owned PID 3222206 exited 0 through `quit_graceful` in a 960x720 virtual
KWin desktop; the physical CRT configuration was not read or changed.

The isolated software-rendered live result is
`/tmp/ygofm-fd-completion-20260910-f/results.json`. Real input reached Duel
Master K at scroll row 5; the copied-save state showed `12/157` with no frame,
then a scratch-only inventory fill produced `157/157`, a frame aligned at guest
`[244,144]`, and distinct animation frames 2 and 3. PID 3052468 shut down
gracefully; no personal card or monitor configuration was used.

The focused supplied-state rerun is
`/tmp/ygofm-fd-scroll-baseline-PVNbW8/fixed-sequence.json` (SHA-256
`c141bf3b11705e804bf19650c2c5623c6fe40fda628faef0c4ccb520db2968f1`).
Its copied state SHA-256 is
`989d1d280887fe8c683fda50999bcc50f7a21f8f62c9720d00d0d4b71499e560`.
Simon starts complete at `[75,40]`; Right to the blank cell immediately
reports selected -1, Left restores Simon, row 3 changes scroll to 52 and
removes Simon's border, row 7 reaches scroll 260 with Duel Master K at
`[244,144]`, and every upward step reverses correctly. The source state and
normal memory-card directory were never mounted by the game.

The generalized software regression then traversed every row upward and
downward, checking each visible-cell bitmask and final guest coordinate at
every settled scroll, plus drop edit/clear, ownership changes, toggle,
animation and package restoration. It exited 0 through `quit_graceful`;
evidence is `/tmp/ygofm-free-duel-final3-20260911/results.json` (SHA-256
`acd8bfed5d01457c25e8c6f30b343e11130613f2546bac696ab77ef2ab276049`).

### CPU rename propagation

CPU renaming was already supported by the CPU page and native Free Duel string
table. The display-name path is now single-sourced and JSON-safe across CPU
status, Drop Tables sorting/selection, story rewards, Drop Missing Cards,
portrait feedback, debug responses, and the starchip opponent picker. A rename
invalidates those live lists immediately, including quoted names. Persisted
drop/starchip conditions continue to use stable numeric opponent IDs and stock
section keys, so a later rename cannot retarget a rule.

The editor accepts at most 20 glyphs from the game's supported set: space,
ASCII letters and digits, plus `. ! ' , ? - # " & / : ( ) $ * > < + %`.
Empty restores the stock name. Package round-trip coverage includes a quoted
name, clear/import, a second export, native Free Duel lookup, Drop Tables, and
the starchip label while confirming that the saved starchip rule remains
`opponent = 2`.

### Conditional duel starchip rewards

Open the editor through `MODS > FM Editor`, choose `Drop Tables`, then press
`Starchip rewards...`; this opens the `Duel starchip rewards` modal. The Drop
Tables page owns an ordered, first-match-wins reward-rule editor.
Each condition can be Any or one campaign/Free Duel mode, opponent ID,
win/loss outcome, and D-through-S TEC/POW rank. Earlier rules have priority;
the UI supports add, remove, reorder, two-step clear, Done-without-save and
Save-and-close. With no matching rule the guest's exact stock 1-through-5
calculation and write remain untouched.

The live hook snapshots the pre-stock 32-bit total at results entry, then
replaces only a matched award with `min(before + amount, 999999)`. Accepted
amounts are 0 through 999999; parsing rejects unsupported values before state
changes. A present-only compact star-times-number row covers the stock star
loop and fits every six-digit value without clipping other results. The whole
100x16 guest overlay is now at `[152,184]`, exactly 16 pixels right of its
former origin with unchanged Y, and every background pixel is opaque black so
the stock one-to-five-star row cannot bleed through. Values 99,999, 100,000
and 999,999 fit the same six-digit-safe canvas; award arithmetic still caps
the saved total at 999,999. Both
mutation and presentation are disabled in stock netplay while the offline
rules remain saved.

The in-progress results decision is mirrored into guest-backed savestate data.
Loading after the rule has applied restores the decided flag, pre-award total,
matched rule and capped result, so the next results frame cannot add the same
reward twice. Loading an older state with no marker starts with no cached
decision. Leaving results clears every per-duel selector and amount, including
the debug state for an unmatched stock fallback.

Actual-result evidence covers campaign and Free Duel wins plus a genuine
CPU-caused loss. The retained boundary duels all resolved S-TEC; other rank
selectors are covered by rule-editor/import checks rather than claimed as
separate live duel outcomes.

The corrected focused evidence is
`/tmp/ygofm-final-starchip-followup2-cpu-20260911/results.json`. A matched 777
reward changed 100 to 877, survived a save/load after an intervening mutation
to 43,210, and remained applied exactly once for another 420 result callbacks.
A nonmatching rule kept 500 with no overlay; an explicit amount zero canceled
the stock increment and kept 700 while rendering `x0`. That result is 6/6 PASS
(SHA-256
`b6279fc1c670141012b6ab4238aca415398323036fed1e45b5b81ed07664dbe4`).
The preceding `/tmp/ygofm-final-starchip-savestate-cpu-20260911/results.json`
is retained as a failed harness attempt: it asserted the pre-load debug cache
instead of the guest-backed state after loading and is not counted as a pass.

Rules and the Smart toggle are additive sections in `drop_table_edits.ini`
format 3. This keeps weight tables, scripted story rewards, Smart filtering
and starchip rules in one validated backend for direct export, card-share and
full `.ygomods` packages. Old formats remain importable; malformed known
fields and future versions fail transactionally.

Both container readers now inflate and CRC-check every archive member before
any manager is allowed to mutate state, including unknown additive entries.
Full MOD-package import also snapshots every managed section and restores that
snapshot if a later manager rejects an otherwise intact payload. Thus a bad
late fusion section cannot leave earlier card/drop changes applied, and a
damaged late image cannot partly replace a card set. Format-3 `smart_drop` is
authoritative when a transitional package also contains the older
`smart_first_drop` setting; packages containing only the old key still migrate.

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

The independent final-binary sweep is
`/tmp/ygofm-equip-bulk-provenance-v5-20260911/results.json` (SHA-256
`46bc3a230dd9e07a7dae77f5f6064873cdeaa4954b0ae4dae2ee435fe4250d94`).
It passes 51 executable checks across stock, empty, two-card and 621-card
lists; duplicate refusal; searchable picker add/remove/clear/cancel/apply;
two-step global equip clearing; exact 25,146-recipe preservation; direct and
full-package round trips; old-package import; and a two-process restart. Both
owned software-rendered processes used separate verified ports and exited 0
through `quit_graceful`. The historical in-duel slot does not retain an
initialized Fusion Hint cache, so that new harness records the probe as
unsupported; the live included/excluded resolution remains established by the
earlier screenshots and guest lookup above. Ctrl+A is likewise not claimed by
the debug injector, which cannot express modifier state; the shipping picker's
Select filtered button, Delete path, and all mass operations are exercised.

The later Restore-stock correction closes the split-backend gap: the visible
Fusions-page action, Ctrl+Z and scripted confirmation now restore both the
ordinary fusion table and per-card Equip usable-monster overrides. The button
is named `Restore stock...`, and its confirmation counts both kinds explicitly.
Cancellation still changes nothing. The 61-check isolated regression at
`/tmp/ygofm-equip-restore-fix-results/results.json` (SHA-256
`0c57f24533d9f85895b9893b3ec1a47c21693ead48843c8f9150afddc66d32cc`)
staged one fusion edit plus an empty Legendary Sword list, then restored the
exact stock fusion-table hash and all 63 stock equip links while preserving
price, password and color fields. Both owned processes used fresh verified
ports and exited 0 through `quit_graceful`.

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

The placement follow-up moves only that shared footer: eight guest pixels left
and two down. Animated deck/chest/duel/shop views now use origin `[280,205]`;
Library uses `[280,185]`. Its 31x5 white canvas, eight-digit/leading-zero
renderer, seven-line clearance, settled-front gate, and first-frame exit
hiding are otherwise unchanged.

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

Current rebuilt-band exports use additive drop-table `format = 3` keys such as
`pow_table = 1:2028, 2:20`. Format 2 is the compatible older sparse form. IDs
must be unique, weights positive, and the exact sparse list must total 2048.
Old unversioned vector/reward files remain valid; future versions, empty exact
lists, malformed numeric rows, duplicates, and wrong totals are rejected
transactionally.

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

The consolidated final-binary regression is
`/tmp/ygofm-drop-clear-provenance-v6-20260911/results.json` (SHA-256
`b72a9d72549bacbbbdf6006655a299495342260bb8d3745a28938acb1b5cab94`).
It repeats each individual band and all-three two-step clears, every empty
Save/export refusal, first-card rebuild, seeded Randomize, per-duelist and
global restore, byte-exact restart, format-2/unversioned compatibility, and
transactional malformed/future rejection. Its real Free Duel awarded card 1
from an exact `1:2048` rebuilt table, changed only that trunk byte, and left
the card-0 sentinel at 2. The copied historical near-results state carried
non-stock resident weights, so the harness explicitly normalizes only those
fixture rows to the exact current baked stock hashes before invoking the
production edit backend; it never writes an edited table directly. Both
software-rendered owned processes used fresh verified ports and exited 0 via
`quit_graceful`.

### Sell

The optional Card Shop now exposes `Triangle: Sell`. It lists every card with
a nonzero trunk count, even when the player has fewer than three total copies.
Up/Down browses; Left/Right changes one; L1/R1 changes ten; Square toggles the
selected maximum; Start selects or clears the complete trunk; L2 clears and
R2 selects everything; and Triangle uses the ordinary full-card viewer.

The panel was relaid out on 2026-09-11 for the native 320x240 screen, where
the game's own font holds about 31 capitals per line. The header carries the
current StarChips; three totals lines give the gross sale value, the credited
value beside the value lost at the 999,999 cap (red when nonzero), and the
balance after the sale. The table header is `CARD n OF m / TRUNK / SELL /
PRICE` over five rows around the cursor, with column ticks in the gutters, a
gold cursor bar, a gold outline on the editable SELL cell, and a scrollbar
once the trunk holds more than five cards. Derived prices are gold, override
prices blue, and zero prices grey. Six-digit prices now end inside the panel
(the earlier layout right-aligned them at x=318 on a 304-pixel canvas and
clipped them). A strip under the table spells out the selected card in full:
name, `No.NNN`, deck count, trunk copies kept, and `DERIVED` or `OVERRIDE`,
followed by the key line and the VIEW / REVIEW / CANCEL buttons. Refusals
and notes (nothing selected, stale inventory or prices, going back from the
confirmation) replace that strip, word-wrapped and centred on up to three
lines, until the next press; the old panel never drew them at all.

Circle is always non-mutating. The first Cross enters an explicit confirmation
screen and a second Cross is required to apply. The confirmation is visibly a
different screen: a red `Confirm Sale` title, the same totals, a dark-red band
`SELL n COPIES OF m CARDS?` / `THIS CANNOT BE UNDONE.`, a `CARD / SELL /
VALUE` list of only the selected cards, one status line (deck unchanged, no
sale value, or cap loss), and only SELL NOW / GO BACK. Circle there now
returns to the editor with every quantity kept; it used to fall through to
the editor's Circle and cancel the whole visit. An empty trunk gets a
three-line explanation with BACK, and a completed sale returns to the pack
panel with `SOLD n COPIES +c CHIPS` in green instead of red. Confirmation rechecks all 722
trunk bytes, all 40 deck slots, StarChips, and every snapshotted sell price.
Any stale value cancels the transaction and requires a fresh review. The
shared commit validates every target before its first write, updates live and
save-mirror storage together, and never removes or reconstructs a deck card.
Selection/review state is harmless during netplay; every selling mutation is
rejected while the saved offline Card Shop setting remains intact.

Sell price is a single additive `sell_price = 0..999999` field in
`card.ini`. When absent, the backend uses deterministic floor division of
the effective integer password purchase price by eight (about one eighth of
the stock password price). The purchase sentinel 999,999 is explicitly capped
to a 500 sell price. The Cards page reports
`derived` versus `override`, and Restore stock deletes the override and
returns to the derived stock-compatible value. Hot reload, direct card shares,
full `.ygomods`, and the shop all consume this same backend. Old card files
need no new key; invalid known values are rejected during archive preflight.
Zero is a valid explicit price. Gross uses 64-bit checked arithmetic and
credited/stored StarChips saturate at 999,999.

The final isolated software run is
`/tmp/ygofm-sell-final3-20260911/results.json` (SHA-256
`3effc78f9f09daf62da78fee064396bf9ea2670c2184a1072c23970fd767160b`).
All 77 checks pass, including
one/partial/255 quantities, Cancel, mandatory review, stale inventory and
hot-reloaded price rejection, zero-value removal, cap saturation, all 722
cards and 184,110 trunk copies, an intentionally greater-than-32-bit gross,
sell-price-only export, clear/import, malformed/future rejection, `.ygomods`,
Cards-page Restore, native Save/Overwrite, and a separate-process restart.
Both owned processes (PIDs 3266261/3266780, ports 53775/34987) used copied
cards and fresh portable directories, exited 0 through `quit_graceful`, and
the source memory-card hash remained unchanged.

The 2026-09-11 layout and price-formula pass was verified on debug binary
`69adbc6c870216ffbea6c6d5967576a329165940f06bf1e1519c78c605efaf2d` (release
`27b1736d2bf4394b1491832b29bda947ffa74e5e67cff26a562a7dc604becfa6`). The
regression reran green, 77 of 77, as
`/tmp/ygofm-sell-regression-20260911/results.json` (SHA-256
`3ce72cb5c91727f618d107403735fbb0fb52b27dcc39e786ee9190b11c047f59`; PIDs
3305281/3305552, ports 34723/38193, both exit 0 via `quit_graceful`). The
screen captures come from `tools/sell_ui_capture.py`, whose run
`/tmp/ygofm-sell-ui-20260911/results.json` (SHA-256
`1dce997fd7fa9b1b730507fdd7cc5110b24bfb9b494c2448683853a2e9d37d97`; PID
3304983, port 56023, exit 0) passed 25 of 25 checks with controller presses
for every binding and a ydotool keyboard pass (X refused with nothing
selected, Right added one, X reviewed, S went back). Its `screenshots/`
directory holds the composed 960x755 `ui-ui-<state>.png` and the exact native
`ui-<state>-native.png` for the empty trunk, quantities 1/10/99/255, long
names, mixed derived/override prices, zero value, six-digit prices and
totals, cap loss, confirmation, GO BACK, zero selection, stale transaction,
cancellation, a completed sale and the 722-card scroll. `menu_preview
--selftest` passes. Known limits: the card name "Graveyard and the Hand of
Invitation" is wider than the 276px detail line and is cut with `...`; the
panel is a host overlay, so the raw `screenshot` command never shows it and
only `present_shot` does; the black band under the button row is the stock
shop-field art, unchanged.

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

- Current recompiler tools, generated game/OpenBIOS, debug title, and release
  title build successfully. The function-entry generator change required a
  forced regeneration; all 69 regenerated game shards matched their
  pre-regeneration content. Final SHA-256 values are
  `89ef0e1f16c8cd1ca4ce1a637773c2b0dd0be849ee2955d7bcf725bd00eafa7a`
  (`psxrecomp-game`),
  `b22d50841a8b47f26a930c09a56b4c1328056a8b7ae61794be09b4046b08ab4d`
  (`psxrecomp-bios`),
  `5ed5fa5805fa2b3eb91f76689585ba994d2debcaa78b172f261e58e143cbfc4a`
  (debug game), and
  `82933155ff497b56b6d2546e6c2fbaaec64a781921775c90ed62f3ecd02b5f9a`
  (release game).
- `build-dbg/menu_preview --selftest`: PASS.
- Description boundary/live suite: PASS.
- Story reward suite: 7/7 PASS plus independent self-launch PASS.
- FM Editor and full manager/package round trip: PASS. The current 722-card
  package includes Card Drops 0, Smart Drops, all rank bands, a story reward,
  starchip rules, passwords, equips, field allow-lists and a quoted CPU rename;
  it survives stock reset/import and bytewise member comparison. Old fixtures,
  future/malformed input, damaged late members, late-manager rollback and
  legacy Smart migration pass with zero failures in
  `/tmp/ygofm-final-followup-cpu-20260911/package-roundtrip.log` (SHA-256
  `15e73f2657725718cd3d2f6bd1f84298749712dc197fa465dc99574a0e91d085`).
- All 20 executable effect regressions: PASS in
  `/tmp/ygofm-final-effects-20260910-d/results.json` (file SHA-256
  `89c6a57cac4a874adc9e334cac7e3b78f02c9030f1a9a60726badf2d4a2cd92f`).
- Offline OpenGL audio speed: PASS at 1x, 2x, 3x, 4x with 59.933, 119.877,
  179.831, and 239.751 game fps and 44,050-44,060 Hz SPU production, with no
  queue underrun/overflow. Evidence is
  `/tmp/ygofm-final-audio-20260911-final-gl/results.json` (SHA-256
  `13ec51267b7937887154f070e450c3af8b325bbbd3d43ee93d41bcf67745d839`).
  The retained software-renderer run passed 1x but could sustain only 114.844
  fps/42,205 Hz at 2x and recorded 50,675 device underruns; it is not counted
  as a speed pass (`/tmp/ygofm-final-audio-20260911-final/results.json`).
- Netplay normal, rollback 35/15, and seeded 5 percent loss: PASS on debug
  binary `da61649a...`, the last build containing guest mutation/serialization
  changes. The final binary's stock-policy rerun above separately covers the
  later editor, Sell, and display-only overlay changes.
  Coverage includes both face-down owners, result/trade continuation, graceful
  carry-back and zero simulator overflow. The old fixed 256-packet
  delay queue did overflow in the first 35/15 attempt; recomp-net commit
  `6cf5b01d` expands burst capacity and adds a deterministic 512-packet
  regression (17/17 recomp-net tests pass). psxrecomp commit `0703254c`
  records that nested gitlink.
- `Play.sh` resolves `build-dbg` normally and `build` with `-rel`; both files
  were rebuilt at this checkpoint.

The final complete psxrecomp CTest rerun has 56/62 enabled tests passing and three
performance tests disabled. Six failures also reproduce from an archive of
the unmodified starting psxrecomp commit: dirty-text continuation source-shape,
reachable-discovery `/tmp/test.exe` fixture, AOT candidate-capacity expectation,
hybrid-input source-shape, launcher pad-mode source-shape, and launcher Vulkan
source-shape. They are baseline test debt, not claimed passes. The independent
static overlay compilation suite passes 10/10. Only the existing third-party
`stb_image.h` GCC string-overflow diagnostic appears in title compilation; no
new title-source warning was introduced. The final CTest log is
`/tmp/ygofm-recompiler-ctest-20260911.log` (SHA-256
`b1572b7bc7f5c9e554667cd386b453fe6ee9cea57060c6ce86add597f8b019fa`).

## Known unverified platform behavior

The current Linux software-rendered editor/live paths and Linux OpenGL gameplay
paths were tested. Software gameplay sustained stock 1x here but not the 2x
audio-speed acceptance threshold; normal OpenGL sustained 1x through 4x. A
physical controller was not available; controller activation is
covered by the menu's shared keyboard/controller handler selftest, not a real
device. Windows, macOS, Vulkan, real HiDPI hardware behavior, and a completed
post-lock in-process launcher cycle were not tested in this checkpoint.
