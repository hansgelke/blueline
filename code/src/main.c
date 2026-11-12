/*
 * blueline - main.c
 * Target: Raspberry Pi Pico 2 (RP2350), Zephyr RTOS
 * Initialises the I/O and creates all application threads.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "main.h"
#include "fsm_main.h"
#include "bt_fsm.h"
#include "pico_io.h"
#include "bt_uart.h"
#include "bm_83.h"
#include "slic.h"
#include "ring.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ---- fsm_thread -------------------------------------------------------- */

K_THREAD_STACK_DEFINE(fsm_thread_stack, FSM_THREAD_STACK_SIZE);
static struct k_thread fsm_thread_data;
static k_tid_t fsm_thread_id;

/* ---- bt_fsm_thread ----------------------------------------------------- */

K_THREAD_STACK_DEFINE(bt_fsm_thread_stack, BT_FSM_THREAD_STACK_SIZE);
static struct k_thread bt_fsm_thread_data;
static k_tid_t bt_fsm_thread_id;

/* ---- bt_rx_thread ------------------------------------------------------ */

K_THREAD_STACK_DEFINE(bt_rx_thread_stack, BT_RX_THREAD_STACK_SIZE);
static struct k_thread bt_rx_thread_data;
static k_tid_t bt_rx_thread_id;

/* ---- ring_thread ------------------------------------------------------- */

K_THREAD_STACK_DEFINE(ring_thread_stack, RING_THREAD_STACK_SIZE);
static struct k_thread ring_thread_data;
static k_tid_t ring_thread_id;

/* ---- further threads will be added here -------------------------------- */

int main(void)
{
	LOG_INF("blueline starting");

	/* first: drive POT_FWD (GP2, PWM1) high as early as possible */
	if (ring_init() < 0) {
		LOG_ERR("ringer PWM init failed, stopping");
		return -1;
	}

	if (pico_io_init() < 0) {
		LOG_ERR("I/O init failed, stopping");
		return -1;
	}

	if (bt_uart_init() < 0) {
		LOG_ERR("BM83 UART init failed, stopping");
		return -1;
	}

	if (slic_init() < 0) {
		LOG_ERR("SLIC init failed, stopping");
		return -1;
	}


	fsm_thread_id = k_thread_create(&fsm_thread_data,
					fsm_thread_stack,
					K_THREAD_STACK_SIZEOF(fsm_thread_stack),
					fsm_main,
					NULL, NULL, NULL,
					FSM_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(fsm_thread_id, "fsm_thread");

	bt_fsm_thread_id = k_thread_create(&bt_fsm_thread_data,
					   bt_fsm_thread_stack,
					   K_THREAD_STACK_SIZEOF(bt_fsm_thread_stack),
					   bt_fsm_main,
					   NULL, NULL, NULL,
					   BT_FSM_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(bt_fsm_thread_id, "bt_fsm_thread");

	bt_rx_thread_id = k_thread_create(&bt_rx_thread_data,
					  bt_rx_thread_stack,
					  K_THREAD_STACK_SIZEOF(bt_rx_thread_stack),
					  bt_rx_main,
					  NULL, NULL, NULL,
					  BT_RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(bt_rx_thread_id, "bt_rx_thread");

	ring_thread_id = k_thread_create(&ring_thread_data,
					 ring_thread_stack,
					 K_THREAD_STACK_SIZEOF(ring_thread_stack),
					 generate_ring,
					 NULL, NULL, NULL,
					 RING_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(ring_thread_id, "ring_thread");

	/* BM83 power-up runs last: bt_rx_thread is already running and shows
	 * the BM83 responses (ACKs, status events) as they arrive. */
	if (IS_ENABLED(BT_UART_TX_DISABLE)) {
		/* BM83 is being programmed through J3: leave it alone */
		LOG_WRN("BT_UART_TX_DISABLE: bm83_init() skipped, GP6/GP8 not driven");
	} else if (bm83_init() < 0) {
		LOG_ERR("BM83 init failed");
		return -1;
	}

	/* main returns; the application threads keep running */
	return 0;
}
