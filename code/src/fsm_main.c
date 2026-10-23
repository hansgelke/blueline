/*
 * blueline - fsm_main.c
 * State machine running in fsm_thread.
 *
 *   1_idle     --shk pushed-->  2_offhook   (LED on)
 *   2_offhook  --shk pushed-->  1_idle      (LED off)
 *   shk released: no action
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "fsm_main.h"
#include "pico_io.h"

LOG_MODULE_REGISTER(fsm_main, LOG_LEVEL_INF);

/* Event queue: filled by ISRs, emptied by fsm_thread */
K_MSGQ_DEFINE(fsm_event_q, sizeof(fsm_event_t), 8, 4);

static fsm_state_t current_state;

static const char *const state_names[STATE_COUNT] = {
	[STATE_1_IDLE]    = "1_idle",
	[STATE_2_OFFHOOK] = "2_offhook",
};

/* ---- helpers ----------------------------------------------------------- */

void fsm_post_event(fsm_event_t ev)
{
	/* K_NO_WAIT: safe to call from an ISR; event is dropped if the queue is full */
	if (k_msgq_put(&fsm_event_q, &ev, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, event %d dropped", ev);
	}
}

fsm_state_t fsm_get_state(void)
{
	return current_state;
}

/* Change state and execute the entry action of the new state */
static void fsm_enter_state(fsm_state_t new_state)
{
	LOG_INF("state %s -> %s",
		state_names[current_state], state_names[new_state]);
	current_state = new_state;

	switch (new_state) {
	case STATE_1_IDLE:
		pico_io_led_set(false);
		break;
	case STATE_2_OFFHOOK:
		pico_io_led_set(true);
		break;
	default:
		break;
	}
}

/* ---- state handlers ---------------------------------------------------- */

static void state_1_idle(fsm_event_t ev)
{
	switch (ev) {
	case FSM_EV_SHK_PRESSED:
		fsm_enter_state(STATE_2_OFFHOOK);
		break;
	default:
		break;
	}
}

static void state_2_offhook(fsm_event_t ev)
{
	switch (ev) {
	case FSM_EV_SHK_PRESSED:
		fsm_enter_state(STATE_1_IDLE);
		break;
	default:
		break;
	}
}

/* ---- thread entry ------------------------------------------------------ */

void fsm_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	fsm_event_t ev;

	/* After power up the FSM starts in state 1_idle, LED off */
	current_state = STATE_1_IDLE;
	pico_io_led_set(false);
	LOG_INF("fsm_thread started, state %s", state_names[current_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&fsm_event_q, &ev, K_FOREVER);

		switch (current_state) {
		case STATE_1_IDLE:
			state_1_idle(ev);
			break;
		case STATE_2_OFFHOOK:
			state_2_offhook(ev);
			break;
		default:
			LOG_ERR("invalid state %d, back to 1_idle", current_state);
			fsm_enter_state(STATE_1_IDLE);
			break;
		}
	}
}
