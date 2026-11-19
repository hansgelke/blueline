/*
 * blueline - slic.h
 * SLIC (Silvertel AG1171, J4) control.
 */

#ifndef SLIC_H
#define SLIC_H

#include <stdbool.h>

/* Pin assignment */
#define POT_FWD_PIN       2   /* PWM1 output (slice 1 A), 25 Hz, see ring.c */
#define POT_PDOWN_N_PIN   3   /* to AG1171 pin 14 via D7 (cathode at GP3):
				 GP3 low pulls the AG1171 pin low,
				 GP3 tristate: AG1171 internal pull-up */
#define RING_MODE_PIN     7   /* output, low after init, set by bell_fsm */

/* Initialise the SLIC control signals: RING_MODE (GP7) low, POT_PDOWN_N (GP3)
 * tristate (input, no pulls). With BT_UART_TX_DISABLE = 1 POT_PDOWN_N is
 * driven low instead (SLIC powered down for BM83 programming).
 * POT_FWD (GP2) is the ringer PWM (ring_init()).
 * Returns 0 on success, negative errno otherwise. */
int slic_init(void);

/* RING_MODE_PIN high (ring mode on) or low (off). */
void slic_ring_mode_set(bool on);

#endif /* SLIC_H */
