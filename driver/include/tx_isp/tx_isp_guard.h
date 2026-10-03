/*
 * Robustness guards shared by the per-SoC ISP drivers.
 *
 *  - tx_isp_qbuf_phys_check(): range check for frame buffers that user space
 *    queues on /dev/framechanN by physical address.
 *  - struct tx_isp_sensor_pins: module references on the sensor drivers the
 *    ISP has registered, held until the last close of /dev/tx-isp.
 *
 * Header only: every SoC driver is a single module that includes it once
 * per user, so the helpers stay static inline.
 */
#ifndef TX_ISP_GUARD_H
#define TX_ISP_GUARD_H

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pfn.h>
#include <linux/spinlock.h>

/*
 * Highest physical address a T10/T20/T21/T23/T31 frame buffer may end at:
 * the 128 MiB DDR aperture of these SoCs (the T31 driver already checks its
 * MSCA addresses against the same limit).
 */
#define TX_ISP_QBUF_PHYS_LIMIT 0x08000000ULL

/*
 * libimp and OpenIMP queue frame buffers by physical address, taken from
 * rmem: the RAM above the kernel's mem= that the kernel does not manage.
 * QBUF invalidates the CPU cache over the range and the ISP then writes the
 * frame there, so an address inside kernel RAM (or past the DDR) would have
 * the ISP overwrite kernel memory and the invalidate drop dirty kernel cache
 * lines. Accept only a non-empty range below the DDR limit that touches no
 * page the kernel manages. Buffers the driver allocates itself (its V4L2
 * MMAP pools, in kernel RAM) are queued from kernel space and are not
 * checked; see the callers.
 *
 * Returns 0 if the range is acceptable, -EINVAL otherwise.
 */
static inline int tx_isp_qbuf_phys_check(u32 phys, u32 len)
{
	u64 end = (u64)phys + len;
	unsigned long pfn;

	if (!phys || !len || end > TX_ISP_QBUF_PHYS_LIMIT)
		return -EINVAL;
	/* Kernel RAM is one block on these SoCs: checking both ends and
	 * every page in between costs a compare per page (<= ~800). */
	for (pfn = PFN_DOWN(phys); pfn <= PFN_DOWN(end - 1); pfn++)
		if (pfn_valid(pfn))
			return -EINVAL;
	return 0;
}

/*
 * Report a rejected QBUF (rate limited) and decide its result: -EINVAL when
 * the guard is enforced, 0 (accept, as before the guard) when the module
 * parameter turned it off.
 */
static inline int tx_isp_qbuf_reject(const char *soc, int channel, u32 index,
				     u32 phys, u32 len, bool enforce)
{
	pr_warn_ratelimited("%s: framechan%d QBUF idx=%u phys=0x%08x len=0x%x outside rmem%s\n",
			    soc, channel, index, phys, len,
			    enforce ? ", rejected" : " (qbuf_guard=0, accepted)");
	return enforce ? -EINVAL : 0;
}

#define TX_ISP_SENSOR_PIN_MAX 4

/*
 * Sensor drivers are separate modules whose i2c remove() frees the sensor
 * subdevice the ISP links to. Holding a reference on the sensor module from
 * registration to the last close of /dev/tx-isp makes "rmmod sensor_x" fail
 * cleanly while the ISP is in use, instead of leaving the ISP with stale
 * subdevice pointers. One reference per module, however often it registers.
 */
struct tx_isp_sensor_pins {
	spinlock_t lock;
	struct module *mod[TX_ISP_SENSOR_PIN_MAX];
};

#define TX_ISP_SENSOR_PINS_INIT(name) \
	{ .lock = __SPIN_LOCK_UNLOCKED(name.lock) }

/*
 * Pin @owner (NULL: built in, nothing to pin). Returns false only if the
 * module is already being unloaded; the caller then fails the registration.
 */
static inline bool tx_isp_sensor_pin(struct tx_isp_sensor_pins *pins,
				     struct module *owner)
{
	unsigned long flags;
	bool keep = false;
	int i;

	if (!owner)
		return true;
	if (!try_module_get(owner))
		return false;
	spin_lock_irqsave(&pins->lock, flags);
	for (i = 0; i < TX_ISP_SENSOR_PIN_MAX; i++)
		if (pins->mod[i] == owner)
			break;
	if (i == TX_ISP_SENSOR_PIN_MAX) {
		for (i = 0; i < TX_ISP_SENSOR_PIN_MAX; i++) {
			if (!pins->mod[i]) {
				pins->mod[i] = owner;
				keep = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&pins->lock, flags);
	if (!keep) {
		/* already pinned, or table full (pr_warn below) */
		if (i == TX_ISP_SENSOR_PIN_MAX)
			pr_warn("tx-isp: sensor pin table full, %s not pinned\n",
				module_name(owner));
		module_put(owner);
	}
	return true;
}

/* Drop every pin: last close of /dev/tx-isp and module exit. */
static inline void tx_isp_sensor_unpin_all(struct tx_isp_sensor_pins *pins)
{
	struct module *mod[TX_ISP_SENSOR_PIN_MAX];
	unsigned long flags;
	int i;

	spin_lock_irqsave(&pins->lock, flags);
	for (i = 0; i < TX_ISP_SENSOR_PIN_MAX; i++) {
		mod[i] = pins->mod[i];
		pins->mod[i] = NULL;
	}
	spin_unlock_irqrestore(&pins->lock, flags);
	for (i = 0; i < TX_ISP_SENSOR_PIN_MAX; i++)
		if (mod[i])
			module_put(mod[i]);
}

#endif /* TX_ISP_GUARD_H */
