#!/usr/bin/env python3
"""Emulator check for the bss-audit fixes (claude/t23-bss-audit).

usage: bss_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko>

The decompiled T23 code had .bss/.data-relative accesses of the stock module
resolved to section-start placeholders (ivdc_threshold_line = .bss+0,
sclk_name = .data+0) or to tparams with the relocation's high half lost.
For every fixed function this runs stock and built code in memu.py with the
same inputs and checks
  * the values stock leaves in its statics equal ours (by name), and
  * the built function stores nothing outside the stack, the heap and the
    statics stock writes (no store into a placeholder object).
"""
import os, sys, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU, u32

OEM, OURS = sys.argv[1:3]
PLACEHOLDERS = ('ivdc_threshold_line', 'sclk_name', 'tparams', 'video_input_cmd_buf',
                'dump_csd', 'tevent_info', 'tiziano_clm_s_t_lut', 'tiziano_gib_deir_r_h',
                'ivdc_mem_line', 'gpio_info')


class LogCPU(CPU):
    def __init__(self, m):
        CPU.__init__(self, m)
        self.stores = []
        self.log = False

    def w8(self, a, v):
        if self.log: self.stores.append((a, 1))
        CPU.w8(self, a, v)

    def w16(self, a, v):
        if self.log: self.stores.append((a, 2))
        CPU.w16(self, a, v)

    def w32(self, a, v):
        if self.log: self.stores.append((a, 4))
        CPU.w32(self, a, v)


def symat(m, a):
    best = None
    for n, sa, sz in m.syms:
        if n and sa and sa <= a < sa + max(sz, 1):
            if best is None or sz < best[2]:
                best = (n, sa, sz)
    return best


def stored_syms(c):
    out = {}
    for a, n in c.stores:
        if 0x7f000000 <= a < 0x7f400000 or 0x60000000 <= a < 0x60400000:
            continue
        s = symat(c.m, a)
        out.setdefault(s[0] if s else '%08x' % a, 0)
        out[s[0] if s else '%08x' % a] += 1
    return out


ok = True


def check(name, cond, detail=''):
    global ok
    print('%-48s %s %s' % (name, 'ok' if cond else 'DIFF', detail))
    ok &= bool(cond)


def guard(name, c, allowed):
    bad = {k: v for k, v in stored_syms(c).items() if k not in allowed}
    check(name + ' stores only ' + ','.join(sorted(allowed)), not bad, bad if bad else '')
    hit = [k for k in stored_syms(c) if k in PLACEHOLDERS]
    if hit:
        check(name + ' placeholder store', False, hit)


def new(path):
    m = Mod(path)
    return m, LogCPU(m)


# 1. tiziano_af_params_refresh: 19 tparams blocks -> AF statics
AF = [('stAFParam_Zone', 'data_98848', 144), ('stAFParam_ThresEnable', 'data_98814', 52),
      ('stAFParam_FIR0_V', 'data_98800', 20), ('stAFParam_FIR0_Ldg', 'data_987e0', 32),
      ('stAFParam_FIR0_Coring', 'data_987d0', 16), ('stAFParam_FIR1_V', 'data_987bc', 20),
      ('stAFParam_FIR1_Ldg', 'data_9879c', 32), ('stAFParam_FIR1_Coring', 'data_9878c', 16),
      ('stAFParam_IIR0_H', 'data_98764', 40), ('stAFParam_IIR0_Ldg', 'data_98744', 32),
      ('stAFParam_IIR0_Coring', 'data_98734', 16), ('stAFParam_IIR1_H', 'data_9870c', 40),
      ('stAFParam_IIR1_Ldg', 'data_986ec', 32), ('stAFParam_IIR1_Coring', 'data_986dc', 16),
      ('AFParam_PointPos', 'data_986d4', 8), ('AFParam_Tilt', 'data_986c0', 20),
      ('AFParam_FvWmean', 'data_98684', 60), ('AFParam_Fv', 'AFParam_Fv', 12),
      ('AFWeight_Param', 'data_98300', 900)]
