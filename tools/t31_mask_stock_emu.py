#!/usr/bin/env python3
"""Tiny MIPS32 LE interpreter over objdump -dr text of the stock
tx-isp-t31.ko mask functions.  Same command protocol as open_harness."""
import re, sys, struct

M32 = 0xffffffff
lines = open(sys.argv[1]).read().splitlines()
code = {}      # addr -> word
relocs = {}    # addr -> (type, sym)
labels = {}
last = None
for ln in lines:
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:', ln)
    if m:
        labels[m.group(2)] = int(m.group(1), 16)
        continue
    m = re.match(r'^\s+([0-9a-f]+):\t([0-9a-f]{8})\s', ln)
    if m:
        last = int(m.group(1), 16)
        code[last] = int(m.group(2), 16)
        continue
    m = re.match(r'^\s+([0-9a-f]+): (R_MIPS_\w+)\t(\S+)', ln)
    if m:
        relocs[int(m.group(1), 16)] = (m.group(2), m.group(3))

# symbol placement: page aligned so only the lui immediate changes
FAKE = {}
nxt = [0x1000]
def symaddr(s):
    if s == '.text':
        return 0
    if s == '.bss':
        return 0x20000000
    if s not in FAKE:
        FAKE[s] = nxt[0] << 16
        nxt[0] += 1
    return FAKE[s]

for a, (t, s) in relocs.items():
    w = code[a]
    if t == 'R_MIPS_HI16':
        code[a] = (w & 0xffff0000) | (((w & 0xffff) + (symaddr(s) >> 16)) & 0xffff)
    elif t == 'R_MIPS_LO16':
        pass
    else:
        raise SystemExit('reloc ' + t)

mem = {}
def rb(a): return mem.get(a & M32, 0)
def wb(a, v): mem[a & M32] = v & 0xff
def rw(a): return rb(a) | rb(a+1) << 8 | rb(a+2) << 16 | rb(a+3) << 24
def ww(a, v):
    for i in range(4): wb(a+i, v >> (8*i))
def rh(a): return rb(a) | rb(a+1) << 8

mmio = {}
writes = []
heap = [0x40000000]
RET = 0x7ffff000
text_funcs = {symaddr(n): labels[n] for n in ('tisp_mscaler_mask_change', 'tisp_mscaler_mask_setreg')}

def sx16(v): return v - 0x10000 if v & 0x8000 else v
def s32(v): v &= M32; return v - (1 << 32) if v & 0x80000000 else v

def call(entry, args, sp):
    R = [0]*32
    R[29] = sp; R[31] = RET
    for i, v in enumerate(args): R[4+i] = v & M32
    run(entry, R)
    return R[2]

def extern(addr, R):
    a0, a1, a2 = R[4], R[5], R[6]
    name = [k for k, v in FAKE.items() if v == addr]
    name = name[0] if name else hex(addr)
    if name == 'system_reg_write':
        writes.append((a0, a1)); mmio[a0] = a1
    elif name == 'system_reg_read':
        R[2] = mmio.get(a0, 0)
    elif name == 'isp_printf':
        pass
    elif name == 'private_kmalloc':
        R[2] = heap[0]; heap[0] += 0x1000
    elif name == 'memset':
        for i in range(a2): wb(a0+i, a1)
        R[2] = a0
    elif name == 'memcpy':
        for i in range(a2): wb(a0+i, rb(a1+i))
        R[2] = a0
    else:
        raise SystemExit('extern ' + name)

