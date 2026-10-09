#ifndef __COMPAT_JZ_PROC_H__
#define __COMPAT_JZ_PROC_H__
#include <linux/proc_fs.h>
#include <linux/version.h>
/* PDE_DATA() was renamed pde_data() in 5.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0)
#ifndef PDE_DATA
#define PDE_DATA(inode) pde_data(inode)
#endif
#endif
/* vendor jz proc helpers (unused by this driver path; declared for completeness) */
struct proc_dir_entry *jz_proc_mkdir(char *s);
struct proc_dir_entry *get_jz_proc_root(void);
#endif
