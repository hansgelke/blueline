/*
 * blueline - bt_fsm.c
 * Bluetooth state machine running in bt_fsm_thread.
 *
 *   BT_S0_IDLE        --BT_EVT_DIALCMPL-->   BT_S1_CALLERWAIT
 *   BT_S1_CALLERWAIT  --BT_EVT_MAIN_IDLE-->  BT_S0_IDLE
 *
 * Both events come from fsm_main: BT_EVT_MAIN_IDLE whenever fsm_main enters
 * STATE_S0_IDLE (this forces bt_fsm to BT_S0_IDLE), BT_EVT_DIALCMPL when
 * dialcmpl_timer expires while fsm_main is in STATE_S1_OFFHOOK.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "bt_fsm.h"

LOG_MODULE_REGISTER(bt_fsm, LOG_LEVEL_INF);

/* Event queue: filled by fsm_main, emptied by bt_fsm_thread */
K_MSGQ_DEFINE(bt_fsm_event_q, sizeof(bt_fsm_event_t), 8, 4);

static bt_fsm_state_t current_state;

static const char *const state_names[BT_STATE_COUNT] = {
	[BT_S0_IDLE]       = "bt_s0_idle",
	[BT_S1_CALLERWAIT] = "bt_s1_callerwait",
};

/* ---- helpers ----------------------------------------------------------- */

void bt_fsm_post_event(bt_fsm_event_t ev)
{
	/* K_NO_WAIT: safe to call from an ISR; event is dropped if the queue is full */
	if (k_msgq_put(&bt_fsm_event_q, &ev, K_NO_WAIT) != 0) {
		LOG_WRN("event queue full, event %d dropped", ev);
	}
}

bt_fsm_state_t bt_fsm_get_state(void)
{
	return current_state;
}

/* Change state and execute the entry action of the new state */
static void bt_fsm_enter_state(bt_fsm_state_t new_state)
{
	LOG_INF("state %s -> %s",
		state_names[current_state], state_names[new_state]);
	current_state = new_state;

	/* no entry actions yet */
}

/* ---- state handlers ---------------------------------------------------- */

static void state_bt_s0_idle(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_DIALCMPL:
		bt_fsm_enter_state(BT_S1_CALLERWAIT);
		break;
	default:
		break;
	}
}

static void state_bt_s1_callerwait(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_MAIN_IDLE:
		bt_fsm_enter_state(BT_S0_IDLE);
		break;
	default:
		break;
	}
}

/* ---- thread entry ------------------------------------------------------ */

void bt_fsm_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bt_fsm_event_t ev;

	/* After power up bt_fsm starts in BT_S0_IDLE */
	current_state = BT_S0_IDLE;
	LOG_INF("bt_fsm_thread started, state %s", state_names[current_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&bt_fsm_event_q, &ev, K_FOREVER);

		switch (current_state) {
		case BT_S0_IDLE:
			state_bt_s0_idle(ev);
			break;
		case BT_S1_CALLERWAIT:
			state_bt_s1_callerwait(ev);
			break;
		default:
			LOG_ERR("invalid state %d, back to bt_s0_idle", current_state);
			bt_fsm_enter_state(BT_S0_IDLE);
			break;
		}
	}
}
