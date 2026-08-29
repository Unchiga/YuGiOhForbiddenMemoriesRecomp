import sys, os, struct
sys.path.insert(0, r'C:\Users\Unchiga\AppData\Local\Temp\claude\C--dev-memories\1a4b12e0-208c-49a5-9e2d-3aa079362284\scratchpad')
from live_ram import snap, BASE
NAME_OFFSETS, NAME_SEGMENT, STATS_BASE = 0x801D5800, 0x801D0000, 0x801D4244
RAW = [0]*128
for i,c in enumerate(" eta oinsrhl.dumcgywfpbk\x00Av I'TSM,DOWHYER\x00\x00GLCNB\x00P-FzKjUxq0V2J#1QZ\x003" ):
    pass
T = {}
tbl = [' ','e','t','a','o','i','n','s','r','h','l','.','d','u','m','c',
       'g','y','w','f','p','b','k',None,'A','v','I',"'",'T','S','M',',',
       'D','O','W','H','Y','E','R',None,None,'G','L','C','N','B',None,'P',
       '-','F','z','K','j','U','x','q','0','V','2','J','#','1','Q','Z',
       None,'3','5','&',None,'7','X',None,None,None,'4',None,None,None,'6',None,
       None,None,None,None,None,'a',None,'8',None,'9',None,None,None,None,None,None]
ram = snap()
def u16(a): return struct.unpack_from('<H', ram, a-BASE)[0]
def u32(a): return struct.unpack_from('<I', ram, a-BASE)[0]
def name(cid):
    off = u16(NAME_OFFSETS + cid*2)
    p = NAME_SEGMENT + off
    out = []
    for k in range(40):
        b = ram[p-BASE+k]
        if b == 0xFF: break
        if b == 0xF8: out.append('<esc>'); break
        ch = tbl[b] if b < len(tbl) else None
        out.append(ch if ch else '<%02X>' % b)
    return ''.join(out)
if __name__ == '__main__':
    want = sys.argv[1].lower() if len(sys.argv) > 1 else 'kuriboh'
    for cid in range(1, 723):
        n = name(cid)
        if want in n.lower():
            print('id=%-4d  %-30s  stats=%08X' % (cid, n, u32(STATS_BASE + (cid-1)*4)))
