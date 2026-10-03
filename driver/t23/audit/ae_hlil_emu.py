#!/usr/bin/env python3
"""Emulator check: HLIL AE (source_ae_oem=0) convergence after a module load.

usage: ae_hlil_emu.py <built tx-isp-t23.ko> [<older built ko> ...]

Runs the built regtrace_t23_source_ae_hlil_work in memu.py on the generic
sc2336 ladder (it 2..1121 lines, then 16 analog-gain rungs to 32x), starting
from the rung a fresh module load uses (longest unity-gain exposure).  The
statistics gate of the IRQ path (snapshot % interval, fast cadence while
over-exposed) is mirrored here because it is inlined in the IRQ handler.
The scene model clips: luma saturates at 187 as on cam-B (AE0 luma of a
fully over-exposed daylight frame) and reports the clipped-pixel share.
The sensor applies a new exposure two frames after it is programmed.
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU

MIN_IT, MAX_IT, MAX_AG = 2, 1121, 0x50000
ZONES = [0.35 + 1.6 * ((i * 37) % 225) / 224.0 for i in range(225)]   # scene spread


def ladder():
    r = []
    it = MIN_IT
    while True:
        r.append((it, 0))
        if it == MAX_IT:
            break
        nxt = it + max(it >> 1, 1)
        it = MAX_IT if nxt > MAX_IT else nxt
    step = -(-MAX_AG // 16)
    g = step
    while g < MAX_AG:
        r.append((MAX_IT, g)); g += step
    r.append((MAX_IT, MAX_AG))
    return r


def measure(scene, ev, collapse=False):
    vals = [min(255.0, scene * k * ev) for k in ZONES]
    luma = sum(vals) / len(vals) * 187.0 / 255.0
    bright = sum(1 for v in vals if v >= 235.0) * 1000 // len(vals)
    if collapse and bright >= 600:
        luma = 62.0     # worst case: clipped frame reads inside the deadband
    return int(luma), bright


def run(path, scene, frames=750, label='', collapse=False):
    m = Mod(path); c = CPU(m)
    A = m.byname
    expo = []
    def sens(c_, R):
        expo.append(R[4]); return 0
    c.hook('regtrace_t23_call_sensor_exposure', sens)
    for n in ('regtrace_t23_source_apply_total_gain_value', 'system_reg_write_gib',
              'regtrace_t23_text_check', 'tisp_bcsh_ev_update', 'schedule_work'):
        c.hook(n, lambda c_, R: 0)
    rungs = A['regtrace_t23_ae_hlil_rungs']
    lad = ladder()
    for i, (it, g) in enumerate(lad):
        gq16 = int(round(2 ** (g / 65536.0) * 65536))
        packed = ((g >> 12) << 16) | it
        for k, v in enumerate((packed, gq16, (it * gq16) >> 6, it, g, 0x400)):
            c.w32(rungs + 24 * i + 4 * k, v)
    c.w32(A['regtrace_t23_ae_hlil_state_count'], len(lad))
    init = lad.index((MAX_IT, 0))
    c.w32(A['regtrace_t23_source_ae_hlil_state'], init)
    c.w32(A['regtrace_t23_source_ae_hlil_ev'], (MAX_IT << 16) >> 6)
    for n in ('regtrace_t23_sensor_streaming', 'regtrace_t23_core_started'):
        c.w8(A[n], 1)
    for n in ('regtrace_t23_source_ccm_events', 'regtrace_t23_source_bcsh_events'):
        if n in A:
            c.w8(A[n], 0)
    interval = c.r32(A['regtrace_t23_source_ae_hlil_interval'])
    fast = c.r32(A['regtrace_t23_source_ae_hlil_fast_interval']) if 'regtrace_t23_source_ae_hlil_fast_interval' in A else 0
    target = c.r32(A['regtrace_t23_source_ae_hlil_target'])
    dead = c.r32(A['regtrace_t23_source_ae_hlil_deadband'])
    applied = [lad[init]] * 3          # sensor latency: 2 frames
    settled = None
    trace = []
    for f in range(1, frames + 1):
        st = c.r32(A['regtrace_t23_source_ae_hlil_state'])
        applied.append(lad[st])
        it, g = applied[-3]
        ev = it * 2 ** (g / 65536.0)
        luma, bright = measure(scene, ev, collapse)
        over = 'regtrace_t23_ae_hlil_over' in A and c.r8(A['regtrace_t23_ae_hlil_over'])
        iv = fast if (over and fast and (not interval or fast < interval)) else interval
        if iv and f % iv:
            continue
        c.w32(A['regtrace_t23_ae_hlil_pending_luma'], luma)
        if 'regtrace_t23_ae_hlil_pending_bright' in A:
            c.w32(A['regtrace_t23_ae_hlil_pending_bright'], bright)
        c.w32(A['regtrace_t23_ae_hlil_pending_snapshot'], f)
        c.w8(A['regtrace_t23_ae_hlil_pending'], 1)
        c.call('regtrace_t23_source_ae_hlil_work', (0,))
        nst = c.r32(A['regtrace_t23_source_ae_hlil_state'])
        trace.append((f, luma, bright, st, nst))
        if settled is None and abs(luma - target) <= dead and bright < 500:
            settled = f
    fin = lad[c.r32(A['regtrace_t23_source_ae_hlil_state'])]
    lum_end = measure(scene, applied[-1][0] * 2 ** (applied[-1][1] / 65536.0), collapse)[0]
    print('%-26s %-10s settled at frame %s (%.1f s @25fps), end it=%d again_log2=0x%x luma=%d, %d runs' % (
        label, os.path.basename(path), settled, (settled or 0) / 25.0, fin[0], fin[1], lum_end, len(trace)))
    print('   runs (frame luma bright state->state):', ' '.join('%d:%d/%d %d->%d' % t for t in trace[:10]))
    return settled, fin


if __name__ == '__main__':
    kos = sys.argv[1:]
    # scene: luma ~60 at the exposure that the stock lands on
    for label, scene, col in (('daylight (EV~94)', 60 * 255 / 187.0 / 94 / 1.15, False),
                              ('bright daylight (EV~30)', 60 * 255 / 187.0 / 30 / 1.15, False),
                              ('daylight, clipped luma', 60 * 255 / 187.0 / 94 / 1.15, True),
                              ('indoor (EV~600)', 60 * 255 / 187.0 / 600 / 1.15, False),
                              ('night (max gain)', 0.004, False)):
        for k in kos:
            run(k, scene, label=label, collapse=col)
