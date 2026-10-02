import sys; sys.path.insert(0,'.')
import io as _io, re as _re2
_buf=_io.StringIO(); _real=sys.stdout; sys.stdout=_buf  # SANITIZE
from lift import Elf, Lifter, render_private
oem=Elf('/mnt/NVMe/git/scratch-clones/expo/tmp/oem-t21.ko')
ours=Elf(sys.argv[1])
ournames={s['name'] for s in ours.syms if s['name'] and s['shndx'] not in (0,) and s['type'] in (1,)}
ext={
 'system_irq_func_set':'r_v0 = (uint32_t)system_irq_func_set(r_a0, (t21_irq_callback_t)(uintptr_t)r_a1);',
 'tisp_event_set_cb':'r_v0 = (uint32_t)tisp_event_set_cb((int32_t)r_a0, (int32_t)r_a1);',
 'private_spin_lock_init':'r_v0 = 0;',
 'isp_printf':'r_v0 = 0;',
 'printk':'r_v0 = 0;',
 'dump_stack':'r_v0 = 0;',
 'arch_local_irq_save':'r_v0 = (uint32_t)arch_local_irq_save();',
 'arch_local_irq_restore':'arch_local_irq_restore(r_a0); r_v0 = 0;',
 '__private_spin_lock_irqsave':'{ unsigned long f_; spin_lock_irqsave(&lift_lock, f_); *(uint32_t *)(uintptr_t)r_a1 = (uint32_t)f_; } r_v0 = 0;',
 'private_spin_unlock_irqrestore':'spin_unlock_irqrestore(&lift_lock, (unsigned long)r_a1); r_v0 = 0;',
 'div64_u64':'{ uint64_t q_ = div64_u64(((uint64_t)r_a1 << 32) | r_a0, ((uint64_t)r_a3 << 32) | r_a2); r_v0 = (uint32_t)q_; r_v1 = (uint32_t)(q_ >> 32); }',
 'system_reg_write':'system_reg_write(r_a0, r_a1); r_v0 = 0;',
 'system_reg_read':'r_v0 = (uint32_t)system_reg_read(r_a0);',
 'memset':'r_v0 = (uint32_t)(uintptr_t)memset((void *)(uintptr_t)r_a0, (int)r_a1, r_a2);',
 'memcpy':'r_v0 = (uint32_t)(uintptr_t)memcpy((void *)(uintptr_t)r_a0, (const void *)(uintptr_t)r_a1, r_a2);',
 'tisp_event_push':'r_v0 = (uint32_t)tisp_event_push((void *)(uintptr_t)r_a0);',
 'dma_cache_sync':'dma_cache_sync(NULL, (void *)(uintptr_t)r_a1, r_a2, (enum dma_data_direction)r_a3); r_v0 = 0;',
 '__ashldi3':'{ uint64_t v_ = (((uint64_t)r_a1 << 32) | r_a0) << (r_a2 & 63); r_v0 = (uint32_t)v_; r_v1 = (uint32_t)(v_ >> 32); }',
 '__lshrdi3':'{ uint64_t v_ = (((uint64_t)r_a1 << 32) | r_a0) >> (r_a2 & 63); r_v0 = (uint32_t)v_; r_v1 = (uint32_t)(v_ >> 32); }',
}
import os as _os
for _n in [x for x in _os.environ.get('CUSE','').split(',') if x]:
    _c=_n.replace('.','_')
    ext[_n]='r_v0 = (uint32_t)((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))LIFT_FNADDR(%s))(r_a0, r_a1, r_a2, r_a3);' % _c
