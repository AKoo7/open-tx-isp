#!/usr/bin/env python3
"""Lift the stock T23 ADR (dynamic range) and defog chains (tx-isp-t23.ko,
md5 8237acb1...) into C.

usage: dolift_adr.py <stock tx-isp-t23.ko> > ../tx_isp_t23_adr_oem.inc
Uses the T21 translator (../../t21/audit/lift.py).  Every stock object the
chain touches becomes a private copy except tparams (the IQ bank, same
layout in this driver); hardware, sensor and event access goes through the
t23_adrlift_* glue in tx_isp_t23_adr_oem_glue.inc.  Names are kept apart
from the AE lift (LD_, oemd_, liftd_).
"""
import io, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 't21', 'audit'))
_buf = io.StringIO(); _real = sys.stdout; sys.stdout = _buf
from lift import Elf, Lifter, render_private
oem = Elf(sys.argv[1])
ext = {
 'system_irq_func_set': 'r_v0 = t23_adrlift_irq_set(r_a0, r_a1, r_a2);',
 'tisp_event_set_cb': 'r_v0 = t23_adrlift_event_set_cb(r_a0, r_a1, r_a2);',
 'private_vmalloc': 'r_v0 = (uint32_t)(uintptr_t)vmalloc(r_a0);',
 'kmem_cache_alloc': 'r_v0 = (uint32_t)(uintptr_t)kmem_cache_alloc((struct kmem_cache *)(uintptr_t)r_a0, (gfp_t)r_a1);',
 'kfree': 'kfree((void *)(uintptr_t)r_a0); r_v0 = 0;',
 'private_vfree': 'vfree((void *)(uintptr_t)r_a0); r_v0 = 0;',
 'tisp_gamma_param_array_get': 'r_v0 = (uint32_t)tisp_gamma_param_array_get((int32_t)r_a0, (int32_t *)(uintptr_t)r_a1, (int32_t *)(uintptr_t)r_a2);',
 'tisp_event_push': 'r_v0 = t23_adrlift_event_push(r_a0, r_a1);',
 'private_complete': 'r_v0 = 0;',
 'private_kmalloc': 'r_v0 = (uint32_t)(uintptr_t)t23_adrlift_kmalloc(r_a0);',
 'private_dma_cache_sync': 'private_dma_cache_sync(NULL, (void *)(uintptr_t)r_a1, r_a2, (enum dma_data_direction)r_a3); r_v0 = 0;',
 '__private_spin_lock_irqsave': '{ unsigned long f_; spin_lock_irqsave(&lift_lock, f_); *(uint32_t *)(uintptr_t)r_a1 = (uint32_t)f_; } r_v0 = 0;',
 'private_spin_unlock_irqrestore': 'spin_unlock_irqrestore(&lift_lock, (unsigned long)r_a1); r_v0 = 0;',
 'isp_printf': 'r_v0 = 0;',
 'printk': 'r_v0 = 0;',
 'dump_stack': 'r_v0 = 0;',
 'system_reg_write': 't23_adrlift_reg_write(r_a0, r_a1); r_v0 = 0;',
 'system_reg_read': 'r_v0 = t23_adrlift_reg_read(r_a0);',
 'memset': 'r_v0 = (uint32_t)(uintptr_t)memset((void *)(uintptr_t)r_a0, (int)r_a1, r_a2);',
 'memcpy': 'r_v0 = (uint32_t)(uintptr_t)memcpy((void *)(uintptr_t)r_a0, (const void *)(uintptr_t)r_a1, r_a2);',
 'div64_u64': '{ uint64_t q_ = div64_u64(((uint64_t)r_a1 << 32) | r_a0, ((uint64_t)r_a3 << 32) | r_a2); r_v0 = (uint32_t)q_; r_v1 = (uint32_t)(q_ >> 32); }',
 '__ashldi3': '{ uint64_t v_ = (((uint64_t)r_a1 << 32) | r_a0) << (r_a2 & 63); r_v0 = (uint32_t)v_; r_v1 = (uint32_t)(v_ >> 32); }',
 '__lshrdi3': '{ uint64_t v_ = (((uint64_t)r_a1 << 32) | r_a0) >> (r_a2 & 63); r_v0 = (uint32_t)v_; r_v1 = (uint32_t)(v_ >> 32); }',
}
L = Lifter(oem, {'tparams'}, ext, prefix='LD_')
L.tail_csp = True
# stock has a few file-local objects with the same name (ev_now of ADR and
# defog, ...): keep them apart
_cnt = {}
for _s in oem.syms:
    if _s['type'] == 1 and _s['name']:
        _cnt[_s['name']] = _cnt.get(_s['name'], 0) + 1
