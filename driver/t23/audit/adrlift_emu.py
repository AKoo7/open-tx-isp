#!/usr/bin/env python3
"""Emulator check: lifted T23 ADR + defog vs. the stock tx-isp-t23.ko.

usage: adrlift_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko> <sensor IQ .bin>

Both modules run in the MIPS emulator (memu.py) with the same IQ bank in
tparams and the same gamma LUT.  tiziano_adr_init / tiziano_defog_init run
first (1920x1080), then per case: day or night bank (dn refresh), an EV
(tisp_adr_ev_update / tisp_defog_ev_update), a DRC / defog strength, and
three frames of identical synthetic ADR and defog statistics in the DMA
rings, each frame running the interrupt handler and (on its event) the
process function -- stock functions vs. t23_adrlift_emu_call.  Register
writes, events and the tuning read-back (param_array_get, str getters)
must be identical.  The last cases also write the read-back tuning with
param_array_set.
"""
import os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU, u32

OEM, OURS, IQ = sys.argv[1:4]
iq = open(IQ, 'rb').read()
BANK, ACT = 0x15844, 0x13100
DAY = iq[0x18:0x18 + BANK]
NIGHT = iq[0x18 + BANK:0x18 + 2 * BANK]
W, H = 1920, 1080
IDX = ['adr_init', 'defog_init', 'adr_irq', 'defog_irq', 'adr_process', 'defog_process',
       'adr_ev', 'defog_ev', 'adr_dn', 'defog_dn', 's_adr_str', 's_defog_str',
       'g_adr_str', 'g_defog_str', 'adr_get', 'defog_get', 'adr_set', 'defog_set',
       'adr_wdr', 'defog_wdr']
STOCK = ['tiziano_adr_init', 'tiziano_defog_init', 'tiziano_adr_interrupt_static',
         'tiziano_defog_interrupt_static', 'tisp_adr_process', 'tisp_defog_process',
         'tisp_adr_ev_update', 'tisp_defog_ev_update', 'tiziano_adr_dn_params_refresh',
         'tiziano_defog_dn_params_refresh', 'tisp_s_adr_str_internal', 'tisp_s_defog_str_internal',
         'tisp_g_adr_str_internal', 'tisp_g_defog_str_internal', 'tisp_adr_param_array_get',
         'tisp_defog_param_array_get', 'tisp_adr_param_array_set', 'tisp_defog_param_array_set',
         'tisp_adr_wdr_en', 'tisp_defog_wdr_en']


def ext_mem(m):
    """undefined data objects (kmalloc_caches) read as zero"""
    extra = bytearray(0x10000)
    off = m.off
    def off2(a):
        if 0xF0000000 <= a < 0xF0010000:
            return extra, a - 0xF0000000
        return off(a)
    m.off = off2


class Side:
    def __init__(self, path, stock, gamma):
        self.m = m = Mod(path)
        ext_mem(m)
        self.c = c = CPU(m)
        self.stock = stock
        self.ev = []
        c.writes = []
        self.tp = m.addr('tparams')
        def ev(c_, R):
            # stock tiziano_{adr,defog}_interrupt_static set only the event
            # id of the on-stack record; the rest is stack contents
            self.ev.append(c_.r32(R[5] + 8)); return 0
        def heap(c_, R):
            p = c_.heapp; c_.heapp += (R[4] + 15) & ~15; return p
        pool = []
        def heap_k(c_, R):
            if pool: return pool.pop()
            p = c_.heapp; c_.heapp += 0x2000; return p
        def kfree(c_, R):
            if R[4]: pool.append(R[4])
            return 0
        zero = lambda c_, R: 0
        for n in ('vmalloc',): c.hook(n, heap)
        for n in ('kmem_cache_alloc',): c.hook(n, heap_k)
        c.hook('kfree', kfree)
        for n in ('vfree', 'private_dma_cache_sync', 'system_irq_func_set', 'tisp_event_set_cb'):
            c.hook(n, zero)
        if stock:
            c.hook('tisp_event_push', ev)
        else:
            for n in m.byname:
                if n.startswith('t23_adrlift_event_push'): c.hook(n, ev)
        self.load_bank(DAY)
        # gamma LUT as the gamma block holds it (day and WDR copy)
        for nm in ('tiziano_gamma_lut', 'tiziano_gamma_lut_wdr'):
            if nm in m.byname:
                c.wrbytes(m.addr(nm), gamma)
        # ADR / defog statistics rings (virt == phys here)
        self.adr = c.heapp; c.heapp += 0x4000
        self.dfg = c.heapp; c.heapp += 0x4000
        if stock:
            ti = m.addr('tispinfo')
            c.w32(ti + 72, self.adr); c.w32(ti + 76, self.adr)
            c.w32(ti + 84, self.dfg); c.w32(ti + 88, self.dfg)
        else:
            c.call('t23_adrlift_emu_rings', (self.adr, self.adr, self.dfg, self.dfg))
        self.slot = 0
        self.out = c.heapp; c.heapp += 0x4000
        self.sz = c.heapp; c.heapp += 0x10

    def load_bank(self, bank):
        self.c.wrbytes(self.tp + ACT, bank)

    def call(self, what, a=(0, 0, 0)):
        a = tuple(a) + (0,) * (3 - len(a))
        if self.stock:
            return self.c.call(STOCK[IDX.index(what)], a, maxsteps=200_000_000)
        return self.c.call('t23_adrlift_emu_call', (IDX.index(what),) + a, maxsteps=200_000_000)

    def frame(self, adr_data, dfg_data):
        c = self.c
        k = self.slot; self.slot = (self.slot + 1) & 3
        c.wrbytes(self.adr + k * 0x1000, adr_data)
        c.wrbytes(self.dfg + k * 0x1000, dfg_data)
        c.regs_hw[0x44b0] = self.adr + k * 0x1000
        c.regs_hw[0x5ba4] = self.dfg + k * 0x1000
        n = len(self.ev)
        self.call('adr_irq')
        if len(self.ev) > n and self.ev[-1] == 2:
            self.call('adr_process')
        n = len(self.ev)
        self.call('defog_irq')
        if len(self.ev) > n and self.ev[-1] == 3:
            self.call('defog_process')

    def readback(self):
        c = self.c
        out = []
        for g in ('adr_get', 'defog_get'):
            c.wrbytes(self.out, bytes(0x4000)); c.w32(self.sz, 0)
            r = self.call(g, (0, self.out, self.sz))
            n = c.r32(self.sz)
            out.append((r, n, c.rdbytes(self.out, min(n, 0x4000))))
        for g in ('g_adr_str', 'g_defog_str'):
            c.w32(self.sz, 0)
            r = self.call(g, (0, self.sz))
            out.append((r, c.r32(self.sz)))
        return out


