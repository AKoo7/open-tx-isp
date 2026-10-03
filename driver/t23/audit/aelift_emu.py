#!/usr/bin/env python3
"""Emulator check: lifted T23 AE0 chain vs. the stock tx-isp-t23.ko.

usage: aelift_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko> <sensor IQ .bin>

Both modules run in the MIPS emulator (memu.py) with the same IQ bank in
tparams, the same sensor description and the same sensor callbacks.  Each
frame feeds identical synthetic AE0 statistics / histogram into the DMA
ring, then runs ae0_interrupt_static, ae0_interrupt_hist and (on event 1)
tisp_ae0_process -- stock functions vs. the t23_aelift_emu_* entries.
Register writes, events and sensor calls must be identical frame by frame.
The scene luminance follows the exposure the AE programmed, so the AE
actually converges (or saturates) like on a camera.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU, u32

OEM, OURS, IQ = sys.argv[1:4]
iq = open(IQ, 'rb').read()
BANK, ACT = 0x15844, 0x13100
DAY = iq[0x18:0x18 + BANK]
NIGHT = iq[0x18 + BANK:0x18 + 2 * BANK]
W, H = 1920, 1080
FPS = (25 << 16) | 1          # sensor fps word as the core holds it
MAXAG, MAXIT, MINIT = 0x50000, 1121, 2
SENS = {0x5c: 'reset_dis', 0x60: 'reset_en', 0x64: 'alloc_ag', 0x68: 'alloc_ag_s',
        0x6c: 'alloc_dg', 0x70: 'alloc_it', 0x74: 'alloc_it_s', 0x78: 'set_it',
        0x7c: 'set_it_s', 0x80: 'start', 0x84: 'end', 0x88: 'set_ag',
        0x8c: 'set_ag_s', 0x90: 'set_dg'}


def sensor_attr_fields():
    """156-byte tisp_init argument (stock ispcore_core_ops_init layout)."""
    b = bytearray(156)
    def p32(o, v): b[o:o + 4] = u32(v).to_bytes(4, 'little')
    def p16(o, v): b[o:o + 2] = (v & 0xffff).to_bytes(2, 'little')
    p32(0, W); p32(4, H); p32(0x20, MAXAG); p32(0x24, 0); p32(0x28, MINIT)
    p32(0x2c, MAXIT); p32(0x30, FPS)
    p16(0x3c, MINIT); p16(0x3e, 1); p16(0x40, MAXIT); p16(0x42, 1)
    p32(0x44, 0); p16(0x48, 2560); p16(0x4a, 1125); p16(0x4c, MAXIT)
    p16(0x4e, 2); p16(0x50, 2); p16(0x52, 2)
    p16(0x54, (1 * 1000000 // 25) // 1125)
    p16(0x56, 1); p16(0x58, 1); p32(0x5c, 0); p32(0x60, 0); p32(0x64, 0)
    return bytes(b)


class Side:
    def __init__(self, path, stock):
        self.m = m = Mod(path)
        self.c = c = CPU(m)
        self.stock = stock
        self.ev = []
        self.sens = []
        c.writes = []
        pre = '' if stock else 'oem_'
        self.tp = m.addr('tparams')
        self.si = m.addr(pre + 'sensor_info')
        self.sc = m.addr(pre + 'sensor_ctrl')
        self.ti = m.addr(pre + 'tispinfo')
        self.fn = {
            'init': 'tiziano_ae_init' if stock else 't23_aelift_emu_init',
            'static': 'ae0_interrupt_static' if stock else 't23_aelift_emu_static',
            'hist': 'ae0_interrupt_hist' if stock else 't23_aelift_emu_hist',
            'process': 'tisp_ae0_process' if stock else 't23_aelift_emu_process',
            'dn': 'tiziano_ae_dn_params_refresh' if stock else 't23_aelift_emu_dn',
            'ev_attr': 'tisp_g_ev_attr' if stock else 't23_aelift_emu_g_ev_attr',
        }
        def ev(c_, R):
            a1 = R[5]
            self.ev.append((c_.r32(a1 + 8),) + tuple(c_.r32(a1 + 16 + 4 * k) for k in range(6)))
            return 0
        def wr(c_, R):
            c_.writes.append((R[4], R[5])); c_.regs_hw[R[4]] = R[5]; return 0
        def rd(c_, R):
            return c_.regs_hw.get(R[4], 0)
        def km(c_, R):
            p = c_.heapp; c_.heapp += (R[4] + 15) & ~15; return p
        zero = lambda c_, R: 0
        if stock:
            c.hook('tisp_event_push', ev)
            c.hook('system_reg_write', wr)
            c.hook('system_reg_read', rd)
            c.hook('system_irq_func_set', zero)
            c.hook('tisp_event_set_cb', zero)
        else:
            for n in m.byname:
                if n.startswith('t23_aelift_event_push'): c.hook(n, ev)
                if n.startswith('t23_aelift_reg_write'): c.hook(n, wr)
                if n.startswith('t23_aelift_reg_read'): c.hook(n, rd)
                if n.startswith('t23_aelift_kmalloc'): c.hook(n, km)
        c.hook('private_dma_cache_sync', zero)
        # IQ bank, sensor description, sensor callbacks
        self.load_bank(DAY)
        c.wrbytes(self.si, sensor_attr_fields())
        sc = self.sc
        c.w32(sc + 0x20, MAXAG); c.w32(sc + 0x24, 0)
        c.w8(sc + 0x3a, 2); c.w8(sc + 0x3b, 2); c.w8(sc + 0x3c, 2)
        c.w32(sc + 0x28, MINIT); c.w32(sc + 0x2c, MAXIT)
        c.w32(sc + 0x50, 1); c.w32(sc + 0x54, 1); c.w32(sc + 0x58, 0)
        for off, nm in SENS.items():
            addr = 0xE0001000 + off
            c.intercept[addr] = 'S_' + nm
            c.hooks['S_' + nm] = self.sensor_cb(nm)
            c.w32(sc + off, addr)
        # stock tisp_init: AE0 statistics ring and histogram ring
        self.buf = c.heapp; c.heapp += 0x6000
        c.w32(self.ti + 12, self.buf); c.w32(self.ti + 24, self.buf + 0x4000)
        c.regs_hw[0xa050] = 0
        self.it, self.ag = MAXIT // 4, 0
        self.evbuf = c.heapp; c.heapp += 0x100
        self.evattr = []

    def ev_attr(self):
        """tisp_g_ev_attr block (0x8c bytes) as the read-back getters see it."""
        c = self.c
        for i in range(0x100 // 4):
            c.w32(self.evbuf + 4 * i, 0)
        self.call('ev_attr', (0, self.evbuf))
        return tuple(c.r32(self.evbuf + 4 * i) for i in range(0x8c // 4))

    def load_bank(self, bank):
        self.c.wrbytes(self.tp + ACT, bank)

    def sensor_cb(self, nm):
        def f(c, R):
            v = R[4]
            self.sens.append((nm, v))
            if nm == 'alloc_ag':
                q = min(v, MAXAG) & ~0xfff
                c.w16(R[5], q >> 12); return q
            if nm == 'alloc_dg':
                c.w16(R[5] + 2, 0); return 0
            if nm == 'alloc_it':
                q = max(MINIT, min(v, MAXIT))
                c.w16(R[5] + 16, q); return q
            if nm == 'set_it': self.it = v & 0xffff
            if nm == 'set_ag': self.ag = v & 0xffff
            return 0
        return f

    def call(self, what, args=()):
        return self.c.call(self.fn[what], args)

    def frame(self, scene, flicker):
        """Fill one statistics + histogram slot for the current exposure."""
        c = self.c
        expo = self.it * 2 ** (self.ag / 16.0)
        rows = []
        for r in range(15):
            for col in range(15):
                # mild vignetting + a bright window in the top centre
                k = 1.0 - 0.02 * (abs(r - 7) + abs(col - 7))
                if r < 4 and 5 <= col <= 9: k *= 2.5
                y = min(255.0, scene * k * expo / 1000.0 * (1.0 - flicker))
                rows.append(y)
        area = (W // 15) * (H // 15) // 16
        for z, y in enumerate(rows):
            mm = int(y * area)
            b = area if y >= 250 else 0
            words = [(mm & 0x7ff) << 21, ((mm >> 11) & 0x3ff) | ((int(250 * b) & 0x1fffff) << 10),
                     (b & 0x1fff) << 12, 0]
            for i, w in enumerate(words):
                c.w32(self.buf + 16 * z + 4 * i, u32(w))
        hist = [0] * 256
        for y in rows: hist[int(y)] += area
        for i in range(512):
            c.w32(self.buf + 0x4000 + 4 * i, hist[i // 2] if i % 2 == 0 else 0)
        return sum(rows) / len(rows)


def run_scene(name, scene, night=False, flicker_amp=0.0, deflicker=None, frames=90):
    sides = [Side(OEM, True), Side(OURS, False)]
    for s in sides:
        if night:
            s.load_bank(NIGHT)
        if deflicker is not None:
            # IQ bank deflicker block (bank +0x248 -> _deflicker_para,
            # copied by tiziano_ae_params_refresh): enable, mode
            s.c.w32(s.tp + ACT + 0x248, deflicker[0]); s.c.w32(s.tp + ACT + 0x24c, deflicker[1])
        s.call('init', (0, 0, MINIT))
        if night:
            s.call('dn', (1,))
    logs = [[], []]
    for f in range(frames):
        flick = flicker_amp * (0.5 + 0.5 * ((f * 2) % 5) / 4.0) if flicker_amp else 0.0
        for i, s in enumerate(sides):
            n0, e0, w0 = len(s.sens), len(s.ev), len(s.c.writes)
            y = s.frame(scene, flick)
            s.call('static'); s.call('hist')
            if s.ev and s.ev[-1][0] == 1:
                s.ev.pop()
                s.call('process')
            logs[i].append((round(y, 1), s.it, s.ag))
            s.evattr.append(s.ev_attr())
    a, b = sides
    same = (a.c.writes == b.c.writes and a.ev == b.ev and a.sens == b.sens and
            a.evattr == b.evattr)
    its = sorted(set(v for n_, v in a.sens if n_ == 'set_it'))
    print('%-22s %s  writes %d/%d events %d/%d sensor %d/%d  it/ag start %s end %s luma %s->%s' % (
        name, 'SAME' if same else 'DIFF', len(a.c.writes), len(b.c.writes), len(a.ev), len(b.ev),
        len(a.sens), len(b.sens), logs[0][0][1:], logs[0][-1][1:], logs[0][0][0], logs[0][-1][0]))
    print('   integration times used:', its)
    w = a.evattr[-1]
    print('   ev_attr end: it %d ev %d us %d again %d ispdg %d tgain_db %d total_gain %d manual %d (%s)' % (
        w[0], w[2], w[4], w[7], w[8], w[9], w[10], w[15],
        'same all frames' if a.evattr == b.evattr else 'DIFF'))
    e6 = [e[1] for e in a.ev if e[0] == 6]
    print('   event 6 (AWB/CCM EV, Q10): %d events, last %s; max again %d max ISP dgain %d (getters)' % (
        len(e6), e6[-1] if e6 else '-', w[11], w[12]))
    if not same:
        for nm, A, B in (('writes', a.c.writes, b.c.writes), ('events', a.ev, b.ev), ('sensor', a.sens, b.sens),
                         ('ev_attr', a.evattr, b.evattr)):
            d = [(i, x, y) for i, (x, y) in enumerate(zip(A, B)) if x != y]
            if d or len(A) != len(B):
                print('   first %s diff' % nm, d[:3], 'len', len(A), len(B))
    extra = [t for t in b.c.trace_calls if t not in ('_raw_spin_lock_irqsave', '_raw_spin_unlock_irqrestore')]
    if extra:
        print('   ours called stubs:', sorted(set(extra)))
    return same


if __name__ == '__main__':
    res = []
    res.append(run_scene('day bright', 400.0))
    res.append(run_scene('day dark', 3.0))
    res.append(run_scene('day very bright', 4000.0))
    res.append(run_scene('night', 0.5, night=True))
    res.append(run_scene('50Hz flicker, deflicker', 60.0, flicker_amp=0.4, deflicker=(1, 50)))
    res.append(run_scene('50Hz flicker, strict', 20.0, flicker_amp=0.4, deflicker=(1, 1)))
    print('scenes', len(res), 'mismatch', res.count(False))
