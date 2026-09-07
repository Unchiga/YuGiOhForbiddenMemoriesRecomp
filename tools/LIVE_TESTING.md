# Live testing: driving the game and the tool windows from a script

Everything a session needs to boot the game, reach a screen, press buttons,
read RAM, take screenshots and drive the Card / Fusion / Drop Table managers
without a human at the keyboard. Read this before touching `tools/*.py`.

## 1. Launch

```sh
./Play.sh -dbg --no-launcher      # debug build, TCP debug server on 127.0.0.1:4370
./Play.sh -rel                    # release build, no debug server
```

Build first if needed:

```sh
cmake --build build-dbg --target psx-runtime     # debug
cmake --build build     --target psx-runtime     # release
```

Player data lives in `~/Documents/My Games/Yu-Gi-Oh Forbidden Memories Recompiled/`
(the runtime prints `psxrecomp: player data in ...` at start). Your own edited
cards are `cards/<id>/`; the Card Effects set is `mods/card_effects/cards/<id>/`.
Which one is live is the Card Manager's "Dev Card Effects" button.

The game speed is whatever the player picked in the VIDEO menu. It has run at
3x here, so never time anything by the frame counter. Use screenshots.

## 2. Talking to the debug server

`tools/dbg.py` wraps `psxrecomp/tools/debug_client.py`:

```python
import sys; sys.path.insert(0, 'tools'); import dbg
dbg.q({'cmd': 'frame'})                         # {'ok': True, 'frame': N}
dbg.rd(0x8009B26C, 1)                           # hex string of RAM bytes
```

`tools/live_probe.py` adds pad presses and screenshots:

```python
import live_probe as p
p.press('start', 60, 3.0)     # button, frames held, seconds to settle after
p.shot('tag')                 # writes $SHOTDIR/shot_tag.png (default /tmp)
p.rd(addr, n)                 # bytes
p.mode()                      # byte at 0x8009B26C
```

Buttons: select start up right down left l2 r2 l1 r1 triangle circle cross square.
`press` sends `{"cmd":"press","buttons":0xFFFF & ~mask,"frames":N}` then
`clear_input`. Check what the game sees with `{"cmd":"pad_status"}` (the
`pad` word is active-low, 0xFFF7 = START held).

RAM: `{"cmd":"read_ram","addr":"8009B26C","len":4}` reads,
`{"cmd":"write_mem","addr":hex,"hex":bytes}` writes several bytes,
`{"cmd":"write_ram","addr":hex,"val":byte}` writes one.

## 3. Hold lengths

| Where | Hold (frames) | Notes |
| --- | --- | --- |
| Title screen | 60 | START only. Ignored while the movie plays. |
| Main menu, loaded-save menu | 20 | |
| Opponent grid (Free Duel) | 6 | 12+ frames steps twice |
| In a duel | 6 | |

## 4. From a cold boot to a screen

The game loops attract title -> movie -> attract title. START during the
movie does nothing. START at the title (PUSH START BUTTON) opens the main
menu: NEW GAME / LOAD / 2P DUEL / TRADE / OPTION, cursor on NEW GAME. A second
START there selects NEW GAME and lands on name entry; restart the game if that
happens (there is no reset command).

`tools/game_route.py` detects the title from the screen (reference image
`tools/refs/title_80x60.png`, mean grey difference under 18) and then presses:

1. start 60 -> main menu
2. down 20 -> LOAD, cross 20, cross 20 (YES), cross 20 (dismiss LOAD COMPLETE)
3. loaded-save menu: CAMPAIGN / FREE DUEL / BUILD DECK / LIBRARY / PASSWORD / SAVE,
   cursor on CAMPAIGN. down x3 + cross -> LIBRARY. down x4 + cross -> PASSWORD.

```sh
SHOTDIR=/tmp/shots python3 tools/game_route.py menu|library|password
python3 tools/game_route.py shot <tag>
```

