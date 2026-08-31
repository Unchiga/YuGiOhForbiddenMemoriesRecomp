"""Sweep SLUS_014.11 for instructions referencing the burn/heal ladder ids
and for jal callers of given functions."""
import struct

EXE = r"C:\dev\ygofm-decomp\SLUS_014.11"
BASE = 0x8000F800
data = open(EXE, 'rb').read()

def words():
    for off in range(0x800, len(data) - 3, 4):
        yield off + BASE, struct.unpack_from('<I', data, off)[0]

# 1) immediates equal to ladder ids 338..347 (0x152..0x15B) or their negatives
ids = set(range(0x152, 0x15C))
neg = {(-i) & 0xFFFF for i in ids}
print("== imm references to ids 338..347 (addiu/slti/li/etc) ==")
for a, w in words():
    op = w >> 26
    imm = w & 0xFFFF
    if op in (8, 9, 10, 11, 12, 13, 14):  # addi(u) slti(u) andi ori xori
        if imm in ids or imm in neg:
            v = imm if imm in ids else -((-imm) & 0xFFFF)
            print(f"{a:08X}  {w:08X}  op={op} imm={imm:#x} ({v if imm in ids else 'id-'+hex((-v))})")

# 2) jal callers
print("\n== callers ==")
for target in (0x800707C4, 0x80071008, 0x80070710):
    t = 0x0C000000 | ((target & 0x0FFFFFFF) >> 2)
    print(f"-- jal {target:08X}")
    for a, w in words():
        if w == t:
            print(f"   {a:08X}")
