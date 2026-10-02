#!/usr/bin/env python3
"""Minimal MIPS32 (LE) emulator for relocatable kernel modules (.ko).
Loads the ELF, applies REL relocations, runs single functions with stubbed
externals and records system_reg_write calls."""
import struct, sys

def s16(x): return x - 0x10000 if x & 0x8000 else x
def u32(x): return x & 0xffffffff
def s32(x):
    x &= 0xffffffff
    return x - 0x100000000 if x & 0x80000000 else x

class Mod:
    def __init__(self, path, base=0x10000000):
        d = self.d = open(path, 'rb').read()
        (e_shoff,) = struct.unpack_from('<I', d, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2e)
        sh = []
        for i in range(e_shnum):
            sh.append(struct.unpack_from('<IIIIIIIIII', d, e_shoff + i * e_shentsize))
        self.sh = sh
        shstr = sh[e_shstrndx]
        def nm(tab, off):
            o = tab[4] + off
            return d[o:d.index(b'\0', o)].decode()
        self.secname = [nm(shstr, s[0]) for s in sh]
        # allocate
        self.mem = bytearray(0x800000)
        self.base = base
        self.secaddr = {}
        cur = 0x1000
        for i, s in enumerate(sh):
            name, typ, flags, addr, off, size, link, info, align, ent = s
            if flags & 2 and size:  # SHF_ALLOC
                a = max(align, 4)
                cur = (cur + a - 1) // a * a
                self.secaddr[i] = base + cur
                if typ != 8:
                    self.mem[cur:cur + size] = d[off:off + size]
                cur += size
        self.top = cur
        # symbols
        symsec = [i for i, s in enumerate(sh) if s[1] == 2][0]
        st = sh[symsec]; strt = sh[st[6]]
        self.syms = []
        self.byname = {}
        self.stubs = {}
        stub = 0xF0000000
        for k in range(st[5] // 16):
            n, val, size, info, oth, shndx = struct.unpack_from('<IIIBBH', d, st[4] + k * 16)
            name = nm(strt, n) if n else ''
            if shndx == 0:
                if name:
                    addr = self.stubs.setdefault(name, stub + len(self.stubs) * 8)
                else:
                    addr = 0
            elif shndx in self.secaddr:
                addr = self.secaddr[shndx] + val
            elif shndx == 0xfff1:
                addr = val
            else:
                addr = 0
            self.syms.append((name, addr, size))
            if name and shndx != 0 and (name not in self.byname or (info >> 4) == 1):
                self.byname[name] = addr
        self.stubname = {v: k for k, v in self.stubs.items()}
        # relocations
        for i, s in enumerate(sh):
            if s[1] != 9:
                continue
            tgt = s[7]
            if tgt not in self.secaddr:
                continue
            tbase = self.secaddr[tgt] - base
            hipend = []
            for k in range(s[5] // 8):
                r_off, r_info = struct.unpack_from('<II', d, s[4] + k * 8)
                typ = r_info & 0xff; si = r_info >> 8
                S = self.syms[si][1]
                if si and self.syms[si][0] == '' and self.syms[si][1] == 0:
                    S = 0
                # section symbols
                o = tbase + r_off
                w = struct.unpack_from('<I', self.mem, o)[0]
                if typ == 2:
                    struct.pack_into('<I', self.mem, o, u32(w + S))
                elif typ == 4:
                    P = base + o
                    t = (((w & 0x3ffffff) << 2) | (P & 0xf0000000)) + S
                    struct.pack_into('<I', self.mem, o, (w & 0xfc000000) | ((t >> 2) & 0x3ffffff))
                elif typ == 5:
                    hipend.append((o, S))
                elif typ == 6:
                    lo = s16(w & 0xffff)
                    for (ho, hS) in hipend:
                        hw = struct.unpack_from('<I', self.mem, ho)[0]
                        ahl = ((hw & 0xffff) << 16) + lo
                        v = u32(hS + ahl)
                        hi = ((v + 0x8000) >> 16) & 0xffff
                        struct.pack_into('<I', self.mem, ho, (hw & 0xffff0000) | hi)
                    v = u32(S + ((self.hi_of(hipend, o) ) if False else 0) + lo)
                    # LO16 standalone: S + AHL where AHL low part = lo (hi part handled via pair)
                    v = u32(S + lo)
                    struct.pack_into('<I', self.mem, o, (w & 0xffff0000) | (v & 0xffff))
                    hipend = []
                elif typ in (37, 0):
                    pass
                else:
                    raise Exception('reloc type %d' % typ)
        # sym name lookup for (section-local) objects
    def hi_of(self, *a): return 0
    def addr(self, name):
        return self.byname[name]
    def symsize(self, name):
        for n, a, s in self.syms:
            if n == name and a == self.byname.get(name):
                return s
        return None
    # memory
    def off(self, a):
        o = a - self.base
        if 0 <= o < len(self.mem):
            return self.mem, o
        if 0x7f000000 <= a < 0x7f400000:
            return self.stack, a - 0x7f000000
        if 0x60000000 <= a < 0x60400000:
            return self.heap, a - 0x60000000
        raise Exception('bad addr %08x' % a)

class CPU:
    def __init__(self, m, regread=None):
        self.m = m
        m.stack = bytearray(0x400000)
        m.heap = bytearray(0x400000)
        self.heapp = 0x60000000
        self.writes = []
        self.regs_hw = dict(regread or {})
        self.trace_calls = []
        self.intercept = {}
        self.hooks = {}
        self.watch = {}
        self.wlog = []
        self.snap_at = None
        self.events = []
        for n in ('system_reg_write', 'system_reg_read', 'memcpy', 'memset', 'isp_printf', 'printk', 'private_kmalloc', 'private_kfree', 'tisp_event_push', 'private_copy_to_user', 'private_copy_from_user'):
            if n in m.byname:
                self.intercept[m.byname[n]] = n
        self.intercept[0xE0000000] = 'ECHO'
        self.intercept[0xE0000010] = 'ZERO'
        for n, a in m.byname.items():
            if 'spin_lock' in n or 'spin_unlock' in n or 'mutex_' in n:
                self.intercept[a] = n
    def r32(self, a):
        b, o = self.m.off(a); return struct.unpack_from('<I', b, o)[0]
    def w32(self, a, v):
        b, o = self.m.off(a); struct.pack_into('<I', b, o, u32(v))
    def r16(self, a):
        b, o = self.m.off(a); return struct.unpack_from('<H', b, o)[0]
    def w16(self, a, v):
        b, o = self.m.off(a); struct.pack_into('<H', b, o, v & 0xffff)
    def r8(self, a):
        b, o = self.m.off(a); return b[o]
    def w8(self, a, v):
        b, o = self.m.off(a); b[o] = v & 0xff
    def rdbytes(self, a, n):
        return bytes(self.r8(a + i) for i in range(n))
    def wrbytes(self, a, data):
        for i, c in enumerate(data):
            self.w8(a + i, c)
    def hook(self, name, fn):
        self.hooks[name] = fn
        if name in self.m.byname:
            self.intercept[self.m.byname[name]] = name
    def stubcall(self, name, R):
        a0, a1, a2, a3 = R[4], R[5], R[6], R[7]
        if name in self.hooks:
            return self.hooks[name](self, R)
        if name == 'tisp_event_push':
            self.events.append(tuple(self.r32(a0 + 4 * k) for k in range(8))); return 0
        if name == 'system_reg_write':
            self.writes.append((a0, a1)); self.regs_hw[a0] = a1; return 0
        if name == 'system_reg_read':
            return self.regs_hw.get(a0, 0)
        if name in ('private_copy_from_user', 'private_copy_to_user'):
            self.wrbytes(a0, self.rdbytes(a1, a2)); return 0
        if name in ('memcpy', '__memcpy', 'memmove'):
            self.wrbytes(a0, self.rdbytes(a1, a2)); return a0
        if name in ('memset', '__memset'):
            self.wrbytes(a0, bytes([a1 & 0xff]) * a2); return a0
        if name in ('private_kmalloc', '__kmalloc', 'kmalloc'):
            p = self.heapp; self.heapp += (a0 + 15) & ~15; return p
        if name == 'int_sqrt':
            import math; return math.isqrt(a0)
        if name == '__udivdi3':
            n=(a1<<32)|a0; d=(a3<<32)|a2; q=n//d if d else 0; R[3]=(q>>32)&0xffffffff; return q&0xffffffff
        if name == '__umoddi3':
            n=(a1<<32)|a0; d=(a3<<32)|a2; q=n%d if d else 0; R[3]=(q>>32)&0xffffffff; return q&0xffffffff
        if name == '__div64_32':
            n=self.r32(a0)|(self.r32(a0+4)<<32); q,r=divmod(n,a1) if a1 else (0,0)
            self.w32(a0,q&0xffffffff); self.w32(a0+4,q>>32); return r
        if name in ('__ashldi3','__lshrdi3'):
            n=(a1<<32)|a0; v=(n<<a2) if name=='__ashldi3' else (n>>a2); v&=(1<<64)-1; R[3]=v>>32; return v&0xffffffff
        if name in ('div64_u64','__udivdi3'):
            n=(a1<<32)|a0; d=(a3<<32)|a2; q=n//d if d else 0; R[3]=q>>32; return q&0xffffffff
        if name == 'ECHO':
            return a0
        if name == 'ZERO':
            return 0
        if name == 'tisp_simple_intp_STUB':
            return 0
        self.trace_calls.append(name)
        return 0
    def call(self, fname, args=(), maxsteps=5_000_000):
        m = self.m
        R = [0] * 32
        R[29] = 0x7f200000
        R[28] = 0x603f0000
        for i, a in enumerate(args):
            if i < 4:
                R[4 + i] = u32(a)
            else:
                self.w32(R[29] + 16 + 4 * (i - 4), a)
        RET = 0xEFFFFFF0
        R[31] = RET
        pc = m.addr(fname) if isinstance(fname, str) else fname
        npc = pc + 4
        HI = LO = 0
        steps = 0
        while True:
            if pc == RET:
                self.laststeps = steps
                return R[2]
            if pc in self.watch and len(self.wlog) < 20000:
                self.wlog.append((self.watch[pc], R[4], R[5], R[6], R[7], R[31]))
                if self.snap_at is not None and len(self.wlog) == self.snap_at:
                    self.snap = self.snapfn(self, R)
            if pc == 0:
                self.trace_calls.append('NULLCALL')
                R[2] = 0; pc = R[31]; npc = pc + 4
                continue
            if pc >= 0xF0000000 or pc in self.intercept:
                name = self.intercept.get(pc) or m.stubname.get(pc, '?%x' % pc)
                R3 = R[3]
                R[2] = u32(self.stubcall(name, R))
                pc = R[31]; npc = pc + 4
                continue
            steps += 1
            if steps > maxsteps:
                raise Exception('too many steps in %s' % fname)
            self.lastpc = pc
            ins = self.r32(pc)
            op = ins >> 26; rs = (ins >> 21) & 31; rt = (ins >> 16) & 31
            rd = (ins >> 11) & 31; sa = (ins >> 6) & 31; fn = ins & 63
            imm = ins & 0xffff; simm = s16(imm)
            nxt = npc + 4
            br = None; likely = False; link = None
            def W(r, v):
                if r: R[r] = u32(v)
            if op == 0:
                if fn == 0: W(rd, R[rt] << sa)
                elif fn == 2:
                    if rs == 1: W(rd, ((R[rt] >> sa) | (R[rt] << (32 - sa))) if sa else R[rt])
                    else: W(rd, R[rt] >> sa)
                elif fn == 3: W(rd, s32(R[rt]) >> sa)
                elif fn == 4: W(rd, R[rt] << (R[rs] & 31))
                elif fn == 6: W(rd, R[rt] >> (R[rs] & 31))
                elif fn == 7: W(rd, s32(R[rt]) >> (R[rs] & 31))
                elif fn == 8: br = R[rs]
                elif fn == 9: br = R[rs]; link = rd
                elif fn == 10:
                    if R[rt] == 0: W(rd, R[rs])
                elif fn == 11:
                    if R[rt] != 0: W(rd, R[rs])
                elif fn == 16: W(rd, HI)
                elif fn == 18: W(rd, LO)
                elif fn == 17: HI = R[rs]
                elif fn == 19: LO = R[rs]
                elif fn == 24:
                    p = s32(R[rs]) * s32(R[rt]); LO = u32(p); HI = u32(p >> 32)
                elif fn == 25:
                    p = R[rs] * R[rt]; LO = u32(p); HI = u32(p >> 32)
                elif fn == 26:
                    a, b = s32(R[rs]), s32(R[rt])
                    if b: q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1); LO = u32(q); HI = u32(a - q * b)
                elif fn == 27:
                    if R[rt]: LO = R[rs] // R[rt]; HI = R[rs] % R[rt]
                elif fn in (32, 33): W(rd, R[rs] + R[rt])
                elif fn in (34, 35): W(rd, R[rs] - R[rt])
                elif fn == 36: W(rd, R[rs] & R[rt])
                elif fn == 37: W(rd, R[rs] | R[rt])
                elif fn == 38: W(rd, R[rs] ^ R[rt])
                elif fn == 39: W(rd, ~(R[rs] | R[rt]))
                elif fn == 42: W(rd, 1 if s32(R[rs]) < s32(R[rt]) else 0)
                elif fn == 43: W(rd, 1 if R[rs] < R[rt] else 0)
                elif fn == 13: raise Exception('break at %08x in %s' % (pc, fname))
                elif fn == 15: pass  # sync
                elif fn == 52: pass  # teq
                else: raise Exception('special fn %d at %08x' % (fn, pc))
            elif op == 1:
                if rt in (0, 2):
                    if s32(R[rs]) < 0: br = npc + (simm << 2)
                    likely = rt == 2
                elif rt in (1, 3):
                    if s32(R[rs]) >= 0: br = npc + (simm << 2)
                    likely = rt == 3
                elif rt == 17:
                    br = npc + (simm << 2); link = 31  # bal / bgezal
                else: raise Exception('regimm %d' % rt)
            elif op == 2: br = (npc & 0xf0000000) | ((ins & 0x3ffffff) << 2)
            elif op == 3: br = (npc & 0xf0000000) | ((ins & 0x3ffffff) << 2); link = 31
            elif op in (4, 20):
                if R[rs] == R[rt]: br = npc + (simm << 2)
                likely = op == 20
            elif op in (5, 21):
                if R[rs] != R[rt]: br = npc + (simm << 2)
                likely = op == 21
            elif op in (6, 22):
                if s32(R[rs]) <= 0: br = npc + (simm << 2)
                likely = op == 22
            elif op in (7, 23):
                if s32(R[rs]) > 0: br = npc + (simm << 2)
                likely = op == 23
            elif op in (8, 9): W(rt, R[rs] + simm)
            elif op == 10: W(rt, 1 if s32(R[rs]) < simm else 0)
            elif op == 11: W(rt, 1 if R[rs] < u32(simm) else 0)
            elif op == 12: W(rt, R[rs] & imm)
            elif op == 13: W(rt, R[rs] | imm)
            elif op == 14: W(rt, R[rs] ^ imm)
            elif op == 15: W(rt, imm << 16)
            elif op == 28:
                if fn == 2: W(rd, s32(R[rs]) * s32(R[rt]))
                elif fn == 0:
                    p = (s32(HI) << 32 | LO) + s32(R[rs]) * s32(R[rt]); LO = u32(p); HI = u32(p >> 32)
                elif fn == 1:
                    p = ((HI << 32) | LO) + R[rs] * R[rt]; LO = u32(p); HI = u32(p >> 32)
                elif fn == 4:
                    p = (s32(HI) << 32 | LO) - s32(R[rs]) * s32(R[rt]); LO = u32(p); HI = u32(p >> 32)
                elif fn == 32:  # clz
                    v = R[rs]; n = 0
                    while n < 32 and not (v & (0x80000000 >> n)): n += 1
                    W(rd, n)
                else: raise Exception('special2 %d at %08x' % (fn, pc))
            elif op == 31:
                if fn == 0:  # ext
                    W(rt, (R[rs] >> sa) & ((1 << (rd + 1)) - 1))
                elif fn == 4:  # ins
                    msb, lsb = rd, sa; size = msb - lsb + 1; mask = ((1 << size) - 1) << lsb
                    W(rt, (R[rt] & ~mask) | ((R[rs] << lsb) & mask))
                elif fn == 32:
                    if sa == 16: W(rd, s32((R[rt] & 0xff) << 24) >> 24)
                    elif sa == 24: W(rd, s32((R[rt] & 0xffff) << 16) >> 16)
                    elif sa == 2: W(rd, ((R[rt] & 0x00ff00ff) << 8) | ((R[rt] >> 8) & 0x00ff00ff))
                    else: raise Exception('bshfl %d' % sa)
                else: raise Exception('special3 %d at %08x' % (fn, pc))
            elif op == 32: W(rt, s32(self.r8(u32(R[rs] + simm)) << 24) >> 24)
            elif op == 33: W(rt, s32(self.r16(u32(R[rs] + simm)) << 16) >> 16)
            elif op == 35: W(rt, self.r32(u32(R[rs] + simm)))
            elif op == 36: W(rt, self.r8(u32(R[rs] + simm)))
            elif op == 37: W(rt, self.r16(u32(R[rs] + simm)))
            elif op == 40: self.w8(u32(R[rs] + simm), R[rt])
            elif op == 41: self.w16(u32(R[rs] + simm), R[rt])
            elif op == 43: self.w32(u32(R[rs] + simm), R[rt])
            elif op in (34, 38):  # lwl/lwr
                a = u32(R[rs] + simm); al = a & ~3; w = self.r32(al); k = a & 3
                if op == 34:
                    sh = (3 - k) * 8; W(rt, (R[rt] & ((1 << sh) - 1)) | u32(w << sh))
                else:
                    sh = k * 8; W(rt, (R[rt] & ~(0xffffffff >> sh)) | (w >> sh))
            elif op in (42, 46):  # swl/swr
                a = u32(R[rs] + simm); al = a & ~3; w = self.r32(al); k = a & 3
                if op == 42:
                    sh = (3 - k) * 8; mask = 0xffffffff >> sh
                    w = (w & ~mask) | (R[rt] >> sh)
                else:
                    sh = k * 8; mask = u32(0xffffffff << sh)
                    w = (w & ~mask) | u32(R[rt] << sh)
                self.w32(al, w)
            elif op == 16:
                if rs == 0: W(rt, 0)   # mfc0
                # mtc0 / di / ei / eret ignored
            elif op == 47: pass  # cache
            elif op == 51: pass  # pref
            else:
                raise Exception('op %d at %08x (%08x) in %s' % (op, pc, ins, fname))
            if link is not None:
                R[link] = npc + 4
            if br is not None:
                # execute delay slot
                pc, npc = npc, br
                continue
            if likely:
                # branch not taken: skip delay slot
                pc, npc = npc + 4, npc + 8
                continue
            pc, npc = npc, npc + 4
