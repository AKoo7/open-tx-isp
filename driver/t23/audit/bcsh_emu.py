#!/usr/bin/env python3
"""Emulator check: BCSH (with its CT colour matrix, Tccm) vs. the stock tx-isp-t23.ko.

usage: bcsh_emu.py <stock tx-isp-t23.ko> <built tx-isp-t23.ko> <sensor IQ .bin>

Both modules load the same IQ bank (stock: tparams active bank; built: the
bank pointer its tuning reader uses), run tiziano_bcsh_init, then for each
(EV, CT) pair tisp_bcsh_ev_update + tisp_bcsh_ct_update.  The BCSH register
block 0x8000..0x80ff must end up identical.  With the sc2336 IQ the bank
bypasses the CCM block (0xc bit 9), so this matrix is the only colour
correction stock applies.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memu import Mod, CPU

OEM, OURS, IQ = sys.argv[1:4]
iq = open(IQ, 'rb').read()
BANK, ACT = 0x15844, 0x13100
BANKS = {'day': iq[0x18:0x18 + BANK], 'night': iq[0x18 + BANK:0x18 + 2 * BANK]}


def side(path, stock, bank):
    m = Mod(path); c = CPU(m)
    if stock:
        c.wrbytes(m.addr('tparams') + ACT, bank)
    else:
        buf = c.heapp; c.heapp += (BANK + 15) & ~15
        c.wrbytes(buf, bank)
        c.w32(m.addr('regtrace_t23_source_active_bank'), buf)
        c.hook('regtrace_t23_valid_ptr', lambda c_, R: 1 if R[4] >= 0x10000000 else 0)
        def vm(c_, R):
            p = c_.heapp; c_.heapp += (R[4] + 15) & ~15; return p
        for n in ('vmalloc', 'private_vmalloc'):
            c.hook(n, vm)
        for n in m.stubs:
            if n == 'vmalloc':
                c.intercept[m.stubs[n]] = n
    c.call('tiziano_bcsh_init')
    return m, c


def regs(c):
    return {a: v for a, v in c.regs_hw.items() if 0x8000 <= a < 0x8100}


ok = True
for bname, bank in BANKS.items():
    a = side(OEM, True, bank); b = side(OURS, False, bank)
    for ev, ct in ((94, 5817), (300, 5817), (141, 5084), (1436, 3187), (20000, 2800), (60, 6500)):
        a[1].call('tisp_bcsh_ev_update', (ev << 10, 0))
        a[1].call('tisp_bcsh_ct_update', (ct, 0))
        b[1].call('tisp_bcsh_ev_update', (ev << 10, 0))
        b[1].call('tisp_bcsh_ct_update', (0, ct))
        ra, rb = regs(a[1]), regs(b[1])
        same = ra == rb
        ok &= same
        print('%-5s ev=%-5d ct=%-4d %s' % (bname, ev, ct, 'SAME' if same else 'DIFF'))
        if not same:
            for r in sorted(set(ra) | set(rb)):
                if ra.get(r) != rb.get(r):
                    print('   0x%04x stock=%s ours=%s' % (r, hex(ra[r]) if r in ra else '-',
                                                       hex(rb[r]) if r in rb else '-'))
    extra = sorted(set(t for t in b[1].trace_calls if 'spin' not in t))
    if extra:
        print('   ours called stubs:', extra)
print('MATCH' if ok else 'DIFF')