Mode byte 0x8009B26C: 0xC8 loaded-save menu, 0xC6 Free Duel grid, 0xC3 deck
view (and it stays 0xC3 into the duel). It is 0 at the title and main menu.
Leaving a sub-screen with circle puts the cursor back on the row you came
from, so count from there, not from CAMPAIGN.

LIBRARY (verified 2026-09-06): the grid lists all 722 cards as frame-colour
tiles (an edited frame colour shows here), name of the selected card at the
bottom. A d-pad hold repeats: 6 frames = one tile, 20 frames = four tiles.
cross 20 opens the card view (art, title strip, stars, attribute orb, type,
guardian stars, ATK/DEF, description). In the card view RIGHT shows the 3D
model, not the next card; circle backs out one level at a time (model ->
card -> grid -> menu).

BUILD DECK: the CHEST list shows name, ATK/DEF, type icon, guardian stars and
the deck count. circle leaves it.

PASSWORD: eight digits, cursor on the first. UP adds one, RIGHT moves to the
next digit (6-frame holds, ~0.5 s apart), cross 12 confirms and shows the card
(title strip, frame colour, art) with CARD NUMBER and the starchip price,
then an EXCHANGE / QUIT menu. down 6 + cross 12 quits without buying.

Free Duel and duels: `tools/goto_freeduel.py`, `tools/goto_duel.py`,
`tools/duel_turns.py`. From the loaded-save menu: FREE DUEL, cross 20,
cross 40 (clears the SELECT OPPONENT prompt) -> grid (mode 0xC6) with the
cursor on Build Deck. Grid layout here: row 0 Build Deck ... Simon; row 1 an
EMPTY tile under Build Deck, then three duelists; row 2 five duelists. DOWN
from Build Deck lands on the empty tile (no label, cross does nothing); RIGHT
twice (6 frames each) reaches the first duelist of row 1. cross 14 -> deck
view (mode 0xC3), circle 12 starts the duel, the hand is dealt ~15 s later.

To control the hand, write the 40-card deck at 0x801D0200 (u16 little-endian
ids) WHILE ON THE GRID, before cross: `{"cmd":"write_mem","addr":"801D0200",
"hex":"0100"*40}` deals five copies of card 1. The deck view shows the count
(40) beside that card. The duel hand shows the 40x32 thumbnails (thumb.png)
with ATK/DEF, the selected card's name, type icon and stars below.

The memory card file's mtime changes when the save is loaded; that is the
runtime mounting it, not a save. Always screenshot between steps.

HAZARD: the campaign's card shop menu has SAVE as its first item and "RETURN TO
TITLE" goes to the loaded-save menu, not the title. Never chain blind presses
in the campaign. Memory card backups: `~/Documents/ygofm-memcard-backup-*`.

## 5. Screenshots

`{"cmd":"screenshot","path":...}` writes the PSX display (320x240 PNG). It is
live while only the game window exists. Once a tool window (Card Manager, Drop
Table, Fusion, Dialogue) has been opened in the same run, it can go stale;
then use a desktop capture and crop the game window:

```sh
spectacle -b -n -o /tmp/desk.png
```

Tool windows never appear in the debug screenshot; each has its own canvas dump
(`card_manager_shot`, `drop_viewer_shot`, `fusion_manager_shot`), a PPM the
Pillow `Image.open` reads directly.

## 6. Card Manager from a script

