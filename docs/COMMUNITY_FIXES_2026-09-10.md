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
`/tmp/ygofm-final-netplay-20260911-policy-root2/stock-policy.json` (SHA-256
`b5a5be9b7c9efcdc8bb79c7d39e9a7dfe09dc34d143d1100d8f7c9713f0ba0d0`).
Both peers exited 0 with requested/effective speed 1. Mutation probes were
rejected, except that a 4x request was accepted only as a clamp back to 1x.
Offline Smart/zero-drop/starchip/CPU/package fixtures stayed loaded but
inactive, with no persistent fixture hash change.

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

### Free Duel collection progress

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

The isolated software-rendered live result is
`/tmp/ygofm-fd-completion-20260910-f/results.json`. Real input reached Duel
Master K at scroll row 5; the copied-save state showed `12/157` with no frame,
then a scratch-only inventory fill produced `157/157`, a frame aligned at guest
`[244,144]`, and distinct animation frames 2 and 3. PID 3052468 shut down
gracefully; no personal card or monitor configuration was used.

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

The Drop Tables page now owns an ordered, first-match-wins reward-rule editor.
Each condition can be Any or one campaign/Free Duel mode, opponent ID,
win/loss outcome, and D-through-S TEC/POW rank. Earlier rules have priority;
the UI supports add, remove, reorder, two-step clear, Done-without-save and
Save-and-close. With no matching rule the guest's exact stock 1-through-5
calculation and write remain untouched.

The live hook snapshots the pre-stock 32-bit total at results entry, then
replaces only a matched award with `min(before + amount, 999999)`. Accepted
amounts are 0 through 999999; parsing rejects unsupported values before state
changes. A present-only compact star-times-number row covers the stock star
loop and fits every six-digit value without clipping other results. Both
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

The repeatable final-binary suite is
`/tmp/ygofm-sell-extras-provenance-v7-20260911/results.json` (SHA-256
`537ec7d7a57016a05cf563adfa83642f72ec443aac1a7bc093aae78e91ca568d`).
All 52 assertions pass, including raw live/save-mirror arrays, the complete
722-card overflow case, zero-value removal, stale inventory and hot-reloaded
price rejection, Circle cancellation, Triangle preview, Cross confirmation,
native Save/Overwrite, copied-card restart, and visible no-extras reopening.
The Card Shop setting is enabled only in the two fresh portable profiles; the
authorized source card is hashed before and after and remains unchanged. Both
tracked software-rendered processes used separate verified ports and exited 0
through `quit_graceful`; the suite retains the native route and panel captures.

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
  `419cb795feee79d2574f5715565198852c0f3d8774826c8d892eaab661e6c454`
  (debug game), and
  `9f64808f656cf8992542d25eda058e2d631cf02e109919087addc3b3ab624f1c`
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
  changes. The later binary differs only in FM Editor native-window geometry.
  Coverage includes both face-down owners, result/trade continuation, graceful
  carry-back and zero simulator overflow. The old fixed 256-packet
  delay queue did overflow in the first 35/15 attempt; recomp-net commit
  `6cf5b01d` expands burst capacity and adds a deterministic 512-packet
  regression (17/17 recomp-net tests pass). psxrecomp commit `0703254c`
  records that nested gitlink.
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

The current Linux software-rendered editor/live paths and Linux OpenGL gameplay
paths were tested. Software gameplay sustained stock 1x here but not the 2x
audio-speed acceptance threshold; normal OpenGL sustained 1x through 4x. A
physical controller was not available; controller activation is
covered by the menu's shared keyboard/controller handler selftest, not a real
device. Windows, macOS, Vulkan, real HiDPI hardware behavior, and a completed
post-lock in-process launcher cycle were not tested in this checkpoint.
