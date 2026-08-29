import sys, os, struct
sys.path.insert(0, r'C:\Users\Unchiga\AppData\Local\Temp\claude\C--dev-memories\1a4b12e0-208c-49a5-9e2d-3aa079362284\scratchpad')
from live_ram import snap, BASE
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
ram = snap()
def dis(addr, n=40):
    o = addr - BASE
    for i in md.disasm(ram[o:o+n*4], addr):
        print('%08X  %-8s %s' % (i.address, i.mnemonic, i.op_str))
if __name__ == '__main__':
    dis(int(sys.argv[1],16), int(sys.argv[2]) if len(sys.argv)>2 else 40)
