/*
 * T41 sensor module pinning (see tx_isp_guard.h).
 *
 * Kept out of the recovered monolith so its layout-sensitive BSS and the
 * linked sections of the objects before this one stay unchanged.
 */

#include <linux/i2c.h>
#include <linux/module.h>

#include "../include/tx_isp/tx_isp_guard.h"

/* Sensor modules registered since the first open of /dev/tx-isp. */
static struct tx_isp_sensor_pins tx_isp_t41_sensor_pins =
	TX_ISP_SENSOR_PINS_INIT(tx_isp_t41_sensor_pins);

int tx_isp_t41_i2c_add_sensor_driver(struct i2c_driver *driver)
{
	return tx_isp_i2c_add_sensor_driver(driver);
}

bool tx_isp_t41_sensor_pin(struct module *owner)
{
	return tx_isp_sensor_pin(&tx_isp_t41_sensor_pins, owner);
}

void tx_isp_t41_sensor_unpin_all(void)
{
	tx_isp_sensor_unpin_all(&tx_isp_t41_sensor_pins);
}
