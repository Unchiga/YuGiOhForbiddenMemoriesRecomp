# The 3D models: monsters, Exodia, the duel mat, the backdrops

Everything below was measured on the USA disc (SLUS-01411) by reading the
EXE's loaders and libgs code, then checked by extracting and rendering the
data. `tools/model_extract.py` is the tool; its docstring carries the exact
byte layouts. This page is the map.

```sh
python3 tools/model_extract.py disc/*.cue out/ 1 5 17-21        # a few cards
python3 tools/model_extract.py disc/*.cue out/ monsters --preview # all 621
python3 tools/model_extract.py disc/*.cue out/ exodia field scenes
```

Each model lands in its own folder with a glTF 2.0 file (skeleton, skin, one
animation clip per sequence id), an OBJ/MTL (bind pose), the textures as
RGBA PNGs, a JSON dump of the parsed structure, and `raw/` with the untouched
disc bytes (model, texture, effect code, sounds, parameters). The output is
derived from the disc like `generated/`, so it is not committed.

## Where the models are

| what | file | location |
| --- | --- | --- |
| 621 monsters | `DATA/MODEL.MRG` (LBA 28534) | block `m` at sector 276*m, 276 sectors each |
| assembled Exodia | `DATA/SU.MRG` (LBA 954) | sector 948, 275 sectors, loaded for fake card id 0x309 |
| 7 duel backdrops | `DATA/SU.MRG` | sector 136 + 116*k, 116 sectors each |
| duel mat | `DATA/WA_MRG.MRG` (LBA 10102) | sector 198 of every terrain package (0x16C6 + 235*t) |

Card id to block: `m = card - 1`, minus 50 if the card is past 350, minus 50
again past 700, minus 1 for card 722. The skipped ranges are exactly the
non-monster cards (301..350 Legendary Sword..Dark-piercing Light, 651..700
Kunai with Chain..Fortress Whale's Oath, 721 Dark Magic Ritual). That is what
`Model_LoadMonsterMerge` (0x80056504) computes before streaming
`M:/mrgSU/model.mrg`, and it accounts for all 621 blocks.

## A monster block

276 sectors, routed by the 17 stages of `func_80056D7C` (the CD stream
callback). Sector ranges:

| sectors | content | destination |
| --- | --- | --- |
| 0..95 | the model (HMD, below) | RAM, arena 0x80100000 (slot 0) or 0x80140000 (slot 1) |
| 96..143 | 8bpp texture, three 128-pixel columns | VRAM x = 256*slot, y = 256, column by column |
| 144..145 | 8 palette rows of 256 entries | VRAM (256*slot, 248) |
| 146 | one more 256-entry palette | VRAM (512, 242+slot) |
| 147..162 | texture column 3, mostly attack-effect art | RAM 0x801DE000 (uploaded later) |
| 163, 164..179 | duplicates of 146 and 147..162, used when the loader's alt flag is set | |
| 180..189 / 190..199 | the attack-effect code module, linked for slot 0 (0x8013B000) / slot 1 (0x8017B000) | RAM |
| 200..219 | the alt pair of effect modules | RAM |
| 220..221 / 222..223 | effect data per slot (a stub function plus tables) | RAM 0x8013A000 / 0x8017A000 |
| 224 | a u16 id list, purpose not established | RAM 0x801A8000 + 0x800*slot |
| 225..274 | the monster's cries, 0x19000 bytes of SPU ADPCM | SPU RAM 0xD810 + 0x19000*slot |
| 275 | parameters: 16 sound cues at +0, ATK/DEF and stat words at +0x100 | copied into the model object |

The polygons reference the exporter's VRAM layout (texture pages 10..13 at
y=256, palettes at x=640 rows 0..7) and the game relocates them per slot
when it links the model (`func_8004D134`: tpage - 10 + 4*slot, CLUT + 0x3BD8
+ 16*slot). The extractor keeps the exporter layout, so texture coordinates
in the JSON match the disc bytes.

## The model format

PsyQ HMD ("hierarchical model data") with two Konami twists: the file size
sits where the 0x50 magic normally is, and the image primitives were
stripped because the loader uploads textures itself. libgs's own `GsMapUnit`,
`GsSortUnit`, `GsScanUnit` and the animation driver `GsU_03000000` (its
assert string still says `S3000000.C`) are in the EXE and do the parsing.

* Header: size, map flag, offset of the primitive header section, block
  count, block table. All offsets are in long words from the top.
* Blocks: `block[0]` is the animation (and, in stock HMD, image) chain,
  `block[1 + i]` belongs to coordinate `i`, and a final extra block holds the
  skinned polygons.