```python
dbg.q({'cmd':'card_manager_set','open':1})       # opens next frame; wait ~2 s
s = dbg.q({'cmd':'card_manager'})                # state json, includes 'geom'
g = s['geom']
g['btn'][i]            # button centres: 0 Save, 1 Restore stock, 2 Open folder,
                       # 3 Pick art, 4 Pick thumbnail, 5 Pick title,
                       # 6 Effect text -> description, 7 + Add effect,
                       # 8 Export Descriptions, 9 Import Descriptions,
                       # 10 Export Config, 11 Import Config, 12 Dev Card Effects
g['value'][f]          # field rects [x,y,w,h]: 0 name, 1 description, 2 atk, 3 def,
                       # 4 star1, 5 star2, 6 type, 7 level, 8 attribute, 9 price,
                       # 10 password, 11 colour, then the effect fields
g['modal_ok'], g['modal_cancel']                 # the confirm dialog buttons
dbg.q({'cmd':'card_manager_set','card':1})       # select a card
dbg.q({'cmd':'card_manager_set','search':'elf'})
dbg.q({'cmd':'card_manager_set','import':path})  # preview modal for a .ygocards
dbg.q({'cmd':'card_manager_click','x':X,'y':Y,'button':1})
dbg.q({'cmd':'card_manager_key','key':'a','ctrl':1})   # return escape backspace tab
                                                         # up down left right home end delete
dbg.q({'cmd':'card_manager_type','text':'New name'})
dbg.q({'cmd':'card_manager_shot','path':'/tmp/cm.ppm'})
```

Editing a text field: click its value rect, ctrl+a, type, return, then click
Save (btn 0). The state json's `msg` is the status line; `edited` says whether
the selected card has a pack; `name`, `desc`, `atk` ... are the editor's values.

Packs without the window: `{"cmd":"card_packs"}` lists loaded packs and the
live directory, `{"cmd":"card_packs_reload"}` re-reads them (add `"card":id`
for one). Hand-written `card.ini` files are picked up on their own within a
second (known cards) or ten (new folders).

Share files without the dialog: `{"cmd":"card_share","op":"export"|"import"|"inspect","path":...}`.

`fusion_manager` also takes `press`:1 / `release`:1 beside `x`/`y` (with
`move`:1 for the waypoints), which is how a scrollbar thumb gets dragged: press
on the thumb, move, release. Its `geom` carries `list_sb`, `makes_sb`,
`from_sb` and `pick_sb`, and the state json has `mk_scroll`, `fr_scroll`,
`pick_scroll` and `sb_drag` (0, else the bar being dragged + 1) to check the
result. All four bars also page when the track is clicked. Verified
2026-09-06 on every one of them.

Other windows: `drop_viewer_set` (`open`, `view` 0 cards / 1 duelists, `card`,
`search`, `sort`, `desc`, and the file pair `export`:path / `import`:path,
which answer `{ok,msg}` and work with the window closed), `drop_viewer_shot`;
`fusion_manager` (`open`, `card`, `search`, `view`) whose state json has
`sel_name`.

The Drop Table Manager's top bar is Save, Import, Export and a view-dependent
slot (`geom` rects `save`, `import`, `export`, `third`; `hover_btn` numbers
them 2, 3, 4, 5 after the two tabs). Import and Export are SDL dialogs like
the Card Manager's, so section 7 applies: with
`SDL_FILE_DIALOG_DRIVER=nosuchdriver` an Export click falls back to
`drop_tables/drop-table.ini` in the player folder and an Import click says the
dialog could not open. Verified 2026-09-06 both ways, dialog and fallback, and
the real KDE dialog picked a file typed with `ydotool type` + Enter.

## 7. A save with barely any cards

The library / collection features need a save that has NOT finished the game,
and the one on this box owns everything. Fabricate the state in RAM instead --
never save while it is fabricated, and put it back afterwards:

```python
LIVE, MIR = 0x801D0200, 0x801D3200     # save struct and its +0x3000 mirror
snap = p.rd(LIVE, 0x680)               # restore from this when done
# trunk (what you own): 722 bytes at +0x50, card N at +(N-1)
write(LIVE+0x50, bytes(722)); write(MIR+0x50, bytes(722))
# seen flags: flag array at +0x418, card N is flag 0x120+N, MSB first
```

