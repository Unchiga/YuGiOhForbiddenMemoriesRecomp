# Card count 722 -> 790: what's done, what's left

> **2026-08-30 session findings are at the bottom** ("THE 2026-08-30 SESSION").
> They RESOLVE bug A's mass-blank form (stock behaviour, not a bug), REPRODUCE
> the crash class live with a full mechanism dossier, and scout the Spell/Trap
> effect machinery for the next card-count push.

The chest / deck-builder scrolls **001-790 as one continuous list**, scroll bar
sized correctly, all 722 existing cards' data byte-for-byte intact, and ids
**723-790 are 68 REAL Yu-Gi-Oh monsters the game does not otherwise have** --
real names, real ATK/DEF, real type and Guardian Stars -- whose counts persist
to the memory card in the mod's own save file. Verified live on `build-dbg`.

**766 -> 790 (2026-08-29 session).** The old ceiling was the name-offset table:
ids past 767 collided with the monster-type / Guardian-Star strings at string id
0x8300. That block turned out to be produced by ONE instruction, not the two an
earlier pass assumed, so moving it is a single word plus 34 mirrored table
entries -- see "the type/Guardian-Star relocation" below. 790 is now set by the
CARD POOL (69 usable monsters, and the count must be even), not by the engine;
the next wall is index 808 (menu text / duelist names), i.e. id 807.

