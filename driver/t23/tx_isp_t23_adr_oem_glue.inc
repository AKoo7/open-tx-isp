/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stock T23 dynamic ADR (tone mapping / DRC) and defog lifted instruction
 * for instruction from tx-isp-t23.ko (audit/dolift_adr.py ->
 * tx_isp_t23_adr_oem.inc), with the glue it needs in this driver.  On by
 * default (source_adr_oem=1); with source_adr_oem=0 the source-derived C
 * ADR/defog of this driver (static curves) is in charge as before.
 *
 * The lifted code keeps private copies of every stock object it touches
 * (oemd_*); only tparams (the active IQ bank, same layout) is shared, and
 * the gamma LUT is read through tisp_gamma_param_array_get.  Side effects:
 *  - system_reg_write/read: the ISP registers, unchanged;
 *  - statistics: tispinfo carries the ADR (0x4494) and defog (0x5b84) DMA
 *    rings of this driver like stock tisp_init;
 *  - interrupts: core status bit 18 (ADR) / 20 (defog) run the lifted
 *    tiziano_*_interrupt_static in the core ISR, as stock irq_func_cb;
 *  - events 2 (ADR) and 3 (defog) the interrupt handlers push run the
 *    lifted tisp_adr_process / tisp_defog_process from a work item (stock:
 *    the tisp event thread).
 *
 * Verified in the MIPS emulator against the stock module
 * (audit/adrlift_emu.py: register writes and events identical frame by
 * frame for day/night, EV, strength and tuning get/set sequences).
 */

module_param_named(source_adr_oem, regtrace_t23_source_adr_oem, bool, 0444);
MODULE_PARM_DESC(source_adr_oem,
		 "run the stock dynamic ADR and defog lifted from the OEM module (default 1); 0 = the static C ADR/defog");

static noinline __attribute__((noipa)) uint32_t t23_adrlift_event_push(uint32_t channel, uint32_t event);

#define T23_ADRLIFT_CALL(fn, a0, a1, a2, a3) ({			\
	uint32_t stk_[8] = { 0 };				\
	fn((a0), (a1), (a2), (a3), (uint32_t)(uintptr_t)stk_);	\
})

static noinline __attribute__((noipa)) void *t23_adrlift_kmalloc(uint32_t size)
{
	return kzalloc(size, GFP_KERNEL);
}

static noinline __attribute__((noipa)) void t23_adrlift_reg_write(uint32_t reg, uint32_t value)
{
	system_reg_write(reg, value);
}

static noinline __attribute__((noipa)) uint32_t t23_adrlift_reg_read(uint32_t reg)
{
	return (uint32_t)system_reg_read(reg);
}

/* the ISR below dispatches by status bit; nothing to register */
static uint32_t t23_adrlift_irq_set(uint32_t ch, uint32_t idx, uint32_t fn)
{
	return 0;
}

static uint32_t t23_adrlift_event_set_cb(uint32_t ch, uint32_t id, uint32_t fn)
{
	return 0;
}

static uint64_t t23_adrlift_call(uint32_t fn, uint32_t a0, uint32_t a1,
				 uint32_t a2, uint32_t a3)
{
	if (!fn) {
		WARN_ONCE(1, "adrlift: NULL call\n");
		return 0;
	}
	return ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))
		(uintptr_t)fn)(a0, a1, a2, a3);
}

int32_t tisp_gamma_param_array_get(int32_t arg1, int32_t *arg2, int32_t *arg3);

#include "tx_isp_t23_adr_oem.inc"

/* ---- runtime (source_adr_oem=1) --------------------------------------- */

static DEFINE_MUTEX(t23_adrlift_mutex);
/* statistics interrupts / process runs of the lifted ADR and defog */
static uint t23_adrlift_counts[4];
static int t23_adrlift_ncounts = 4;
module_param_array_named(adr_oem_runs, t23_adrlift_counts, uint,
			 &t23_adrlift_ncounts, 0444);
MODULE_PARM_DESC(adr_oem_runs, "lifted stock ADR irq,process / defog irq,process runs");
static bool t23_adrlift_adr_ready, t23_adrlift_defog_ready;
/* stock tisp_s_defog_str_internal(a0, const u8 *strength) */
static uint32_t t23_adrlift_adr_str = 0x80U;
static uint8_t t23_adrlift_defog_str = 0x80U;
static bool t23_adrlift_adr_str_set, t23_adrlift_defog_str_set;

