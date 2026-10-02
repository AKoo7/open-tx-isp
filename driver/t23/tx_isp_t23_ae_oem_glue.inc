/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stock T23 AE0 chain lifted instruction for instruction from tx-isp-t23.ko
 * (audit/dolift_ae.py -> tx_isp_t23_ae_oem.inc), with the glue it needs in
 * this driver.  Off by default (source_ae_oem=0): then nothing here runs and
 * the HLIL substitute in tx_isp_t23_ae_runtime.inc stays in charge.
 *
 * The lifted code keeps private copies of every stock object it touches
 * (oem_*), only tparams (the active IQ bank, same layout) is shared.  The
 * stock side effects are routed here:
 *  - system_reg_write/read: the ISP registers, unchanged;
 *  - sensor_ctrl callbacks (oem_sensor_ctrl, filled like stock sensor_init):
 *    alloc_* call the sensor attribute allocators, set_* only latch the new
 *    integration time / analog gain like stock does, the latched pair is
 *    sent to the sensor after the AE run (stock: next frame interrupt);
 *  - tisp_event_push: event 1 (AE0 histogram ready) schedules the AE run,
 *    the gain events the AE run emits are applied after it.
 *
 * NOT DEVICE TESTED.  Verified in the MIPS emulator only (register writes,
 * events and sensor calls identical to stock for the scene set in
 * audit/aelift_emu.py).
 */

module_param_named(source_ae_oem, regtrace_t23_source_ae_oem, bool, 0444);
MODULE_PARM_DESC(source_ae_oem,
		 "experimental: run the stock AE0 algorithm lifted from the OEM module instead of the AE substitute (default 0, needs reload)");

static noinline __attribute__((noipa)) uint32_t t23_aelift_event_push(uint32_t channel, uint32_t event);
static void *t23_aelift_buf;

/* lifted entry: the caller's outgoing argument area (stock 16(sp)..) */
#define T23_AELIFT_CALL(fn, a0, a1, a2, a3) ({			\
	uint32_t stk_[8] = { 0 };				\
	fn((a0), (a1), (a2), (a3), (uint32_t)(uintptr_t)stk_);	\
})

static noinline __attribute__((noipa)) void *t23_aelift_kmalloc(uint32_t size)
{
	/* only tiziano_ae_init allocates (0x3580 bytes); keep one buffer */
	if (!t23_aelift_buf)
		t23_aelift_buf = kzalloc(size, GFP_KERNEL);
	return t23_aelift_buf;
}

static noinline __attribute__((noipa)) void t23_aelift_reg_write(uint32_t reg, uint32_t value)
{
	system_reg_write(reg, value);
}

static noinline __attribute__((noipa)) uint32_t t23_aelift_reg_read(uint32_t reg)
{
	return (uint32_t)system_reg_read(reg);
}

/* stock code calling through a function pointer that is not lifted code */
static uint64_t t23_aelift_call(uint32_t fn, uint32_t a0, uint32_t a1,
				uint32_t a2, uint32_t a3)
{
	if (!fn) {
		WARN_ONCE(1, "aelift: NULL call\n");
		return 0;
	}
	return ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))
		(uintptr_t)fn)(a0, a1, a2, a3);
}

/* only their addresses appear in the lifted code (calls go to the glue) */
int tisp_event_set_cb(uint32_t channel, uint32_t event_id, void *callback);
int32_t tisp_event_push(uint32_t channel, const void *event);
int32_t system_irq_func_set(int32_t arg1, int32_t arg2, int32_t arg3);

#include "tx_isp_t23_ae_oem.inc"

/* ---- runtime (source_ae_oem=1) ---------------------------------------- */

#define T23_AELIFT_EVENTS 16U
struct t23_aelift_event {
	uint32_t id;
	uint32_t args[8];
};
static DEFINE_SPINLOCK(t23_aelift_ev_lock);
static struct t23_aelift_event t23_aelift_ev[T23_AELIFT_EVENTS];
static unsigned int t23_aelift_ev_count;
static bool t23_aelift_ready;
static uint32_t t23_aelift_it, t23_aelift_again;   /* latched by set_* */
static bool t23_aelift_expo_dirty;

