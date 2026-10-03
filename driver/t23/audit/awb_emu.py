#!/usr/bin/env python3
"""Emulator check: AWB gains of the built driver's HLIL AWB vs. the stock
tx-isp-t23.ko AWB (awb_interrupt_static -> JZ_Isp_Awb -> Tiziano_awb_set_gain).

usage: awb_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko> <sensor IQ .bin>

Both see the same synthetic 15x15 AWB zone statistics and the same scene EV
(Q10).  Stock: the gains it writes to 0x1804/0x1808 (Q10) and _awb_ct.
Built: regtrace_t23_source_awb_hlil_work with the IQ AWB block loaded.
Prints the white balance each one leaves on a grey patch (R/G, B/G after the
gains; 1.000 = neutral).
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU

OEM, OURS, IQ = sys.argv[1:4]
iq = open(IQ, 'rb').read()
BANK, ACT, HDR = 0x15844, 0x13100, 0x18
DAY = iq[HDR:HDR + BANK]


def zone_words(r, g, b, pix):
    return [(r & 0x1fffff) | ((g & 0x7ff) << 21),
            ((g >> 11) & 0x3ff) | ((b & 0x1fffff) << 10),
            (pix & 0xfff) << 20, (pix >> 12) & 1]


def scene_zones(kind):
    """15x15 zones: (r, g, b, pixels).  Grey in daylight has raw G/R 2.0,
    G/B 1.8 (cam-B AWB statistics at ~5800 K)."""
    out = []
    for z in range(225):
        row, col = divmod(z, 15)
        pix = 2000
        lum = 40.0
        rg, bg = 1 / 2.0, 1 / 1.8           # grey
        if kind == 'garden':
            if 4 <= row <= 11 and 4 <= col <= 10:
                rg, bg = 0.30, 0.25          # foliage
            elif row < 2:
                rg, bg = 0.45, 0.70; lum = 90.0   # bright sky / wall
        elif kind == 'foliage':
            if (row + col) % 3:
                rg, bg = 0.30, 0.25
        elif kind == 'indoor':
            rg, bg = 1 / 1.4, 1 / 2.6        # ~3000 K grey
        g = lum * pix
        out.append((int(g * rg), int(g), int(g * bg), pix))
    return out


def fill_stats(c, buf, zones):
    for z, (r, g, b, p) in enumerate(zones):
        for k, w in enumerate(zone_words(r, g, b, p)):
            c.w32(buf + 16 * z + 4 * k, w)


class Stock:
    def __init__(self):
        m = self.m = Mod(OEM); c = self.c = CPU(m)
        zero = lambda c_, R: 0
        for n in ('system_irq_func_set', 'tisp_event_set_cb', 'private_dma_cache_sync', 'tisp_event_push', 'tisp_ae_mean_update'):
            c.hook(n, zero)
        c.wrbytes(m.addr('tparams') + ACT, DAY)
        self.buf = c.heapp; c.heapp += 0x4000
        c.w32(m.addr('tispinfo') + 60, self.buf)
        c.regs_hw[0xb050] = 0
        c.call('tiziano_awb_init', (1080, 1920))

    def run(self, zones, ev):
        c = self.c
        c.call('tisp_awb_ev_update', (ev << 10, 0))
        fill_stats(c, self.buf, zones)
        c.call('awb_interrupt_static')
        c.call('JZ_Isp_Awb')
        return (c.regs_hw.get(0x1804, 0) & 0x3fff, c.regs_hw.get(0x1808, 0) & 0x3fff,
                c.r32(self.m.addr('_awb_ct')))


class Ours:
    OFF = {  # file offset, symbol, words
        0x10f8: ('regtrace_t23_awb_hlil_mf_parameters', 6),
        0x1124: ('regtrace_t23_awb_hlil_wb_static', 2),
        0x112c: ('regtrace_t23_awb_hlil_light_sources', 20),
        0x1180: ('regtrace_t23_awb_hlil_rg_positions', 15),
        0x11bc: ('regtrace_t23_awb_hlil_bg_positions', 15),
        0x1238: ('regtrace_t23_awb_hlil_distance_parameters', 3),
        0x1244: ('regtrace_t23_awb_hlil_indoor_ct_weight_mesh', 225),
        0x15c8: ('regtrace_t23_awb_hlil_color_temperature_mesh', 225),
        0x194c: ('regtrace_t23_awb_hlil_zone_weight_mesh', 225),
        0x1cd0: ('regtrace_t23_awb_hlil_outdoor_ct_weight_mesh', 225),
        0x2054: ('regtrace_t23_awb_hlil_light_source_weight_lut', 514),
    }

    def __init__(self):
        m = self.m = Mod(OURS); c = self.c = CPU(m)
        for n in ('schedule_work', 'private_dma_cache_sync', 'regtrace_t23_text_check'):
            if n in m.byname or n in m.stubs:
                c.hook(n, lambda c_, R: 0)
        W = lambda o: struct.unpack_from('<I', iq, o)[0]
        for off, (sym, n) in self.OFF.items():
            c.wrbytes(m.addr(sym), iq[off:off + 4 * n])
        sc = {'regtrace_t23_awb_hlil_pixel_threshold': W(0x10dc),
              'regtrace_t23_awb_hlil_point_position': W(0x10e8),
              'regtrace_t23_awb_hlil_history_window': W(0x10f0),
              'regtrace_t23_awb_hlil_outdoor_ev': W(0x1110),
              'regtrace_t23_awb_hlil_indoor_ev': W(0x1114),
              'regtrace_t23_awb_hlil_startup_ev': W(0x1118) << 10,
              'regtrace_t23_awb_hlil_startup_ct': W(0x111c),
              'regtrace_t23_awb_hlil_light_source_count': W(0x117c),
              'regtrace_t23_source_awb_hlil_tuning_loaded': 1,
              'regtrace_t23_source_awb_profile_rbias': 1024,
              'regtrace_t23_source_awb_profile_bbias': 1024}
        for k, v in sc.items():
            if k not in m.byname:
                continue        # unused, optimised out
            if k == 'regtrace_t23_source_awb_hlil_tuning_loaded':
                c.w8(m.addr(k), v)
            else:
                c.w32(m.addr(k), v)
        self.snap = m.addr('regtrace_t23_awb_hlil_snapshot_data')

    def run(self, zones, ev):
        c, m = self.c, self.m
        c.w32(m.addr('regtrace_t23_source_ae_hlil_ev'), ev << 10)
        for z, (r, g, b, p) in enumerate(zones):
            c.w32(self.snap + 4 * z, r)
            c.w32(self.snap + 4 * (225 + z), g)
            c.w32(self.snap + 4 * (450 + z), b)
            c.w32(self.snap + 4 * (675 + z), p)
        c.call('regtrace_t23_source_awb_hlil_work', (0,))
        return (c.r32(m.addr('regtrace_t23_source_awb_last_rgain')),
                c.r32(m.addr('regtrace_t23_source_awb_last_bgain')),
                c.r32(m.addr('regtrace_t23_source_awb_hlil_ct')))


def neutral(g, kind):
    # a grey patch under the scene light: daylight raw R/G 0.5, B/G 1/1.8
    rg, bg = (1 / 1.4, 1 / 2.6) if kind == 'indoor' else (0.5, 1 / 1.8)
    return (rg * g[0] / 1024.0, bg * g[1] / 1024.0)


if __name__ == '__main__':
    for kind in ('grey', 'garden', 'foliage', 'indoor'):
        for ev in (30, 94, 141, 300, 2000):
            st, ou = Stock(), Ours()
            z = scene_zones(kind)
            for _ in range(20):
                a = st.run(z, ev); b = ou.run(z, ev)
            na, nb = neutral(a, kind), neutral(b, kind)
            print('%-8s ev=%-5d stock gain 0x%03x/0x%03x ct=%-5d grey R/G %.3f B/G %.3f | ours 0x%03x/0x%03x ct=%-5d grey R/G %.3f B/G %.3f' % (
                kind, ev, a[0], a[1], a[2], na[0], na[1], b[0], b[1], b[2], nb[0], nb[1]))
