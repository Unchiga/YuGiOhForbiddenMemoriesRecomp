"""Disassemble a RAM range of SLUS_014.11 (file offset = RAM - 0x8000F800)."""
import sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN

EXE = r"C:\dev\ygofm-decomp\SLUS_014.11"
BASE = 0x8000F800  # RAM = file_offset + BASE

def dis(addr, n_words):
    off = addr - BASE
    data = open(EXE, 'rb').read()[off:off + n_words * 4]
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    out = []
    for i in range(0, len(data), 4):
        w = int.from_bytes(data[i:i+4], 'little')
        line = None
        for ins in md.disasm(data[i:i+4], addr + i):
            line = f"{ins.address:08X}  {w:08X}  {ins.mnemonic} {ins.op_str}"
        if line is None:
            # capstone won't decode jal/j sometimes? decode manually
            op = w >> 26
            if op == 3:
                line = f"{addr+i:08X}  {w:08X}  jal 0x{((addr+i)&0xF0000000)|((w&0x3FFFFFF)<<2):08X}"
            elif op == 2:
                line = f"{addr+i:08X}  {w:08X}  j 0x{((addr+i)&0xF0000000)|((w&0x3FFFFFF)<<2):08X}"
            else:
                line = f"{addr+i:08X}  {w:08X}  .word"
        out.append(line)
    return out

if __name__ == '__main__':
    a = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    print('\n'.join(dis(a, n)))
