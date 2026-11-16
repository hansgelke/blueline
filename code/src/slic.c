/*
 * blueline - slic.c
 * SLIC (Silvertel AG1171, J4) control.
 *
 * POT_PDOWN_N (GP3) drives AG1171 pin 14 through diode D7 (cathode at GP3),
 * so GP3 can only pull that pin low. After init GP3 is tristate (input
 * without pulls): D7 blocks and the AG1171 internal pull-up holds the pin high.
 *
 * After init: RING_MODE (GP7) low, POT_PDOWN_N (GP3) tristate.
 * POT_FWD (GP2) is the ringer PWM, see ring.c. RING_MODE is switched by
 * bell_fsm (ring.c) through slic_ring_mode_set().
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "slic.h"

LOG_MODULE_REGISTER(slic, LOG_LEVEL_INF);

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* ---- init -------------------------------------------------------------- */

int slic_init(void)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}

	/* RING_MODE: output, low - first, as early as possible */
	ret = gpio_pin_configure(gpio0, RING_MODE_PIN, GPIO_OUTPUT_LOW);
	if (ret < 0) {
		LOG_ERR("RING_MODE pin config failed (%d)", ret);
		return ret;
	}

	/* POT_PDOWN_N: input without pulls = tristate */
	ret = gpio_pin_configure(gpio0, POT_PDOWN_N_PIN, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("POT_PDOWN_N pin config failed (%d)", ret);
		return ret;
	}

	LOG_INF("GPIO%d RING_MODE low / GPIO%d POT_PDOWN_N tristate",
		RING_MODE_PIN, POT_PDOWN_N_PIN);
	return 0;
}

void slic_ring_mode_set(bool on)
{
	gpio_pin_set(gpio0, RING_MODE_PIN, on ? 1 : 0);
}