_dups = {n for n, k in _cnt.items() if k > 1}
_ne = L.namedexpr
_objs = {}   # private C name -> stock (section index, offset, size)
def _namedexpr(t, off):
    if t['name'] in _dups:
        t = dict(t, name='%s_%x' % (t['name'], t['val']))
    r = _ne(t, off)
    _objs['oem_' + re.sub(r'[^A-Za-z0-9_]', '_', t['name'])] = (t['shndx'], t['val'], t['size'])
    return r
L.namedexpr = _namedexpr
L.callbacks = set()
L.cfuncs = set()
todo = os.environ.get('ADR_FUNCS', 'tiziano_adr_init,tiziano_adr_dn_params_refresh,tisp_adr_ev_update,tiziano_adr_interrupt_static,tisp_adr_process,tisp_adr_param_array_get,tisp_adr_param_array_set,tisp_adr_wdr_en,tisp_s_adr_str_internal,tisp_g_adr_str_internal,tiziano_defog_init,tiziano_defog_dn_params_refresh,tisp_defog_ev_update,tiziano_defog_interrupt_static,tisp_defog_process,tisp_defog_param_array_get,tisp_defog_param_array_set,tisp_defog_wdr_en,tisp_s_defog_str_internal,tisp_g_defog_str_internal').split(',')
done = []; bodies = []
def drain():
    while todo:
        f = todo.pop(0)
        if f in done: continue
        bodies.append(L.lift(f)); done.append(f)
        for nfn in L.need:
            if nfn not in done and nfn not in todo: todo.append(nfn)
        L.need = []
drain()
fnames = {x['name'] for x in oem.syms if x['type'] == 2}
for fn in list(L.fnids) + sorted(L.fnaddr):
    if fn not in done and fn not in ext and fn in fnames:
        todo.append(fn)
drain()
disp = ['static uint64_t LD_dispatch(uint32_t fn, uint32_t r_a0, uint32_t r_a1, uint32_t r_a2, uint32_t r_a3, uint32_t sp_base)', '{']
for fn_ in done:
    disp.append('\tif (fn == (uint32_t)(uintptr_t)&LD_%s) return LD_%s(r_a0, r_a1, r_a2, r_a3, sp_base);' % (fn_, fn_))
# stock calls through a register holding an external function (jalr s0
# reused across calls): route those to the same glue as the direct calls
mapped = {ex_ for sz_, ex_ in L.vamap.values()}
for nm_ in sorted(ext):
    if any(('&%s ' % nm_) in m_ or ('&%s)' % nm_) in m_ for m_ in mapped):
        disp.append('\tif (fn == (uint32_t)(uintptr_t)&%s) { uint32_t r_v0 = 0, r_v1 = 0; %s return ((uint64_t)r_v1 << 32) | r_v0; }' % (nm_, ext[nm_]))
disp += ['\treturn t23_adrlift_call(fn, r_a0, r_a1, r_a2, r_a3);', '}']
bodies.append('\n'.join(disp))
# Stock objects next to each other in .data/.bss stay next to each other:
# the ADR/defog code indexes past the end of some arrays into the next stock
# object (as the stock module does), so each cluster of private objects is
# one blob with the stock layout and the stock initial bytes.
blobs = []
for shndx in sorted({v[0] for k, v in _objs.items() if k in L.private}):
    sec = oem.secname[shndx]
    if not (sec.startswith('.data') or sec.startswith('.bss')):
        continue
    ob = sorted((v[1], v[2], k) for k, v in _objs.items() if k in L.private and v[0] == shndx)
    cl = [[ob[0]]]
    for x in ob[1:]:
        q = cl[-1][-1]
        if x[0] - (q[0] + q[1]) > 256: cl.append([x])
        else: cl[-1].append(x)
    for c in cl:
        if len(c) < 2:
            continue
        start = c[0][0] & ~15
        end = max(x[0] + x[1] for x in c)
        bname = 'oem_blob%s_%x' % (sec.replace('.', '_'), start)
        if sec.startswith('.bss'):
            data = bytes(end - start)
        else:
            sh_ = oem.sh[shndx]
            data = oem.d[sh_[4] + start:sh_[4] + end]
        blobs.append((bname, data))
        base = L.secbase(shndx)
        for off_, sz_, nm_ in c:
            del L.private[nm_]
            L.vamap.pop(base + off_, None)
            bodies = [re.sub(r'&%s\b' % nm_, '(%s + 0x%x)' % (bname, off_ - start), b_) for b_ in bodies]
            L.vamap = {k_: (z_, re.sub(r'&%s\b' % nm_, '(%s + 0x%x)' % (bname, off_ - start), e_)) for k_, (z_, e_) in L.vamap.items()}
        L.vamap[base + start] = (end - start, '((uintptr_t)%s)' % bname)
