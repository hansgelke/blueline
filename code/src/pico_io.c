/*
 * blueline - pico_io.c
 * GPIO setup for the Raspberry Pi Pico 2 (RP2350): shk and user LED.
 *
 * All GPIOs of bank 0 share one interrupt line to the NVIC (IO_IRQ_BANK0).
 * The Zephyr GPIO driver handles that NVIC interrupt and calls the registered
 * callback with a bit mask of the pins that fired. gpio_int is that common
 * interrupt service routine: further pins are added to GPIO_INT_PIN_MASK and
 * handled in gpio_int as the project grows.
 *
 * shk events:
 *   EVT_SHK_INACTIVE: falling edge on SHK_FALLING_PIN and the pin is still
 *                     low SHK_CHECK_MS after the edge
 *   EVT_SHK_ACTIVE:   rising edge on SHK_RISING_PIN and the pin is still
 *                     high SHK_CHECK_MS after the edge
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
#define GPIO_INT_PIN_MASK  (BIT(SHK_FALLING_PIN) | BIT(SHK_RISING_PIN))

static struct gpio_callback gpio_int_cb;

/* ---- shk level check, SHK_CHECK_MS after the edge ---------------------- */

/* Timer expiry functions run in ISR context */
static void shk_falling_check(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (gpio_pin_get_raw(gpio0, SHK_FALLING_PIN) == 0) {
		fsm_post_event(EVT_SHK_INACTIVE);
	}
}

static void shk_rising_check(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (gpio_pin_get_raw(gpio0, SHK_RISING_PIN) == 1) {
		fsm_post_event(EVT_SHK_ACTIVE);
	}
}

K_TIMER_DEFINE(shk_falling_timer, shk_falling_check, NULL);
K_TIMER_DEFINE(shk_rising_timer, shk_rising_check, NULL);

/* ---- common interrupt service routine ---------------------------------- */

static void gpio_int(const struct device *dev, struct gpio_callback *cb,
		     uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(SHK_FALLING_PIN)) {
		k_timer_start(&shk_falling_timer, K_MSEC(SHK_CHECK_MS), K_NO_WAIT);
	}

	if (pins & BIT(SHK_RISING_PIN)) {
		k_timer_start(&shk_rising_timer, K_MSEC(SHK_CHECK_MS), K_NO_WAIT);
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

	/* Shk falling: input with pull-up */
	ret = gpio_pin_configure(gpio0, SHK_FALLING_PIN,
				 GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		LOG_ERR("shk falling pin config failed (%d)", ret);
		return ret;
	}

	/* Shk rising: input with pull-up */
	ret = gpio_pin_configure(gpio0, SHK_RISING_PIN,
				 GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		LOG_ERR("shk rising pin config failed (%d)", ret);
		return ret;
	}

	/* Register the common ISR for all interrupt pins */
	gpio_init_callback(&gpio_int_cb, gpio_int, GPIO_INT_PIN_MASK);
	ret = gpio_add_callback(gpio0, &gpio_int_cb);
	if (ret < 0) {
		LOG_ERR("adding gpio_int callback failed (%d)", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure(gpio0, SHK_FALLING_PIN,
					   GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("shk falling IRQ config failed (%d)", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure(gpio0, SHK_RISING_PIN,
					   GPIO_INT_EDGE_RISING);
	if (ret < 0) {
		LOG_ERR("shk rising IRQ config failed (%d)", ret);
		return ret;
	}

	LOG_INF("GPIO%d shk falling / GPIO%d shk rising / GPIO%d LED initialised",
		SHK_FALLING_PIN, SHK_RISING_PIN, PICO_IO_LED_PIN);
	return 0;
}

void pico_io_led_set(bool on)
{
	gpio_pin_set(gpio0, PICO_IO_LED_PIN, on ? 1 : 0);
}
