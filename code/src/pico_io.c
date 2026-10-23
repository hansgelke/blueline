/*
 * blueline - pico_io.c
 * GPIO setup for the Raspberry Pi Pico 2 (RP2350): shk and user LED.
 *
 * All GPIOs of bank 0 share one interrupt line to the NVIC (IO_IRQ_BANK0).
 * The Zephyr GPIO driver handles that NVIC interrupt and calls the registered
 * callback with a bit mask of the pins that fired. gpio_int is that common
 * interrupt service routine: further pins are added to GPIO_INT_PIN_MASK and
 * handled in gpio_int as the project grows.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "pico_io.h"
#include "fsm_main.h"

LOG_MODULE_REGISTER(pico_io, LOG_LEVEL_INF);

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* All pins that are served by the common ISR gpio_int */
#define GPIO_INT_PIN_MASK  (BIT(PICO_IO_SHK_PIN))

static struct gpio_callback gpio_int_cb;

/* ---- common interrupt service routine ---------------------------------- */

static void gpio_int(const struct device *dev, struct gpio_callback *cb,
		     uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(PICO_IO_SHK_PIN)) {
		fsm_post_event(FSM_EV_SHK_PRESSED);
	}

	/* further GPIO interrupt sources will be handled here */
}

/* ---- public functions -------------------------------------------------- */

int pico_io_init(void)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}

	/* LED: output, initially off */
	ret = gpio_pin_configure(gpio0, PICO_IO_LED_PIN, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("LED pin config failed (%d)", ret);
		return ret;
	}

	/* Shk: input with pull-up, high = released, low = pushed */
	ret = gpio_pin_configure(gpio0, PICO_IO_SHK_PIN,
				 GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		LOG_ERR("shk pin config failed (%d)", ret);
		return ret;
	}

	/* Register the common ISR for all interrupt pins */
	gpio_init_callback(&gpio_int_cb, gpio_int, GPIO_INT_PIN_MASK);
	ret = gpio_add_callback(gpio0, &gpio_int_cb);
	if (ret < 0) {
		LOG_ERR("adding gpio_int callback failed (%d)", ret);
		return ret;
	}

	/* Shk interrupt on falling edge (= shk pushed) */
	ret = gpio_pin_interrupt_configure(gpio0, PICO_IO_SHK_PIN,
					   GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("shk IRQ config failed (%d)", ret);
		return ret;
	}

	LOG_INF("GPIO%d shk / GPIO%d LED initialised",
		PICO_IO_SHK_PIN, PICO_IO_LED_PIN);
	return 0;
}

void pico_io_led_set(bool on)
{
	gpio_pin_set(gpio0, PICO_IO_LED_PIN, on ? 1 : 0);
}