Open: card **ART** (the viewer's id->WA_MRG table ends at 722), and the **crash**
on a deck edit followed by a pane switch (not reproduced this session in ~30
edits). The two bugs that headed this list are resolved -- see below.

The count is one parameter: `PSX_CARD_EXT_LAST` in `src/psx_card_extend.h`.
Everything (card DB tables, chest structure stretch, layout asserts) derives
from it. The next larger number is mostly a matter of raising it and re-checking
the two ceilings called out below.

---

## What works now (do not re-derive)

### `src/psx_card_extend.c` — the card DB side (unchanged mechanism from 740)
Relocates **stats / alpha-rank / aux** into `0x801CD5A0` and up, now laid out
**derived from `EXT_COUNT`, 32-byte aligned** (the old 740 build had hand-placed
bases that 800 would have overrun). Repoints the 79 addiu sites, re-asserted per
frame. Clone names for ids **723–767** are written into the stock name table's
45 free filler slots and re-asserted per frame (the name table is re-streamed
from disc per screen, so a one-time write goes stale). `psx_card_ext_trunk_get/
set` hold clone trunk counts mod-side.

### `src/psx_card_chest.c` — the chest structure stretch (new, the hard part)
The chest arena (`*(u32*)0x80010000` == `0x80100000`) holds two instances
(stride `0x6344`). The chest and deck panes are the **same generic list code**:
`base = inst + 4 + idx*0x2D4C`, metadata at `base+0x2D34`. That `*0x2D4C` stride
is **not an immediate** — it is three 9-instruction shift/add chains
(`0x80031924`, `0x8003355C`, `0x8003370C`), each rewritten to `li/mult/mflo`.
The stretch shifts every field group by its own amount (chest meta +S, deck
list +S, deck meta/seen +2S, trunk +2S+D, slot-lookup +2S+2D, tail +2S+3D,
instance stride to `align16(0x6344+2S+3D)`), where `S = 16*(N-722)`. The 175
intra-instance immediate sites are in the **generated** `psx_card_chest_sites.h`
(regenerate with `scratchpad/gen_c_table.py` if the count model ever changes;
it reads a stock-code RAM dump). Six verified count literals and the builder's
one computed instance-2 access are patched as full words.

Two live traps this cost time on, now handled:
- **No self-heal.** An early version cleared the mode init bit to rebuild a
  stale chest; it fired during the MENU→chest transition (mode reads 0xC7 while
  the arena is still menu scratch) and **deadlocked the menu's modal loop**,
  needing the process killed. Removed. A wrong-looking stale chest just needs
  re-entering; that beats a hang.
- **Viewer clamp.** The triangle card viewer's art loader resolves id→WA_MRG
  through a 722-entry table; asking for a clone id wedges it and the chest hangs
  behind the pump forever. The two chest-side `sh s0, B246` mailbox writers are
  redirected through a 7-word stub at `0x801CFFC0` that stores `min(id, 58)`, so
  a clone shows Kuriboh (right art/name/stats/Stars/description). A between-
  frames clamp loses the race — it must be the writer.

### `src/psx_card_extend.c` — the type/Guardian-Star relocation (new)
A card's name is string id `0x8000+id`, and the offset table at 0x801D5800 is
indexed by `id`. Indices 723..767 are free filler; 768 is not -- string id
0x8300 begins the monster-type and Guardian-Star words. That was the 766 wall.

**There is exactly ONE producer, not two.** The earlier note said the Guardian-
Star producer "has not been located" and treated a partial move as dangerous.
It is the same instruction as the type producer. All three consumers --
type `(stats>>26)&0x1F`, Guardian Star 2 `(stats>>22)&0xF` +0x17, Guardian
Star 1 `(stats>>18)&0xF` +0x17 -- fall through to `ori v0,zero,0x8300 ; addu
a1,a1,v0` at 0x80037F14/0x80037F18. A whole-binary scan finds no other 0x8300
immediate. (Found by reading /c/dev/ygofm-decomp/asm/asm_285A4.s.)

So the block moves with one word plus its 34 table entries. The 34 offsets are a
constant in the source, not copied at runtime -- once card names occupy 768..
there is nothing left to copy from.

**THE TABLE IS EXACTLY 1024 ENTRIES, and that is a trap.** 0x801D5800 + 1024*2
= 0x801D6000, and the STRING BLOB starts at 0x801D6001 with card id 1's name.
Reading 0x801D5800 as table all the way up keeps returning plausible-looking
"entries" past the end -- they are string bytes. The first attempt put the block
at index 1024 (0x8400) on exactly that misreading and **overwrote the names of
ids 1..5**: the chest drew them as garbage while type and Star words still
looked correct, so the obvious check passed and the damage was two columns away.
(The old "823-entry table" figure in this file was also low; real entries end at
index 863.)

The real free run is **864..1023**, all holding one filler offset (0x8ACE). The
block now sits at **896..929 (string ids 0x8380..0x83A1)**, which also stays
clear of 0x83BF / 0x83C0 -- two high ids func_80035E20 references. Three static
asserts in psx_card_extend.c now pin the range inside the table, above the last
real entry, and off 0x83BF/0x83C0.

Verified after the move: ids 1..8 read Blue-eyes White Dragon / Mystical Elf /
Hitotsu-me Giant / Baby Dragon / Ryu-kishin / Feral Imp / Winged Dragon #1 /
Mushroom Man, and the viewer reads type "Dragon", Stars "Sun" / "Mars".

`assert_type_gs_strings()` republishes the entries per frame (same re-streamed
table as the card names) and only points the producer at them once all 34 are
in place, reverting to stock if a screen stream wipes them. Verified live in the
card viewer with the patch active: type "Fiend", Guardian Stars "Mercury" and
"Saturn". This raises `NAME_FREE_LAST` from 767 to **807**.

### `src/psx_ygo_cheats.c` — ALL CARDS
Now **clamps save-trunk writes to 722** unconditionally (writing id 723+ into the
save block is corruption — the stock block has exactly 722 trunk bytes with live
fields right after) and routes 723+ to `psx_card_ext_trunk_set`. UI-copy test
uses `psx_card_chest_ui_trunk_cell()` so it hits the relocated working buffer.

---

## Environment / testing (same as before)
- `export PATH="/c/msys64/mingw64/bin:$PATH"`; build `cmake --build build-dbg -j 12`
  with `/c/msys64/mingw64/bin/cmake.exe`. Close the game before linking.
- Launch detached, **no stdout/stderr redirect** (SDL dies). Debug server
  `127.0.0.1:4370`; helpers in `tools/dbg.py` (`q`, `rd`, `u32`).
- **Savestate slot 11 (wire) = main menu, cursor on LIBRARY** — made this
  session; `up cross` enters BUILD DECK. Slot 6 = a new-game library.
- Whole-RAM dump in one command: `{"cmd":"ram_dump_file","addr":..,"len":..,
  "path":..}` — far faster than socket reads for offline disassembly.
- **L2/R2 fast-scroll the list**, L1/R1 page by 8, up/down by 1 (per maintainer).
- `scratchpad/mdis.py ADDR N` disassembles the RAM dump offline; `prov3.py` is
  the dataflow tracker that found the 175 sites.
- Memory-card backups this session: `playerdata-backup-2026-08-29/
  mcd-backup-800-verified/`. **Re-copy before every risky run.**

---

## The added cards are REAL cards now, not Kuriboh clones

`src/psx_card_new_cards.h` (generated by `tools/cards/gen_new_cards.py` -- the
generator and all its inputs live in the repo, NOT in a session scratchpad)
holds 68 real Yu-Gi-Oh monsters that Forbidden Memories does not have, at ids
723..790, each with its real name, ATK, DEF and monster type.

**The pool is the ceiling now, not the engine.** 69 of the 179 missing cards are
monsters and all 69 encode cleanly (no skips), so ids run to 791; the count must
be EVEN, hence 790. Going past that means either the remaining 110 Spell/Trap
cards -- which need effects bound to card ids in code, see below -- or accepting
placeholder names. The engine itself is good to id 807.

### How "not in the game" was decided
The maintainer supplied two yugipedia debut-date queries (582 unique cards).
Diffing them by name against the game's own 722 is WRONG on its own: FM ships
many cards under older or misspelled names, so 37 of the 722 would look
"missing" and get added twice. The authoritative mapping comes from yugipedia
itself -- every `<FM name> (FMR)` page carries `| main = <canonical name>`:

    scratchpad/fetch_fmr.py       -> fmr_cards.json  (722 rows, SMW ask API)
    scratchpad/fetch_fmr_main.py  -> fmr_main.json   (FM name -> canonical, 37 renamed)

With that, matching on EITHER name, 179 of the 582 are genuinely absent. It
correctly rejects Archfiend Marmot of Nefariousness (= FM's Air Marmot of
Nefariousness), Archfiend Mirror (= Wicked Mirror), Thunder Kid (= FM's
untranslated Kaminarikozou, #211), Trial of Nightmare (= Trial of Nightmares)
and Fiend Reflection #2 (= FM's typo Fiend Refrection #2). It confirms FM
really has no Mirror Force, Monster Reborn, Pot of Greed, Change of Heart,
Waboku, Fissure or Trap Hole -- and that FM's burn ladder (Sparks 50 / Hinotama
100 / Final Flame 200 / Ookazi 500 / Tremendous Fire 1000) and heal ladder
(Mooyan Curry 200 / Red Medicine 500 / Goblin's Secret Remedy 600 / Soul of the
Pure 800 / Dian Keto 1000) are each missing exactly one rung: Raimei 300 and
Blue Medicine 400.

### MONSTERS ONLY, on purpose
Of the 179, 69 are monsters and 110 are Spell/Trap. An FM monster has no
effect -- it IS its stats -- so a new monster written into the tables duels
correctly. A Spell or Trap also needs its EFFECT, which FM binds to specific
card ids in code; a new "Mirror Force" would collect, sort and sit in a deck
and then do nothing. So the added cards are monsters.

### The stats word, fully decoded
Correlating all 722 stock words against the game's published data: 0 mismatches
on ATK/DEF across 722, and 621/621 on Guardian Star pairs.

    bits  0..8   ATK / 10
    bits  9..17  DEF / 10
    bits 18..21  Guardian Star 1     1 Mars     2 Jupiter  3 Saturn   4 Uranus
    bits 22..25  Guardian Star 2     5 Pluto    6 Neptune  7 Mercury  8 Sun
    bits 26..30  type                9 Moon    10 Venus
                 0..19 monster types in stock order, 20 Spell, 21 Trap,
                 22 Ritual, 23 Equip

Level is NOT in this word (FM has no tributes and never shows one), and the aux
byte at 0x801D5332 is still unidentified -- new cards borrow a stock one.
Guardian Stars have no real-world counterpart, so each new card gets the most
common pair among the stock cards of its type (measured; gs_convention.json).

### Real names, written into the text blob
A card name is one byte per character in the game's own frequency-ordered code
table (the table in psx_card_name_color.c / psx_card_db.c), 0xFF-terminated, at
`0x801D0000 + <the u16 in the offset table at 0x801D5800>`. So a real name only
needs bytes somewhere in u16 reach of 0x801D0000:

  - the stock indexed blob ends at 0x801D8C66 (measured: furthest offset in the
    whole 823-entry table, plus its string);
  - 0x801D916F..0x801DA000 past it reads ZERO in all six sampled states
    (library, chest, duel, fresh game, two others), so the strings go at
    0x801D9200.

Both halves are re-asserted per frame for different reasons: the offset table
is re-streamed from disc per screen, and the strings sit in space a savestate
restores. The strings are checked through one marker word ('NAME' at the end of
the region) rather than byte by byte.

### What decides the count now
  - **Name slots: 723..807.** 723..767 are the stock table's free filler;
    768..807 were freed by the type/Guardian-Star relocation above. Index 808
    is menu text and duelist names -- that is the next real wall.
  - **The count must be EVEN.** psx_card_chest.c's stretch shifts the u16 tail
    fields by 3*D and that has to stay halfword-aligned (`chest_d_even`).
  - **The monster pool: 69 usable.** This is what actually binds at 790.
Geometry is fine well past that: at N=806 two stretched instances still fit
(0xDD80 <= 0xE000), and the chest arrays are good to ~1004.

---

## DECK PANE: two open bugs with the new ids (measured, not yet fixed)

Both were found by putting Bite Shoes (id 766, 500/300 Fiend) in the deck.

### A. The deck row draws blank name / type icon / Guardian Stars
The row shows the right card number and the right ATK/DEF, and nothing else.

**It is NOT an id bound.** Poking only the deck record's id halfword and letting
the row redraw:

    id 514 -> renders fully   id 722 -> renders fully
    id 723 -> renders fully   id 766 -> renders fully (after the list re-sorted)

and in every case it drew *Bite Shoes'* name and stats, not the poked id's --
so the row content comes from the record / row object, not from a fresh lookup
by id. The record itself is correct in both panes:

    chest rec 766  00 01 00 00 fe 02 f4 01 2c 01 07 08 c7 80 00 00
    deck  rec 766  b1 04 f4 01 fe 02 f4 01 2c 01 07 00 6c 01 4a 00
                   ^sort key  ^atk  ^id   ^atk  ^def  ^type

(deck record [0:2] is the sort key and is exactly `DEF*4 + 1` -- verified on
five cards.) The row-draw at 0x800319EC gates the extra columns on the record's
flag byte, and 0x80031B0C gates ATK/DEF on `type < 20`; Bite Shoes passes both.

So the blank is a **build-time** state, not a draw-time bound: the row resolved
its name/icons once, at a moment when something was not yet in place, and kept
the empty result until the list was rebuilt (a re-sort fixes it). Prime suspect
is the per-frame re-assertion racing the row build -- the name-offset table is
re-streamed from disc per screen and index 767 shows the stock filler resolves
to the EMPTY string, so a row built in that window would cache "".

### B. Deck add/remove -- DOES NOT REPRODUCE (2026-08-29)

The earlier entry here said the deck edit updated neither the working trunk nor
the "per-card deck-slot lookup" for extended ids. **Re-measured on the running
game, that is wrong, and the reasoning behind it was wrong too.**

**The array it measured is not in the edit path.** The gate that limits a card
to three copies in a deck reads `inst+0x5AC4+id` -- the array this file calls
the "seen flags". `inst+0x606A+id` is written only by the builder
(func_800323F8) and read by the chest row draw; nothing in add or remove
touches it, so `deckslot[766] = 0` was a reading of an uninvolved array.

The decomp at /c/dev/ygofm-decomp names the whole path:

    func_800336F0   add to deck     gates on idx!=0, 0x5AA0<40,
                                    trunk[id]!=0, in_deck[id]<3
                                    -> func_800320BC (claim slot, fill from the
                                       stats table), func_80031F7C (trunk--,
                                       total--), func_8003201C (histogram)
    func_8003353C   remove from deck -> clear the row flag, func_80032C48 (sort),
                                    func_8003201C, func_80031EE4 (trunk++, total++)
    func_8003201C   rebuilds in_deck[1..N] from the 40 slots and 0x5AA0

Measured across four full out-and-in cycles on id 766, then again on id 790
after the count was raised:

    start   total=769  sum(trunk)=769  deck=37  trunk[766]=0  in_deck[766]=1
    remove  total=770  sum(trunk)=770  deck=36  trunk[766]=1  in_deck[766]=0
    add     total=769  sum(trunk)=769  deck=37  trunk[766]=0  in_deck[766]=1

`owned_total == sum(trunk)` held at EVERY step -- 0x5A9C counts what is in the
trunk, so that is the invariant, not `sum(trunk)+deck_size`. The write trace
shows all four writes landing: the histogram, 0x5AA0 (deck size), 0x5A9C
(total) and the trunk cell. The three-copy gate holds for extended ids too:
given five copies of 766 the adds stopped dead at three.

The report of four copies in the deck therefore came from a state this build
does not produce. Most likely it was an N=800 build against an arena built at a
different count -- this file quotes N=800 addresses throughout. If it recurs,
capture `owned_total` and `sum(trunk)` together: they diverge only if something
outside func_80031EE4/F7C writes 0x5A9C, and `sync_ext_trunk` is the only such
writer in the mod.

### A2. The deck's left-hand row numbers -- FIXED (2026-08-29)

The deck pane numbers its rows `scroll + row + 1`, so a full deck should read
01..40. It read `01, 0, 00, 0, 0, 0, 0, 0`: **row 0 correct, rows 1..7 garbage.**

**Cause: a patched immediate inside a loop body is only honoured on the first
iteration.** The scroll comes from `lh a0, 0x2D3C(list)` at 0x80031A34, an
A-group site the stretch moves to 0x2D3C+SH_A. `psx_mod_write_code_word` marks
the page executable-dirty, so entering func_80031874 routes through the
recovering dirty-RAM interpreter -- but the interpreter hands back to the
statically compiled block, and the row loop's back edge (0x80031C9C ->
0x800319E0) stays native from then on. Rows 1..7 run the ORIGINAL instruction.

