/*
 * Robustness guards shared by the per-SoC ISP drivers.
 *
 *  - tx_isp_qbuf_phys_check(): rmem range check for frame buffers that user
 *    space queues on /dev/framechanN by physical address.
 *  - struct tx_isp_sensor_pins: module references on the sensor drivers the
 *    ISP has registered, held until the last close of /dev/tx-isp.
 *  - tx_isp_i2c_add_sensor_driver(): private_i2c_add_driver() body that
 *    keeps the sensor module as the i2c_driver owner the pins rely on.
 *
 * Header only: every SoC driver is a single module that includes it once
 * per user, so the helpers stay static inline.
 */
#ifndef TX_ISP_GUARD_H
#define TX_ISP_GUARD_H

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/spinlock.h>

/*
 * libimp and OpenIMP queue frame buffers by physical address, taken from
 * rmem: the region the kernel command line reserves with rmem=SIZE@BASE
 * (outside the kernel's mem=). QBUF invalidates the CPU cache over the range
 * and the ISP then writes the frame there, so an address outside rmem would
 * have the ISP overwrite kernel memory and the invalidate drop dirty kernel
 * cache lines. Accept only ranges inside the rmem window. The kernel does
 * not export the command line to modules, so read /proc/cmdline once, in
 * process context, on the first QBUF. Without a usable rmem= (no procfs, no
 * or malformed entry) the window is unknown and nothing is rejected, as
 * before the guard. Buffers the driver allocates itself (its V4L2 MMAP
 * pools) are queued from kernel space and are not checked; see the callers.
 */
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>

#ifdef READ_ONCE
#define TX_ISP_READ_ONCE(x) READ_ONCE(x)
#else
#define TX_ISP_READ_ONCE(x) ACCESS_ONCE(x)
#endif

struct tx_isp_rmem_window {
	int state;		/* 0 not read yet, 1 known, -1 unknown */
	u32 base;
	u32 size;
};

static struct tx_isp_rmem_window tx_isp_rmem_window;

static inline int tx_isp_rmem_parse(const char *cmdline, u32 *base, u32 *size)
{
	const char *p = cmdline;
	unsigned long long b, sz;
	char *end;

	while ((p = strstr(p, "rmem=")) != NULL) {
		if (p != cmdline && p[-1] != ' ') {
			p += 5;
			continue;
		}
		sz = memparse(p + 5, &end);
		if (*end != '@')
			return -EINVAL;
		b = memparse(end + 1, &end);
		if (!sz || b + sz > 0x100000000ULL)
			return -EINVAL;
		*base = (u32)b;
		*size = (u32)sz;
		return 0;
	}
	return -ENOENT;
}

static inline void tx_isp_rmem_probe(void)
{
	struct tx_isp_rmem_window w = { .state = -1 };
	struct file *file;
	char *buf;
	ssize_t n;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	loff_t pos = 0;
#endif

	buf = kzalloc(2048, GFP_KERNEL);
	if (!buf)
		return;		/* retry on the next QBUF */
	file = filp_open("/proc/cmdline", O_RDONLY, 0);
	if (!IS_ERR(file)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		n = kernel_read(file, buf, 2047, &pos);
#else
		n = kernel_read(file, 0, buf, 2047);
#endif
		filp_close(file, NULL);
		if (n > 0 && !tx_isp_rmem_parse(buf, &w.base, &w.size))
			w.state = 1;
	}
	kfree(buf);
	if (w.state == 1)
		pr_notice("tx-isp: QBUF guard: rmem window 0x%08x+0x%x\n",
			w.base, w.size);
	else
		pr_warn("tx-isp: QBUF guard inactive: no rmem=SIZE@BASE on the kernel command line\n");
	tx_isp_rmem_window.base = w.base;
	tx_isp_rmem_window.size = w.size;
	smp_wmb();
	tx_isp_rmem_window.state = w.state;
}

/* Returns 0 if the range is acceptable, -EINVAL otherwise. */
static inline int tx_isp_qbuf_phys_check(u32 phys, u32 len)
{
	u64 end = (u64)phys + len;

	if (!TX_ISP_READ_ONCE(tx_isp_rmem_window.state))
		tx_isp_rmem_probe();
	if (TX_ISP_READ_ONCE(tx_isp_rmem_window.state) != 1)
		return 0;
	smp_rmb();
	if (!phys || !len || phys < tx_isp_rmem_window.base ||
	    end > (u64)tx_isp_rmem_window.base + tx_isp_rmem_window.size)
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

/*
 * Register a sensor's i2c_driver for the sensor module. Sensors call the
 * ISP's private_i2c_add_driver(); i2c_add_driver() there is a macro for
 * i2c_register_driver(THIS_MODULE, ...), which overwrote driver.owner with
 * the ISP module. The pins above then took a reference on the ISP itself
 * instead of the sensor, so "rmmod sensor_x" went through while streaming.
 * The i2c_driver is static data of the sensor module, which identifies it.
 * Also hide the sysfs bind/unbind files: unbinding the sensor from user
 * space would free the subdevice under a running ISP just like rmmod.
 */
static inline int tx_isp_i2c_add_sensor_driver(struct i2c_driver *driver)
{
	struct module *owner;

	/*
	 * The i2c_driver is static data of the sensor module, whose initializer
	 * sets .driver.owner = THIS_MODULE, so owner already identifies the
	 * sensor (NULL when the sensor is built in).  The reference port
	 * recovered it with __module_address((unsigned long)driver), but that
	 * symbol is not exported to modules on mainline (7.1).  .driver.owner is
	 * the exported-API equivalent and is not clobbered here: we call
	 * i2c_register_driver() directly rather than the i2c_add_driver() macro
	 * that would overwrite owner with the ISP's THIS_MODULE.
	 */
	owner = driver->driver.owner;
	driver->driver.suppress_bind_attrs = true;
	return i2c_register_driver(owner, driver);
}

#endif /* TX_ISP_GUARD_H */