rnd = random.Random(23)
blob = bytes(rnd.randrange(256) for _ in range(0x28944))
mo, co = new(OEM); mb, cb = new(OURS)
for m, c in ((mo, co), (mb, cb)):
    c.wrbytes(m.addr('tparams') + 0x27c74, blob[0x27c74:0x28258])
    c.log = True
    c.call('tiziano_af_params_refresh')
same = all(co.rdbytes(mo.addr(s), n) == cb.rdbytes(mb.addr(d), n) for s, d, n in AF)
check('tiziano_af_params_refresh AF blocks == stock', same)
guard('tiziano_af_params_refresh', cb, {d for _, d, _ in AF})

# 2. tisp_ae_mean_update: wmean_new.  Values are not compared: the
# recovered body still scales the table pointer (uint32_t * arithmetic) and
# reads _ae_reg + 8 from a 4-byte object (both outside this audit), so only
# the store targets are checked (table placed where the scaled pointer lands).
m, c = new(OURS)
c.w32(0x60100000, 0x1000)
c.w32(m.addr('IspAe0WmeanParam') + 0x1c, 0x18040000)
c.w16(m.addr('_ae_parameter') + 4, 1)
c.w16(m.addr('_ae_parameter') + 12, 1)
c.w32(m.addr('_ae_reg') + 8, 0x400)
c.log = True
c.call('tisp_ae_mean_update', (0x60200000, 0x60200004))
guard('tisp_ae_mean_update', c, {'wmean_new'})

# 3. jz_isp_ccm: prologue up to the first callee (the rest of the recovered
# body is outside this audit): reads the CCM EV, sets ccm_real[16] = 100.
class Stop(Exception):
    pass


def stop(c_, R):
    raise Stop()


for flag in (0, 1):
    res = []
    for path in (OEM, OURS):
        m, c = new(path)
        cr = m.addr('ccm_real')
        c.w32(cr, flag)
        c.w32(cr + 8, 0xffffffff)
        for k, n in enumerate(('cm_ev_list_now', 'cm_sat_list_now')):
            c.w32(m.addr(n), 0x60100000 + 0x100 * k)
            for i in range(9):
                c.w32(0x60100000 + 0x100 * k + 4 * i, 0x100 * (i + 1))
        c.hook('jz_isp_ccm_parameter_convert', stop)
        c.hook('tiziano_ct_ccm_interpolation', stop)
        c.log = True
        try:
            c.call('jz_isp_ccm')
        except Stop:
            pass
        res.append((c.r32(cr), c.r32(cr + 16)))
        if path == OURS:
            guard('jz_isp_ccm flag=%d' % flag, c, {'ccm_real'})
    check('jz_isp_ccm flag=%d ccm_real == stock' % flag, res[0] == res[1], res)

# 4. ivdc_core_interrupt_service_routine: per-bit counters
CNT = ['ivdc_fifo_c_overflow', 'ivdc_fifo_y_overflow', 'ivdc_ddr_c_overflow', 'ivdc_ddr_y_overflow',
       'isp_frm_done', 'isp_frm_start', 'vpu0_end', 'vpu0_half_end', 'vpu1_end', 'vpu1_half_end',
       'vpu_err', 'isp_rst', 'isp_frm_err', 'vpu_stop_over', 'vpu_discard_over', 'ivdc_dma_done']
for st in (0x400000, 0x7fffff, 0x0000ff, 0x5a5a5a):
    res = []
    for path in (OEM, OURS):
        m, c = new(path)
        sd, priv, regs = 0x60000000, 0x60020000, 0x60010000
        c.hook('private_complete', lambda c_, R: 0)
        c.w32(sd + 212, priv)
        c.w32(priv + 220, regs)
        c.w32(regs + 68, st)
        c.log = True
        c.call('ivdc_core_interrupt_service_routine', (sd,))
        res.append([c.r32(m.addr(n)) for n in CNT])
        if path == OURS:
            guard('ivdc_core_isr st=%06x' % st, c, set(CNT) | {'%08x' % (0x60010000 + 84)})
    check('ivdc_core_isr st=%06x counters == stock' % st, res[0] == res[1], '' if res[0] == res[1] else res)