Four measurements pin it, in increasing force:
  - the digit buffer at 0x1F800000 (s3 is 0x1F800000, NOT the 0x1F800060 sprite)
    showed rows 1..7 staging 0xD9/0xFF, 0xD9/0x00, 0xDA/0xF7 ...;
  - the stale address (deck list base + UNPATCHED 0x2D3C = inst+0x5D4C) held
    -2953, and `int_to_digits(-2953+row+1, 2)` reproduces those byte pairs
    exactly, row for row;
  - poking the scroll term to a constant 0 changed nothing;
  - poking the whole value to a constant 42 rendered "42" on row 0 ONLY.

**Fix: `mirror_deck_scroll()` in psx_card_chest.c.** Patching harder is the
wrong lever -- the site is already correct, the recompiler just does not re-enter
it. Instead both forms are made to read the same number: the stale address lands
in the deck list's dead record tail (the space a 766-record chest list would
occupy; the 40-slot deck never touches it, watchpointed across a full scroll),
so the real scroll is mirrored there every frame. Verified in the built binary:
rows read 01..08 at the top and 33..40 at the bottom.

**This is a class, not a one-off.** Any patched immediate inside a loop body has
the same exposure. The one in this loop was the only visible casualty, but a
sweep of `psx_card_chest_sites.h` for sites inside loops is worth doing before
trusting the next stretch.