xl = ['static const struct lift_va { uint32_t va, size; uintptr_t ours; } lift_vamap[] = {']
for va_ in sorted(L.vamap):
    sz_, ex_ = L.vamap[va_]
    xl.append('\t{ 0x%xu, %du, (uintptr_t)%s },' % (va_, sz_, ex_))
xl.append('};')
xl.append('''static uint32_t lift_xlate(uint32_t va)
{
	int lo = 0, hi = ARRAY_SIZE(lift_vamap) - 1, best = -1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (lift_vamap[mid].va <= va) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if (best >= 0 && va - lift_vamap[best].va <= lift_vamap[best].size)
		return (uint32_t)(lift_vamap[best].ours + (va - lift_vamap[best].va));
	if (best + 1 < (int)ARRAY_SIZE(lift_vamap) &&
	    lift_vamap[best + 1].va - va <= 64)
		return (uint32_t)(lift_vamap[best + 1].ours -
				  (lift_vamap[best + 1].va - va));
	WARN_ONCE(1, "adrlift: unmapped stock address %#x\\n", va);
	return va;
}
#define LIFT_XLATE(va) lift_xlate((uint32_t)(va))''')
# constant stock addresses: resolve at generation time (no table search)
_vm = sorted((va_, sz_, ex_) for va_, (sz_, ex_) in L.vamap.items())
def _xl(va):
    best = -1
    for k_, (v_, z_, x_) in enumerate(_vm):
        if v_ <= va: best = k_
        else: break
    if best >= 0 and va - _vm[best][0] <= _vm[best][1]:
        return '((uint32_t)((uintptr_t)%s + %du))' % (_vm[best][2], va - _vm[best][0])
    if best + 1 < len(_vm) and _vm[best + 1][0] - va <= 64:
        return '((uint32_t)((uintptr_t)%s - %du))' % (_vm[best + 1][2], _vm[best + 1][0] - va)
    return 'LIFT_XLATE(0x%08xu)' % va
_kre = re.compile(r'LIFT_XLATE_K\(0x([0-9a-f]+)u\)')
bodies = [_kre.sub(lambda m_: _xl(int(m_.group(1), 16)), b_) for b_ in bodies]
print('/* SPDX-License-Identifier: GPL-2.0 */')
print('/* Generated by audit/dolift_adr.py from stock tx-isp-t23.ko (md5 8237acb18a548d8ad8c2d3fcf1517724): do not edit. */')
print('/* Lifted: %s */' % ', '.join(done))
print('static DEFINE_SPINLOCK(lift_lock);')
print(render_private(L.private))
for bname, data in blobs:
    print('static uint8_t %s[%d] __aligned(16) = { %s };' % (bname, len(data), ', '.join('0x%02x' % b for b in data) if any(data) else '0'))
print('\n'.join('static uint64_t LD_%s(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);' % f for f in done))
print('static uint64_t LD_dispatch(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);')
print('\n'.join(xl)); print('\n\n'.join(bodies))
sys.stderr.write('UNDEF %s\nlifted %d %s\nprivate %d\n' % (sorted(L.undef), len(done), done, len(L.private)))
sys.stdout = _real
out_ = re.sub(r'([A-Za-z_][A-Za-z0-9_]*)\.(isra|constprop|part)\.(\d+)', r'\1_\2_\3', _buf.getvalue())
# keep one C frame per stock frame: inlining would stack every inlined
# callee's frame[] into one kernel stack frame (24 KiB seen with GCC 16)
out_ = re.sub(r'\boem_', 'oemd_', out_)
# the stock .rodata image is the same bytes the AE lift already emits as
# oem_rodata (both lifts copy the whole section of the same module), and it
# is const: share that copy instead of a second 6.7 KiB one
out_, n = re.subn(r'^static const uint8_t oemd_rodata\[\d+\] __aligned\(4\) = \{[^\n]*\};$',
                  '/* stock .rodata: identical to the AE lift\'s const oem_rodata */\n'
                  '#define oemd_rodata oem_rodata', out_, flags=re.M)
assert n == 1
out_ = re.sub(r'\blift_(vamap|xlate|va|lock)\b', r'liftd_\1', out_)
out_ = out_.replace('LIFT_XLATE', 'LIFTD_XLATE')
out_ = re.sub(r'^static uint64_t LD_', 'static noinline uint64_t LD_', out_, flags=re.M)
print(out_, end='')