static void t23_adrlift_adr_work_fn(struct work_struct *work)
{
	mutex_lock(&t23_adrlift_mutex);
	if (t23_adrlift_adr_ready) {
		T23_ADRLIFT_CALL(LD_tisp_adr_process, 0, 0, 0, 0);
		t23_adrlift_counts[1]++;
	}
	mutex_unlock(&t23_adrlift_mutex);
}

static void t23_adrlift_defog_work_fn(struct work_struct *work)
{
	mutex_lock(&t23_adrlift_mutex);
	if (t23_adrlift_defog_ready) {
		T23_ADRLIFT_CALL(LD_tisp_defog_process, 0, 0, 0, 0);
		t23_adrlift_counts[3]++;
	}
	mutex_unlock(&t23_adrlift_mutex);
}

static DECLARE_WORK(t23_adrlift_adr_work, t23_adrlift_adr_work_fn);
static DECLARE_WORK(t23_adrlift_defog_work, t23_adrlift_defog_work_fn);

static noinline __attribute__((noipa)) uint32_t t23_adrlift_event_push(uint32_t channel, uint32_t event)
{
	const uint32_t *rec = (const uint32_t *)(uintptr_t)event;

	(void)channel;
	if (!rec)
		return (uint32_t)-EINVAL;
	/* stock record: id at +8 */
	if (rec[2] == 2U)
		schedule_work(&t23_adrlift_adr_work);
	else if (rec[2] == 3U)
		schedule_work(&t23_adrlift_defog_work);
	return 0;
}

/* stock tisp_init: tispinfo +72/+76 ADR ring, +84/+88 defog ring */
static void t23_adrlift_set_rings(uint32_t adr_virt, uint32_t adr_phys,
				  uint32_t defog_virt, uint32_t defog_phys)
{
	uint32_t *info = (uint32_t *)(void *)oemd_tispinfo;

	info[18] = adr_virt;
	info[19] = adr_phys;
	info[21] = defog_virt;
	info[22] = defog_phys;
}

static bool t23_adrlift_rings(void)
{
	if (!regtrace_t23_core_dma_bufs[3].virt ||
	    !regtrace_t23_core_dma_bufs[4].virt)
		return false;
	t23_adrlift_set_rings(
		(uint32_t)(uintptr_t)regtrace_t23_core_dma_bufs[3].virt,
		regtrace_t23_core_dma_bufs[3].phys,
		(uint32_t)(uintptr_t)regtrace_t23_core_dma_bufs[4].virt,
		regtrace_t23_core_dma_bufs[4].phys);
	return true;
}

/* core ISR: status bit 18 (ADR statistics), 20 (defog statistics) */
static void t23_adrlift_irq(uint32_t status)
{
	if (!regtrace_t23_source_adr_oem)
		return;
	if ((status & BIT(18)) && t23_adrlift_adr_ready) {
		T23_ADRLIFT_CALL(LD_tiziano_adr_interrupt_static, 0, 0, 0, 0);
		t23_adrlift_counts[0]++;
	}
	if ((status & BIT(20)) && t23_adrlift_defog_ready) {
		T23_ADRLIFT_CALL(LD_tiziano_defog_interrupt_static, 0, 0, 0, 0);
		t23_adrlift_counts[2]++;
	}
}

/*
 * Stock runs tiziano_adr_init/tiziano_defog_init once in tisp_init; this
 * driver starts the core at every stream start.  A user DRC/defog strength
 * is set again after each init, as if set after stock tisp_init.
 */
static int t23_adrlift_adr_init(uint32_t a0, uint32_t width, uint32_t height)
{
	if (!t23_adrlift_rings())
		return -ENODEV;
	flush_work(&t23_adrlift_adr_work);
	mutex_lock(&t23_adrlift_mutex);
	t23_adrlift_adr_ready = false;
	T23_ADRLIFT_CALL(LD_tiziano_adr_init, a0, width, height, 0);
	if (t23_adrlift_adr_str_set)
		T23_ADRLIFT_CALL(LD_tisp_s_adr_str_internal,
				 t23_adrlift_adr_str, 0, 0, 0);
	t23_adrlift_adr_ready = true;
	mutex_unlock(&t23_adrlift_mutex);
	printk(KERN_WARNING
	       "tx_isp_t23_recovered: stock dynamic ADR (lifted) active %ux%u\n",
	       width, height);
	return 0;
}

