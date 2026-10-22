/*
 * blueline - main.c
 * Target: Raspberry Pi Pico 2 (RP2350), Zephyr RTOS
 * Initialises the I/O and creates all application threads.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "main.h"
#include "fsm_main.h"
#include "pico_io.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ---- fsm_thread -------------------------------------------------------- */

K_THREAD_STACK_DEFINE(fsm_thread_stack, FSM_THREAD_STACK_SIZE);
static struct k_thread fsm_thread_data;
static k_tid_t fsm_thread_id;

/* ---- further threads will be added here -------------------------------- */

int main(void)
{
	LOG_INF("blueline starting");

	if (pico_io_init() < 0) {
		LOG_ERR("I/O init failed, stopping");
		return -1;
	}

	fsm_thread_id = k_thread_create(&fsm_thread_data,
					fsm_thread_stack,
					K_THREAD_STACK_SIZEOF(fsm_thread_stack),
					fsm_main,
					NULL, NULL, NULL,
					FSM_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(fsm_thread_id, "fsm_thread");

	/* main returns; the application threads keep running */
	return 0;
}
