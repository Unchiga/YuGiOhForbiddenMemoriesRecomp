# Release notes

## 0.3.3

Extract this over your existing install as usual. Saves, savestates and your
`card_shop.ini` from 0.3.2 are unaffected.

### CARD SHOP is finished — `MODS → CARD SHOP`

![The card shop's pack panel](docs/screenshots/card-shop.png)

0.3.2 shipped it as `CARD SHOP WIP`. The WIP is gone: the shop now works the
same at **all three** of the game's shopkeepers and its card viewer is
indistinguishable from the deck builder's.

**It works at every shop now.** The row was only appearing at the campaign
shop. The other two — the tournament's card convention and the third — opened
their menu before the mod had staged its labels, so `CARD SHOP` simply was not
in the list. It is staged from the moment the game is running now, so whichever
shopkeeper you walk up to has it.

**Cards look right in the viewer.** TRIANGLE on a card you just bought opens
the game's own card viewer, and at the two shops that never stock its art that
came up as a bare face over an empty frame — no name plate, no card body, no
stone panels, no orb, no stars, no ATK/DFD, and the flip-in animation playing
with half a card back. All of it is baked from your disc and lent to the viewer
for as long as it is open, then handed back. Monsters, magic, equip, traps and
rituals all read correctly, at every shop.

**The pack-open ceremony reads better.** Every card a pack will yield shows as
an empty slot from the moment you buy, so you can see how many are coming and
each **X** fills one. The highlight no longer draws a line through the name it
is highlighting, the rows have room to breathe, and the button legend is
honest: **X** says *Next* while cards are still turning over and *Continue*
once they are all up, and **X BUY / O CLOSE** comes back afterwards instead of
leaving the panel with no legend at all.

`cards` in `card_shop.ini` is limited to 1–3, which is what the results box can
print. It used to accept 5 and hand you two cards it never showed you.

### Fixed: the save-state menu could be hidden behind the game

**F7** opened the slot list and took your keys, but anything the mods drew in
the game's own picture — the card shop's panel most obviously — was composited
on top of it, so on a full-screen panel you could not see the menu you were
driving. Menus, the rewind bar and the volume readout now sit above everything
the game is wearing.

## 0.3.2

Extract this over your existing install as usual. Saves and savestates from
0.3.1 are unaffected.

### New: game speed that keeps the music in tune — `GAME → SPEED`

Speed up or slow down the game **without the audio changing pitch**. The
speed control scales the frame pacer and the console's video timing
together, so the sound hardware's own clock never moves: music and effects
play at their proper pitch whether you are running at half speed or double.
Handy for grinding drops at speed, or slowing a duel down to read what the
opponent just did.

### Improved: sound effects respond faster

The delay between pressing a button and hearing the game answer is down
from roughly **170 ms to about 60 ms**. Menus feel considerably crisper.

### Fixed: CARD DROPS listed the previous duel's cards after a loss

The extra results page describes what a duel awarded, and a duel you LOSE
awards nothing — but the page kept showing the cards from the last duel you
won. It now forgets a duel's cards once you leave its results screen, so a
loss shows nothing. The chest's "New!" marking is unaffected.

### New, and unfinished: CARD SHOP — `MODS → CARD SHOP WIP`

**Off by default, and marked WIP for a reason: it is still being built.**
Turn it on and the shopkeeper's menu grows a fifth row, CARD SHOP, that
buys card packs with your starchips.

What is there today: the shopkeeper asks what you are looking for in his
own textbox; four pack types (monster, magic, equip, trap) across four
rarities — common, uncommon, rare and legendary — priced 20 / 80 / 200 /
800 starchips; every one of the game's 722 cards sits in a pool, so
anything can be pulled. Buying deals the pack out one card at a time,
and once the cards are face up you can pick one and press TRIANGLE to
open the game's own card viewer on it. Cards land in your trunk marked
"New!", exactly like a duel drop.

Prices, rarity bands and individual card placements are yours to edit:
the shop writes **`card_shop.ini`** next to your saves the first time it
runs, and re-reads it whenever you leave and re-enter the shop.

Expect rough edges — that is what WIP means here. Feedback welcome.

## 0.3.1

Extract this over your existing install as usual. Saves and savestates from
0.3.0 are unaffected.

### Fixed: duelist portraits in the Drop Table Manager

**0.3.0 shipped with every portrait plate empty.** The portraits are Konami's
art, so they are never in the download — but 0.3.0 had no way to get them at
all: the capture only existed as a developer tool, so player builds drew
plain plates everywhere.

Now **the game captures its own portraits, from its own screen.** Open FREE
DUEL and move through the opponent list once; every portrait on screen is
captured as you go, the Manager fills in live if it is open, and the set is
kept next to your saves (`duelist_icons.bin`) so it is there for good.

Nothing changes about what ships: the art comes from your machine, your
disc, your screen, and never leaves.

**A duelist your campaign never met draws nothing on that screen**, so
their plate stays empty until you meet them — the Manager fills in as
your campaign does. The portraits live in the Manager's own cache, never
in your save.

### New: Widescreen (experimental) — `VIEW → WIDESCREEN`

Contributed by **yamyi** — this project's first outside contribution.
Projected 3D (the duel field) renders genuinely wider at 16:9; flat 2D
screens stay 4:3, pillarboxed rather than stretched. Toggle it in the VIEW
menu or enable the `psx.enhancement.widescreen` mod-catalog feature; the
toggle reshapes the window live. Works with both `VIDEO → SCALING` modes:
FILL WINDOW stretches to fit, INTEGER keeps whole pixels and snaps the
wide frame to the largest multiple that fits. Overlays that ride the
duel screen (the rank badge, the fusion hint) stay anchored to the boxes
they label in wide mode, and menu screens no longer pulse between wide
and 4:3 after a duel (the FREE DUEL cursor's sparkle was misread as 3D
for a moment every couple of seconds). Experimental: culling pop-in at
the wide edges has not been fully checked for this title.

### Fixed: first-run build error on machines with the Vulkan SDK

If 0.3.0's first-run setup failed for you with
`error: use of undeclared identifier 'cpu'` (around main.cpp:13332) and
`no matching function for call to 'psx_netplay_bind_cpu'`, this release
fixes it. The CPU state variable was accidentally guarded out of the
build exactly when a Vulkan SDK was detected on your machine, so setups
on machines with the LunarG SDK installed could not compile. 0.3.0
workaround, if you need one before updating: temporarily clear the
`VULKAN_SDK` environment variable and re-run the setup.

## 0.3.0

Extract this over your existing install as usual. Your saves live in
`Documents\My Games\Yu-Gi-Oh Forbidden Memories Recompiled` and nothing this
update writes can reach them.

> **Save STATES from 0.2.x will not load** in this version — the recompiler
> itself changed underneath them (see *Framework* below) and the loader
> refuses a state made by different codegen rather than resuming into it.
> **Memory-card saves are unaffected** and carry over as always. If you have
> a savestate you care about, open it in 0.2.6 once and save in-game first.

### New: the Drop Table Manager — `VIEW → DROP TABLE MANAGER`

The drop table viewer grew into a **manager**: it no longer just shows every
duelist's drop table — **you can rewrite them, and the game rolls what you
wrote.**

- **Edit anything.** Click a weight and type a new one; click the rank cell
  to move a drop between S/A POW, B/C/D and S/A TEC. Right-click any row for
  add / move / remove. Drag a card from the list and drop it on a duelist to
  add it — the new `ALL CPU` toggle lists every duelist under a card, greyed
  where they do not drop it, so all 39 are drop targets without switching
  views.
- **The math stays honest.** Every band always totals exactly 2048 — the
  invariant the game's own drop roll assumes — so whatever you add or grow
  comes off that duelist's other drops in proportion, and an edit that cannot
  balance is refused with a message instead of fudged.
- **Nothing sticks until you press `SAVE`**, which writes
  `drop_table_edits.ini` next to your saves — hand-editable, same shape as
  the DROP MISSING CARDS file. `DEFAULTS` returns the selected duelist to
  stock (stock *plus* the mod, if that row is on: your edits layer cleanly on
  top of it, in the window and in the game alike).
- **Share your tables.** `LOAD → EXPORT CURRENT` writes a timestamped file
  into `drop_tables\` beside your saves; anyone can drop that file into their
  own `drop_tables\` folder and pick it from `LOAD`. Loading, too, is nothing
  until saved.
- Quality of life throughout: scrollbars in both panes (722 cards is a lot
  of mouse wheel), TYPE and DROPS columns with full column sorting in every
  pane, a blinking search caret, hover highlights, and `N-M OF K` position
  indicators.
- Fixed: hovering the manager's window while it overlapped the game no
  longer hovers the game's F10 menu underneath it.

### Framework catch-up

The bundled PSXRecomp framework caught up with 53 upstream commits. The
headline gains: an LWL/LWR load-delay fix in the recompiler (a real
correctness class, and the reason old savestates are refused), the PGXP
Precision mod (framework-owned, off by default), SPU voice address
alignment, CD-DA position-report interrupts, and a fix for vsync and the
frame pacer double-blocking presents.

## 0.2.6

Extract this over your existing install as usual. Your saves live in
`Documents\My Games\Yu-Gi-Oh Forbidden Memories Recompiled` from 0.2.3 onward,
so nothing this update writes can reach them.

### New: `MODS → DROP MISSING CARDS`

**82 of the game's 722 cards are dropped by nobody.** Both of Exodia's legs are
among them, which is why the card list cannot be completed by duelling in the
stock game. Turn this on and every one of them has a source.

Nothing on your disc is touched. A duel loads the current opponent's drop
weights into memory, and this rewrites that copy — so the change lasts exactly
as long as the duel does.

The placement is yours to change. On first run it writes
**`drop_missing_cards.ini`** next to your saves, listing every card by name
under the duelist that drops it:

```ini
[Weevil Underwood]
52  =  30,  20,   0   ; Hercules Beetle
278 =  30,  20,   0   ; Petit Moth
```

The three numbers are the S/A POW, B/C/D and S/A TEC rates, as weights out of
2048 — 20 is about 1%. Each band always totals 2048, so whatever you add comes
off that duelist's normal drops in proportion. The shipped table adds about 1–6%
per duelist, which you will not notice. Delete the file to get the defaults
back.

### Fixed: `CHEATS → ALL CARDS` before the title screen

**Turning `ALL CARDS` on during the Konami logos or the intro movie distorted
the picture and then froze the game.** It never reached the title. This is in
0.2.5 too.

The row writes card counts to three places in memory, and one of those is only
the card chest's working buffer while the chest is the screen you are on. Before
a save is loaded it belongs to the intro instead, so the row was writing over
the movie as it played.

The three rows that write save data — `ALL CARDS`, `STARCHIPS` and
`FREE SPENDING` — now check that a save is actually loaded first, and say
"load a save first" instead of doing anything if it is not. `ALL CARDS` also
leaves the chest's buffer alone unless it really is the chest's buffer, which
means the same mistake cannot corrupt a duel or a shop screen either.

### No more startup toast for cheats you never touched

Stored cheat settings are re-applied as the game starts, and the two reveal rows
announced themselves while that happened — every launch opened with
"Their cards: face up", even when the setting was OFF and nothing had changed.
The message is still there when you move the row yourself.

## 0.2.5

Extract this over your existing install as usual. Your saves live in
`Documents\My Games\Yu-Gi-Oh Forbidden Memories Recompiled` from 0.2.3 onward,
so nothing this update writes can reach them.

### Updates now actually rebuild

**If your install folder has brackets in its name — `ygofm-0.2.5-win-x64(1)`,
which is what your browser makes the second time you download the same zip —
every update so far has silently done nothing.** You would extract the new
version, launch it, watch it generate, and still be playing the old build. No
error, anywhere.

The updater hands off to a rebuild step that has to run after the game exits.
That handoff lost the folder path at the first bracket, so the rebuild never
started. Fixed. A `&` in the folder name broke it the same way.

If you are on a broken install right now, this release repairs it — extract it
over the top and launch it once. It will rebuild, and that first launch takes a
few minutes.

### Two new cheats — `CHEATS`

**`SHOW OPPONENT HAND`.** See what the CPU is holding.

**`FORCE FACE UP`.** The computer sets cards face down constantly and you are
meant to guess. With this on, anything it sets plays face up instead — and
cards already sitting face down on its field turn over too, the next time the
view redraws.

### Fusion Assistant

Equips are worth what they add. Equips such as 'Megamorph' weren't being
properly considered, fixed.

### `VIDEO → WINDOWED SCALE` is a slider

It was a cycling option: eight presses to get from 3x to 7x, reading the label
after each one. It is now a track with a notch per step — drag it, click
anywhere on it, or type a value.

### Disc timing: a stability fix

Duels would occasionally freeze while fast loading was set to INSTANT, fixed.

## 0.2.4

> **About 0.2.3:** it was published and withdrawn within about five minutes and
> was not stable — its launch-time update check silently disabled itself. If
> you happened to grab it, replace it with this build. Everything below applies
> to 0.2.4, and 0.2.3 is best treated as never having shipped.

**Read this if you are updating from 0.2.2 or earlier — it affects your saves.**

### Install this update over your existing folder

Extract this release **on top of your current install**, replacing the files
when Windows asks. That is the whole procedure, and it is what lets the game
find your existing saves.

On the first launch after that, the game copies your memory cards and save
states out of the game folder and into

```
Documents\My Games\Yu-Gi-Oh Forbidden Memories Recompiled
```

You do not have to move anything yourself. The originals are **copied, not
moved** — they stay in the old folder untouched, so nothing is lost if
something goes wrong or you want to go back.

### If you extracted somewhere else instead

Your saves are not gone. They are still sitting in your old game folder,
where every previous version kept them. Copy these from the **old** folder
into `Documents\My Games\Yu-Gi-Oh Forbidden Memories Recompiled`:

- `card1.mcd`, `card2.mcd` — memory cards
- the `saves\` folder (its `openbios\` subfolder holds your save states)

Then launch the game again.

### After this, updates stop being fragile

From 0.2.3 onward your saves live outside the game folder for good. Later
updates can be extracted anywhere — over the top, into a fresh folder, it no
longer matters, because nothing an update writes can reach them.

Before this change, saves survived an update only because the release archive
happened not to contain a `saves\` folder, and extracting into a new folder
silently left every save behind. That is what this release fixes.

Running the game off a USB stick or a shared machine? Put an empty file named
`portable.txt` next to the exe (or set `PSX_PORTABLE=1`) and everything stays
in the game folder as before.

---

### Also in this release

**Update notifications.** The game now checks for a newer release each time it
starts and offers to open the download page. It never downloads or installs
anything by itself. Set `update_check=0` in `menu_settings.ini` to turn the
check off entirely — with that set, no request is made at all.

**VIDEO > WINDOWED SCALE.** A new option under SCREEN, 1x to 8x, default 3x.
It resizes the window so the picture is an exact whole-number multiple of the
original 320x240 — at 3x that is a 960x720 picture with no blurring and no
uneven pixels. It applies when SCREEN is WINDOWED and SCALING is INTEGER; the
row says so when it is not.

**The menu bar no longer covers the game.** The picture was being drawn behind
the menu bar instead of below it, so the top of the screen was hidden and the
image sat off-centre. Maximising changed how much was covered but never fixed
it.
