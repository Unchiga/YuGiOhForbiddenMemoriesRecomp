# Yu-Gi-Oh! Forbidden Memories — Recompiled

A static recompilation of **Yu-Gi-Oh! Forbidden Memories** (USA, SLUS-01411).
The game's MIPS code is translated to C ahead of time and compiled into a native
executable — it is not interpreted by an emulator.

![A duel in progress, with the overlay menu bar across the top of the window and the duel-rank meter reading S 99 beside the FIELD box.](media/duel.png)

*A duel running natively. The menu bar across the top and the duel-rank meter
beside the FIELD box are this project's, drawn in the game's own art.*

On top of that sits a set of quality-of-life features built for this game: a
live duel-rank meter, a fusion assistant that reads your actual hand, a card-drop
multiplier with a proper results screen, a full **drop-table manager** in its
own window — view, edit and share every duelist's drops — and a cheat menu.
All drawn in the game's own art, all toggleable at runtime.

Built on [PSXRecomp](https://github.com/mstan/psxrecomp).

> **You bring your own disc.** Nothing in this repository, and nothing in the
> download, contains any of the game's code or data — the screenshot above is
> just that, a screenshot. The C is generated on your machine, from your copy,
> the first time you run it.

| | |
|---|---|
| Serial | SLUS-01411 (USA / NTSC-U) |
| Players | 2 |
| Publisher | Konami, 1999 |
| BIOS | OpenBIOS, bundled — nothing to supply |

---

## What this adds

Everything below lives in the in-game overlay menu on **`F10`**, and every
setting takes effect immediately — no restart, no patched save.

### ⚔️ Duel rank meter — `VIEW → DUEL RANK`

The game grades every duel you win but only tells you afterwards. This puts the
grade on screen **while you play**, in the game's own HUD sprites, lifted from
its own VRAM. It tracks the picture through scaling and aspect changes and hides
itself when a card view covers the box it labels.

| Mode | What you get |
|---|---|
| `OFF` | stock behaviour |
| `IN GAME` | the game's POW/TEC badge and rank letter, beside the FIELD box |
| `IN GAME + SCORE` | the same, plus the raw 0–99 score |
| `OVERLAY TEXT` | plain text in the corner — never covered by a card view |

### 🧩 Fusion assistant — `VIEW → FUSION HINT`

Forbidden Memories has thousands of fusions and teaches you none of them. This
reads the cards in your hand against the game's real fusion and equip tables in
memory — not a copied list — so its answers are the game's answers, including
the awkward three-step rule the game actually implements.

| Mode | What you get |
|---|---|
| `OFF` | stock behaviour |
| `NUMBERS` | pick order marked on the cards themselves |
| `NUMBERS + INFO` | pick order plus the name of the card it produces |

`VIEW → SUGGEST FUSION BY` chooses **ATTACK** or **DEFENSE**.

### 🎴 Card drops — `MODS → CARD DROPS`

Stock, a won duel awards exactly one card. This makes it **1–99**, so grinding a
specific drop stops being a weekend.

It comes with a results screen stock never had: the cards you won across three
pages you flip with **D-pad Left/Right**, with the game's own yellow **New!**
tag on anything you didn't already own.

### 🃏 Drop missing cards — `MODS → DROP MISSING CARDS`

**82 of the game's 722 cards are dropped by nobody** — both of Exodia's legs
among them, which is why the set cannot be completed by duelling in the stock
game. This gives every one of them a source.

Nothing on your disc is touched: the duel loads the opponent's drop weights into
memory and this rewrites that copy, so the change lasts as long as the duel does.

Placement is yours to change. On first run the mod writes
**`drop_missing_cards.ini`** into your player-data folder, listing every card by
name under the duelist that drops it:

```ini
[Weevil Underwood]
52  =  30,  20,   0   ; Hercules Beetle
278 =  30,  20,   0   ; Petit Moth
```

The three numbers are the S/A POW, B/C/D and S/A TEC rates, as weights out of
2048 — 20 is about 1%. Each band always totals 2048, so what you add comes off
that duelist's normal drops in proportion; the shipped table adds 1–6% each.
Delete the file for the defaults back.

### 🗂️ Drop Table Manager — `VIEW → DROP TABLE MANAGER`

![The Drop Table Manager](docs/screenshots/drop-table-manager.png)

Opens a **separate window** you can move to another monitor and leave open
while you play. It knows every card and every duelist's drop table — and it
does not just show them. **You can rewrite any duelist's drops and the game
rolls what you wrote.**

| View | What you get |
|---|---|
| `By card` | all 722 cards — id, name, type, ATK, DEF, how many tables drop it — sortable on any column, with every duelist that drops the selected one, the rank band needed, and the chance |
| `By duelist` | all 39 duelists, with everything they drop, the band, and the weight both raw and as a percentage |

Type to search by name or id, click a column heading to sort, scroll with the
wheel or the scrollbars, click a row on the right to cross over into the other
view. Weights are out of 2048, which is what lets one read as a percentage.

**Editing.** Click a weight and type a new one. Click the rank cell to move a
drop between bands. Right-click anything for the rest: add a card to a
duelist, move it, remove it. Or just **drag a card out of the left list and
drop it on a duelist** — the `All CPU` toggle lists every duelist under a
card, greyed where they do not drop it, so every one of them is a drop target
without leaving the view. Every band still totals exactly 2048 — whatever you
add or grow comes off that duelist's other drops in proportion, the same
arithmetic the game's own roll assumes — and an edit that cannot balance is
refused, not fudged.

**Nothing is written until you press `Save`**, which persists your table as
`drop_table_edits.ini` in your player-data folder (hand-editable, same format
as the mod's file). `Defaults` clears the selected duelist back to stock.

**Sharing.** `Load… → Export the current table` writes your table as a
timestamped file in `drop_tables/` next to your saves — send it to someone,
they drop it in their own `drop_tables/` folder and pick it from `Load…`.
Loading replaces the edit layer in memory only; it too is nothing until saved.

If `DROP MISSING CARDS` is on, the manager shows — and edits on top of — the
table you will actually roll against: it runs the same transform the mod runs,
so the two cannot disagree, and your edits apply over the mod's placements.

Card names and ATK/DEF are read out of the running game; the drop tables are
baked from your disc when you build. Duelist portraits are the game's FREE
DUEL art and, like every other piece of Konami art here, are **never
shipped** — the Manager **reads them off your own disc** the first time it
opens, the same forty tiles the FREE DUEL screen loads, so every duelist has
a face whether or not your campaign has met them. (If a disc image fails that
read, the older fallback still applies: the game captures portraits from its
own FREE DUEL screen as you browse it, and keeps them next to your saves.)

### 🎨 Card Manager — `VIEW → CARD MANAGER`

Change any of the 722 cards: **name, description, face art, duel thumbnail,
ATK, DEF, both Guardian Stars, type, level, attribute, price and password**.
The change shows up **everywhere the card is drawn** — the Library page, the
chest's TRIANGLE viewer, the deck-edit list, the password screen and the duel
itself — because it is applied where the game reads, not where it draws.

An edited card is a folder in your player-data:

```
cards/<id>/card.ini     name = Blue-eyes Ultimate Dragon
                        color = purple       (yellow, green, pink, blue, purple, orange)
                        description = Text with|a line break   (| = new line; no | = wrapped at 20)
                        attack = 4500        defense = 3800
                        star1 = Sun          star2 = Mars
                        type = Dragon        level = 12      attribute = Light
                        price = 999999       password = 12345678
cards/<id>/art.png      any size — becomes the 102x96 / 256-colour card face
cards/<id>/thumb.png    optional; the 40x32 / 64-colour duel card, made from art.png when absent
cards/<id>/title.png    optional 96x14 title strip; rendered from `name` when absent
```

Every key is optional and a missing one keeps the stock value, so a card that
only changes its art keeps its numbers. **Folders are watched**: edit a file,
or drop a new folder in, and the game picks it up within seconds — the next
screen that draws the card shows the change. Nothing on your disc image or in
your save is touched; the mod serves replacement disc sectors and table
entries to the running game, and removing the folder restores stock.

The **Card Manager** window is the front end for that: pick a card, see the
face and thumbnail exactly as the game will stream them, type new values
(green = your edit, `x` = back to stock), step through stars, types and
attributes, `Pick art…` to install a PNG, `Open folder` to get at the files,
`Save` to write `card.ini` and apply it, `Restore stock` to undo the card.
Titles are rasterised in the game's own style (Times New Roman Bold, 12 px,
squeezed for long names) when a `timesbd.ttf` sits in `cards/`; without one,
a plain bitmap font is used.

#### Effects

The same `card.ini` carries what a card **does**, and the manager shows the
rows that make sense for the card's type:

```
effect = damage            what a Magic card does when played (see the list)
amount = 2000              its number: LP healed or lost, the ATK threshold,
                           or how much the opponent's monsters lose
target = Dragon            for effect = destroy_type
terrain = Yami             for effect = field
ritual = 58, 58, 58 -> 26  for effect = ritual: three materials on your field -> result
equip_bonus = 1500         an Equip card's ATK/DEF bonus (stock 500, Megamorph 1000)
equips = Dragon, Warrior, 5, 12   the monsters it fits: types, card ids, all or none
                           (this REPLACES the stock list; leave it out to keep it)
boost = Dragon +1000, Fairy -500  a field card (Forest..Yami): each type's boost
trap_atk_max = 3000        a trap (House of Adhesive Tape..Widespread Ruin):
                           attackers at or under this ATK are stopped
```

Effects a Magic card can be given: `none`, `heal`, `damage`, `destroy_type`,
`destroy_atk`, `raigeki` (every opponent monster), `dark_hole` (both
fields), `dragon_jar` (their Dragons), `stop_defense`, `flip` (face-down
monsters up), `weaken` (opponent's monsters lose `amount` ATK and DEF; a
negative amount strengthens them), `swords`, `cursebreaker`, `harpie`
(their magic/trap zone), `field`, `ritual`. Any Magic or Ritual card can take
any of them; the game's own handler for that effect runs, with your number.
Equip compatibility, ritual recipes, field boosts and trap ceilings are the
game's own tables, served edited. Every one of these was verified in a live
Free Duel.

What stays as it is, because it is code rather than data: which cards count
as traps (only 681–686 have a ceiling), the trap's response, Goblin Fan /
Bad Reaction to Simochi / Reverse Trap / Fake Trap, `weaken` reaching only
the opponent's side, the guardian-star wheel, and the granularity of heal
(steps of 100, up to 25500) and damage (steps of 10, up to 2550).

#### Monster effects

Stock monsters do nothing but fight. An edited monster can carry, in the
same `card.ini` (and the manager's right-hand column):

```
battle    = indestructible      never destroyed in battle (its owner still takes the damage)
            mutual              destroys itself and whatever it battles
            slayer              destroys whatever it battles
on_summon = damage 1000         cast a magic effect when it lands face-up (set face-down, it is lost)
on_flip   = raigeki             ... when it is turned face-up: attacked, flipped, revealed
on_death  = raigeki             ... when it is destroyed (battle, trap or magic)
on_attack = heal 500            ... when it declares an attack
each_turn = damage 200          ... at the start of its owner's turn, while on the field
bonus     = 500, 200 per ally, 100 per enemy    ATK and DEF while on the field (face-up)
            500 per Lava Battleguard            ... per face-up copy of a card, or a type
            300 per enemy Dragon                ("per <card or type>" counts your side)
immune    = traps, magic        traps never fire on it; destruction magic passes it by
```

A face-down monster has no effect: its bonus and each-turn effect start
when it is turned over.

The cast effects are the same list a Magic card can be given (`heal`,
`damage`, `destroy_type Dragon`, `destroy_atk 1500`, `raigeki`, `dark_hole`,
`dragon_jar`, `stop_defense`, `flip`, `weaken 500`, `swords`, `cursebreaker`,
`harpie`, `field Yami`), plus `gamble`, Time Wizard's coin: heads destroys
the opponent's monsters, tails destroys your own and their total ATK comes
off your LP. They run through the game's own effect engine, with
the popup and sound the matching spell would show, the moment the duel is
idle after the trigger. A monster with any of these draws with the orange
effect-monster frame unless `color` says otherwise, and `Effect text →
description` writes the wording onto the card.

#### The Card Effects mod

`MODS → Card effects` (or the `Dev Card Effects` button top-right in the
Card Manager) switches to a second, separate card set kept in
`mods/card_effects/cards/`: the original cards with their real effects
adapted to Forbidden Memories, built one card at a time in the Card
Manager. Turning it on asks first; while it is on, the manager, Export and
Import all work on that set, and your own `cards/` edits sit untouched
until you switch back. It is off by default.

Effects added for it: `destroy_strongest` (the opponent's strongest monster
goes), `lose_lp` (you lose LP), `coin_lp` (tails: half your LP), equips
that fit an attribute (`equips = Dark`), and a bonus per card in your hand
(`bonus = 300 per hand`).

#### One file to share

`Export…` writes every edited card — `card.ini` and PNGs — plus your
`drop_table_edits.ini` to a single `.ygocards` file (a plain zip with a
manifest, so it opens anywhere). `Import…` reads one, first showing how many
cards it holds, which of **your** edited cards it would replace and whether
it carries drop table edits, and only then replaces them. A file re-zipped
by hand, stored or deflated, imports too.

### 🛒 Card shop — `MODS → CARD SHOP`

![The card shop's pack panel: MONSTER, MAGIC, EQUIP and TRAP rows, each set to its own rarity and price, over a RESULTS box listing the three cards the pack just yielded.](docs/screenshots/card-shop.png)

The card shop has never sold a card. This makes it an actual card shop: the
shopkeeper's menu grows a fifth row that **buys card packs** with your
starchips.

Four pack types — monster, magic, equip, trap — across four rarities, priced
20 / 80 / 200 / 800 starchips. **All 722 cards sit in a pool**, so anything
can come out. A pack deals its cards one at a time: each **X** turns over the
next waiting slot, then you can move between them and press **TRIANGLE** to
open the game's own card viewer on whichever one you like. Bought cards land
in your trunk marked **New!**, exactly like a duel drop.

Prices, rarity bands and where individual cards sit are yours. The shop writes
**`card_shop.ini`** next to your saves on first run and re-reads it whenever
you leave the shop and come back:

```ini
[prices]
legendary = 800

[packs]
cards = 3          ; 1-3, the results box prints three

[monster]
legendary_atk = 2500   ; a monster lands in the highest band its ATK reaches

[cards]
Exodia the Forbidden One = legendary   ; or `rare+legendary` for both
```

Delete the file for the defaults back.

### 🖥️ Widescreen — `VIEW → WIDESCREEN` *(experimental)*

16:9, contributed by [yamyi](https://github.com/Unchiga/YuGiOhForbiddenMemoriesRecomp/pull/1).
Projected 3D — the duel field — renders genuinely wider; flat 2D screens stay
4:3 and are pillarboxed rather than stretched. Also available as a mod-catalog
feature (`psx.enhancement.widescreen`). Experimental, as the framework labels
this feature class: culling pop-in at the wide edges has not been fully
checked for this title.

### 💰 Cheats — `CHEATS`

| Row | Range | Notes |
|---|---|---|
| `LIFE POINTS` | 1–9999 | 8000 is stock. Applies to both duellists |
| `SHOW OPPONENT HAND` | on / off | their hand is drawn face-up, like yours |
| `FORCE FACE UP` | on / off | their set cards play face-up — and stay face-up |
| `STARCHIPS` | 0–99999 | written straight to your save |
| `FREE SPENDING` | on / off | purchases succeed, the deduction is undone |
| `ALL CARDS` | 1, 2 or 3 of each | fills the trunk. Apply with the chest closed |

Neither reveal is an overlay. `SHOW OPPONENT HAND` clears the one flag that
keeps their hand hidden, so you get their real sprites, names and ATK/DEF in the
game's own renderer. `FORCE FACE UP` clears the face-down bit, which is real
game state and not a drawing choice — a monster revealed that way genuinely is
face-up and will not flip when attacked. Turning it off stops new reveals; it
does not re-hide what has already turned over.

`LIFE POINTS`, `SHOW OPPONENT HAND` and `FORCE FACE UP` are preferences,
restored on every launch. The other three write live save data, so they are
deliberately *not* re-applied at startup and decline with a message until a save
is loaded, rather than writing over whatever else is in memory.

### From the runtime

Also in the `F10` menu, courtesy of PSXRecomp: save states, rewind (`F8`), an
emulation-speed multiplier, and **`GAME → FAST LOADING`**, which cuts disc loads
to near-instant. That one ships **off**; the setting persists once you turn it
on.

---

## Controls

### Controller

**Xbox controllers work out of the box** — plug one in, no setup. So do PS4 and
PS5 DualShock/DualSense pads (rumble supported on DualSense), and Steam's
virtual controller through Steam Input. The game is a PS1 title, so it starts in
**digital** pad mode and the sticks map to the d-pad.

> Very old DirectInput-only pads are the exception. DirectInput is off by
> default because enumerating it stalled startup by up to 40 seconds on some
> machines. Set `SDL_JOYSTICK_DIRECTINPUT=1` in your environment to bring it
> back.

### Keyboard

| PlayStation | Key | | PlayStation | Key |
|---|---|---|---|---|
| D-pad | Arrow keys | | L1 | `Q` |
| ✕ Cross | `X` | | R1 | `W` |
| ○ Circle | `S` | | L2 | `E` |
| □ Square | `Z` | | R2 | `R` |
| △ Triangle | `A` | | L3 / R3 | `T` / `Y` |
| Start | `Enter` | | Select | `Right Shift` |

Mostly you need **arrows** to move, **`X`** to confirm, **`S`** to cancel.

### Hotkeys

| Key | Does |
|---|---|
| `F10` | Open the overlay menu — every feature on this page lives there |
| `F7` | Save / load state |
| `F8` | Rewind |
| `Tab` | Turbo (hold) |
| `Alt`+`Enter` or `Ctrl`+`F` | Fullscreen |
| `F` | Show performance stats |
| Numpad `+` / `-` | Volume |

Keys are stored in `keybinds.ini` next to the executable, in plain text with the
accepted names listed at the top. Edit it and restart. Each input takes an
optional second binding after a comma, so `cross = X, Mouse1` binds both.

---

## First run

The download is a **setup host** — a small executable plus the recompiler and
the framework source. It has no game code in it until you supply a disc.

1. Run `Yu_Gi_Oh_Forbidden_Memories_Recompiled.exe`.
2. It asks for your disc image and checks it against the CRC32 of the data track
   this build expects. A mismatch is **refused**, naming the release it needs
   and the one you gave it.
3. It downloads a compiler if you have none, translates the game to C from your
   copy, and compiles it.
4. It builds into `build-release/` and starts the game.

Nothing needs installing first — the setup brings its own compiler and Python,
and uses yours if you already have them.

> ### ⏳ The first run takes a few minutes — let it finish
>
> A compiler download, a whole game translated to C, and a real compile happen
> before you see anything; the console window working away is not hanging.
> **Every run after the first starts immediately.**
>
> **That wait is the point.** This download contains no game code and no game
> assets — not the executable, not the sprites, not even the font. All of it is
> produced on your machine, from the disc you own, and never leaves it.

### Which dump

`.cue` is preferred, with its `.bin` beside it; `.bin`, `.img`, `.iso` and
`.car` also work.

This build is compiled from the USA release, serial **SLUS-01411**. A PAL,
Japanese or Greatest Hits disc is a different program and cannot run here. The
expected data-track CRC32 is recorded in `game.toml` as `disc_crc`, and is
computed once when you choose the disc, not on every boot. To repoint it, use
`FILE → CHANGE GAME DISC` in the `F10` menu.

### Command line

Scripted and headless runs have no picker and must be told:

```bash
Yu_Gi_Oh_Forbidden_Memories_Recompiled.exe --disc "/path/to/game.cue"
```

Also available: `--memcard-dir <path>`, `--no-launcher`.

---

## Building from source

The framework is a submodule at `psxrecomp/`, so clone recursively (or run
`git submodule update --init --recursive` if you already cloned):

```bash
git clone --recurse-submodules https://github.com/Unchiga/YuGiOhForbiddenMemoriesRecomp.git
```

`generate` produces **both** the recompiled BIOS and the game's C — the
framework ships `bios/openbios.bin` but not its recompiled form, so a fresh
clone has no BIOS backend until this runs:

```bash
python3 psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . --disc /path/to/your.cue
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target psx-runtime
```

(The setup host does exactly this for you — see [First run](#first-run).)

`generated/` and the baked sprite and font sources come from **your** disc; they
are gitignored and must not be published — see [NOTICE](NOTICE). The art is
baked at build time, so CMake must know where your disc is: running `generate`
first is enough. Failing that it tries `-DYGOFM_DISC=<path>`, the disc this
build directory already verified, then the one recorded beside the executable,
and stops with a message rather than shipping a runtime with no art.

### macOS

Builds and runs natively on Apple Silicon with the commands above — no flags of
its own. Prerequisites are the Xcode Command Line Tools plus
`brew install cmake ninja pkg-config sdl3`.

Build with `-j4` rather than the default on an 8 GB machine: the generated C
includes shards of 400k+ lines, and four concurrent clang instances is where
this stops being memory-bound. Vulkan compiles as a software stub unless the
SDK is installed; the OpenGL renderer runs over Metal at full speed regardless.

Add `-DPSX_DEBUG_TOOLS=ON` for a debug build with the TCP inspection server on
`127.0.0.1:4370`.

### Packaging a release

```bash
cmake -S psxrecomp/recompiler -B build-recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-recompiler --target psxrecomp-game psxrecomp-bios
cmake -S . -B build-setup -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSXRECOMP_FORCE_SETUP_HOST=ON
cmake --build build-setup --target psx-runtime
scripts/package_setup_release.sh build-setup <artifact-tag>
```

Writes `dist/ygofm-<version>-<tag>.zip`. Needs `objdump` on `PATH` so bundled
MinGW DLLs can be resolved, and the build directory must be a **setup host**
build (`PSXRECOMP_FORCE_SETUP_HOST=ON`), not the game.

---

## Framework and symbols

`psxrecomp/` is pinned to the
[`ygofm`](https://github.com/Unchiga/psxrecomp/tree/ygofm) branch of a fork of
[PSXRecomp](https://github.com/mstan/psxrecomp), because this project needs
framework work not upstream yet: the disc-identity gate, registration APIs so a
title owns its own debug commands and guest-space overlays, per-vblank game and
SDL-event hooks (how the Drop Table Manager owns its second window), and a
launcher-less setup host. All additive and intended for upstream; the branch
exists so this repository builds today.

Symbols: `symbols.toml` → `python3 tools/sync_symbols.py` → `psx_symbols.h`
(`PSX_FN_*`). See `psxrecomp/docs/SYMBOLS.md`.

---

## Licence and legal

PolyForm Noncommercial License 1.0.0 — see [LICENSE](LICENSE). Noncommercial use
only, and the licence cannot be sublicensed or swapped for a permissive one,
because the framework it builds on is offered on the same terms
(Copyright © 2026 Matthew Stan).

That covers this project and the framework. It grants nothing in respect of the
game, which is Konami's — use only a disc image you obtained legally.

Read [NOTICE](NOTICE) before redistributing anything — particularly before
sharing a *compiled build*, which is not the same as sharing this repository.

## How to Help
Join our newly created Discord, https://discord.gg/SR8qWG9Ve
