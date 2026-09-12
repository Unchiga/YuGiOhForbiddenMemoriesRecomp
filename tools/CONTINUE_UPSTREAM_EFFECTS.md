# Continue upstream qualification and fix all effect freezes

Work in `/home/codyj/YuGiOhForbiddenMemoriesRecomp`, then reuse the candidate
worktree `/tmp/ygofm-upstream-2026-09-09/title`. Carry this task through fixes,
builds, live regressions, local commits and updated evidence. Do not stop at a
plan. The user is going to bed; make routine decisions autonomously.

## Rules and starting context

- Read applicable AGENTS.md files, `tools/AGENT-BRIEF.md`, `tools/LIVE_TESTING.md`,
  `docs/UPSTREAM_CATCHUP_2026-09-09.md`, and memory notes
  `ygofm-psxrecomp-upstream-gap`, `ygofm-netplay-2p`, `ygofm-linux-setup`,
  `ygofm-file-dialogs`. Memory files are under
  `/home/codyj/.claude/projects/-home-codyj/memory/`.
- Work on branches only. No pushes, releases, deployments or unsolicited remote
  messages. Commit reviewable changes with
  `Co-Authored-By: Codex <noreply@openai.com>`. Keep unrelated edits untouched.
- Never access the personal card/profile under
  `~/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled/`. Every game
  process must receive `--memcard-dir <scratch>` and the explicit disc path.
  Use existing scratch seeds, including for netplay; inspect harness defaults
  before invoking anything that might read a personal seed.
- Use a free private debug port; verify its process and `card_packs.dir` belong
  to the scratch profile before sending mutating commands or shutting down.
  Never use global pkill patterns or attach to a user's game. Use owned
  `quit_graceful` for netplay carry-back. Unset APPIMAGE/APPDIR for native tests.
- Do not build an executable while a test is running it. Use composed captures
  for overlays: `screenshot_present` on OpenGL, `present_shot` on software.
  Plain `screenshot` is only suitable for native game screen comparisons.
- Collaborating game-code reference: https://github.com/krystalgamer/memories-decomp .
  The user says we collaborate on it. Runtime `docs/YGOFM_REFERENCE.md` records
  it too. `~/memories-decomp` is absent; `~/ygofm-decomp` is a separate Unchiga
  checkout, so check remotes/revision and matching symbols before using it.

## Exact checkpoint

- Candidate title branch: `upstream-catchup-2026-09-09`. Implementation checkpoint
  `ecb7b66`, followed by this handoff documentation commit.
- Nested runtime branch: `ygofm-upstream-2026-09-09`, HEAD `718a9ce9`.
  History is upstream master `ed55299be34710a90fc080484a83e8634bd41fa9` plus
  twelve private layer/fix commits. Do not restart the port or merge upstream
  onto the old unrelated snapshot root.
- `recomp-ui` was freshly fetched while preparing this handoff: HEAD and
  origin/master both `8bf473830388409195aae4962f72bd5337053e3d`, ahead/behind 0/0.
  Its origin is `https://github.com/mstan/recomp-ui.git`; fork is Unchiga.
- `psxrecomp/lib/recomp-net`: `4952fae`, upstream `46ef6ed` plus retransmission
  fix and deterministic initial-BEGIN/chunk-zero loss regression. Existing
  upstream PR https://github.com/RetroPortingToolKit/recomp-net/pull/12 was
  opened from an already-pushed fork branch; the new test commit is LOCAL.
  Do not push it. CTest 15/16: rollback_episode_test has the same four failures
  on pristine upstream; evidence exists, do not silently mark it green.
- `retcomm-rbengine`: `a7b9850`. Re-measure all remote tips before claiming current.
- Original checkout stays `netplay-2p` / `ygofm-netplay`, runtime `4e5c8e93`.
  Original `Play.sh` DOES NOT launch the candidate. Preserve its unrelated
  user asset/icon changes and runtime debug_client pycache modification.
- Candidate debug, release and setup-host builds all link. Game and OpenBIOS
  C were regenerated. If recompiler changes again, regenerate both, compare
  emitted C sizes and reconfigure CMake to discover changed shard counts.
- Full report/evidence is PARTIAL qualification, not a certificate that every
  feature works. Read all unverified items and failed-probe explanations.

## Saved artifacts and launching

Durable handoff directory: `/home/codyj/ygofm-upstream-handoff-2026-09-09/`.
It contains this prompt, attachment copies with SHA256 manifest, and an evidence
snapshot. Original evidence remains `/tmp/ygofm-upstream-2026-09-09/evidence/`.
Preserve both. Build directories and launch helpers are still under `/tmp`.
If `/tmp` was cleared, restore worktrees from the LOCAL branch refs in the
original repositories; all five repos' common git directories live under the
original title `.git`. Recover recorded local gitlinks without fetching our
unpublished branch from GitHub. Rebuild instead of assuming stale paths work.

Disc: `/home/codyj/YuGiOhForbiddenMemoriesRecomp/disc/Yu-Gi-Oh! Forbidden Memories (USA).bin`.

