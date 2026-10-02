#include "../../../external/ingenic-sdk/3.10.14/isp/t20/tx-isp-debug.c"

#ifdef TX_ISP_T10
/* The stock T10 sensor modules (sensor_jxh42_t10.ko) import isp_printf from
 * the ISP module ("Unknown symbol isp_printf" otherwise).  T10 builds only:
 * the Thingino T20 ingenic-sdk already exports it from this file. */
EXPORT_SYMBOL(isp_printf);
#endif