def scene(kind, seed, n=0x1000):
    random.seed(seed)
    if kind == 'rand':
        return bytes(random.getrandbits(8) for _ in range(n))
    lvl = {'dark': 20, 'mid': 90, 'bright': 200, 'mixed': None}[kind]
    return bytes(random.randint(0, 255) if lvl is None else
                 max(0, min(255, int(random.gauss(lvl, 25)))) for _ in range(n))


def main():
    st = Mod(OEM)
    g = st.addr('tiziano_gamma_lut')
    b, o = st.off(g)
    gamma = bytes(b[o:o + 258])
    sides = [Side(OEM, True, gamma), Side(OURS, False, gamma)]
    for s in sides:
        s.call('adr_init', (0, W, H))
        s.call('defog_init', (0, W, H))
    tot = bad = 0
    def check(label):
        nonlocal tot, bad
        a, b_ = sides
        ra, rb = a.readback(), b_.readback()
        same = a.c.writes == b_.c.writes and a.ev == b_.ev and ra == rb
        tot += 1
        if not same:
            bad += 1
            print('DIFF %-32s writes %d/%d events %d/%d readback %s' % (
                label, len(a.c.writes), len(b_.c.writes), len(a.ev), len(b_.ev), ra == rb))
            d = [(i, (hex(x[0]), hex(x[1])), (hex(y[0]), hex(y[1]))) for i, (x, y) in enumerate(zip(a.c.writes, b_.c.writes)) if x != y]
            print('   first write diffs', d[:3])
            print('   first event diffs', [(x, y) for x, y in zip(a.ev, b_.ev) if x != y][:2])
        else:
            print('SAME %-32s writes %d events %d' % (label, len(a.c.writes), len(a.ev)))
        for s in sides:
            s.c.writes.clear(); s.ev.clear()
    check('init')
    case = 0
    for night in (0, 1):
        for s in sides:
            s.load_bank(NIGHT if night else DAY)
            s.call('adr_dn'); s.call('defog_dn')
        check('dn refresh night=%d' % night)
        for kind in ('dark', 'mid', 'bright', 'mixed', 'rand'):
            for ev, strength in ((100, 0x80), (1700, 0x40), (8000, 0xc0), (40000, 0xff)):
                case += 1
                da = scene(kind, ev + night * 7)
                dd = scene(kind, ev + night * 7 + 1)
                for s in sides:
                    s.call('adr_ev', (ev << 10, 0))
                    s.call('defog_ev', (ev << 10, 0))
                    if strength != 0x80 or case % 5 == 0:
                        s.call('s_adr_str', (strength,))
                        s.c.w8(s.sz + 8, strength); s.call("s_defog_str", (0, s.sz + 8))
                    for f in range(3):
                        s.frame(da, dd)
                check('night=%d %-6s ev=%-5d str=%#x' % (night, kind, ev, strength))
    # tuning write-back (libimp SetTuning path): read, set, run
    for s in sides:
        for gname, sname in (('adr_get', 'adr_set'), ('defog_get', 'defog_set')):
            s.c.wrbytes(s.out, bytes(0x4000)); s.c.w32(s.sz, 0)
            s.call(gname, (0, s.out, s.sz))
            s.call(sname, (0, s.out))
        s.frame(scene('mixed', 5), scene('mixed', 6))
    check('param_array_set round trip')
    print('cases', tot, 'mismatch', bad)
    extra = sorted(set(sides[1].c.trace_calls) - {'_raw_spin_lock_irqsave', '_raw_spin_unlock_irqrestore'})
    if extra:
        print('ours called stubs:', extra)
    extra = sorted(set(sides[0].c.trace_calls))
    if extra:
        print('stock called stubs:', extra)


if __name__ == '__main__':
    main()