static void t23_aelift_work_fn(struct work_struct *work);
static DECLARE_WORK(t23_aelift_work, t23_aelift_work_fn);

static noinline __attribute__((noipa)) uint32_t t23_aelift_event_push(uint32_t channel, uint32_t event)
{
	const uint32_t *rec = (const uint32_t *)(uintptr_t)event;
	unsigned long flags;

	(void)channel;
	if (!rec)
		return (uint32_t)-EINVAL;
	/* stock record: id at +8, arguments from +0x10 */
	if (rec[2] == 1U) {
		if (t23_aelift_ready)
			schedule_work(&t23_aelift_work);
		return 0;
	}
	spin_lock_irqsave(&t23_aelift_ev_lock, flags);
	if (t23_aelift_ev_count < T23_AELIFT_EVENTS) {
		struct t23_aelift_event *e = &t23_aelift_ev[t23_aelift_ev_count++];

		e->id = rec[2];
		memcpy(e->args, &rec[4], sizeof(e->args));
	}
	spin_unlock_irqrestore(&t23_aelift_ev_lock, flags);
	return 0;
}

static unsigned char *t23_aelift_attr(void)
{
	return regtrace_t23_sensor_owned_attr();
}

typedef uint32_t (*t23_aelift_alloc_fn)(uint32_t value, uint32_t shift,
					 uint32_t *sensor_value);

static uint32_t t23_aelift_alloc(unsigned int attr_off, uint32_t value,
				 uint32_t shift, uint16_t *out)
{
	unsigned char *attr = t23_aelift_attr();
	t23_aelift_alloc_fn fn;
	uint32_t sensor_value = 0;
	uint32_t ret;

	fn = attr ? *(t23_aelift_alloc_fn *)(void *)(attr + attr_off) : NULL;
	if (!fn) {
		*out = (uint16_t)value;
		return value;
	}
	ret = fn(value, shift, &sensor_value);
	*out = (uint16_t)sensor_value;
	return ret;
}

/* stock sensor_alloc_* write the sensor code into the struct at a1 */
static uint32_t t23_aelift_alloc_again(uint32_t v, uint32_t p, uint32_t a2, uint32_t a3)
{
	return t23_aelift_alloc(0xc4, v, 16, (uint16_t *)(uintptr_t)p);
}

static uint32_t t23_aelift_alloc_dgain(uint32_t v, uint32_t p, uint32_t a2, uint32_t a3)
{
	return t23_aelift_alloc(0xcc, v, 16, (uint16_t *)(uintptr_t)(p + 2));
}

static uint32_t t23_aelift_alloc_it(uint32_t v, uint32_t p, uint32_t a2, uint32_t a3)
{
	return t23_aelift_alloc(0xd4, v, 0, (uint16_t *)(uintptr_t)(p + 16));
}

static uint32_t t23_aelift_set_it(uint32_t v, uint32_t a1, uint32_t a2, uint32_t a3)
{
	v &= 0xffffU;
	if (v != t23_aelift_it) {
		t23_aelift_it = v;
		t23_aelift_expo_dirty = true;
	}
	return 0;
}

static uint32_t t23_aelift_set_again(uint32_t v, uint32_t a1, uint32_t a2, uint32_t a3)
{
	v &= 0xffffU;
	if (v != t23_aelift_again) {
		t23_aelift_again = v;
		t23_aelift_expo_dirty = true;
	}
	return 0;
}

static uint32_t t23_aelift_nop(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	return 0;
}

static void t23_aelift_put(unsigned int off, const void *fn)
{
	*(uint32_t *)(void *)(oem_sensor_ctrl + off) = (uint32_t)(uintptr_t)fn;
}

