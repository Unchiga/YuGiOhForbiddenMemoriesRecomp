#!/usr/bin/env python3
"""Clone a Forbidden Memories save onto another memory card with a new duelist code.

2P DUEL refuses two cards that carry the same duelist code (the game treats
them as one player). To test 2P locally from a single save, clone card 1 onto
card 2 and give the copy a different code:

    python3 tools/save_clone.py card1.mcd card2.mcd            # code = old + 1
    python3 tools/save_clone.py card1.mcd card2.mcd --code 1234

The save block layout comes from the decomp (src/game/save_data.h): a 0x200
header, then the 0x680 state, then a duplicate of the state. Three regions of
the state are covered by a CRC-16/XMODEM and a run of mask words seeded from
that CRC; all three are rewritten here so the copy validates.
"""
import argparse
import struct
import sys

MCD_SIZE = 128 * 1024
BLOCK = 8192
FILE_NAME = b"BASLUS-01411-YUGIOH"
HEADER = 0x200
STATE = 0x680
DUELIST_CODE = 0x334

REGIONS = [  # (offset, length, checksum_offset, mask_last_offset, mask_words)
    (0x000, 0x340, 0x37C, 0x378, 15),
    (0x380, 0x06C, 0x3FC, 0x3F8, 4),
    (0x400, 0x204, 0x604, 0x624, 8),
]


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Mask:
    def __init__(self, seed):
        self.low = (seed | (seed << 16)) & 0xFFFFFFFF
        self.high = self.low

    def next(self):
        low, high = self.low, self.high
        nxt = ((high << 31) | (low >> 1)) & 0xFFFFFFFF
        nxt ^= (low << 12) & 0xFFFFFFFF
        high = (high + high + (low & 1)) & 0xFFFFFFFF
        self.high = high
        self.low = (nxt ^ (nxt >> 20)) & 0xFFFFFFFF
        return self.low


def seal(state):
    """Rewrite the checksum halves and mask words of every region in place."""
    for off, length, ck, mask_last, n in REGIONS:
        crc = crc16(state[off:off + length])
        struct.pack_into("<HH", state, ck, crc, crc)
        m = Mask(crc)
        for i in range(n):
            struct.pack_into("<I", state, mask_last - 4 * i, m.next())


def validate(state):
    for off, length, ck, mask_last, n in REGIONS:
        m = Mask(crc16(state[off:off + length]))
        for i in range(n):
            if struct.unpack_from("<I", state, mask_last - 4 * i)[0] != m.next():
                return False
    return True


def find_save(card):
    """Return the byte offset of the save block, or None."""
    for i in range(15):
        e = card[0x80 + i * 0x80:0x80 + (i + 1) * 0x80]
        if struct.unpack_from("<I", e, 0)[0] == 0x51 and e[10:10 + len(FILE_NAME)] == FILE_NAME:
            return (i + 1) * BLOCK
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="memory card holding the save to copy")
    ap.add_argument("dst", help="memory card to write (its save block is replaced; other blocks kept)")
    ap.add_argument("--code", type=lambda s: int(s, 0), help="new duelist code (default: old + 1)")
    args = ap.parse_args()

    src = bytearray(open(args.src, "rb").read())
    if len(src) != MCD_SIZE:
        sys.exit(f"{args.src}: not a 128 KB memory card")
    at = find_save(src)
    if at is None:
        sys.exit(f"{args.src}: no {FILE_NAME.decode()} save")
    state = bytearray(src[at + HEADER:at + HEADER + STATE])
    if not validate(state):
        sys.exit(f"{args.src}: the save does not validate (mask words), refusing to guess")
    old = struct.unpack_from("<i", state, DUELIST_CODE)[0]
    new = args.code if args.code is not None else old + 1
    struct.pack_into("<i", state, DUELIST_CODE, new)
    seal(state)
    assert validate(state)

    try:
        dst = bytearray(open(args.dst, "rb").read())
        if len(dst) != MCD_SIZE:
            sys.exit(f"{args.dst}: not a 128 KB memory card")
    except FileNotFoundError:
        dst = bytearray(src)  # start from a copy of the source card's format
    dst_at = find_save(dst)
    if dst_at is None:
        # copy the directory entry and the whole block from the source
        idx = at // BLOCK - 1
        dst[0x80 + idx * 0x80:0x80 + (idx + 1) * 0x80] = src[0x80 + idx * 0x80:0x80 + (idx + 1) * 0x80]
        dst[at:at + BLOCK] = src[at:at + BLOCK]
        dst_at = at
    else:
        dst[dst_at:dst_at + BLOCK] = src[at:at + BLOCK]
    dst[dst_at + HEADER:dst_at + HEADER + STATE] = state
    dst[dst_at + HEADER + STATE:dst_at + HEADER + 2 * STATE] = state
    open(args.dst, "wb").write(dst)
    print(f"{args.dst}: save cloned, duelist code {old} -> {new}")


if __name__ == "__main__":
    main()