Manual candidate launch: `/tmp/ygofm-upstream-2026-09-09/Try-upstream.sh`.
It uses `manual-check` scratch data and honors configured OpenGL.
Tool windows: `/tmp/ygofm-upstream-2026-09-09/Try-upstream-tools.sh`, forced
software, same scratch profile. Close the first game before using the other.
Tool windows have a known GL crash limitation; use software for their checks.

Latest follow-up fixes/results:

- F10's SDL menu had input but no drawing. Fixed previously in `c75132a6`.
- SDL toast was behind the bar/dropdown. Fixed in `718a9ce9`: draw last, below
  the bar, in drawable coordinates. Negative test fails old binary; software
  and OpenGL preserve 24/24 opaque text pixels over the File dropdown.
  See evidence `new/toast-{before,final,gl}`, `new/toast-pixel-comparison.json`.
- `a39229c1` improved software rasterization enough for clean 2x menu audio;
  its 64-case raster equivalence test passes. Software 3x/4x remains constrained.
- The manual helper incorrectly forced software while normal play used OpenGL.
  It now honors the configured GPU renderer. Do not claim software 3x/4x fixed.
- OpenGL 30-second samples at 1x/2x/3x/4x/1x: 59.93/119.88/179.80/239.34/59.94
  fps, normal ~44.1 kHz SPU production, ZERO host underruns and overflow.
  Evidence `new/audio-all-speeds`; old-fork GL agrees in
  `baseline/audio-gl-34-v2`. First baseline 3x probe was invalid because late
  startup settings reset it; v2 checks effective speed. WAVs retained.
- Game speed MUST accelerate logic while preserving normal audio pace, pitch
  and quality. Do not hide failures by lowering effective speed, muting sound,
  stretching music to game speed, reducing chosen quality or changing settings.
- Tests: `tools/software_menu_regression.py` (software or OpenGL),
  `tools/audio_speed_regression.py` (default OpenGL, 1/2/3/4/1, 30s each).
  Read --help for required executable/disc/seed/matching-state/reference/scratch.
  Audio tests establish clock/queue health; listen to retained PCM too.

## Priority: reproduce and fix the user's effect freezes

User: freezes happen when assigning effects such as Dragon Capture Jar to a
monster. They require ALL effects tested and broken behavior fixed, not just
one lucky Dragon Jar example. These newly attached freezes have NOT been
reproduced or fixed at this checkpoint.

The durable `freeze-fixtures/` holds:

- `freeze_report_1.txt`, `state_800129D8_slot11.pst`, matching `.thumb`.
- `freeze_report_2.txt`, `state_800129D8_slot10.pst`, matching `.thumb`.
- `manifest.json` mapping original attachment paths and SHA256 hashes.

Both reports: OpenBIOS checksum C4DAEB88, entry PC800129D8, stalled 900 guest
vblanks; busy word 8009B0F4=00081810 (wait1/gate0/pump1), busy2 8009B134=0,
effect index 8009B100=6, effect flags 8009B112=C001, phase 800EADA0=17,
mode C3/sub81. Report1 is player turn0; report2 opponent turn1. Both report
zero stale-CD-gate repairs. Do not assume this is the old bit10 CD gate bug.
The signature is evidence of a shared stuck state, not proof of a root cause.
Reports came from a Windows build; revision/settings.toml/mod packages were
not supplied. Do not assume the transferred states include host-side mod
configuration or are compatible with the newly regenerated code.

1. Hash/preserve originals; only load copies into fresh scratch openbios dirs.
   Check serialization/version/BIOS and host-mod restoration compatibility.
   Inspect report states, guest RAM, active effect module, dispatch/hook events
   and native cast/queue bookkeeping. A state that cannot load is not a pass;
   reconstruct an equivalent cold-boot fixture from the reports and available
   card configuration. Ask only if missing data truly blocks independent work.
2. Compare original fork and candidate with matching generated-code states or
   fresh equivalent scenes to distinguish existing bugs from port regressions.
   Start with Dragon Capture Jar attached to a monster for both ownership sides,
   actual qualifying Dragons, no targets, and multiple targets.
3. Read `src/psx_card_effects.c`, `src/psx_monster_effects.c`,
   `src/psx_card_packs.{c,h}`, manager exposure, `src/psx_freeze_report.c`,
   relevant runtime hooks/overlay/interpreter and matching decomp/generated C.
   Trace effect driver entry, completion and cleanup, side/row-map restoration,
   module loading, animation waits, chained destruction/death/flip triggers,
   patched guest instructions and interpreter fallback.
4. Existing `casts_tick` has a 900-frame fallback that increments casts_done;
   `psx_card_effects` also releases its hold after 900 frames. A timeout or
   casts_done increment alone is NOT successful effect completion. Instrument
   genuine progress/completion and distinguish timeout/cancel from success.
   Fix the actual broken transition/invariant; do not blanket-clear busy flags,
   disable effects, extend timeouts or suppress the reporter to hide hangs.