/* stock sensor_init(&sensor_ctrl) */
static int t23_aelift_sensor_init(void)
{
	unsigned char *attr = t23_aelift_attr();
	unsigned char *sc = oem_sensor_ctrl;
	unsigned int off;

	if (!attr)
		return -ENODEV;
	*(uint32_t *)(void *)(sc + 0x20) = regtrace_t23_get_le32(attr + 152);
	*(uint32_t *)(void *)(sc + 0x24) = regtrace_t23_get_le32(attr + 156);
	sc[0x3a] = attr[186];
	sc[0x3b] = attr[188];
	sc[0x3c] = attr[190];
	*(uint32_t *)(void *)(sc + 0x28) = *(uint16_t *)(void *)(attr + 168);
	*(uint32_t *)(void *)(sc + 0x2c) = *(uint16_t *)(void *)(attr + 184);
	*(uint32_t *)(void *)(sc + 0x50) = *(uint16_t *)(void *)(attr + 220);
	*(uint32_t *)(void *)(sc + 0x54) = *(uint16_t *)(void *)(attr + 222);
	*(uint32_t *)(void *)(sc + 0x58) = regtrace_t23_get_le32(attr + 228);
	for (off = 0x5c; off <= 0xb0; off += 4)
		t23_aelift_put(off, t23_aelift_nop);
	t23_aelift_put(0x64, t23_aelift_alloc_again);
	t23_aelift_put(0x6c, t23_aelift_alloc_dgain);
	t23_aelift_put(0x70, t23_aelift_alloc_it);
	t23_aelift_put(0x78, t23_aelift_set_it);
	t23_aelift_put(0x88, t23_aelift_set_again);
	return 0;
}

/*
 * Stock ispcore_core_ops_init builds the 156-byte tisp_init argument that
 * tisp_init copies into sensor_info; the AE reads its sensor fields
 * (deflicker: +0x30 fps, +0x48/+0x4a; ae_init: +0x3c).  Same fields here.
 */
static void t23_aelift_fill_sensor_info(unsigned char *attr, uint32_t width,
					uint32_t height, uint32_t fps)
{
	unsigned char *si = oem_sensor_info;
	static const unsigned short u16map[][2] = {
		{ 0x3c, 168 }, { 0x3e, 170 }, { 0x40, 172 }, { 0x42, 174 },
		{ 0x48, 180 }, { 0x4a, 182 }, { 0x4c, 184 }, { 0x4e, 186 },
		{ 0x50, 188 }, { 0x52, 190 }, { 0x56, 220 }, { 0x58, 222 },
	};
	static const unsigned short u32map[][2] = {
		{ 0x20, 152 }, { 0x24, 156 }, { 0x28, 160 }, { 0x2c, 164 },
		{ 0x44, 176 }, { 0x5c, 224 }, { 0x60, 228 }, { 0x64, 232 },
	};
	unsigned int i;
	uint32_t vts = *(uint16_t *)(void *)(attr + 182);

	memset(si, 0, 156);
	*(uint32_t *)(void *)(si + 0x00) = width;
	*(uint32_t *)(void *)(si + 0x04) = height;
	*(uint32_t *)(void *)(si + 0x30) = fps;
	for (i = 0; i < ARRAY_SIZE(u16map); i++)
		*(uint16_t *)(void *)(si + u16map[i][0]) =
			*(uint16_t *)(void *)(attr + u16map[i][1]);
	for (i = 0; i < ARRAY_SIZE(u32map); i++)
		*(uint32_t *)(void *)(si + u32map[i][0]) =
			regtrace_t23_get_le32(attr + u32map[i][1]);
	/* stock: line time, 1000000 * (fps & 0xffff) / (fps >> 16) / vts (u16) */
	if ((fps >> 16) && vts)
		*(uint16_t *)(void *)(si + 0x54) = (uint16_t)
			(((fps & 0xffffU) * 1000000U / (fps >> 16)) / vts);
}

/*
 * Process context, after the AE statistics grid is programmed and tparams
 * holds the active bank.  Stock tisp_init calls
 * tiziano_ae_init(tispinfo[1], tispinfo[0], u16 sensor_info+0x3c, ...);
 * only the third argument is used (stored as the first IspAeExp word).
 */
static uint32_t regtrace_t23_sensor_held_fps(void);

static int t23_aelift_init(uint32_t width, uint32_t height, uint32_t fps)
{
	uint32_t *info = (uint32_t *)(void *)oem_tispinfo;
	unsigned char *attr = t23_aelift_attr();
	int ret;

	t23_aelift_ready = false;
	if (!regtrace_t23_core_dma_bufs[0].virt || !attr)
		return -ENODEV;
	ret = t23_aelift_sensor_init();
	if (ret)
		return ret;
	/* stock tisp_init: AE0 statistics ring, then the histogram ring */
	info[3] = (uint32_t)(uintptr_t)regtrace_t23_core_dma_bufs[0].virt;
	info[6] = info[3] + 0x4000U;
	t23_aelift_fill_sensor_info(attr, width, height, fps);
	T23_AELIFT_CALL(LA_tiziano_ae_init, 0, 0,
			*(uint16_t *)(void *)(oem_sensor_info + 0x3c), 0);
	t23_aelift_ready = true;
	return 0;
}

