#!/usr/bin/env python3
"""Find .bss/.data accesses in the recovered T23 C that hit the wrong object.

usage: bss_scan.py <stock tx-isp-t23.ko> <objdump> <readelf> <core.c> [more .c/.inc]

Stock tx-isp-t23.ko addresses its statics section-relative (lui %hi(.bss+X) /
lw %lo(.bss+X)).  The decompiler often kept only the low half or resolved the
pair to whatever symbol sits at the section start (ivdc_threshold_line =
.bss+0, sclk_name = .data+0) or to tparams.  For every stock function this
resolves each HI16/LO16 pair to the stock symbol it really hits and then
checks the same-named C function:

  BASE   &SYM +/- N whose implied stock address (stock SYM + N) is not hit by
         the stock function, but an address with the same low 16 bits is
         (-> the correct stock symbol is printed)
  REG    REG = &PLACEHOLDER; ... *(T *)((char *)REG + N)
  ARG    &PLACEHOLDER passed as a call argument (memcpy/memset/...)

#if 0 blocks are skipped.  Output: one line per finding.
"""
import bisect, collections, re, subprocess, sys

KO, OD, RE = sys.argv[1:4]
CFILES = sys.argv[4:]
PH = ('ivdc_threshold_line', 'sclk_name', 'tevent_info', 'tiziano_clm_s_t_lut',
      'tiziano_gib_deir_r_h')


def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout.splitlines()


secs = {}
for l in run(RE, '-SW', KO):
    m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)', l)
    if m:
        secs[int(m.group(1))] = m.group(2)
bysec = collections.defaultdict(list)
sym = {}
for l in run(RE, '-sW', KO):
    p = l.split()
    if len(p) >= 8 and p[3] in ('OBJECT', 'NOTYPE') and p[6].isdigit():
        s = secs[int(p[6])]
        if s in ('.bss', '.data', '.rodata'):
            e = (int(p[1], 16), int(p[2], 0), p[7])
            bysec[s].append(e)
            sym[p[7]] = (s,) + e[:2]
for s in bysec:
    bysec[s].sort()


def name_at(sec, off):
    L = bysec.get(sec, [])
    i = bisect.bisect_right([x[0] for x in L], off) - 1
    if i < 0:
        return '%s+%#x' % (sec, off)
    a, sz, n = L[i]
    if off - a >= max(sz, 1):
        return '%s+%#x' % (sec, off)
    return n if off == a else '%s+%#x' % (n, off - a)


# stock: function -> [(kind, section, offset, name)]
refs = collections.defaultdict(list)
func = None
hi = {}
pend = None
for l in run(OD, '-dr', '--no-show-raw-insn', KO):
    m = re.match(r'^[0-9a-f]+ <(\S+)>:', l)
    if m:
        func, hi = m.group(1), {}
        continue
    m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$', l)
    if m and 'R_MIPS' not in l:
        pend = (m.group(2), m.group(3))
        continue
    m = re.match(r'^\s+[0-9a-f]+: (R_MIPS_\S+)\s+(\S+)', l)
    if not m or pend is None or m.group(2) not in ('.bss', '.data', '.rodata'):
        continue
    rt, sec = m.groups()
    op, args = pend
    if rt == 'R_MIPS_HI16' and op == 'lui':
        r, v = args.split(',')
        hi[r] = (sec, int(v, 0))
    elif rt == 'R_MIPS_LO16':
        mm = re.match(r'(\w+),(-?\d+)\((\w+)\)', args)
        if mm:
            imm, base = int(mm.group(2)), mm.group(3)
        else:
            a = args.split(',')
            if len(a) < 3:
                continue
            base, imm = a[1], int(a[2], 0)
        h = hi.get(base)
        if h and h[0] == sec:
            off = (h[1] << 16) + imm
            kind = 'W' if op in ('sw', 'sh', 'sb') else ('A' if op == 'addiu' else 'R')
            refs[func].append((kind, sec, off, name_at(sec, off)))


def live_lines(path):
    out, stack = [], []
    for l in open(path).read().split('\n'):
        s = l.strip()
        if re.match(r'#\s*if', s):
            stack.append(bool(re.match(r'#\s*if\s+0\b', s)))
            out.append('')
        elif re.match(r'#\s*(else|elif)', s) and stack:
            stack[-1] = False
            out.append('')
        elif re.match(r'#\s*endif', s) and stack:
            stack.pop()
            out.append('')
        else:
            out.append('' if any(stack) else l)
    return out


fre = re.compile(r'^[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;]*\)\s*$')
pat = re.compile(r'&\s*(\w+)\s*\)?\s*(?:([+-])\s*(0x[0-9a-fA-F]+|\d+|-\s*\d+))?')
asg = re.compile(r'^\s*(\w+)\s*=\s*\([^)]*\)\s*&(\w+)\s*;')
for cf in CFILES:
    L = live_lines(cf)
    fn = None
    regs = {}
    for i, line in enumerate(L):
        m = fre.match(line)
        if m and i + 1 < len(L) and L[i + 1].startswith('{'):
            fn, regs = m.group(1), {}
            continue
        if line.startswith('}'):
            fn = None
            continue
        if not fn:
            continue
        where = '%s:%d %s' % (cf.split('/')[-1], i + 1, fn)
        R = refs.get(fn, [])
        for m in pat.finditer(line):
            n = m.group(1)
            if n not in sym:
                continue
            s, a, sz = sym[n]
            add = 0
            if m.group(2):
                add = int(m.group(3).replace(' ', ''), 0) * (-1 if m.group(2) == '-' else 1)
            imp = a + add
            hits = [r for r in R if r[1] == s]
            if any(r[2] == imp for r in hits):
                continue
            if 0 <= add < max(sz, 1) and any(a <= r[2] < a + sz for r in hits):
                continue
            cand = sorted({r[3] for r in hits if (r[2] - imp) % 0x10000 == 0})
            if cand:
                kinds = ''.join(sorted({r[0] for r in hits if (r[2] - imp) % 0x10000 == 0}))
                print('BASE %s &%s%+d -> %s %s' % (where, n, add, ','.join(cand), kinds))
        for r, b in list(regs.items()):
            for mm in re.finditer(r'\(char \*\)\s*%s\s*\+\s*(-?\d+)\)' % r, line):
                print('REG  %s %s=&%s %+d | %s' % (where, r, b, int(mm.group(1)), line.strip()[:100]))
        if re.search(r'[(,]\s*(\(\w+\s*\*?\)\s*)*&(%s)\s*[,)]' % '|'.join(PH), line) \
                and not asg.match(line):
            print('ARG  %s | %s' % (where, line.strip()[:110]))
        m = asg.match(line)
        if m:
            if m.group(2) in PH or m.group(2) == 'tparams':
                regs[m.group(1)] = m.group(2)
            else:
                regs.pop(m.group(1), None)
        else:
            m2 = re.match(r'^\s*(\w+)\s*=', line)
            if m2:
                regs.pop(m2.group(1), None)
