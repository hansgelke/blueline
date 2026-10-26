/*
 * blueline - pico_io.h
 * GPIO setup for the Raspberry Pi Pico 2 (RP2350): shk and user LED.
 */

#ifndef PICO_IO_H
#define PICO_IO_H

#include <stdbool.h>

/* Pin assignment */
#define SHK_FALLING_PIN   15   /* shk, internal pull-up, IRQ on falling edge */
#define SHK_RISING_PIN    14   /* shk, internal pull-up, IRQ on rising edge */
#define PICO_IO_LED_PIN   25   /* user LED on the Pico 2, active high */

/* Time after an shk edge at which the pin level is checked */
#define SHK_CHECK_MS      10

/* Initialise shk pins (input, pull-up, edge IRQs) and LED (output, off).
 * Returns 0 on success, negative errno otherwise. */
int pico_io_init(void);

/* Switch the user LED on (true) or off (false). */
void pico_io_led_set(bool on);

#endif /* PICO_IO_H */
