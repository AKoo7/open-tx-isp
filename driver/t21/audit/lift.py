#!/usr/bin/env python3
"""Lift MIPS32 functions from a relocatable .ko into C (one register file per
function, explicit delay slots, relocations resolved to C symbols)."""
import os, struct, sys, re

def s16(x): return x - 0x10000 if x & 0x8000 else x

class Elf:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        (shoff,) = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
        self.sh = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
        def nm(tab, off):
            o = self.sh[tab][4] + off
            return d[o:d.index(b'\0', o)].decode()
        self.secname = [nm(shstrndx, s[0]) for s in self.sh]
        symsec = [i for i, s in enumerate(self.sh) if s[1] == 2][0]
        st = self.sh[symsec]
        self.syms = []
        for k in range(st[5] // 16):
            n, val, size, info, oth, shndx = struct.unpack_from('<IIIBBH', d, st[4] + k * 16)
            self.syms.append(dict(name=nm(st[6], n) if n else '', val=val, size=size,
                                  type=info & 15, bind=info >> 4, shndx=shndx))
        self.text = self.secname.index('.text')
        self.relocs = {}  # text offset -> (type, sym)
        for i, s in enumerate(self.sh):
            if s[1] == 9 and s[7] == self.text:
                for k in range(s[5] // 8):
                    off, info = struct.unpack_from('<II', d, s[4] + k * 8)
                    self.relocs[off] = (info & 0xff, self.syms[info >> 8])
    def rodata_text_targets(self):
        if hasattr(self, '_rtt'): return self._rtt
        out = set()
        for i, sh in enumerate(self.sh):
            if sh[1] == 9 and self.secname[sh[7]].startswith('.rodata'):
                tgt = sh[7]
                for k in range(sh[5] // 8):
                    off, info = struct.unpack_from('<II', self.d, sh[4] + k * 8)
                    sym = self.syms[info >> 8]
                    if (info & 0xff) == 2 and sym['shndx'] == self.text:
                        add = struct.unpack_from('<I', self.d, self.sh[tgt][4] + off)[0]
                        out.add(add + (sym['val'] if sym['type'] != 3 else 0))
        self._rtt = out
        return out
    def secdata(self, idx):
        s = self.sh[idx]
        return self.d[s[4]:s[4] + s[5]]
    def func(self, name):
        for s in self.syms:
            if s['name'] == name and s['type'] == 2 and s['shndx'] == self.text:
                return s['val'], s['size']
        raise KeyError(name)
    def word(self, off):
        base = self.sh[self.text][4]
        return struct.unpack_from('<I', self.d, base + off)[0]
    def sym_at(self, shndx, off):
        best = None
        for s in self.syms:
            if s['shndx'] == shndx and s['name'] and s['type'] in (1, 0) and s['val'] <= off < s['val'] + max(s['size'], 1):
                best = s
        return best

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 's8', 'ra']

class Lifter:
    def __init__(self, elf, ours, extern_c, prefix='L_'):
        self.e = elf
        self.ours = ours          # set of symbol names that exist in our C
        self.extern_c = extern_c  # name -> C call template
        self.prefix = prefix
        self.private = {}         # private symbol name -> (bytes, size, is_const)
        self.lifted = set()
        self.need = []
        self.fnids = {}
        self.alias = {}
        self.undef = set()
        self.shared = {}
        self.fnaddr = set()
        self.cfuncs = set()
        self.vamap = {}
        self.extidx = {}
        self.callbacks = set()
    def symexpr(self, sym, addend):
        """C expression (uint32) for symbol address + addend."""
        e = self.e
        if sym['type'] == 3 or not sym['name']:   # section symbol
            sec = e.secname[sym['shndx']]
            if sec == '.rodata' or sec.startswith('.rodata'):
                key = 'oem_rodata' if sec == '.rodata' else 'oem_' + sec.strip('.').replace('.', '_')
                self.private[key] = (e.secdata(sym['shndx']), True)
                return '((uint32_t)(uintptr_t)%s + 0x%x)' % (key, addend & 0xffffffff)
            if sym['shndx'] == e.text:
                fs = [x for x in e.syms if x['shndx'] == e.text and x['type'] == 2 and x['val'] == addend]
                if not fs:
                    raise Exception('no function at .text %#x' % addend)
                return self.symexpr(fs[0], 0)
            t = e.sym_at(sym['shndx'], addend)
            if not t:
                raise Exception('no symbol in %s at %#x' % (sec, addend))
            return self.namedexpr(t, addend - t['val'])
        if sym['shndx'] == e.text or sym['shndx'] == 0:
            n_ = sym['name']
            self.fnaddr.add(n_)
            if sym['shndx'] == 0 or n_ in self.extern_c or n_ in self.callbacks:
                return '((uint32_t)(uintptr_t)&%s + %d)' % (n_, addend)
            self.need.append(n_)
            return '((uint32_t)(uintptr_t)&%s%s + %d)' % (self.prefix, n_, addend)
        return self.namedexpr(sym, addend)
    def secbase(self, shndx):
        return ((shndx + 1) & 0x7f) << 24
    def resolve(self, sym, addend):
        """Return the OEM virtual address of sym+addend and register the
        containing object in the translation table."""
        e = self.e
        if sym['shndx'] == 0:
            name = sym['name']
            idx = self.extidx.setdefault(name, len(self.extidx))
            va = 0xE0000000 + idx * 16
            self.vamap[va] = (1, '((uintptr_t)&%s)' % name)
            return va + addend
        if sym['type'] == 3 or not sym['name']:
            sec = e.secname[sym['shndx']]
            base = self.secbase(sym['shndx'])
            if sec.startswith('.rodata'):
                key = 'oem_rodata' if sec == '.rodata' else 'oem_' + sec.strip('.').replace('.', '_')
                self.private[key] = (e.secdata(sym['shndx']), True)
                self.vamap[base] = (len(e.secdata(sym['shndx'])), '((uintptr_t)%s)' % key)
                return base + addend
            if sym['shndx'] == e.text:
                fs = [x for x in e.syms if x['shndx'] == e.text and x['type'] == 2 and x['val'] <= addend < x['val'] + max(x['size'], 1)]
                if not fs:
                    raise Exception('no function at .text %#x' % addend)
                return self.resolve(fs[0], addend - fs[0]['val'])
            t = e.sym_at(sym['shndx'], addend)
            if not t:
                raise Exception('no symbol in %s at %#x' % (sec, addend))
            return self.resolve(t, addend - t['val'])
        va0 = self.secbase(sym['shndx']) + sym['val']
        if va0 not in self.vamap:
            expr = self.symexpr(sym, 0)
            self.vamap[va0] = (max(sym['size'], 1), '((uintptr_t)(%s))' % expr)
        return va0 + addend
    def namedexpr(self, t, off):
        name = self.alias.get('%s@%x' % (t['name'], t['val']), self.alias.get(t['name'], t['name']))
        if name in self.ours or name in self.alias.values():
            cname = name
            self.shared[name] = t['size']
        else:
            cname = 'oem_' + re.sub(r'[^A-Za-z0-9_]', '_', name)
            sec = self.e.secname[t['shndx']]
            if sec.startswith('.bss') or sec.startswith('.sbss'):
                data = bytes(t['size'])
            else:
                s = self.e.sh[t['shndx']]
                data = self.e.d[s[4] + t['val']:s[4] + t['val'] + t['size']]
            self.private[cname] = (data, sec.startswith('.rodata'))
        return '((uint32_t)(uintptr_t)&%s + %d)' % (cname, off)
    def lift(self, fname):
        e = self.e
        start, size = e.func(fname)
        n = size // 4
        ins = [e.word(start + 4 * i) for i in range(n)]
        # frame size
        frame = 0
        for w in ins:
            if w >> 26 == 9 and (w >> 21) & 31 == 29 and (w >> 16) & 31 == 29 and s16(w & 0xffff) < 0:
                frame = -s16(w & 0xffff); break
        # HI16/LO16 addends
        hi_add = {}
        rel = {i: e.relocs.get(start + 4 * i) for i in range(n)}
        for i in range(n):
            r = rel[i]
            if r and r[0] == 5:
                # the paired LO16 usually follows; gcc may also schedule it
                # before the HI16 across a branch (func_zone_ct_weight)
                # prefer a LO16 based on the lui's own register: with
                # section-relative relocs several HI16s of one section can
                # be interleaved (jz_isp_lsc_ct)
                rt_ = (ins[i] >> 16) & 31
                order_ = list(range(i + 1, n)) + list(range(i - 1, -1, -1))
                order_ = [j for j in order_ if (ins[j] >> 21) & 31 == rt_] + order_
                for j in order_:
                    rj = rel[j]
                    if rj and rj[0] == 6 and rj[1] is r[1]:
                        hi_add[i] = ((ins[i] & 0xffff) << 16) + s16(ins[j] & 0xffff)
                        break
                else:
                    raise Exception('unpaired HI16 at %x' % (start + 4 * i))
        lo_add = {}
        def _writes(w):
            op = w >> 26
            if op == 0:
                fn = w & 63
                if fn in (8, 24, 25, 26, 27, 17, 19, 13, 15, 52): return None
                return (w >> 11) & 31
            if op == 28:
                return (w >> 11) & 31 if (w & 63) in (2, 32) else None
            if op in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38):  # 15 = lui
                return (w >> 16) & 31
            if op == 3: return 31
            return None
        lo_lui = {}
        for i in range(n):
            r = rel[i]
            if r and r[0] == 6:
                base = (ins[i] >> 21) & 31
                found = None
                for j in range(i - 1, -1, -1):
                    wj = ins[j]
                    if _writes(wj) == base:
                        rj = rel[j]
                        if rj and rj[0] == 5 and (wj >> 26) == 15:
                            found = j
                            break
                        if (wj >> 26) == 15:
                            break
                        continue
                if found is None:
                    for j in range(i - 1, -1, -1):
                        rj = rel[j]
                        if rj and rj[0] == 5 and rj[1] is r[1]:
                            found = j; break
                lo_lui[i] = found
                if found is not None:
                    lo_add[i] = ((ins[found] & 0xffff) << 16) + s16(ins[i] & 0xffff)
                else:
                    lo_add[i] = s16(ins[i] & 0xffff)
        # branch targets
        targets = set()
        for i, w in enumerate(ins):
            op = w >> 26; rt = (w >> 16) & 31
            if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3)):
                targets.add(i + 1 + s16(w & 0xffff))
            elif op == 2:
                t = ((w & 0x3ffffff) << 2) - start
                targets.add(t // 4)
        for t_ in e.rodata_text_targets():
            if start <= t_ < start + size:
                targets.add((t_ - start) // 4)
        for i, w in enumerate(ins):
            op = w >> 26; rt = (w >> 16) & 31
            if (op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3))) and (i + 1) in targets:
                targets.add(i + 2)
        out = []
        P = lambda s: out.append('\t' + s)
        def reg(r):
            if r == 0: return '0u'
            if r == 29: return 'sp_base'
            return 'r_' + R[r]
        def addr(rs, imm):
            if rs == 29:
                if imm >= frame:
                    return '(csp + %d)' % (imm - frame)
                return '(sp_base + %d)' % imm
            return '(%s + (uint32_t)%d)' % (reg(rs), imm)
        def lo_full(i, rs):
            r = rel[i]
            va = self.resolve(r[1], lo_add[i]) & 0xffffffff
            return 'LIFT_XLATE(%s + (uint32_t)%d)' % (reg(rs), s16(va & 0xffff))
        def immexpr(i, default):
            r = rel[i]
            if r and r[0] == 6:
                full = self.symexpr(r[1], lo_add[i])
                return '((int32_t)(int16_t)(%s & 0xffff))' % full
            return str(default)
        def writes_reg(w):
            op = w >> 26
            if op == 0:
                fn = w & 63
                if fn in (8, 24, 25, 26, 27, 17, 19, 13, 15, 52): return None
                return (w >> 11) & 31
            if op == 28:
                return (w >> 11) & 31 if (w & 63) in (2, 32) else None
            if op == 31:
                return (w >> 16) & 31 if (w & 63) in (0, 4) else (w >> 11) & 31
            if op in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38):  # 15 = lui
                return (w >> 16) & 31
            if op == 3: return 31
            return None
        def last_set_sym(i, rr):
            for j in range(i - 1, max(-1, i - 40), -1):
                if (j + 1) in targets:
                    return None
                w = ins[j]; r = rel[j]
                rt = (w >> 16) & 31; op = w >> 26
                if r and r[0] == 6 and op == 9 and rt == rr:
                    s_ = r[1]
                    return s_ if (s_['type'] == 2 or s_['shndx'] == 0) else None
                if r and r[0] == 5 and op == 15 and rt == rr:
                    s_ = r[1]
                    return s_ if (s_['type'] == 2 or s_['shndx'] == 0) else None
                if writes_reg(w) == rr:
                    return None
                if j in targets:
                    return None
            return None
        def emit(i, in_delay=False):
            w = ins[i]
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
            sa = (w >> 6) & 31; fn = w & 63; imm = w & 0xffff; simm = s16(imm)
            def W(r, expr):
                if r == 0: return
                P('%s = (uint32_t)(%s);' % (reg(r), expr))
            S = lambda r: '(int32_t)' + reg(r)
            if w == 0: return
            if op == 0:
                if fn == 0: W(rd, '%s << %d' % (reg(rt), sa))
                elif fn == 2: W(rd, '%s >> %d' % (reg(rt), sa))
                elif fn == 3: W(rd, '(int32_t)%s >> %d' % (reg(rt), sa))
                elif fn == 4: W(rd, '%s << (%s & 31)' % (reg(rt), reg(rs)))
                elif fn == 6: W(rd, '%s >> (%s & 31)' % (reg(rt), reg(rs)))
                elif fn == 7: W(rd, '(int32_t)%s >> (%s & 31)' % (reg(rt), reg(rs)))
                elif fn == 10: P('if (%s == 0) %s = %s;' % (reg(rt), reg(rd), reg(rs))) if rd else None
                elif fn == 11: P('if (%s != 0) %s = %s;' % (reg(rt), reg(rd), reg(rs))) if rd else None
                elif fn == 16: W(rd, 'r_hi')
                elif fn == 18: W(rd, 'r_lo')
                elif fn == 17: P('r_hi = %s;' % reg(rs))
                elif fn == 19: P('r_lo = %s;' % reg(rs))
                elif fn == 24: P('{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; r_lo = (uint32_t)p; r_hi = (uint32_t)((uint64_t)p >> 32); }' % (reg(rs), reg(rt)))
                elif fn == 25: P('{ uint64_t p = (uint64_t)%s * %s; r_lo = (uint32_t)p; r_hi = (uint32_t)(p >> 32); }' % (reg(rs), reg(rt)))
                elif fn == 26: P('if (%s) { r_lo = (uint32_t)((int32_t)%s / (int32_t)%s); r_hi = (uint32_t)((int32_t)%s %% (int32_t)%s); }' % (reg(rt), reg(rs), reg(rt), reg(rs), reg(rt)))
                elif fn == 27: P('if (%s) { r_lo = %s / %s; r_hi = %s %% %s; }' % (reg(rt), reg(rs), reg(rt), reg(rs), reg(rt)))
                elif fn in (32, 33): W(rd, '%s + %s' % (reg(rs), reg(rt)))
                elif fn in (34, 35): W(rd, '%s - %s' % (reg(rs), reg(rt)))
                elif fn == 36: W(rd, '%s & %s' % (reg(rs), reg(rt)))
                elif fn == 37: W(rd, '%s | %s' % (reg(rs), reg(rt)))
                elif fn == 38: W(rd, '%s ^ %s' % (reg(rs), reg(rt)))
                elif fn == 39: W(rd, '~(%s | %s)' % (reg(rs), reg(rt)))
                elif fn == 42: W(rd, '(int32_t)%s < (int32_t)%s' % (reg(rs), reg(rt)))
                elif fn == 43: W(rd, '%s < %s' % (reg(rs), reg(rt)))
                elif fn == 13 or fn == 52: P('/* break/teq */')
                elif fn == 15: pass
                else: raise Exception('fn %d' % fn)
            elif op == 15:
                r = rel[i]
                if r and r[0] == 5:
                    va = self.resolve(r[1], hi_add[i]) & 0xffffffff
                    W(rt, '0x%xu' % (((va + 0x8000) >> 16) << 16 & 0xffffffff))
                else:
                    W(rt, '0x%xu' % (imm << 16))
            elif op in (8, 9):
                if rs == 29 and rt == 29: return
                if rs == 29:
                    W(rt, addr(29, simm)); return
                if rel[i] and rel[i][0] == 6:
                    W(rt, lo_full(i, rs))
                else:
                    W(rt, '%s + (uint32_t)%s' % (reg(rs), simm))
            elif op == 10: W(rt, '(int32_t)%s < %d' % (reg(rs), simm))
            elif op == 11: W(rt, '%s < (uint32_t)%d' % (reg(rs), simm))
            elif op == 12: W(rt, '%s & 0x%x' % (reg(rs), imm))
            elif op == 13: W(rt, '%s | 0x%x' % (reg(rs), imm))
            elif op == 14: W(rt, '%s ^ 0x%x' % (reg(rs), imm))
            elif op in (32, 33, 35, 36, 37, 40, 41, 43):
                if rs == 29 and not (rel[i] and rel[i][0] == 6):
                    a = addr(29, simm)
                else:
                    a = lo_full(i, rs) if (rel[i] and rel[i][0] == 6) else '(%s + (uint32_t)%s)' % (reg(rs), simm)
                if op == 32: W(rt, '(int32_t)*(int8_t *)(uintptr_t)%s' % a)
                elif op == 33: W(rt, '(int32_t)*(int16_t *)(uintptr_t)%s' % a)
                elif op == 35: W(rt, '*(uint32_t *)(uintptr_t)%s' % a)
                elif op == 36: W(rt, '*(uint8_t *)(uintptr_t)%s' % a)
                elif op == 37: W(rt, '*(uint16_t *)(uintptr_t)%s' % a)
                elif op == 40: P('*(uint8_t *)(uintptr_t)%s = (uint8_t)%s;' % (a, reg(rt)))
                elif op == 41: P('*(uint16_t *)(uintptr_t)%s = (uint16_t)%s;' % (a, reg(rt)))
                elif op == 43: P('*(uint32_t *)(uintptr_t)%s = %s;' % (a, reg(rt)))
            elif op == 28:
                if fn == 2: W(rd, '%s * %s' % (reg(rs), reg(rt)))
                elif fn == 0: P('{ int64_t p = (int64_t)(((uint64_t)r_hi << 32) | r_lo) + (int64_t)(int32_t)%s * (int32_t)%s; r_lo = (uint32_t)p; r_hi = (uint32_t)((uint64_t)p >> 32); }' % (reg(rs), reg(rt)))
                elif fn == 1: P('{ uint64_t p = (((uint64_t)r_hi << 32) | r_lo) + (uint64_t)%s * %s; r_lo = (uint32_t)p; r_hi = (uint32_t)(p >> 32); }' % (reg(rs), reg(rt)))
                elif fn == 4: P('{ int64_t p = (int64_t)(((uint64_t)r_hi << 32) | r_lo) - (int64_t)(int32_t)%s * (int32_t)%s; r_lo = (uint32_t)p; r_hi = (uint32_t)((uint64_t)p >> 32); }' % (reg(rs), reg(rt)))
                elif fn == 32: W(rd, '%s ? __builtin_clz(%s) : 32' % (reg(rs), reg(rs)))
                else: raise Exception('sp2 %d' % fn)
            elif op == 31:
                if fn == 0: W(rt, '(%s >> %d) & 0x%x' % (reg(rs), sa, (1 << (rd + 1)) - 1))
                elif fn == 4:
                    size = rd - sa + 1; mask = ((1 << size) - 1) << sa
                    W(rt, '(%s & 0x%xu) | ((%s << %d) & 0x%xu)' % (reg(rt), ~mask & 0xffffffff, reg(rs), sa, mask))
                elif fn == 32 and sa == 16: W(rd, '(int32_t)(int8_t)%s' % reg(rt))
                elif fn == 32 and sa == 24: W(rd, '(int32_t)(int16_t)%s' % reg(rt))
                else: raise Exception('sp3')
            elif op in (47, 51): pass
            else:
                raise Exception('op %d at %x' % (op, start + 4 * i))
        def cond(w, i):
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31
            if op in (4, 20): return '%s == %s' % (reg(rs), reg(rt))
            if op in (5, 21): return '%s != %s' % (reg(rs), reg(rt))
            if op in (6, 22): return '(int32_t)%s <= 0' % reg(rs)
            if op in (7, 23): return '(int32_t)%s > 0' % reg(rs)
            if op == 1:
                return ('(int32_t)%s < 0' if rt in (0, 2) else '(int32_t)%s >= 0') % reg(rs)
        def call(i):
            w = ins[i]
            r = rel[i]
            if w >> 26 == 3:
                sym = r[1]
            else:
                sym = last_set_sym(i, (w >> 21) & 31)
            if not sym:
                self.dispatch_used = True
                P('{ uint64_t q_ = %sdispatch(%s, r_a0, r_a1, r_a2, r_a3, sp_base); r_v0 = (uint32_t)q_; r_v1 = (uint32_t)(q_ >> 32); }' % (self.prefix, reg((w >> 21) & 31)))
                return
            name = sym['name']
            if sym['shndx'] == 0 and name not in self.extern_c:
                self.undef.add(name)
                P('r_v0 = LIFT_EXTERN_%s(r_a0, r_a1, r_a2, r_a3);' % name)
                return
            if name in self.extern_c:
                P(self.extern_c[name])
            else:
                self.need.append(name)
                P('{ uint64_t q_ = %s%s(r_a0, r_a1, r_a2, r_a3, sp_base); r_v0 = (uint32_t)q_; r_v1 = (uint32_t)(q_ >> 32); }' % (self.prefix, name))
        heap = fname in set(x for x in os.environ.get('LIFT_HEAP_FRAMES', '').split(',') if x)
        if heap and frame > 256:
            # per-call kzalloc'd frame (lift_frame_alloc, see heapframes.py):
            # reentrant and kept out of .bss; only for functions that already
            # run in sleepable (ioctl) context
            sig = '(uint32_t r_a0, uint32_t r_a1, uint32_t r_a2, uint32_t r_a3, uint32_t csp'
            f_ = self.prefix + fname
            out.append('static uint64_t %s__body%s, uint32_t *frame);' % (f_, sig))
            out.append('static uint64_t %s%s)' % (f_, sig))
            out.append('{')
            out.append('\t/* stock frame on the heap (reentrant, not in .bss); see audit/heapframes.py */')
            out.append('\tuint32_t *frame = lift_frame_alloc(%d);' % (max(frame, 8) // 4))
            out.append('\tuint64_t q_;')
            out.append('')
            out.append('\tif (!frame)')
            out.append('\t\treturn (uint32_t)-ENOMEM;')
            out.append('\tq_ = %s__body(r_a0, r_a1, r_a2, r_a3, csp, frame);' % f_)
            out.append('\tlift_frame_free(frame);')
            out.append('\treturn q_;')
            out.append('}')
            out.append('')
            out.append('static uint64_t %s__body%s, uint32_t *frame)' % (f_, sig))
            out.append('{')
        else:
            out.append('static uint64_t %s%s(uint32_t r_a0, uint32_t r_a1, uint32_t r_a2, uint32_t r_a3, uint32_t csp)' % (self.prefix, fname))
            out.append('{')
        if heap and frame > 256:
            pass
        elif frame > 256:
            P('/* large stock frame: static (single caller context, not reentrant) */')
            P('static uint32_t frame[%d] __aligned(8);' % (max(frame, 8) // 4))
        else:
            P('uint32_t frame[%d] __aligned(8);' % (max(frame, 8) // 4))
        P('uint32_t sp_base = (uint32_t)(uintptr_t)frame;')
        used = ['r_' + x for x in R if x not in ('zero', 'a0', 'a1', 'a2', 'a3', 'sp')]
        P('uint32_t %s, r_hi = 0, r_lo = 0;' % ', '.join(u + ' = 0' for u in used))
        P('(void)csp; (void)r_at; (void)r_k0; (void)r_k1; (void)r_gp; (void)r_ra;')
        i = 0
        while i < n:
            if i in targets:
                out.append('L_%x:' % (start + 4 * i))
            w = ins[i]
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; fn = w & 63
            isbr = op in (4, 5, 6, 7) or (op == 1 and rt in (0, 1))
            islikely = op in (20, 21, 22, 23) or (op == 1 and rt in (2, 3))
            if isbr:
                tgt = start + 4 * (i + 1 + s16(w & 0xffff))
                if op == 4 and rs == 0 and rt == 0:
                    emit(i + 1); P('goto L_%x;' % tgt)
                else:
                    P('{ int c_ = (%s);' % cond(w, i)); emit(i + 1); P('if (c_) goto L_%x; }' % tgt)
                if i + 1 in targets:
                    P('goto L_%x;' % (start + 4 * (i + 2)))
                    out.append('L_%x:' % (start + 4 * (i + 1)))
                    emit(i + 1)
                i += 2; continue
            if islikely:
                tgt = start + 4 * (i + 1 + s16(w & 0xffff))
                P('if (%s) {' % cond(w, i)); emit(i + 1); P('goto L_%x; }' % tgt)
                if i + 1 in targets:
                    P('goto L_%x;' % (start + 4 * (i + 2)))
                    out.append('L_%x:' % (start + 4 * (i + 1)))
                    emit(i + 1)
                i += 2; continue
            if op == 2:
                tgt = (w & 0x3ffffff) << 2
                emit(i + 1); P('goto L_%x;' % tgt); i += 2; continue
            if op == 0 and fn == 8:  # jr
                emit(i + 1)
                if rs == 25:
                    call(i); P('return ((uint64_t)r_v1 << 32) | r_v0;'); i += 2; continue
                if rs != 31:
                    cands = sorted(t for t in e.rodata_text_targets() if start <= t < start + size)
                    if not cands: raise Exception('jr reg in %s at %x' % (fname, start + 4 * i))
                    P('switch (%s) {' % reg(rs))
                    for t_ in cands:
                        P('case 0x%xu: goto L_%x;' % (t_, t_))
                    P('default: WARN_ON_ONCE(1); return 0; }')
                    i += 2; continue
                P('return ((uint64_t)r_v1 << 32) | r_v0;'); i += 2; continue
            if (op == 0 and fn == 9) or op == 3:
                emit(i + 1); call(i); i += 2; continue
            emit(i)
            i += 1
        out.append('\treturn ((uint64_t)r_v1 << 32) | r_v0;')
        out.append('}')
        self.lifted.add(fname)
        return '\n'.join(out)

def render_private(private):
    out = []
    for name, (data, const) in sorted(private.items()):
        body = ', '.join('0x%02x' % b for b in data) if any(data) else '0'
        out.append('static %suint8_t %s[%d] __aligned(4) = { %s };' % ('const ' if const else '', name, max(len(data), 1), body))
    return '\n'.join(out)
