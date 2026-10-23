/*
 * blueline - pico_io.h
 * GPIO setup for the Raspberry Pi Pico 2 (RP2350): shk and user LED.
 */

#ifndef PICO_IO_H
#define PICO_IO_H

#include <stdbool.h>

/* Pin assignment */
#define PICO_IO_SHK_PIN   15   /* shk to GND, internal pull-up, active low */
#define PICO_IO_LED_PIN   25   /* user LED on the Pico 2, active high */

/* Initialise shk (input, pull-up, IRQ on falling edge) and LED (output, off).
 * Returns 0 on success, negative errno otherwise. */
int pico_io_init(void);

/* Switch the user LED on (true) or off (false). */
void pico_io_led_set(bool on);

#endif /* PICO_IO_H */
