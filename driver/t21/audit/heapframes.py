#!/usr/bin/env python3
"""Move the static stock frames of the lifted ioctl-path functions to the heap.

The translator (lift.py) gives every lifted function whose stock stack frame
exceeds 256 bytes a function-local *static* frame.  For the tuning ioctl path
(apical s_ctrl and the ROI/zone/hist/weight helpers it dispatches to) that
costs ~5.7 KB of .bss and makes the helpers non-reentrant (two concurrent
tuning ioctls share one frame).  These functions already private_kmalloc()
with GFP_KERNEL themselves, so a per-call kzalloc'd frame is safe.

usage: heapframes.py tx_isp_t21_adr_oem.inc   (idempotent; edits in place)
lift.py emits the same shape for names listed in $LIFT_HEAP_FRAMES.
"""
import re, sys

HEAP = ['apical_isp_core_ops_s_ctrl', 'apical_isp_ae_s_roi_isra_45',
        'apical_isp_ae_zone_weight_s_attr_isra_50', 'tisp_s_ae_hist',
        'apical_isp_af_weight_s_attr_isra_54', 'apical_isp_ae_g_roi_isra_64',
        'apical_isp_ae_zone_g_ctrl_isra_69', 'apical_isp_ae_zone_weight_g_attr_isra_70',
        'apical_isp_af_weight_g_attr_isra_74', 'tisp_s_aeroi_weight']
SIG = '(uint32_t r_a0, uint32_t r_a1, uint32_t r_a2, uint32_t r_a3, uint32_t csp'

def wrapper(prefix, name, words):
    f = prefix + name
    return ('static uint64_t %s__body%s, uint32_t *frame);\n'
            'static uint64_t %s%s)\n{\n'
            '\t/* stock frame on the heap (reentrant, not in .bss); see audit/heapframes.py */\n'
            '\tuint32_t *frame = lift_frame_alloc(%d);\n\tuint64_t q_;\n\n'
            '\tif (!frame)\n\t\treturn (uint32_t)-ENOMEM;\n'
            '\tq_ = %s__body(r_a0, r_a1, r_a2, r_a3, csp, frame);\n'
            '\tlift_frame_free(frame);\n\treturn q_;\n}\n\n'
            'static uint64_t %s__body%s, uint32_t *frame)\n{\n') % (f, SIG, f, SIG, words, f, f, SIG)

def main(path):
    s = open(path).read()
    for name in HEAP:
        pat = re.compile(r'static uint64_t (L_)%s%s\)\n\{\n\t/\* large stock frame: static \(single caller context, not reentrant\) \*/\n\tstatic uint32_t frame\[(\d+)\] __aligned\(8\);\n' % (re.escape(name), re.escape(SIG)))
        m = pat.search(s)
        if not m:
            if ('L_%s__body' % name) in s:
                continue
            sys.exit('pattern not found: ' + name)
        s = s[:m.start()] + wrapper(m.group(1), name, int(m.group(2))) + s[m.end():]
    if 'lift_frame_alloc(uint32_t words)' not in s:
        helper = ('static uint32_t *lift_frame_alloc(uint32_t words)\n{\n'
                  '\tuint32_t *f_ = private_kmalloc(words * 4, GFP_KERNEL);\n\n'
                  '\tif (f_)\n\t\tmemset(f_, 0, words * 4);\n\treturn f_;\n}\n\n'
                  'static void lift_frame_free(uint32_t *frame)\n{\n\tprivate_kfree(frame);\n}\n\n')
        i = s.index('static uint64_t L_')
        s = s[:i] + helper + s[i:]
    open(path, 'w').write(s)

if __name__ == '__main__':
    main(sys.argv[1])
