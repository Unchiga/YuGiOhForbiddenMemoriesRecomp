# Agent brief: what this project is and how to work on it

Paste this into a fresh session before touching the code. It is the map;
`tools/LIVE_TESTING.md` is the manual for driving the game, and the module
headers in `src/` are the authority on their own subsystems.

## The project

Yu-Gi-Oh! Forbidden Memories, statically recompiled to native code, plus a
layer of mods and tool windows on top. Two repositories:

| | |
| --- | --- |
| `~/YuGiOhForbiddenMemoriesRecomp` | this game: `src/*.c` is every mod and window, branch `main`, remote `Unchiga/YuGiOhForbiddenMemoriesRecomp` |
| `psxrecomp/` (submodule) | the framework: the runtime, the recompiler, the overlay menu, save states. Its own repo `Unchiga/psxrecomp`, branch `ygofm` |

`generated/` is the recompiled game C, and `build-*/game-assets/` holds tables
baked from the player's disc (`tools/gen_drop_db.py` and friends). Neither is
committed: both are derived from the player's own disc image at build time.

Two references live outside the repo and are worth reading before guessing at
anything guest-side:

* `~/memories-decomp` - the community decompilation. `config/slus_01411/symbols.txt`
  names thousands of functions and globals, `src/game/*.c` holds the ones that
  are decompiled, and `notes/research/the-game.md` is a measured map of the
  save, the disc, the text bank and the duel.
* `~/teatools` - documentation of the community's browser mod tools, one folder
  per tool, each with the file offsets and the patch bytes it writes. Good for
  "has someone already found where X lives".

## Building and running

```sh
cmake --build build-dbg --target psx-runtime     # debug tools + debug server
cmake --build build     --target psx-runtime     # release
./Play.sh -dbg --no-launcher                     # what you test with
```

`./Play.sh` prefers `build-dbg` when it exists, so ALWAYS build both before
telling the user their build is ready.

Player data (saves, edited cards, ini files, save states) lives in
`~/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled/`. It is the
user's. Snapshot what you touch, put it back, and never save in-game on their
memory card.

## Testing: the part that matters

Nothing here is finished until it has been seen working in the running game.
The debug server is how: `tools/dbg.py` and `tools/live_probe.py` wrap it,
`tools/game_route.py` drives a cold boot to a screen, and every window has a
command (`card_manager`, `drop_viewer_set`, `fusion_manager`, `cpu_manager`,
`dialogue_manager`, plus data commands like `cpu_data`, `story_rewards`,
`fill_library`, `video_menu`).

Rules learned the hard way:

* The user may have the game open. It holds port 4370, so launch yours with
  `--debug-port 4371` and set `dbg.P = 4371` / `live_probe.P = 4371`. Never
  kill a process you did not start.
* The debug screenshot is the PSX display only. Overlays composited at present
  time and the tool windows are not in it: use each window's own `*_shot`
  (a PPM) or a desktop capture (`spectacle -b -n -o file.png`).
* Guest-side claims get checked against two independent sources where possible.
  Every disc write in this codebase reads the stock sectors back and refuses
  unless they match a table baked from the same bytes by a different route.
  That check caught a wrong LBA on its first run.
* When something crashes only for a user, build a host harness: compile the
  real module against a dump of the guest memory it reads, stub the handful of
  `psx_mod_*` calls, and run it under `-fsanitize=address,undefined`. That is
  how the dialogue double free was found in minutes after two days of it being
  invisible here.

## House style

* Comments explain WHY, with the measurement behind them. "Measured live
  2026-09-06" beats an adjective. If something is a guess, the comment says so.
* No em dashes or en dashes in anything user-facing, including code comments
  and commit messages. American spelling: "color", not "colour".
* Match the file you are editing: its comment density, naming, and idiom.
* One implementation of a piece of arithmetic. The drop weights, the deck
  pools and the story rewards all rescale through `psx_drop_pins_rescale`.
* Report faithfully. Say what was verified, what was not, and what was left
  out. A failed step is reported, not smoothed over.

## Git

* Commits stay local unless the user asks to push. The tree should be clean
  when you stop.
* End commit messages with the attribution the session prompt gives.
* The `psxrecomp` submodule is a separate repo: commit inside it first, then
  the pointer bump in the superproject, and say so in the message.
* Its `tools/__pycache__/*.pyc` is checked in and gets rewritten whenever a
  Python tool imports the debug client. Restore it (`git checkout --`) rather
  than committing the churn.

## What exists today

Windows (all under VIEW): Card Manager, Drop Table Manager, Fusion Manager,
Dialogue Manager, CPU Manager. Mods (under MODS): card drops, drop missing
cards, card shop, library placeholders, fusion hint, and the scripted story
drops that live in the Drop Table Manager rather than a row of their own.
At the bottom of MODS, Import / Export MOD package (`src/psx_mod_package.c`)
bundles every manager's share file and every mod setting into one .ygomods;
it never parses a manager's format itself, it re-packs and calls that
manager's own import.

Everything a manager edits is a hand-editable file beside the player's saves,
every manager's Import keeps what it imported, and no manager writes to the
memory card. Read the header comment of a module before changing it: each one
carries the guest addresses it uses and how they were established.

## Verifying a function

The task that follows this brief is a double-check pass. For any function that
touches the guest:

1. Read its header comment: it should name the address and how it was found.
2. Cross-check the address against `~/memories-decomp` symbols and notes, and
   against the recompiled C in `generated/` (find the block label for the
   address; the operand shapes and call targets are all there).
3. Check the arithmetic against a second source where the codebase has one:
   the baked tables, the stock sectors, the decompiled function.
4. Prove it in the running game, and say what you saw.
5. Where a claim cannot be proven, say that plainly instead of dressing it up.