static void t23_aelift_start(void)
{
	uint32_t fps = regtrace_t23_sensor_held_fps();
	int ret;

	if (!regtrace_t23_source_ae_oem)
		return;
	ret = t23_aelift_init(regtrace_t23_source_sensor_width,
			      regtrace_t23_source_sensor_height, fps);
	printk(KERN_WARNING
	       "tx_isp_t23_recovered: stock AE0 (lifted, experimental) %s ret=%d fps=0x%x\n",
	       ret ? "NOT started" : "active", ret, fps);
}

/* core interrupt: status bits 26 (AE0 statistics) and 27 (histogram) */
static bool t23_aelift_irq(uint32_t status)
{
	if (!regtrace_t23_source_ae_oem || !t23_aelift_ready)
		return false;
	if (status & BIT(26))
		T23_AELIFT_CALL(LA_ae0_interrupt_static, 0, 0, 0, 0);
	if (status & BIT(27))
		T23_AELIFT_CALL(LA_ae0_interrupt_hist, 0, 0, 0, 0);
	return true;
}

static void t23_aelift_work_fn(struct work_struct *work)
{
	struct t23_aelift_event ev[T23_AELIFT_EVENTS];
	unsigned long flags;
	unsigned int n, i;

	if (!t23_aelift_ready)
		return;
	T23_AELIFT_CALL(LA_tisp_ae0_process, 0, 0, 0, 0);
	if (t23_aelift_expo_dirty) {
		t23_aelift_expo_dirty = false;
		regtrace_t23_call_sensor_exposure(
			(t23_aelift_again << 16) | t23_aelift_it, "ae-oem");
	}
	spin_lock_irqsave(&t23_aelift_ev_lock, flags);
	n = t23_aelift_ev_count;
	memcpy(ev, t23_aelift_ev, n * sizeof(ev[0]));
	t23_aelift_ev_count = 0;
	spin_unlock_irqrestore(&t23_aelift_ev_lock, flags);
	for (i = 0; i < n; i++) {
		/* stock tisp_long_tgain_update: arg0 = log2 total gain, Q16 */
		if (ev[i].id == 4U)
			regtrace_t23_source_apply_total_gain_value(
				tisp_math_exp2(ev[i].args[0], 16, 16), 0,
				false, false);
	}
}

/*
 * Core stop / statistics DMA free (any context): no new runs; a run that is
 * already queued sees !ready and returns before touching any state.
 */
static void t23_aelift_halt(void)
{
	t23_aelift_ready = false;
}

/*
 * Emulator entry points (audit/aelift_emu.py drives these against the
 * stock functions of the same name); unused in the kernel.
 */
static noinline __used uint32_t t23_aelift_emu_init(uint32_t w, uint32_t h,
						     uint32_t fps)
{
	return (uint32_t)T23_AELIFT_CALL(LA_tiziano_ae_init, w, h, fps, 0);
}

static noinline __used uint32_t t23_aelift_emu_static(void)
{
	return (uint32_t)T23_AELIFT_CALL(LA_ae0_interrupt_static, 0, 0, 0, 0);
}

static noinline __used uint32_t t23_aelift_emu_hist(void)
{
	return (uint32_t)T23_AELIFT_CALL(LA_ae0_interrupt_hist, 0, 0, 0, 0);
}

static noinline __used uint32_t t23_aelift_emu_process(void)
{
	return (uint32_t)T23_AELIFT_CALL(LA_tisp_ae0_process, 0, 0, 0, 0);
}

static noinline __used uint32_t t23_aelift_emu_dn(uint32_t a0)
{
	return (uint32_t)T23_AELIFT_CALL(LA_tiziano_ae_dn_params_refresh, a0, 0, 0, 0);
}