Entering the LIBRARY re-marks every owned card and every deck card as seen
(Library_MarkOwnedCards), so an emptied trunk still shows the deck's cards.
`{"cmd":"fill_library"}` reports seen / owned / cards without a screenshot,
and `{"cmd":"fill_library","on":1}` drives the MODS row.

## 8. Scripted story drops

`{"cmd":"story_rewards"}` reads the pairs and what the last drop decided;
`{"cmd":"story_rewards","duelist":0-38,"card":id,"every":0|1}` sets one (card
0 clears) and `"save":1` writes drop_table_edits.ini, which is where they live
now -- there is no MODS row and no separate file.

The gates, both measured 2026-09-06 in a campaign duel against Heishin:

| byte | meaning |
| --- | --- |
| 0x8009B361 | opponent id, 1..39, and the DROP DB index + 1 |
| 0x8009B365 | 0x80 while the duel came from the FREE DUEL screen (0x00 in the campaign) |
| 0x801D06F4 | Free Duel unlock mask, MSB first: id -> byte id>>3, bit 0x80>>(id&7). Set AFTER the drop, so "clear" is "never beaten" |

gFreeDuel_aDuelistRecords (0x801D071C) is NOT a first-win gate: a won campaign
duel leaves it at 0 wins, because FreeDuel_Init is what updates it.

Winning a duel from a script is the hard part -- writing 0 into the opponent's
life points (0x800EA024, player at 0x800EA004) does NOT end it, the game only
checks on damage. Ask the player to fight one, and watch with a one-second
poll of the bytes above plus the trunk byte (0x801D024F + card id).

## 9. File dialogs

Export/Import open an SDL3 save/open dialog, which goes through the KDE
portal here. Nothing scripted can fill it in through the debug server. Options:

- Skip it: use the `card_share` debug command.
- Make it fail on purpose so the fallback path runs:
  `SDL_FILE_DIALOG_DRIVER=nosuchdriver ./Play.sh -dbg --no-launcher`
  (Export Config then writes `edited-cards.ygocards` in the player folder).
- Drive the real dialog with `ydotool key 28:1 28:0` (Enter). This only works
  when the dialog has keyboard focus, which it gets when it was opened by a
  click while the game had focus. Typed text that lands nowhere visible means
  the dialog was not focused. `ydotool mousemove -a` coordinates do not map
  to this desktop's screens.

The KDE dialog has once come up with an empty name field (only `.ygocards`);
the manager fills in `edited-cards.ygocards` when that happens.

## 10. In-game places where an edited card is visible

- LIBRARY: art, title strip, level (stars), attribute orb, guardian stars,
  ATK/DEF, type, description, frame colour (card view and grid tile).
- PASSWORD: type the 8 digits; shows art, title, stars, ATK/DEF and price
  before you accept.
- BUILD DECK chest list: name, ATK/DEF, type icon, guardian stars.
- Duel hand: thumbnails (thumb.png), ATK/DEF, name; face-up cards on the
  field: art.

Checked on 2026-09-06 after an export -> wipe -> import cycle: name, four-line
description, ATK/DEF, both guardian stars, type, level, attribute, frame
colour (orange, purple, pink), art.png, thumb.png, an explicit title.png and a
title strip derived from the name, price and password on the PASSWORD screen.

## 11. Reading duel results

Field rows at 0x801A7AD8, 30 rows of 0x1C bytes (player hand 0..4, monsters
5..9, magic 10..14; opponent +15): +0xC id, +0xE ATK, +0x10 DEF, +0x12 modifier,
+0x14 terrain boost, +0x16 flags (0x8000 occupied, 0x1000 face-down, 0x800
defence). LP u16 at 0x800EA004 (player) / 0x800EA024 (opponent). Turn side
0x8009B1D5, terrain 0x8009B364, deck written at the grid: 0x801D0200.

## 12. Stopping

```sh
kill $(pidof Yu_Gi_Oh_Forbidden_Memories_Recompiled)
```

`pkill -f` with the binary name in the pattern also matches the shell that
runs it; use `pidof`.
