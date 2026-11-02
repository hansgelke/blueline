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
 *
 * Dial pulses:
 *   - entering s3_closed:  pulse_counter++
 *   - s1_offhook -> s2_open:  pulse_counter = 0, stop dialcmpl_timer
 *   - s3_closed -> s1_offhook:  store pulse_counter in number_queue,
 *                               start dialcmpl_timer
 *   - s0_idle -> s1_offhook and EVT_DIALCMPL:  number_queue pointer back to
 *                                              the first digit
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

/* Number of times s3_closed was entered since the last s1_offhook -> s2_open */
static uint8_t pulse_counter;

/* Dialled digits (pulse_counter values) and the position of the next digit */
static uint8_t number_queue[NUMBER_QUEUE_SIZE];
static uint8_t number_queue_pos;

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

static void dialcmpl_timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	fsm_post_event(EVT_DIALCMPL);
}

K_TIMER_DEFINE(pulse_timer, pulse_timer_expired, NULL);
K_TIMER_DEFINE(hangup_timer, hangup_timer_expired, NULL);
K_TIMER_DEFINE(dialcmpl_timer, dialcmpl_timer_expired, NULL);

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

/* Debug: print all digits stored in number_queue so far */
static void number_queue_log(void)
{
	/* up to "10 " per digit plus terminator */
	char buf[NUMBER_QUEUE_SIZE * 3 + 1];
	int len = 0;

	buf[0] = '\0';
	for (uint8_t i = 0; i < number_queue_pos; i++) {
		len += snprintk(&buf[len], sizeof(buf) - len, "%u ", number_queue[i]);
	}

	LOG_INF("number_queue (%u): %s", number_queue_pos, buf);
}

/* Store a dialled digit at the current queue position */
static void number_queue_put(uint8_t digit)
{
	if (number_queue_pos >= NUMBER_QUEUE_SIZE) {
		LOG_WRN("number_queue full, digit %u dropped", digit);
		return;
	}

	number_queue[number_queue_pos] = digit;
	LOG_INF("digit %u stored at position %u", digit, number_queue_pos);
	number_queue_pos++;
	number_queue_log();
}

/* Set the queue pointer back to the first digit */
static void number_queue_reset(void)
{
	number_queue_pos = 0;
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
		pulse_counter++;
		break;
	default:
		break;
	}
}

/* Transition s3_closed -> s1_offhook: one digit has been dialled */
static void fsm_s3_to_s1(void)
{
	number_queue_put(pulse_counter);
	k_timer_start(&dialcmpl_timer, K_MSEC(DIALCMPL_TIMER_MS), K_NO_WAIT);
	fsm_enter_state(STATE_S1_OFFHOOK);
}

/* ---- state handlers ---------------------------------------------------- */

static void state_s0_idle(fsm_event_t ev)
{
	switch (ev) {
	case EVT_SHK_ACTIVE:
		number_queue_reset();
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
		pulse_counter = 0;
		k_timer_stop(&dialcmpl_timer);
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
			fsm_s3_to_s1();
			state_s1_offhook(ev);
		}
		break;
	case EVT_PULSE_TIMEOUT:
		if (timer_expired(&pulse_timer)) {
			fsm_s3_to_s1();
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

		/* EVT_DIALCMPL is handled in every state */
		if (ev == EVT_DIALCMPL) {
			if (!timer_expired(&dialcmpl_timer)) {
				continue;	/* stale, dialcmpl_timer was restarted */
			}
			number_queue_reset();
		}

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