* Coordinates: a count, then 0x50-byte `GsCOORDUNIT` records: flag, local
  matrix (3x3 s16 in 1/4096 plus s32 translation), work matrix, rotation
  vector (4096 = one turn), parent pointer. The stored bind matrices equal
  `RotMatrix(rot)` = Rx*Ry*Rz (verified by running the EXE's libgte).
* Primitive headers: `[size, words...]`, bit 31 marks a pointer. Rigid
  polygons: poly table, vertex table, normal table, coords. Skinned polygons:
  poly table, vertex table, transformed-vertex work, normal table, lit-color
  work, coords. Animation: 5, interpolator list, control stream, key table,
  coords.
* Primitive blocks: `{next, header, count|0x80000000, entries...}`; each
  entry `{type, (count<<16)|size, first-polygon offset}`.

Polygon types, low bits: 0x09 flat textured tri (5 words), 0x0D gouraud
textured tri (6), 0x11 flat quad (6), 0x15 gouraud quad (7); 0x08/0x0C/0x10
/0x14 are the untextured versions; +0x200 adds a leading flat color word.
Index words pack `normal | vertex << 16`. Flag bits: 0x00100000 double sided
(the driver skips NCLIP), 0x00200000 semi-transparent (ABE, blend mode from
the tpage bits), 0x00800000 "map the coordinates on this block's first
entry", 0x00020000 the duel mat's own drivers, 0x01000000 skinned. The driver
families were told apart by diffing them: 0x8006151C (plain), 0x80062BC0
(no backface test), 0x80064248 (ABE), 0x80065BCC (both); the game relinks
all polygons to a flat-gray family (0x80069F94) for its silhouette effect.

Skinning: a `0x01000000` "shared calculation" entry in a coordinate's block
says which slice of the shared vertex and normal tables that bone transforms
(`vcount, voffset, work, ncount, noffset, work`). Every shared vertex belongs
to exactly one bone, so the glTF skin has one weight per vertex.

## Animation

`block[0]` carries one 7-word sequence descriptor per animated bone (all
bones but the root). Descriptor: coord reference, size 7, speed 0x10 (one
frame per tick, frame counts are stored times 16), sequence id, and the
start index of the bone's run in the control stream.

The control stream is one u32 per step:

* key entry `(interpolator << 24) | (frames << 16) | key offset`: reach this
  key over `frames` frames from the previous one;
* `0x80000000 | (new sid << 23) | (match sid << 16) | target`: jump to
  `target` when `match sid` is 0 or equals the descriptor's sequence id,
  setting the id to `new sid`;
* `0xC0800000 | (sid << 16)`: stop.

Keys are 5 words: translation xyz, rotation xyz (YXZ Euler, 4096 = a turn,
applied with `RotMatrixYXZ` = Ry*Rx*Rz), scale xyz (4096 = 1.0). The one
interpolator every monster uses (0x03002119, `func_8005D378`) lerps all three
between the previous and the next key, then rebuilds the coordinate's
matrix. On the disc the idle loop is followed by a jump table for sequence
ids 2..8, and because every jump carries new-sid 0 the section it lands in
plays once and the idle loop resumes. The game starts a sequence with
`func_800597C8(slot, sid, speed)`, which sets the id on every bone at once.

The extractor writes one glTF clip per sequence id: `seq0` is the idle loop,
`seq2`..`seq8` the one-shot sections (transition from idle, the section, and
the cut back to idle). Which id is which action is not pinned down yet; the
duel's action dispatcher (`duel_action_resolution_dispatch`) picks them from
state, so the safe way to learn the mapping is to watch the model object's
current id byte (`D_800F2C40[slot] + 0xBF5`) during an attack.

## Attack effects

Every monster ships its own MIPS code module (sectors 180..199, two links of
the same code) plus a data module and a texture column of effect art. The
module starts with a word count and is called by the duel while the attack
plays; it is not data-driven, so "editing an effect" means editing code. The
extractor dumps the raw modules into `raw/` for disassembly; nothing more has
been decoded about them.

## The duel mat and the backdrops

The mat is a one-bone HMD (114 vertices, 78 triangles for the border art and
64 quads for the floor) that every terrain package repeats byte for byte.
Its textures come from the package's last stage: sectors 203..234 stream to
(640, 256) as two 4bpp columns, the floor with its palettes on rows 254..255
and the border art with palettes on rows 226..228. `field` in the extractor
writes the mat once per terrain with that terrain's textures.

The seven SU.MRG packages are the 3D scenes around the mat (grass, trees,
walls). Each is 18 sectors of HMD, 64 texture sectors to (512, 256), two
palette sectors, and 32 texture sectors to (896, 0). Some carry a tiny
animation (scene 4 rocks two bones). Which package the duel picks for a
terrain goes through a table at 0x80091008 and has not been checked.

## Exodia

Loaded for card id 0x309 when all five pieces are in hand: 53 bones, 1586
polygons, its own 96-sector texture (six columns at x=0 plus a half column at
x=384) and 52 animated bones with the same sequence layout as the monsters.
Extract it with `exodia`.

## Putting models back

The recomp already serves replacement sectors for card art through
`src/psx_card_packs.c` at the CD read layer. A model pack would do the same
for the 276-sector block: the HMD, the texture columns and the palette rows
are plain sector ranges, so a re-exporter that writes this HMD variant (same
type codes, same 5-word keys) and a 512x256 8bpp atlas could drop in with no
engine changes. The effect module and the sounds can stay stock.
