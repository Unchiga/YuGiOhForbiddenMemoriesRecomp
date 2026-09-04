import time, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_probe as probe
# boot_to_menu.py: after `./Play.sh -dbg`, waits for the title, loads the memory-card save, lands on CAMPAIGN/FREE DUEL/... menu.
t0 = time.time()
while time.time() - t0 < 240:
    try:
        f = probe.frame()
        if f and f > 8400: break
    except Exception: pass
    time.sleep(2)
time.sleep(3)
probe.press('start', 60, 3.0)
probe.press('down', 20, 1.0)
probe.press('cross', 20, 2.0)
probe.press('cross', 20, 4.0)
probe.press('cross', 20, 3.0)
print('frame', probe.frame(), 'mode 0x%02X' % probe.mode())
print(probe.shot('r_menu'))
