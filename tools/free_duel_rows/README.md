# Extra Free Duel rows (parked)

`attic/psx_free_duel_rows.c` is milestone 1 of "more opponents": a ninth
row of five placeholder cells under the stock 8x5 Free Duel grid, every
cell showing Simon's portrait and record and duelling Simon. It is not
compiled (nothing under `tools/` is) and was pulled from the build on
2026-09-04 so the shipped grid is stock again. Kept as research: the file's
header documents how the free_duel overlay (0x80168000..0x801690A8) lays
out the grid, uploads portraits, clamps the cursor and maps a cell to an
opponent, and which words it re-asserts per frame to grow the grid.

Where it stood:

- Verified live: the ninth row draws (portraits, name, record), picking a
  cell duels Simon, the stock records are untouched.
- Next step was the "duelist pack": a folder per new duelist (`duelist.ini`
  + `portrait.png`), a stock AI to copy, a deck pool and three drop tiers
  summing to 2048, and a mod-owned win/loss file, since the save's 39-pair
  record table has no slot for new rows.
- Reclaimed RAM it claimed: 0x801CFE00..0x801CFEF0 (availability table and
  stubs). Free again while it is parked.

To bring it back, move the file into `src/` (the build globs that folder).