L=Lifter(oem, ournames, ext)
L.alias={'_ev@41d3c':'awb_ev','lut_num':'lsc_lut_num','ev_changed@143d8':'tisp_adr_ev_changed','ev_now@143dc':'tisp_adr_ev_now','ev_changed@133ac':'tisp_defog_ev_changed','ev_now@133b0':'tisp_defog_ev_now'}
ournames|={'defog_fpga_para'}
import re as _re
_src=open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'tx_isp_t21_recovered.c')).read()
_decl=set(_re.findall(r'^static\s+(?:volatile\s+)?(?:const\s+)?(?:unsigned\s+char|uint32_t|uintptr_t|int32_t|u32|int|uint8_t|char|uint16_t)\s+(?:__attribute__\(\(aligned\(4\)\)\)\s+)?\**([A-Za-z_][A-Za-z0-9_]*)\s*[\[;=]', _src, _re.M))
_ovr=set(sys.argv[3].split(',')) if len(sys.argv)>3 else set()
ournames|=(_decl-_ovr)
L.callbacks={'tisp_ae_process','ae_interrupt_static','ae_interrupt_hist','tisp_adr_process','tiziano_adr_interrupt_static','tisp_defog_process','tiziano_defog_interrupt_static','awb_interrupt_static','tisp_ae_ir_update','JZ_Isp_Awb'}
L.cfuncs=set(_re.findall(r'^[a-z][a-z0-9_ \*]*?\b([A-Za-z_][A-Za-z0-9_]*)\([^;]*\)\s*\n\{', _src, _re.M))
todo=sys.argv[2].split(',')
done=[]; bodies=[]
while todo:
    f=todo.pop(0)
    if f in done: continue
    try: bodies.append(L.lift(f)); done.append(f)
    except Exception as x_: raise Exception('%s: %s' % (f, x_))
    for nfn in L.need:
        if nfn not in done and nfn not in todo: todo.append(nfn)
    L.need=[]
# dispatch: every referenced function id must be lifted or extern
for fn in list(L.fnids):
    if fn not in done and fn not in ext and fn in {x['name'] for x in oem.syms if x['type']==2}:
        todo.append(fn)
while todo:
    f=todo.pop(0)
    if f in done: continue
    try: bodies.append(L.lift(f)); done.append(f)
    except Exception as x_: raise Exception('%s: %s' % (f, x_))
    for nfn in L.need:
        if nfn not in done and nfn not in todo: todo.append(nfn)
    L.need=[]
disp=['static uint64_t L_dispatch(uint32_t fn, uint32_t r_a0, uint32_t r_a1, uint32_t r_a2, uint32_t r_a3, uint32_t sp_base)','{','\tuint32_t r_v0 = 0, r_v1 = 0;','\t(void)r_v1;']
for fn_ in done:
    disp.append('\tif (fn == (uint32_t)(uintptr_t)&L_%s) return L_%s(r_a0, r_a1, r_a2, r_a3, sp_base);' % (fn_, fn_))
disp+=['\tswitch (fn) {']
disp+=['\tdefault: return ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)fn)(r_a0, r_a1, r_a2, r_a3);','\t}','\treturn ((uint64_t)r_v1 << 32) | r_v0;','}']
bodies.append('\n'.join(disp))
xl=['static const struct lift_va { uint32_t va, size; uintptr_t ours; } lift_vamap[] = {']
for va_ in sorted(L.vamap):
    sz_,ex_=L.vamap[va_]
    xl.append('\t{ 0x%xu, %du, (uintptr_t)%s },' % (va_, sz_, ex_))
xl.append('};')
xl.append('''static uint32_t lift_xlate(uint32_t va)
{
	int lo = 0, hi = ARRAY_SIZE(lift_vamap) - 1, best = -1;

	/* Last object starting at or below va: a pointer one past the end of
	 * an object still maps to that object unless another one starts there. */
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
	WARN_ONCE(1, "lift: unmapped stock address %#x\\n", va);
	return va;
}
#define LIFT_XLATE(va) lift_xlate((uint32_t)(va))''')
protos='\n'.join('static uint64_t L_%s(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);'%f for f in done)
print('static DEFINE_SPINLOCK(lift_lock);')
print('/* Hide the function type from -Wcast-function-type: the stock code calls C helpers through untyped pointers. */')
print('#define LIFT_FNADDR(f) ({ uintptr_t f_; __asm__("" : "=r"(f_) : "0"((uintptr_t)&(f))); f_; })')
print('/* Generated by lift.py from oem-t21.ko: %s */' % ', '.join(done))
print(render_private(L.private)); print('\n'.join(xl[:0])); print(protos); print('static uint64_t L_dispatch(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);'); print('\n'.join(xl)); print('\n\n'.join(bodies))
ourszs={x['name']:x['size'] for x in ours.syms if x['name']}
for n_,z_ in sorted(L.shared.items()):
    if ourszs.get(n_)!=z_: sys.stderr.write('SIZE %s oem=%s ours=%s\n'%(n_,z_,ourszs.get(n_)))
sys.stderr.write('UNDEF %s\n' % sorted(L.undef)); sys.stderr.write('lifted %s\nprivate %s\n' % (done, list(L.private)))

sys.stdout=_real
print(_re2.sub(r'([A-Za-z_][A-Za-z0-9_]*)\.(isra|constprop|part)\.(\d+)', r'\1_\2_\3', _buf.getvalue()), end='')
