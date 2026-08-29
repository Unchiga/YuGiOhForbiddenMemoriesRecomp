import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbg import q
BASE, LEN = 0x80000000, 0x200000
BTN = {'select':0x0001,'start':0x0008,'up':0x0010,'right':0x0020,'down':0x0040,
       'left':0x0080,'l1':0x0400,'r1':0x0800,'triangle':0x1000,'circle':0x2000,
       'cross':0x4000,'square':0x8000}
def snap():
    r = q({'cmd':'read_ram','addr':'%08X'%BASE,'len':LEN})
    h = r.get('data') or r.get('hex') or ''
    return bytes.fromhex(h) if h else b''
def press(name, frames=6, settle=0.55):
    q({'cmd':'set_input','buttons':0xFFFF & ~BTN[name],'frames':frames})
    time.sleep(0.30); q({'cmd':'clear_input'}); time.sleep(settle)
def diff(a, b, maxn=400):
    out = []
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            out.append(BASE + i)
            if len(out) > maxn: break
    return out
