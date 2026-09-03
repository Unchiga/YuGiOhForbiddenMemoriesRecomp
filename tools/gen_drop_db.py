#!/usr/bin/env python3
"""gen_drop_db.py — bake every duelist's full drop table from the PLAYER'S disc.

    gen_drop_db.py <disc.cue|disc.bin|disc.iso> <out_dir> [--check]

The disc argument is whatever the setup wizard recorded — a .cue, a raw .bin
or a cooked .iso — and disc_image.py turns it into a user-data stream.

MODS > DROP MISSING CARDS only ever needed the CURRENT opponent's table, which
the game leaves resident at 0x801781D8 while a duel is loaded. The Drop Table
Viewer needs all forty at once, and only one is ever in RAM, so they have to
come from the disc.

These are Konami's numbers, so they are build output and not source: this emits
into the build directory beside the baked sprites, exactly like disc_assets.py,
and nothing here is committed or shipped. What IS committed is the offsets —
coordinates into the player's own file, not the file's contents.

LAYOUT (confirmed here on every run, not assumed)
-------------------------------------------------
WA_MRG.MRG carries one 6144-byte record per duelist, the first at stream offset
15310260, and record 0 is unused. Inside a record are four 722-entry u16 weight
arrays 1460 bytes apart: the deck pool at -1460, then drop tiers 0, 1, 2 —
S/A POW, B/C/D and S/A TEC. Each drop tier sums to exactly 2048, which is what
makes a weight readable as a percentage, and is asserted for all 120 tiers
before anything is written.

Card names and ATK/DEF are deliberately NOT baked. They live in the game EXE
and are resident from the moment it starts, so the viewer reads them out of
guest RAM instead — see psx_drop_viewer.c. Nothing about the cards themselves
is copied out of the disc by this tool.
"""

import os
import struct
import sys

from disc_image import open_disc

# WA_MRG.MRG begins here in the reassembled user-data stream, and record 0 sits
# this far into that file. Split so the second number stays the one
# tools/gen_drop_table.py already uses against an extracted WA_MRG.MRG.
WA_MRG_BASE = 20688896
DROP0 = WA_MRG_BASE + 15310260   # stream offset of record 0
REC = 6144            # bytes per duelist record
TIER = 1460           # bytes between the four weight arrays
CARDS = 722
TIERS = 3
TIER_TOTAL = 2048
DUELISTS = 39         # records 1..39; record 0 is never loaded

# Roster order is the duelist index the drop mod already uses, so the two
# features name the same opponent by the same number.
ROSTER = [
    "Simon Muran", "Teana", "Jono", "Villager1", "Villager2", "Villager3",
    "Seto", "Heishin", "Rex Raptor", "Weevil Underwood", "Mai Valentine",
    "Bandit Keith", "Shadi", "Yami Bakura", "Pegasus", "Isis", "Kaiba",
    "Mage Soldier", "Jono 2nd", "Teana 2nd", "Ocean Mage",
    "High Mage Secmeton", "Forest Mage", "High Mage Anubisius",
    "Mountain Mage", "High Mage Atenza", "Desert Mage", "High Mage Martis",
    "Meadow Mage", "High Mage Kepura", "Labyrinth Mage", "Seto 2nd",
    "Guardian Sebek", "Guardian Neku", "Heishin 2nd", "Seto 3rd",
    "DarkNite", "Nitemare", "Duel Master K",
]


def load_tables(disc_path):
    """[duelist][tier] -> tuple of 722 weights."""
    tables = []
    with open_disc(disc_path) as disc:
        print('gen_drop_db: reading %s (%s)' % (disc.path, disc.layout))
        for d in range(DUELISTS):
            rec = d + 1
            tiers = []
            for t in range(TIERS):
                raw = disc.read(DROP0 + rec * REC + t * TIER, CARDS * 2)
                w = struct.unpack('<%dH' % CARDS, raw)
                total = sum(w)
                if total != TIER_TOTAL:
                    raise SystemExit(
                        'duelist %d (%s) tier %d sums to %d, not %d — this is '
                        'not the expected disc layout, so nothing was written'
                        % (d, ROSTER[d], t, total, TIER_TOTAL))
                tiers.append(w)
            tables.append(tiers)
    return tables


def emit(tables, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, 'psx_drop_db.c')
    L = []
    L.append('/* psx_drop_db.c — GENERATED from the player\'s disc by')
    L.append(' * tools/gen_drop_db.py. Build output, not source: do not commit.')
    L.append(' *')
    L.append(' * Every duelist\'s three drop tiers, sparse. Only about 1 card in 15')
    L.append(' * has a non-zero weight, so storing (card, weight) pairs costs a few KB')
    L.append(' * where the dense form costs 170. Each tier\'s weights sum to 2048,')
    L.append(' * verified at bake time, which is what lets a weight read as a percent.')
    L.append(' */')
    L.append('#include "psx_drop_db.h"')
    L.append('')

    for d in range(DUELISTS):
        for t in range(TIERS):
            pairs = [(c + 1, w) for c, w in enumerate(tables[d][t]) if w]
            L.append('static const PsxDropWeight DB_D%02d_T%d[] = {' % (d, t))
            line = '   '
            for c, w in pairs:
                item = ' {%3d,%4d},' % (c, w)
                if len(line) + len(item) > 78:
                    L.append(line)
                    line = '   '
                line += item
            if line.strip():
                L.append(line)
            L.append('};')
    L.append('')
    L.append('const PsxDropDbDuelist PSX_DROP_DB[PSX_DROP_DB_DUELISTS] = {')
    for d in range(DUELISTS):
        counts = [sum(1 for w in tables[d][t] if w) for t in range(TIERS)]
        L.append('    { "%s",' % ROSTER[d])
        L.append('      { DB_D%02d_T0, DB_D%02d_T1, DB_D%02d_T2 },'
                 % (d, d, d))
        L.append('      { %d, %d, %d } },' % tuple(counts))
    L.append('};')
    L.append('')
    L.append('const char *const PSX_DROP_TIER_NAMES[PSX_DROP_DB_TIERS] = {')
    L.append('    "S/A POW", "B/C/D", "S/A TEC"')
    L.append('};')
    L.append('')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    return path


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: gen_drop_db.py <disc.bin> <out_dir> [--check]')
    disc, out_dir = sys.argv[1], sys.argv[2]
    if not os.path.isfile(disc):
        raise SystemExit('no disc image at %s' % disc)
    tables = load_tables(disc)
    if '--check' in sys.argv:
        print('gen_drop_db: %d duelists, all %d tiers sum to %d'
              % (DUELISTS, DUELISTS * TIERS, TIER_TOTAL))
        return
    path = emit(tables, out_dir)
    nz = sum(sum(1 for w in tables[d][t] if w)
             for d in range(DUELISTS) for t in range(TIERS))
    print('gen_drop_db: wrote %s (%d duelists, %d non-zero weights)'
          % (path, DUELISTS, nz))


if __name__ == '__main__':
    main()