# 5. subdev_sensor_ops_release_all_sensor: list walk + i2c release
def rel_run(path, types):
    m, c = new(path)
    calls = []
    c.hook('private_i2c_put_adapter', lambda c_, R: calls.append(('put', R[4])) or 0)
    c.hook('private_i2c_unregister_device', lambda c_, R: calls.append(('unreg', R[4])) or 0)
    sd = 0x60000000
    head = sd + 0xdc
    c.w32(sd + 0xf4, 2)
    ents = []
    prev = head
    for k, t in enumerate(types):
        e = 0x60001000 + 0x100 * k
        client = 0x60008000 + 0x100 * k
        c.w32(e - 16, client)
        c.w32(client + 24, (0x6000c000 + k) if k % 2 == 0 else 0)
        c.w32(e + 44, t)
        c.w32(prev + 0, e)
        c.w32(e + 4, prev)
        prev = e
        ents.append(e)
    c.w32(prev + 0, head)
    c.w32(head + 4, prev)
    c.log = True
    ret = c.call('subdev_sensor_ops_release_all_sensor', (sd,), maxsteps=200000)
    links = [(c.r32(e), c.r32(e + 4)) for e in ents]
    return u32(ret), calls, (c.r32(head), c.r32(head + 4)), links, c


for types in ((1, 1, 2), (2,), (1, 5, 1), ()):
    a = rel_run(OEM, types)
    b = rel_run(OURS, types)
    check('release_all_sensor %s == stock' % (types,), a[:4] == b[:4], '' if a[:4] == b[:4] else (a[:4], b[:4]))
    bad = [k for k in stored_syms(b[4]) if k in PLACEHOLDERS]
    check('release_all_sensor %s no placeholder store' % (types,), not bad, bad)

# 6. ispcore_interrupt_service_routine: counters per status bit
ICNT = ['isp_err', 'isp_overflow', 'isp_breakfrm', 'isp_ip_frm_done', 'isp_ch0_frm_done']
STUB = ('tx_isp_send_event_to_remote', 'exception_handle', 'tisp_set_frame_drop', 'mbus_to_bayer_write',
        'tisp_top_sel', 'ip_done_interrupt_static', 'tisp_lsc_write_lut_datas', 'tisp_msca_Shd_ctrl',
        'private_schedule_work', 'private_do_gettimeofday', 'isp_printf', 'system_reg_write',
        'system_reg_read', 'tisp_event_push')
for st in (0x0008, 0x0200, 0x0100, 0x0300, 0x03f8):
    res = []
    for path in (OEM, OURS):
        m, c = new(path)
        for n in STUB:
            c.hook(n, lambda c_, R: 0)
        sd, regs, core = 0x60000000, 0x60010000, 0x60020000
        c.w32(sd + 184, regs)
        c.w32(sd + 212, core)
        c.w32(regs + 180, st)
        c.w32(core + 352, 0)
        c.w32(core + 384, 0)
        c.w32(core + 340, 0x60030000)
        c.w32(core + 344, 3)
        c.log = True
        try:
            c.call('ispcore_interrupt_service_routine', (sd,), maxsteps=200000)
            err = None
        except Exception as e:
            err = str(e)[:50]
        res.append(([c.r32(m.addr(n)) for n in ICNT], err))
        if path == OURS:
            bad = [k for k in stored_syms(c) if k in PLACEHOLDERS]
            check('ispcore_isr st=%04x no placeholder store' % st, not bad, bad)
    check('ispcore_isr st=%04x counters == stock' % st, res[0][0] == res[1][0], res)

print('MATCH' if ok else 'DIFF')