5. Build a data-driven regression inventory from every effect ID, parser/UI
   option and supported trigger. Fail coverage if a new effect lacks a case.
   Current enum has 22 entries including NONE: heal, damage, destroy_type,
   destroy_atk, raigeki, dark_hole, dragon_jar, stop_defense, flip, weaken,
   swords, cursebreaker, harpie, field, ritual, destroy_strongest, lose_lp,
   gamble_lp, gamble, destroy_own, destroy_own_lp, plus none. Recount from code.
6. Test every supported effect as a spell and/or monster trigger where exposed:
   on_summon, on_flip, on_death, on_attack, each_turn, opp_turn; both owners.
   Include no target/one/many, friendly/enemy/mixed, facedown/faceup,
   attack/defense, empty/full zones, self/source destruction, strongest ties,
   immunities, lethal/nonlethal LP and parameter boundaries. Unsupported
   combinations need explicit validation and a documented reason, not hangs.
   Cover deterministic heads/tails and chance/else sequences, chained effects,
   queue exhaustion/reentrancy, card replacement and repeated activations.
   Also cover battle none/indestructible/mutual/slayer (2D and 3D paths),
   flat/per-ally/per-enemy bonuses, trap/magic immunity, equip eligibility and
   amounts, terrain boosts, trap thresholds and ritual recipes.
7. Each case must assert expected cards/LP/stats/terrain/positions, correct
   owner/row-map restoration, queue drained, return to playable input, and
   successful subsequent turn/action. Keep state traces, screenshots and a
   machine-readable coverage table. Add a failing-before/passing-after test
   for each fixed bug. Frame counter movement alone does not prove duel progress.
8. Test representative repaired/chained effects at 1x/2x/3x/4x, plus save/load
   during a cast and loopback delay/rollback for deterministic state restoration.
   Check host-side RNG/queues/holds are restored consistently in netplay;
   include a targeted rollback regression if a defect is found there.

## Finish upstream and feature qualification

Fetch and pin current upstream refs for psxrecomp, recomp-ui, recomp-net and
rbengine. Inspect `.gitmodules` recursively and CMake dependency pins/URLs for
anything else stale or pointing at obsolete sources. Report remote, pinned
commit, current tip, ahead/behind and fork-only patches. If updating, preserve
all private facilities and meaningful retransmit tests, rebuild and rerun
affected tests. Keep runtime history upstream plus reviewable private commits.

Complete the existing report rather than discarding its baseline:

1. Debug PSX_DEBUG_TOOLS=ON, release and Linux setup/codegen host link; no new
   title src warnings. Regenerate game/OpenBIOS if recompiler changes.
2. Cold boot/title, LOAD, Library, Password; screenshot parity at 80x60 mean
   gray difference <18, explain any intentional change.
3. Free Duel two turns, Pause latency/freeze behavior, scripted win/drop then
   Library; include the new full effect suite above.
4. Software Card/Fusion/Drop/CPU managers: open, edit and show results in game;
   Drop Randomize, Card Packs import/export, .ygomods import after scratch backup.
5. File/View/Video/Audio/Game/Cheats/Mods: every row applies, F10 and toast in
   composed captures, renderer switching software/OpenGL/Vulkan, aspect and
   widescreen, GAME savestate save/load, offline update check, HiDPI scaling.
6. Card/monster effects, frame colors/skins/descriptions, portraits LBA17952,
   dialogue, story rewards, cheats, fill Library and actual edited/random drops.
   Prior report lacks exhaustive campaign, cheat, fusion and random-drop outcomes.
7. Loopback `netplay_scenario.py`, `netplay_scenario.py rollback 35 15`, 5% loss
   boot; require every ok, trade, both cards carried back with .pre-netplay
   backups and no "left as it was". STOP=p2turn: host s_p2_turn/s_p2_tri show
   hand backs, PLAYER 2 IS CHOOSING, hidden fusion assistant and triangle view.
8. Scratch card directory frames 0x80..0x800 unchanged; only intended trade
   data differs from backups. `save_clone.py` creates an accepted 2P save.
9. Thirty-second duel/Card Manager performance against baseline, plus menu and
   duel audio pace/quality at all four speeds. Distinguish renderer limitations.
10. Restage `scripts/package_setup_release.sh` AFTER fixes; existing archive
    predates menu/audio/toast follow-ups. Configure staged Linux setup offline
    with RETCOMM_TOOLCHAIN_DIR and local dependencies. Toolchain previously:
    `/home/codyj/.local/share/retcomm/toolchains/cmake-clang-v1/1.0.14`.
    Report SDL3/Windows or other unavailable platform tests honestly.

Use existing upstream_regression.py, compare_upstream_regression.py, game_route.py,
goto_duel.py, duel_turns.py, netplay_pair.py and new menu/audio regressions.
Do not repeat already valid expensive tests unless changed code or an unresolved
concern justifies it. Run new regressions where previous sampling missed bugs.

## Done

Fix freezes with repeatable evidence and enumerate every effect outcome. Update
the upstream qualification table (item, baseline, candidate, evidence), separate
untested from passed, update both memory notes, commit runtime/dependencies
before title gitlinks, and leave a precise launch command for the actual rebuilt
candidate with scratch cards. State what is still limited. Nothing pushed or
released; the user decides publication and replacing their normal checkout.