### Do not SAVE while this is unfixed
The counts in the working trunk and the deck are inconsistent, and the chest
exit copies the working trunk into the save. The stock save file cannot be hurt
(the session writes BASLUS-01411-YUGIOX), but the mod save can be left wrong.

---

## What's left

### 1. Save persistence — DONE (`src/psx_card_save.{c,h}`)
Extended trunk counts now survive a power cycle, in a versioned block appended
to the memory-card image. Verified live this session: the block reaches the
card, comes back on LOAD, and the stock save file is byte-identical afterwards.

**The trunk is NOT grown in place, deliberately.** 722 -> 800 at struct+0x50
shifts every field past +0x322 (starchips, met bitmap, New! ring, progress),
reached both absolutely and as register+offset off the struct base across the
duel/chest/shop/library code -- a whole-binary dataflow job whose failure mode
is silent save corruption. And it would buy nothing: the chest exit copy at
0x80033B7C writes ONE contiguous run, so extended ids still have to stage
separately (which sync_ext_trunk already does). 0x80033B7C is therefore still
bounded at 722, on purpose.

What the memory-card image actually is (measured, and confirmed against the
.mcd on disk):

    0x801D3000  0x200  card header, memcpy'd from the template at 0x801D4000
    0x801D3200  0x680  save struct copy 1      file offset 0x200
    0x801D3880  0x680  save struct copy 2      file offset 0x880  (the game's
                       own backup: func_8003D03C's memcpy at 0x8003D0D0)
    0x801D3F00  0x100  UNUSED, zero in every state sampled  <-- the mod block

0xD00 == 2 * 0x680 is what identifies the tail as the backup rather than slack.
The file is one 8192-byte block, so 0xF00..0x2000 is erased 0xFF and there is
room to write past it without changing the block count func_8003EBD8 asks for.

Two immediates carry the block in and out:

    0x8003F8B0  write length  0xD00 -> 0xE00
    0x8003F7E4  read  length  0x680 -> 0xE00

The read grows past copy 1 because the block is at the END of the image, so
reading it back means reading the whole thing; that also refills the copy-2
buffer from the file, which is the same content it already held.

Block layout: magic `YGXT`, u16 version, u16 first, u16 last, u16 sum, then one
count byte per extended id. Wrong magic = "this save has no extended cards"
(counts cleared, silent); wrong version or a bad sum = refused with an OSD
line; a save from a LARGER build imports what fits and says so.

**Its own save FILE, not its own .mcd.** A deck slot can hold an id above 722,
which the stock game would resolve through 722-entry tables, so the saves must
not be interchangeable -- but the layout is a strict superset, so nothing has
to move on disk. The mod writes `BASLUS-01411-YUGIOX` and leaves
`BASLUS-01411-YUGIOH` alone; both live on the same card, and a player who turns
the row off finds their stock save exactly where they left it. Verified: after
a full mod save, block 1 of card1.mcd was byte-identical to the pre-run backup,
and only the new file's block plus its directory entry changed.

The rename is ONE byte at 0x80010384, not a longer name: the string is followed
immediately by "Make File" with a single NUL between them, so it cannot grow in
place, and a 20th character would also test whether the strcpy destination at
0x800EFE18 has room for it.

**First run needs a new campaign.** Only the NEW GAME path sets the create-
permission byte at $gp+0x4CC (0x8002D4C0); every other site in the card module
only clears it. So SAVE on a card with no mod file shows the game's own
"UNABLE TO LOCATE LOAD DATA" box -- stock behaviour on any fresh card, and
exactly what a renamed .mcd would have done too. psx_card_save.c detects that
state (op = write, state 0x0E) and explains it on the OSD. Deliberately NOT
fixed by writing that byte ourselves: it also disables the card module's
post-write verify (func_8003ECB0), and quietly weakening checking on the save
path is the wrong trade for skipping one campaign start. (Poking it by hand
through the debug server is how the mod file was first created for testing, and
it worked cleanly -- result 1, state 0x0A.)

**The toggle.** `Extra cards` in the MODS menu, settings key `card_extension`,
**default Off**. Latched at boot by a start hook (psx_video_menu_apply_restored
runs immediately before psx_game_run_start_hooks, so the stored value is in by
then) because the memory-card FILE the session uses is chosen with it; flipping
the row asks for a restart rather than swapping save identities under a running
game. With it off the game is stock down to the filename and both length
immediates -- verified by reading them back at 0x8003F7E4 / 0x8003F8B0.

### 2. Clone ownership — DONE, except the duel
CHEATS -> "All cards" -> "1 of each" through the real menu row was driven end to
end: all 722 stock counts go to 1, ids 723..800 go through
psx_card_ext_trunk_set, the block picks them up (sum updates 0x0245 -> 0x0293),
and BUILD DECK shows every clone owned with count 1 -- screenshotted at rows
723-726 and 751-758, and checked in RAM (all 78 working-trunk cells = 1, all 78
record owned-flags = 1).

**This found and fixed a real bug** -- the one the maintainer hit in the deck
builder. The chest builder's record loop (0x80032640..0x800326EC) re-seeds one
working-trunk cell per card from save+0x50+i EVERY time it runs, and past i=721
that byte is live save data. "Push on entry, pull afterwards" loses to a
rebuild: the next pull wrote the overread bytes into the mod store,
permanently. Measured: ids 723..740 came back 0 (the free bytes after the
trunk) and 741..743 came back 226/11/39, matching save+0x372 onward byte for
byte, and the rows showed exactly that.

A function-entry hook on the builder (0x800323F8, already listed in game.toml)
would say "it just ran", but psx_card_chest.c patches the builder's text, which
diverges it to the dirty-RAM interpreter, and that path skips entry hooks
(psx_card_drops.c records the same trap). So the rebuild is now detected from
its own footprint: it leaves the extended cells byte-for-byte equal to the save
bytes it copied them from, which a single-cell player edit cannot fake. See
sync_ext_trunk's comment.

**Still not done: a duel with a clone in the deck.** Blocked by the crash below.

### 3. CRASH: deck edit then pane switch (seen once, not yet reproduced)
Removing a card from the DECK pane and pressing LEFT back to the chest killed
the process. Captured state (build-dbg/psx_cps_exit_trace.json +
psx_last_run_report.json):

    pc = 0x00000000   ra = 0x80033C7C   a0 = 0x80100000 (arena)
    v0 = 0x00000064   v1 = 0x80090DF8

That is the chest tail-state dispatcher at 0x80033C50..0x80033C74:
`lhu v0,0x633e+SH_TAIL(a0); andi v0,0x3f; sll 2; addu 0x80090DF8; lw; jalr`.
The handler table at 0x80090DF8 has only FIVE entries (states 0..4); index 8
lands in the decimal-constant table that follows it (1, 1, 10, 100, 1000, ...),
so v0 = 0x64 and the jalr goes nowhere. **The tail state read 8, which is not a
legal state.**

Every writer of the tail fields is patched: a whole-RAM scan for immediates
0x633E..0x6343 finds 29 sites, all 29 present in psx_card_chest_sites.h, and
every one of them writes 1..4. So the 8 did not come from those.

Four scripted attempts from savestate 11 (with ALL CARDS applied, with the
cursor deep in the clone range, with repeated right/cross/left) did NOT
reproduce it; the tail cycled cleanly 0x8002 <-> 0x8003 every time. The run
that crashed differed in one way not yet reproduced: the deck pane's cross
ACTUALLY removed a card (deck 40 -> 39, chest 800 -> 801); in the scripted runs
it removed nothing.

Next session: get a deck removal to land, and arm `wtrace_add` on the tail word
(inst + 0x633E + SH_TAIL, = 0x80106DE8 at N=800) before doing it --
scratchpad/crash_repro*.py already wires that up. Leading hypothesis: the
generated site table only swept [0x80031000, 0x80034200), so a writer into the
instance from outside that range would have been missed. 0x8002892C is jal'd
from 0x80033C40 immediately before the dispatch and is outside the sweep.

### 4. Real names for ids 768..807 — DONE (see the relocation section above)
Unchanged. 768..800 show a Guardian-Star/type word because their name string id
(0x8300+) is the same id space as the monster-type and Guardian-Star strings.
Real names mean relocating the whole name-offset table AND moving both the type
producer (`ori 0x8300` at 0x80037F14, found) and the Guardian-Star producer
(NOT yet located) to 0x8400+, plus fixing the text-segment derivation in the
four readers -- three have a private `and reg,reg,0xFFFF0000` that can become
`lui reg,0x801D`; the fourth (0x8003B79C) shares its tail with the 0x801Cxxxx
string bank and needs a redirect stub. A base at 0x801CExxx sends every string
a segment low and the text renderer polls forever: hard hang, measured. Do it
behind its own flag, verified in isolation.

### 5. Commit
Still nothing committed. Branch `card-library-extend`. New files:
`src/psx_card_chest.{c,h}`, `src/psx_card_chest_sites.h`,
`src/psx_card_save.{c,h}`, plus the rewritten `src/psx_card_extend.{c,h}`;
modified `src/psx_ygo_cheats.c`, `src/psx_ygo_overlays.c`. `tools/dbg.py` is
the committed debug helper.

## Testing notes added this session
- The title screen's LOAD is a real menu entry (NEW GAME / LOAD / 2P DUEL /
  TRADE / OPTION). A load ends on a **"LOAD COMPLETE!" box that waits for a
  button** -- the card state machine parks at state 7 with result 1 while it is
  up, which reads exactly like a wedge if you only poll `sio_state` and
  `mc_reads`/`mc_read_done`. It is not one. Half an hour went into that.
