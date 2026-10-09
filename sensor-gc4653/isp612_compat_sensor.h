#ifndef ISP612_COMPAT_SENSOR_H
#define ISP612_COMPAT_SENSOR_H
/* 3.10 -> 6.12 compatibility shim for the open Ingenic ISP driver.
   Force-included before every TU via Kbuild ccflags -include. */
#include <linux/version.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/time64.h>

/* phys_t removed on MIPS (use phys_addr_t) */
typedef phys_addr_t phys_t;

/* struct timeval/timespec removed in 5.6 (Y2038). Re-provide the legacy
   32-bit-ABI layout used by the libimp/IMP timestamp fields. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
struct timeval  { __kernel_long_t tv_sec; __kernel_long_t tv_usec; };
struct timespec { __kernel_long_t tv_sec; long tv_nsec; };
#endif

/* set_fs()/get_fs()/mm_segment_t removed in 5.10. No-op; kernel-buffer file IO
   uses kernel_read() in the source. */
#if !defined(KERNEL_DS)
typedef struct { unsigned long seg; } mm_segment_t;
#define KERNEL_DS ((mm_segment_t){ 0 })
#define USER_DS   ((mm_segment_t){ 0 })
#define get_fs()  (KERNEL_DS)
#define set_fs(x) do { (void)(x); } while (0)
#endif

/* (v4l2_mbus_pixelcode comes from the vendor tx-isp-common.h for the sensor build) */


/* field-wise copy between struct timeval (driver/libimp) and __kernel_v4l2_timeval
   (v4l2_buffer) — both expose tv_sec/tv_usec; sidesteps the type mismatch. */
#define TS_COPY(d, s) do { (d).tv_sec = (s).tv_sec; (d).tv_usec = (s).tv_usec; } while (0)

/* seq_printf/seq_puts/seq_putc return void since 4.3; allow legacy `return seq_*()`. */
#include <linux/seq_file.h>
#define seq_printf(...) (seq_printf(__VA_ARGS__), 0)
#define seq_puts(m, s)  (seq_puts((m), (s)), 0)
#define seq_putc(m, c)  (seq_putc((m), (c)), 0)


/* --- link-stage API renames (removed/changed in 6.12) --- */
#include <linux/io.h>
#include <linux/completion.h>
#include <linux/i2c.h>
#ifndef ioremap_nocache
#define ioremap_nocache(a,s) ioremap((a),(s))   /* ioremap is non-cached by default since 5.6 */
#endif
#define INIT_COMPLETION(x) reinit_completion(&(x)) /* removed 3.13 */
static inline struct i2c_client *isp_i2c_new_device(struct i2c_adapter *a,
                                                    struct i2c_board_info const *info) {
    struct i2c_client *c = i2c_new_client_device(a, info);  /* i2c_new_device removed 5.x */
    return IS_ERR(c) ? NULL : c;
}
#define i2c_new_device(a, i) isp_i2c_new_device((a), (i))


#include <linux/proc_fs.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0)
#ifndef PDE_DATA
#define PDE_DATA(inode) pde_data(inode)
#endif
#endif

#endif /* ISP612_COMPAT_SENSOR_H */
