/*
 * isp_printf must be exported exactly once per module: the stock T10 sensor
 * modules (sensor_jxh42_t10.ko) import it from the ISP module ("Unknown
 * symbol isp_printf" otherwise), but depending on the ingenic-sdk revision
 * the included file exports it itself (Thingino's SDK does) or not (the
 * external/ submodule), and a second EXPORT_SYMBOL breaks the build with a
 * duplicate __ksymtab / __kstrtab definition.
 *
 * So the include swallows the SDK's own EXPORT_SYMBOL(isp_printf) and the
 * single export is made here, in every build.
 */
#include <linux/module.h>

#pragma push_macro("EXPORT_SYMBOL")
#undef EXPORT_SYMBOL
#define EXPORT_SYMBOL(sym) EXPORT_SYMBOL_SWALLOWED_##sym
#define EXPORT_SYMBOL_SWALLOWED_isp_printf \
	extern int isp_printf(unsigned int level, unsigned char *fmt, ...)

#include "../../../external/ingenic-sdk/3.10.14/isp/t20/tx-isp-debug.c"

#undef EXPORT_SYMBOL_SWALLOWED_isp_printf
#pragma pop_macro("EXPORT_SYMBOL")

EXPORT_SYMBOL(isp_printf);