- Driving the F10 host menu from the debug server: `menu_click` takes WINDOW
  pixels. Cheats title is (1085, 36); the "All cards" row is (1300, 428).
  Clicking the title again collapses the menu -- leave it collapsed, or the
  expanded menu swallows the pad and `tools/nav.py` presses do nothing.
- The card-I/O request block is $gp-relative with $gp = 0x8009AF08 (from the one
  `lui gp / addiu gp` pair at 0x80012A54): +0x4BA length, +0x4BC offset,
  +0x4C8 buffer, +0x4D4 blocks, +0x4D6 op (0/1 read, 2 write, 4 header),
  +0x4E3 state, +0x4E7 result, +0x4F2 busy/flags.


---

# THE 2026-08-30 SESSION

Everything below was measured live on build-dbg (extension ON — note
`card_extension` in menu_settings.ini was left **=1**; it was 0/Off at session
start) plus offline disassembly of `C:/dev/ygofm-decomp/SLUS_014.11`
(tools/exe_dis.py — REMEMBER it disassembles the STOCK exe file, not live RAM;
one wrong conclusion this session came from forgetting that. For live code use
tools/dbg.py `rd`).

## The mass-blank rows are STOCK BEHAVIOUR, not a bug — verdict, measured

Fast-scrolling (R2) to the bottom of the 790 list draws EVERY visible row
completely empty: no row number, no name, no ATK/DEF, no icons — only the row
frames and trunk/deck count boxes. It looks catastrophic. It is not.

