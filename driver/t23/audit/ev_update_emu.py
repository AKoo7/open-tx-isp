#!/usr/bin/env python3
"""Emulator check: tisp_adr_ev_update / tisp_defog_ev_update vs. stock.

usage: ev_update_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko>

Both must store ev >> 10 (64-bit EV, Q10) in the ADR ev_now (and set
ev_changed) and the defog ev_now; stock .bss 0x13e88/0x13e8c/0x1553c."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU
oem, ours = sys.argv[1:3]
def run(path, names, fn, args):
    m=Mod(path); c=CPU(m)
    c.call(fn, args)
    return [c.r32(m.addr(n)) if isinstance(n,str) else c.r32(m.addr(n[0])+n[1]) for n in names]
ok=True
for lo,hi in ((0x23071234,0),(587726848,0),(0x400,1),(0xffffffff,0x3)):
    a=run(oem,[('ev_changed',0),('ev_changed',4)],'tisp_adr_ev_update',(lo,hi))
    b=run(ours,['ev_changed','adr_ev_now'],'tisp_adr_ev_update',(lo,hi))
    s=run(oem,[('defog_wdr_en',4)],'tisp_defog_ev_update',(lo,hi))
    d=run(ours,['defog_ev_now'],'tisp_defog_ev_update',(lo,hi,0,0))
    print(hex(lo),hi,'adr stock',a,'ours',b,'| defog stock',s,'ours',d)
    ok &= a==b and s==d
print('MATCH' if ok else 'DIFF')
