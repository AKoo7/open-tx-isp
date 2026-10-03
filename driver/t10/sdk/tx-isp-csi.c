/*
 * The shared SDK sources select the CSI clock code from the kernel's SoC
 * config.  A T10 kernel has no "csi" clock: built against a T20 kernel tree
 * (same vermagic, so insmod accepts it) this module logs "Failed to get csi
 * clock -22" at probe and oopses in isp_csi_set_clk() at stream start.
 * Refuse such a build instead of producing a module that loads.
 */
#ifndef CONFIG_SOC_T10
#error "tx-isp-t10 must be built against a CONFIG_SOC_T10 kernel tree (ROOT=<T10 output>)"
#endif

#include "../../t20/sdk/tx-isp-csi.c"