static int t23_adrlift_defog_init(uint32_t a0, uint32_t width, uint32_t height)
{
	if (!t23_adrlift_rings())
		return -ENODEV;
	flush_work(&t23_adrlift_defog_work);
	mutex_lock(&t23_adrlift_mutex);
	t23_adrlift_defog_ready = false;
	T23_ADRLIFT_CALL(LD_tiziano_defog_init, a0, width, height, 0);
	if (t23_adrlift_defog_str_set)
		T23_ADRLIFT_CALL(LD_tisp_s_defog_str_internal, 0,
				 (uint32_t)(uintptr_t)&t23_adrlift_defog_str, 0, 0);
	t23_adrlift_defog_ready = true;
	mutex_unlock(&t23_adrlift_mutex);
	printk(KERN_WARNING
	       "tx_isp_t23_recovered: stock defog (lifted) active %ux%u\n",
	       width, height);
	return 0;
}

/*
 * Core stop / statistics DMA free (any context): no new interrupt or
 * process run; a queued run sees !ready and returns.
 */
static void t23_adrlift_halt(void)
{
	t23_adrlift_adr_ready = false;
	t23_adrlift_defog_ready = false;
}

/* stock tisp_day_or_night_s_ctrl: ADR then defog from the new bank */
static void t23_adrlift_dn_refresh(void)
{
	mutex_lock(&t23_adrlift_mutex);
	if (t23_adrlift_adr_ready)
		T23_ADRLIFT_CALL(LD_tiziano_adr_dn_params_refresh, 0, 0, 0, 0);
	if (t23_adrlift_defog_ready)
		T23_ADRLIFT_CALL(LD_tiziano_defog_dn_params_refresh, 0, 0, 0, 0);
	mutex_unlock(&t23_adrlift_mutex);
}

/* plain entries: EV, strength, tuning get/set, WDR, gamma refresh */
static uint32_t t23_adrlift_entry(uint64_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t),
				  uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
				  bool sleep)
{
	uint32_t ret;

	if (sleep)
		mutex_lock(&t23_adrlift_mutex);
	ret = (uint32_t)T23_ADRLIFT_CALL(fn, a0, a1, a2, a3);
	if (sleep)
		mutex_unlock(&t23_adrlift_mutex);
	return ret;
}

static uint32_t t23_adrlift_s_adr_str(uint32_t v)
{
	t23_adrlift_adr_str = v;
	t23_adrlift_adr_str_set = true;
	return t23_adrlift_entry(LD_tisp_s_adr_str_internal, v, 0, 0, 0, true);
}

static uint32_t t23_adrlift_s_defog_str(uint32_t v)
{
	t23_adrlift_defog_str = (uint8_t)v;
	t23_adrlift_defog_str_set = true;
	return t23_adrlift_entry(LD_tisp_s_defog_str_internal, 0,
				 (uint32_t)(uintptr_t)&t23_adrlift_defog_str, 0, 0, true);
}

/* ---- emulator entries (audit/adrlift_emu.py) -------------------------- */

static noinline __used uint32_t t23_adrlift_emu_rings(uint32_t av, uint32_t ap,
						      uint32_t dv, uint32_t dp)
{
	t23_adrlift_set_rings(av, ap, dv, dp);
	return 0;
}

static noinline __used uint32_t t23_adrlift_emu_call(uint32_t which, uint32_t a0,
						     uint32_t a1, uint32_t a2)
{
	static uint64_t (*const fns[])(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = {
		LD_tiziano_adr_init, LD_tiziano_defog_init,
		LD_tiziano_adr_interrupt_static, LD_tiziano_defog_interrupt_static,
		LD_tisp_adr_process, LD_tisp_defog_process,
		LD_tisp_adr_ev_update, LD_tisp_defog_ev_update,
		LD_tiziano_adr_dn_params_refresh, LD_tiziano_defog_dn_params_refresh,
		LD_tisp_s_adr_str_internal, LD_tisp_s_defog_str_internal,
		LD_tisp_g_adr_str_internal, LD_tisp_g_defog_str_internal,
		LD_tisp_adr_param_array_get, LD_tisp_defog_param_array_get,
		LD_tisp_adr_param_array_set, LD_tisp_defog_param_array_set,
		LD_tisp_adr_wdr_en, LD_tisp_defog_wdr_en,
	};

	if (which >= ARRAY_SIZE(fns))
		return 0;
	return (uint32_t)T23_ADRLIFT_CALL(fns[which], a0, a1, a2, 0);
}