**The mixed-window experiment settles it.** Parked at scroll=716 (read the real
scroll at `list+0x2D3C+SH_A` = list+0x317C and nudge with short presses —
that's how to position deterministically): rows 717-722 (stock, owned) draw
fully; rows 723-724 (clones, unowned) draw blank, PER ROW, in the same frame.
No pane-wide poisoning. Screenshot verified.

**The gate is the builder's, and it is stock logic.** The record-seed loop at
0x80032640..0x800326EC writes each record's flag byte as:

    flag = (save_trunk_byte != 0) ? s5 : (in_deck[id] ? t3 : 0)

i.e. **owned-or-in-deck**. The row draw at 0x800319EC skips the whole row body
when the (copied) flag is 0. A stock unowned card blanks exactly the same way;
we just added 68 never-owned ids in one contiguous run so the whole window
blanks at once. With ALL CARDS applied (trunk=1) clone rows render names —
which is what every earlier session's screenshot showed. **No fix needed;**
optionally CHEATS could grant 1-of-each to make the tail browsable, but blank
IS the stock presentation of unowned.

Two traps hit while proving this:
  - Poking record flag bytes does NOT redraw: the 8 visible-row objects COPY
    record fields at scroll/rebuild time (bug A's "row object" again). A poke
    shows only after a scroll actually moves the window.
  - tools/exe_dis.py showed `slti v0,t0,0x2D2` (722) at 0x800326E8 and caused
    a false "count patches not applied!" alarm — that was the stock FILE.
    Live RAM has 0x29020316 (790). All CHEST_COUNT_SITES verified live.

The handoff's original bug A (OWNED Bite Shoes blank in the DECK pane until
re-sort) is a different, racy thing and did NOT reproduce this session.

## The crash class: REPRODUCED live, full mechanism dossier

During a scripted stress run (deck remove/add + pane bounces + L2/R2 deep
scrolls), the game WEDGED: frame counter frozen, debug server alive. Same
family as the one-shot 0x80033C7C crash from last session, caught this time
with the corpse intact.

**The chain, from the inside out (all measured):**

1. The guest was in the ROW-TEXT WALK inside func_80035E20 (0x80035E20..
   0x80036BCC, the text-widget draw method; sp frame 0x100; the walk loop is
   0x80036058..0x80036B94, records stride 0x1C, "keep walking" = bit 0x80 of
   rec+0x11). `recent_fn` in psx_last_run_report.json showed the last 64
   function entries ALL = 0x800849F0 (the libgs prim emitter): the walk was
   emitting primitives FOREVER within one frame. The record pool around the
   stuck pointer (s2=0x800EEC42) read flag=0x80 for every slot — no
   terminator left in reach.
2. The prim/store spray eventually corrupted kernel state. On the next
   interrupt, openbios ReturnFromException (0x800029CC: k1=[[0x108]],
   restore ctx from TCB, `lw k0,0x80(k1)`, `jr k0` at 0x80002A78) jumped to
   **0xB65827AC** — which decodes as a GPU packet tag (len 0xB6, next
   0x5827AC), i.e. a prim tag landed where a code address was expected.
3. The runtime fail-fasted: psx_unknown_dispatch -> psx_fatal_halt
   (build-dbg/psx_crash.txt: "unknown dispatch addr=0xB65827AC ra=0x80036904").
   ra 0x80036904 = return of the `jal 0x800849F0` at 0x800368FC. The
   dispatch_tail's last 40 entries walk the kernel exception path
   (0x3xxx -> 0xB0 -> 0x2AA0 -> 0x29CC) and then the bogus target.

**Diagnosis toolkit that cracked it (reusable):**
  - Frozen guest + live server: `get_registers` returns the LAST SAVED
    exception context (it reads the TCB at 0x80009088), so identical samples
    across seconds = guest thread parked host-side, not looping.
  - **gdb attach names the C-level truth in one shot**: with msys64 gdb on
    PATH, `gdb -p <pid> -batch -ex "thread apply all bt 16"` — thread 1's
    stack read psx_fatal_halt < psx_unknown_dispatch < psx_check_interrupts <
    func_800849F0 < dirty_ram_dispatch. Detaches cleanly; the wedge survives.
  - build-dbg/psx_crash.txt + psx_last_run_report.json (reason, cpu ctx,
    dispatch_tail, unknown_dispatch_tail, recent_fn) hold everything; read
    them BEFORE killing the process.

**What is still open on the crash:** the trigger for the runaway walk. The
text pool is rebuilt EVERY FRAME (immediate-mode: 0x80035DDC invalidates,
func_80036C14 allocates/writes records, called from the emitter around
0x80037FF4/0x80038040/0x80039730), so a runaway needs that frame's build to
produce an unterminated/looping chain — a one-frame race, almost certainly
draw-vs-rebuild during a deck edit + pane switch, same as the original
0x80033C7C incident (tail state 8 = another flavour of "table data read as
control state"). It did NOT reproduce under 8 scripted edit+bounce cycles
without scroll churn; it DID wedge once cycles included L2/R2 deep scrolls.
Rare, timing-dependent, pre-existing stock race widened by the bigger list.
Next attempt: wtrace the active pane's chain records (heads move every frame
— re-read `0x800EB0F8 + idx*100 + 0x24` per sample, don't arm fixed windows)
across an edit landing, and/or rtrace the walk loop's pc range and catch a
frame whose walk count explodes.

The five text-widget objects (draw method ptr 0x80035E20) live at
0x800EFE94/0x800EFF04/0x800EFF74/0x800EFFE4/0x800F0054 (stride 0x70).

## Spell/Trap effect machinery scouted (the road past 790)

The 69-monster pool is exhausted; going past 790 means Spell/Trap cards with
effects. The burn/heal ladders are the cheapest entry (each is missing one
real rung: **Raimei 300 burn, Blue Medicine 400 heal** -> ids 791/792 keeps
the count even). Found in SLUS_014.11 (all stock addresses):

  - **The LP amounts are BYTE TABLES at gp+0x28 = 0x8009AF30**:
    heal ids 338..342 -> bytes[0..4] = 02 05 0A 14 32, **x100** =
    200/500/1000/2000/5000 (FM's real engine values, not card text);
    burn ids 343..347 -> bytes[8..12] (=0x8009AF38[0..4]) = 05 0A 14 32 64,
    **x10** = 50/100/200/500/1000.
    Consumers: func_800250C8 (heal path; `s1 = id - 0x152` at 0x800250E0,
    LP += table[s1]*100 clamped at max; also spawns the effect anim Obj kind 5
    at (160,120) with field 0x1A = the ladder index) and func_8002525C (burn;
    `s0 = id - 0x157`, field14 -= table[s0]*10, clamped at 0; decomp has it
    MATCHED in src/func_8002525C.c).
  - **Animation cel ranges**: func_800707C4 takes ladder index 0..9, jumps
    through the 10-entry table at 0x8001196C -> pairs (1,5)(6,10)(11,+10)
    (56,60)(61,65)(66,70).
  - **A per-card "effect strength" dispatch**: func_80071008 — for burn ids
    uses `id-0x157`, 5-entry jump table at 0x80011994 -> 50/100/200/500/1000
    into D_800F5B98[]. No jal callers in the exe; reached via **function
    pointer table: 0x80090A70 holds &func_800250C8, 0x80090A78 holds
    &func_8002525C** — that pointer-table region (0x80090Axx) is the magic
    effect handler table, indexed by something per-card. NOT yet mapped —
    map it first next session (dump 0x800909xx..0x80090Bxx, correlate
    indices with magic ids).
  - Range checks to widen for new ids: the `sltiu v0,v1,5` after each
    `id-0x157` / `id-0x152`, the amount tables (5 bytes each; the bytes
    after each table in gp data are NOT known-free — check), and whatever
    the 0x80090A70 table is indexed by.
  - The whole-exe scan for ladder-id immediates found only:
    0x800250E0, 0x80025270, 0x80071114 (+1 noise hit at 0x80086DF8, li t0,341
    — unidentified, check it before assuming three sites is all).

Tools kept in the repo this time (the last session's scratchpad died with
its tools): tools/exe_dis.py (capstone disasm of SLUS_014.11 by RAM addr,
file offset = RAM - 0x8000F800), tools/exe_sweep.py (immediate/jal scans),
tools/pad_nav.py (active-low pad presses + tail peek + screenshots; buttons
word = 0xFFFF & ~mask; L2=0x0100 scrolls UP, R2=0x0200 scrolls DOWN).

Memory-card backup made this session:
"Documents/My Games/.../mcd-backup-2026-08-30/" (card1+card2). The old
2026-08-29 backups are GONE (lived in a dead session scratchpad).

## Session hygiene notes
  - The game was closed at session end, but `card_extension=1` was left set
    in menu_settings.ini (it was 0/Off at session start). Restore =0 if
    handing the build to a player-mode test.
  - `press` frames>~40 auto-repeats; a 600-frame R2 hold traverses the whole
    790 list. The F10 host menu can swallow injected pad input if it pops
    (close with menu_key 0x40000043 and re-check menu_state.visible=0).
  - Savestate slot 11 still = main menu cursor on LIBRARY; `up cross` enters
    BUILD DECK.

---

# THE CRASH: ROOT MECHANISM FOUND, CONTAINED (2026-08-30, evening session)

The user hit the freeze twice in real play (deck edit swapping Meteor B.
Dragon for #790). Root-caused to full mechanism with a 3/3 deterministic
repro, then contained with a new guard. Artifacts in
bugs/crash-2026-08-30-deckedit/ (full 2MB RAM dump of the user's crash +
reports).

## The repro (was 3/3 fatal, stock never crashes)

Extension ON + ALL CARDS granted + BUILD DECK: remove any deck card, scroll
the CHEST to the very bottom (row 790), press CROSS to add it. Freeze on the
add. Controls: stock (ext off) same sequence 6/6 clean; stock + all cards
4/4 clean; ext at TOP-of-list edits 8/8 clean. It needs ext + bottom + add.

## The mechanism (every step measured)

1. On the fatal frame the row-text walk in func_80035E20 (records stride
   0x1C, continue = bit 0x80 of rec+0x11) runs away and emits THOUSANDS of
   prims through the libgs emitter func_800849F0 -- one textured sprite per
   record, write cursor = the global at 0x800FE240, advanced +0x18 per prim
   at 0x80084B60.
2. The march tramples, in whatever order the addresses come up: the record
   pool it is feeding from (the walk then eats its own packet bytes --
   self-sustaining), the five 100-byte widget structs at 0x800EB0F8, the
   cursor global itself (teleporting the march), and the kernel TCB at
   0x80009088+ -- wtrace caught pcs 0x80084ADC/0x80084B24/0x80084B50/
   0x80084A6C writing packet words (0x05xxxxxx tags, 0xE100020C texpage,
   0x64808080 color) over TCB+0x88, the saved exception EPC at 0x80009110.
3. The next ReturnFromException (`lw k0,0x80(k1); jr k0` at 0x80002A78)
   jumps into a packet's UV/CLUT word and the runtime fail-fasts.
   The four observed "unknown dispatch" targets -- 0xB65827AC, 0xB95827AC,
   0xBA7827AC, 0xB56027AC -- all decode as glyph-sprite words (u=0xAC,
   v=0x27/0x60/0x78, clut=0xB5xx..0xBAxx). NOT pointers; packet data.
   (Both earlier theories in this file's morning section -- "prim overflow"
   and "interpreter/exception-path bug" -- resolve to this: the spray is
   real, the interpreter is innocent.)
4. The first domino is timing-dependent: arming ANY wtrace range (the hook
   slows every store) closes the window completely -- 0/13 crashes with
   instrumentation vs 3/3 without. The suspected collision is the vsync
   exception landing inside the edit-frame's rebuild of the row records
   (the edit frame is slower with ext ON: 790-record loops through
   dirty-page interpreted patches). The exact interleaving is UNPROVEN --
   contained instead of chased.

Useful spelunking facts learned on the way:
  - The kernel low 64K is permanently dirty (TCB writes share the region
    with kernel code), so ALL exception entry/return runs via
    dirty_ram_dispatch -- normal, high-traffic, and fine.
  - openbios B-table is at 0x49CC (B(0x17)=ReturnFromException=0x29CC);
    the ToT chains and B-table were intact in every corpse.
  - `dirty_insn_dump_file` dumps the interpreter's 65536-deep insn ring;
    it is gated (kernel ranges + `dirty_insn_gate`), so game-code pcs do
    not appear unless gated in.
  - get_registers on a frozen guest returns the LAST SAVED TCB context;
    identical across samples == guest parked host-side.

## The guard (new: src/psx_card_guard.c)

func_800849F0's first instruction is redirected (j, ra-preserving; the
displaced `move t3,a0` runs in the stub tail) to a 15-word stub at
0x801D9E00: if the cursor global is outside the chest screen's legit prim
area [0x80090000,0x800F8000) it is re-parked at a sacrificial pit at
0x801D9C00 before the packet is written. Checked on EVERY prim, so a wild
cursor can never get more than one packet past the pit: kernel, record
pool and widget structs become unreachable. Armed only while mode==0xC7
(other screens may park prim buffers elsewhere legitimately); asserted per
frame, restored on leaving the chest; OSD "Prim guard: runaway draw
parked" + pit clear when residue shows. The stub executes from a dirty
data page exactly like the viewer clamp; entry patches are honoured per
call (A2 only bites loop back-edges).

**Verified:** entry word patched live, chest renders pixel-perfect through
the stub, and the 3/3-fatal repro ran 4/4 clean on the guarded build.
**Honesty:** the pit stayed empty in those runs -- the stub's own ~15-insn
overhead per prim also shifts the timing out of the fatal window (same
effect as the wtrace), so the parking path itself has not fired in anger
yet. Belt (timing) and suspenders (containment); if the OSD line ever
appears, the guard did its job and the frame that produced it is worth
dumping.

## Open questions for a future session

  - The true first domino: which chain head / record goes bad at the add,
    and what the vsync handler touches mid-rebuild. The repro + a
    zero-overhead capture (savestate just before the add, or
    `insn_freeze_target` once a target value is predictable) would nail it.
  - Why ext+bottom specifically: suspicion is the 8 extended-name rows +
    edit redraw + patched-site interpreter overhead making the edit frame
    overrun into the vsync window; unproven.
  - The guard's LEGIT_LO/HI were measured on the chest screen only; if the
    chest ever legitimately parks its cursor outside [0x80090000,
    0x800F8000), rows would draw into the pit (visible as missing UI) --
    the bound is deliberately wide, but keep it in mind.

## Second incident + guard v2 (2026-08-30, late)

The user hit a SOFT-BREAK flavour: game renders, input dead. Post-mortem:
the widget-list walker at 0x80040D40 (112-byte objects at 0x800EFE48,
`next` index at +2, terminator = negative) was spinning on a chain that
reached index 0 whose next was 0 -- and function pointers nearby held GPU
packet tags (a jalr later fail-fasted on 0x050E9D98, a prim tag pointing at
0x800E9D98). Same spray, different victims: this march ran INSIDE guard
v1's [0x80090000,0x800F8000) window and chewed the record pool + widget
structs without ever reaching the kernel. Breaking the cycle live
(write_mem 800EFE4A=FFFF) let it run one step to the next corrupted
pointer -- the state was already gone.

Guard v2 (src/psx_card_guard.c), two composed walls:
  - ADDRESS: window tightened to the MEASURED banks [0x800A0000,
    0x800D8000) -- healthy cursor sampled 0x800A5768..0x800CB658 across
    browse/scroll/edits/re-sort; pool, widgets and kernel all outside.
  - VOLUME: per-prim counter at 0x801D9BF8 vs cap at 0x801D9BFC; the tick
    resets the counter between frames and self-calibrates the cap
    (8 x observed peak, floor 4096; real peak measured < 512/frame). The
    runaway's own never-ending frame can never reset its budget, and 4096
    prims of intra-bank marching stays inside the banks (98KB < 229KB
    span), so a tripped runaway is parked before it can leave prim space.
Verified: 6/6 clean on the 3/3-fatal bottom-add repro + 3/3 clean on
sort+edit cycles; healthy frame counter ~0x187. Pit stayed clean (timing
shift again) -- but the two walls now provably bracket any march.

Also fixed: the FUSION OVERLAY drew phantom hints on the deck builder
(user saw "Tri-horned Dragon 2850/2350") -- psx_fusion_assist reads hand
cells at 0x800EA030/0x800E9F25 which are duel state only; on other screens
that memory is scratch and the turn-byte gate passes by luck. compose() in
psx_fusion_overlay.c now requires mode 0xC3 (in-duel) before composing;
composing nothing is what clears a stale hint, so the gate sits below the
diff, not as a tick early-return.

Debug-server note that cost twenty minutes: the RAM poke command is
`write_ram` (addr/val, ONE byte) or `write_mem` (addr/hex blob) --
`{'cmd':'ramw'}` is NOT a command (it is a trace-kind label) and fails
with 'unknown command'; several earlier "pokes did nothing" moments in
this file's morning section trace to exactly that.
