#!/usr/bin/env python3
"""disc_image.py — open the player's disc image however they pointed at it.

disc_assets.py and gen_drop_db.py read Konami's data straight off the disc at
offsets into the ISO9660 user-data stream. They used to open the path they
were handed as a raw MODE2/2352 image and nothing else. The path they are
handed is whatever the setup wizard recorded in disc.cfg, and the wizard
accepts a .cue — it even prefers one, because the runtime mounts the cue to
keep the track layout. A player who picked the .cue therefore reached the
product build with a 97-byte text file where a disc was expected, and both
scripts died with "disc ended early at stream offset 32518144": the first
manifest read, on a file that was never a disc. Picking the .bin instead
"fixed" it, which is how the workaround spread on Discord.

So this module owns the question "what file, and what sector layout":

  * a .cue resolves to the BINARY file its data track names (the first FILE
    line, which for a Redump multi-track dump is "(Track 1).bin"), looked up
    beside the cue with a case-insensitive fallback for a renamed extension;
  * a raw image is probed for its sector layout by finding the ISO9660
    primary volume descriptor at sector 16, so a MODE2/2352 Redump .bin, a
    MODE1/2352 dump, a 2448-byte-per-sector dump with subchannel data and a
    cooked 2048-byte .iso all read the same user-data stream;
  * anything else fails up front, naming the file, its size and what to pick
    instead — never a read that runs off the end of a cue sheet.

Both scripts get the same answer for the same disc no matter which file the
player pointed at, which is the property the wizard's "pick either" promise
depends on.
"""

import os
import re

# ISO9660 primary volume descriptor: type 1, "CD001", at logical sector 16.
_PVD_MAGIC = b'\x01CD001'
_PVD_SECTOR = 16

# (sector size, user-data offset within the sector, label).  Probe order
# matters only for the label: a sector 16 that matches two layouts cannot
# happen, because the magic sits at different file offsets in each.
LAYOUTS = (
    (2352, 24, 'MODE2/2352 raw'),
    (2352, 16, 'MODE1/2352 raw'),
    (2448, 24, 'MODE2/2448 raw + subchannel'),
    (2448, 16, 'MODE1/2448 raw + subchannel'),
    (2048, 0,  'cooked 2048-byte sectors (.iso)'),
)

UD_LEN = 2048


class DiscImageError(SystemExit):
    """A SystemExit with a message: the build logs it verbatim, and a build
    step that fails on the disc should say so in one readable line."""


def _cue_files(cue_path):
    """Every FILE entry in a cue sheet, in order: (name, type)."""
    with open(cue_path, 'rb') as f:
        raw = f.read()
    text = raw.decode('utf-8', errors='replace')
    files = []
    for m in re.finditer(r'^\s*FILE\s+(?:"([^"]*)"|(\S+))\s+(\S+)\s*$',
                         text, re.I | re.M):
        files.append((m.group(1) if m.group(1) is not None else m.group(2),
                      m.group(3).upper()))
    return files


def _find_beside(cue_path, name):
    """The file a cue names, resolved beside the cue. Tries the exact name,
    then a case-insensitive match, then the same stem with any raw-image
    extension — a renamed ".BIN" or a cue copied from another dump."""
    cue_dir = os.path.dirname(os.path.abspath(cue_path))
    cand = name if os.path.isabs(name) else os.path.join(cue_dir, name)
    if os.path.isfile(cand):
        return cand
    base = os.path.basename(name)
    stem = os.path.splitext(base)[0].lower()
    try:
        entries = os.listdir(cue_dir)
    except OSError:
        return None
    for e in entries:
        if e.lower() == base.lower():
            return os.path.join(cue_dir, e)
    for e in entries:
        s, ext = os.path.splitext(e)
        if s.lower() == stem and ext.lower() in ('.bin', '.img', '.iso'):
            return os.path.join(cue_dir, e)
    return None


def resolve_disc_path(path):
    """The binary to read for a disc the player pointed at.

    A .cue becomes its data track's BINARY file; anything else is returned as
    given. Raises DiscImageError when the cue names nothing usable.
    """
    if os.path.splitext(path)[1].lower() != '.cue':
        return path
    if not os.path.isfile(path):
        raise DiscImageError('no cue sheet at %s' % path)
    files = _cue_files(path)
    binaries = [n for n, t in files if t == 'BINARY']
    if not binaries:
        raise DiscImageError(
            '%s names no BINARY file, so it is not a usable cue sheet. Point '
            'the setup at the disc\'s .bin instead.' % path)
    missing = []
    for name in binaries:
        found = _find_beside(path, name)
        if found:
            return found
        missing.append(name)
    raise DiscImageError(
        '%s refers to "%s", which is not next to it. Put the .bin beside the '
        '.cue (do not rename either), or point the setup at the .bin itself.'
        % (path, missing[0]))


def detect_layout(f):
    """(sector size, user-data offset, label) for an open raw image, found by
    locating the ISO9660 primary volume descriptor. None when it is nowhere."""
    for size, off, label in LAYOUTS:
        f.seek(_PVD_SECTOR * size + off)
        if f.read(len(_PVD_MAGIC)) == _PVD_MAGIC:
            return size, off, label
    return None


class DiscStream(object):
    """Random access into the reassembled user-data stream of a disc image."""

    def __init__(self, path):
        self.picked = path
        self.path = resolve_disc_path(path)
        if not os.path.isfile(self.path):
            raise DiscImageError('no disc image at %s' % self.path)
        self.size = os.path.getsize(self.path)
        self.f = open(self.path, 'rb')
        layout = detect_layout(self.f)
        if layout is None:
            self.f.close()
            raise DiscImageError(self._not_a_disc_message())
        self.sector_size, self.ud_off, self.layout = layout
        self.user_data_len = (self.size // self.sector_size) * UD_LEN

    def _not_a_disc_message(self):
        via = ''
        if self.picked != self.path:
            via = ' (named by %s)' % self.picked
        return (
            '%s%s is not a PlayStation disc image: no ISO9660 volume '
            'descriptor at sector 16 as a raw 2352/2448-byte-sector dump or '
            'a cooked 2048-byte .iso (file is %d bytes). Use the disc\'s .bin '
            'with its .cue beside it, dumped as MODE2/2352 (Redump).'
            % (self.path, via, self.size))

    def read(self, off, n):
        """n bytes at an offset into the user-data stream."""
        out = bytearray()
        sec, pos = divmod(off, UD_LEN)
        while len(out) < n:
            self.f.seek(sec * self.sector_size + self.ud_off + pos)
            take = min(UD_LEN - pos, n - len(out))
            chunk = self.f.read(take)
            if len(chunk) < take:
                raise DiscImageError(
                    '%s ended early at stream offset %d: it holds %d bytes '
                    'of user data (%d bytes, %s), but this title\'s data '
                    'sits past that. This is not a complete dump of the USA '
                    'disc (SLUS-01411).'
                    % (self.path, off + len(out), self.user_data_len,
                       self.size, self.layout))
            out += chunk
            sec += 1
            pos = 0
        return bytes(out)

    def close(self):
        if self.f:
            self.f.close()
            self.f = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def open_disc(path):
    """Open a disc by .cue, raw .bin/.img or cooked .iso for stream reads."""
    return DiscStream(path)


def describe(path):
    """One line saying what will be read, for a build log."""
    with open_disc(path) as d:
        if d.picked != d.path:
            return '%s -> %s (%s)' % (d.picked, d.path, d.layout)
        return '%s (%s)' % (d.path, d.layout)


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        print(describe(p))
