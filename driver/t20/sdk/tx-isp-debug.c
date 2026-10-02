#include "../../../external/ingenic-sdk/3.10.14/isp/t20/tx-isp-debug.c"

/* The stock T10/T20 modules export isp_printf, and the stock/Thingino sensor
 * modules built against them import it (sensor_jxh42_t10.ko fails with
 * "Unknown symbol isp_printf" without this). */
EXPORT_SYMBOL(isp_printf);