def step(pc, R):
    """Execute one instruction; return (branch_target or None, likely, taken)."""
    w = code[pc]
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
    sa = (w >> 6) & 31; fn = w & 63; imm = w & 0xffff; si = sx16(imm)
    br = None; likely = False; isbr = False
    def setr(r, v):
        if r: R[r] = v & M32
    if op == 0:
        if fn == 0x00: setr(rd, R[rt] << sa)
        elif fn == 0x02: setr(rd, R[rt] >> sa)
        elif fn == 0x08: isbr = True; br = R[rs]
        elif fn == 0x09: isbr = True; br = R[rs]; setr(rd, pc + 8)
        elif fn == 0x0a:
            if R[rt] == 0: setr(rd, R[rs])
        elif fn == 0x21: setr(rd, R[rs] + R[rt])
        elif fn == 0x23: setr(rd, R[rs] - R[rt])
        elif fn == 0x24: setr(rd, R[rs] & R[rt])
        elif fn == 0x25: setr(rd, R[rs] | R[rt])
        elif fn == 0x26: setr(rd, R[rs] ^ R[rt])
        elif fn == 0x27: setr(rd, ~(R[rs] | R[rt]))
        elif fn == 0x2b: setr(rd, 1 if R[rs] < R[rt] else 0)
        elif fn == 0x2a: setr(rd, 1 if s32(R[rs]) < s32(R[rt]) else 0)
        else: raise SystemExit('special %x @%x' % (fn, pc))
    elif op == 0x1c and fn == 2:
        setr(rd, s32(R[rs]) * s32(R[rt]))
    elif op == 1:
        isbr = True
        if rt == 2: likely = True
        elif rt != 0: raise SystemExit('regimm %x' % rt)
        if s32(R[rs]) < 0: br = pc + 4 + (si << 2)
    elif op in (4, 5, 0x14, 0x15):
        isbr = True
        eq = R[rs] == R[rt]
        take = eq if op in (4, 0x14) else not eq
        likely = op >= 0x14
        if take: br = pc + 4 + (si << 2)
    elif op == 9: setr(rt, R[rs] + si)
    elif op == 0xa: setr(rt, 1 if s32(R[rs]) < si else 0)
    elif op == 0xb: setr(rt, 1 if R[rs] < (si & M32) else 0)
    elif op == 0xc: setr(rt, R[rs] & imm)
    elif op == 0xd: setr(rt, R[rs] | imm)
    elif op == 0xf: setr(rt, imm << 16)
    elif op == 0x24: setr(rt, rb(R[rs] + si))
    elif op == 0x25: setr(rt, rh(R[rs] + si))
    elif op == 0x23: setr(rt, rw(R[rs] + si))
    elif op == 0x22:  # lwl
        a = (R[rs] + si) & M32; n = a & 3
        word = rw(a & ~3); sh = (3 - n) * 8; mask = (M32 << sh) & M32
        setr(rt, (R[rt] & ~mask) | ((word << sh) & mask))
    elif op == 0x26:  # lwr
        a = (R[rs] + si) & M32; n = a & 3
        word = rw(a & ~3); sh = n * 8; mask = M32 >> sh
        setr(rt, (R[rt] & ~mask) | (word >> sh))
    elif op == 0x28: wb(R[rs] + si, R[rt])
    elif op == 0x29: wb(R[rs] + si, R[rt]); wb(R[rs] + si + 1, R[rt] >> 8)
    elif op == 0x2b: ww(R[rs] + si, R[rt])
    else:
        raise SystemExit('op %x @%x' % (op, pc))
    return isbr, br, likely

def run(pc, R):
    while True:
        if pc == RET:
            return
        if pc in text_funcs:
            pc = text_funcs[pc]
        if pc not in code:
            extern(pc, R); pc = R[31]; continue
        isbr, br, likely = step(pc, R)
        if not isbr:
            pc += 4; continue
        if br is None:
            pc += 8 if likely else 4
            if not likely:
                ib, _, _ = step(pc, R)
                assert not ib
                pc += 4
            continue
        ib, _, _ = step(pc + 4, R)
        assert not ib, hex(pc)
        pc = br

# ---- state / protocol --------------------------------------------------
STK = 0x7f000000
bss_active = 0x20000000 + 0x27b90
bss_saved = 0x20000000 + 0x27b8c

def chen(): return rw(symaddr('msca_ch_en'))

for tok_line in sys.stdin.read().split('\n'):
    t = tok_line.split()
    if not t: continue
    c = t[0]
    if c == 'rd':
        mmio[int(t[1], 16)] = int(t[2], 16)
    elif c == 'ds':
        ch = int(t[1]); base = symaddr('ds%d_attr' % ch)
        for i in range(13): ww(base + 4*i, int(t[2+i], 16))
    elif c == 'chen':
        ww(symaddr('msca_ch_en'), int(t[1], 16))
    elif c == 'set':
        b = bytes(int(x, 16) for x in t[1:1+0xac])
        sp = STK
        for i, x in enumerate(b): wb(sp + i, x)
        args = [struct.unpack_from('<I', b, 4*i)[0] for i in range(4)]
        print('SET')
        writes.clear()
        call(labels['tisp_s_mscaler_mask_attr'], args, sp)
        for r, v in writes: print('W %x %x' % (r, v))
    elif c == 'flip':
        print('FLIP %x' % int(t[1], 16))
        writes.clear()
        call(labels['tisp_s_mscaler_hvflip_mask'], [int(t[1], 16)], STK)
        for r, v in writes: print('W %x %x' % (r, v))
    elif c == 'get':
        sv = rw(bss_saved); ac = rw(bss_active)
        print('GET' + ''.join(' %02x' % (rb(sv+i) if sv else 0) for i in range(0xac)))
        print('ACT' + ''.join(' %02x' % (rb(ac+i) if ac else 0) for i in range(0xac)))
        print('CHEN %x' % chen())
