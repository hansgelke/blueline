/*
 * blueline - fsm_main.c
 * State machine running in fsm_thread.
 *
 *   s0_idle     --EVT_SHK_ACTIVE-->                         s1_offhook  (LED on)
 *   s1_offhook  --EVT_SHK_INACTIVE-->                       s2_open     (start hangup_timer)
 *   s2_open     --EVT_SHK_ACTIVE, hangup_timer running-->   s3_closed   (start pulse_timer)
 *   s2_open     --hangup_timer expired-->                   s0_idle
 *   s3_closed   --EVT_SHK_INACTIVE, pulse_timer running-->  s2_open     (start hangup_timer)
 *   s3_closed   --pulse_timer expired-->                    s1_offhook
 *
 * The LED is on only in s1_offhook.
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
	[STATE_S0_IDLE]    = "s0_idle",
	[STATE_S1_OFFHOOK] = "s1_offhook",
	[STATE_S2_OPEN]    = "s2_open",
	[STATE_S3_CLOSED]  = "s3_closed",
};

/* ---- timers ------------------------------------------------------------ */

/* Timer expiry functions run in ISR context: only post an event */
static void pulse_timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	fsm_post_event(EVT_PULSE_TIMEOUT);
}

static void hangup_timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	fsm_post_event(EVT_HANGUP_TIMEOUT);
}

K_TIMER_DEFINE(pulse_timer, pulse_timer_expired, NULL);
K_TIMER_DEFINE(hangup_timer, hangup_timer_expired, NULL);

/* A timer that is not running (expired or never started) has no time left.
 * Used to ignore stale timeout events from an earlier run of a timer. */
static bool timer_expired(struct k_timer *timer)
{
	return k_timer_remaining_ticks(timer) == 0;
}

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
	case STATE_S0_IDLE:
		pico_io_led_set(false);
		break;
	case STATE_S1_OFFHOOK:
		pico_io_led_set(true);
		break;
	case STATE_S2_OPEN:
		pico_io_led_set(false);
		k_timer_start(&hangup_timer, K_MSEC(HANGUP_TIMER_MS), K_NO_WAIT);
		break;
	case STATE_S3_CLOSED:
		pico_io_led_set(false);
		k_timer_start(&pulse_timer, K_MSEC(PULSE_TIMER_MS), K_NO_WAIT);
		break;
	default:
		break;
	}
}

/* ---- state handlers ---------------------------------------------------- */

static void state_s0_idle(fsm_event_t ev)
{
	switch (ev) {
	case EVT_SHK_ACTIVE:
		fsm_enter_state(STATE_S1_OFFHOOK);
		break;
	default:
		break;
	}
}

static void state_s1_offhook(fsm_event_t ev)
{
	switch (ev) {
	case EVT_SHK_INACTIVE:
		fsm_enter_state(STATE_S2_OPEN);
		break;
	default:
		break;
	}
}

static void state_s2_open(fsm_event_t ev)
{
	switch (ev) {
	case EVT_SHK_ACTIVE:
		if (!timer_expired(&hangup_timer)) {
			fsm_enter_state(STATE_S3_CLOSED);
		} else {
			/* hangup_timer expired, its timeout event is still queued */
			fsm_enter_state(STATE_S0_IDLE);
			state_s0_idle(ev);
		}
		break;
	case EVT_HANGUP_TIMEOUT:
		if (timer_expired(&hangup_timer)) {
			fsm_enter_state(STATE_S0_IDLE);
		}
		break;
	default:
		break;
	}
}

static void state_s3_closed(fsm_event_t ev)
{
	switch (ev) {
	case EVT_SHK_INACTIVE:
		if (!timer_expired(&pulse_timer)) {
			fsm_enter_state(STATE_S2_OPEN);
		} else {
			/* pulse_timer expired, its timeout event is still queued */
			fsm_enter_state(STATE_S1_OFFHOOK);
			state_s1_offhook(ev);
		}
		break;
	case EVT_PULSE_TIMEOUT:
		if (timer_expired(&pulse_timer)) {
			fsm_enter_state(STATE_S1_OFFHOOK);
		}
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

	/* After power up the FSM starts in state s0_idle, LED off */
	current_state = STATE_S0_IDLE;
	pico_io_led_set(false);
	LOG_INF("fsm_thread started, state %s", state_names[current_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&fsm_event_q, &ev, K_FOREVER);

		switch (current_state) {
		case STATE_S0_IDLE:
			state_s0_idle(ev);
			break;
		case STATE_S1_OFFHOOK:
			state_s1_offhook(ev);
			break;
		case STATE_S2_OPEN:
			state_s2_open(ev);
			break;
		case STATE_S3_CLOSED:
			state_s3_closed(ev);
			break;
		default:
			LOG_ERR("invalid state %d, back to s0_idle", current_state);
			fsm_enter_state(STATE_S0_IDLE);
			break;
		}
	}
}
