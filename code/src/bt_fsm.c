/*
 * blueline - bt_fsm.c
 * Bluetooth state machine running in bt_fsm_thread.
 *
 *   BT_IDLE            --BT_EVT_DIALCMPL-->       BT_MAKE_CALL
 *   BT_IDLE            --BT_EVT_CALL_INCOMING-->  BT_RING_MODE
 *   BT_RING_MODE       --BT_EVT_RING_PROTECT-->   BT_RING_BELL
 *   BT_RING_MODE       --BT_EVT_CALL_IDLE-->      BT_IDLE
 *   BT_RING_MODE       --BT_EVT_OFFHOOK-->        BT_OUT_ESTABLISHED  (RING_MODE low, MMI 0x04)
 *   BT_RING_BELL       --BT_EVT_OFFHOOK-->        BT_STOP_RING
 *   BT_RING_BELL       --BT_EVT_CALL_IDLE-->      BT_IDLE
 *   BT_STOP_RING       --BT_EVT_RING_PROTECT-->   BT_OUT_ESTABLISHED  (RING_MODE low, MMI 0x04)
 *   BT_STOP_RING       --BT_EVT_CALL_IDLE-->      BT_IDLE
 *   BT_MAKE_CALL       --BT_EVT_CALL_ACK_OK-->    BT_CALLER_WAIT
 *   BT_MAKE_CALL       --BT_EVT_CALL_ACK_FAIL-->  BT_NOHF
 *   BT_CALLER_WAIT     --BT_EVT_CALL_OUTGOING-->  BT_OUT_ESTABLISHED
 *   BT_CALLER_WAIT     --BT_EVT_MAIN_IDLE-->      BT_IDLE  (MMI 0x06 end call)
 *   BT_OUT_ESTABLISHED --BT_EVT_MAIN_IDLE-->      BT_IDLE  (MMI 0x06 end call)
 *   BT_OUT_ESTABLISHED --BT_EVT_CALL_IDLE-->      BT_IDLE
 *   every state except BT_IDLE  --BT_EVT_MAIN_IDLE-->  BT_IDLE
 *
 * Entry actions:
 *   BT_IDLE       RING_MODE_PIN low, ring_stop() (PWM_ON withdrawn)
 *   BT_MAKE_CALL  dialed_number_reg (fsm_main) as ASCII -> Make_Call (0x00)
 *   BT_RING_MODE  RING_MODE_PIN high, start ring_protect_timer (50 ms)
 *   BT_RING_BELL  ring_start() (PWM_ON: ring.c generates the cadence)
 *   BT_STOP_RING  ring_stop() (PWM_ON withdrawn, POT_FWD high at once),
 *                 start ring_protect_timer (50 ms)
 *
 * Event sources:
 *   BT_EVT_MAIN_IDLE      fsm_main, whenever it enters STATE_S0_IDLE
 *   BT_EVT_DIALCMPL       fsm_main, dialcmpl_timer expired in STATE_S1_OFFHOOK
 *   BT_EVT_CALL_ACK_OK    bt_rx_thread, Command_ACK of Make_Call, status 0x00
 *   BT_EVT_CALL_ACK_FAIL  bt_rx_thread, Command_ACK of Make_Call, status != 0x00;
 *                         bt_fsm itself if no Command_ACK arrives
 *   BT_EVT_CALL_OUTGOING  bt_rx_thread, Call_Status 0x03 or 0x04
 *   BT_EVT_CALL_IDLE      bt_rx_thread, Call_Status 0x00
 *   BT_EVT_CALL_INCOMING  bt_rx_thread, Call_Status 0x02
 *   BT_EVT_OFFHOOK        fsm_main, s0_idle -> s1_offhook (handset lifted)
 *   BT_EVT_RING_PROTECT   ring_protect_timer expired
 * The ACK result comes from bt_rx_thread (not from the return value of
 * bm83_make_call()) so it is queued before the Call_Status event that
 * follows the ACK.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "bt_fsm.h"
#include "fsm_main.h"
#include "bm_83.h"
#include "slic.h"
#include "ring.h"

LOG_MODULE_REGISTER(bt_fsm, LOG_LEVEL_INF);

/* Event queue: filled by fsm_main and bt_rx_thread, emptied by bt_fsm_thread */
K_MSGQ_DEFINE(bt_fsm_event_q, sizeof(bt_fsm_event_t), 8, 4);

static bt_fsm_state_t current_state;

static const char *const state_names[BT_STATE_COUNT] = {
	[BT_IDLE]            = "bt_idle",
	[BT_MAKE_CALL]       = "bt_make_call",
	[BT_CALLER_WAIT]     = "bt_caller_wait",
	[BT_NOHF]            = "bt_nohf",
	[BT_OUT_ESTABLISHED] = "bt_out_established",
	[BT_RING_BELL]       = "bt_ring_bell",
	[BT_RING_MODE]       = "bt_ring_mode",
	[BT_STOP_RING]       = "bt_stop_ring",
};

/* ---- RING_PROTECT timer ------------------------------------------------ */

/* Timer expiry function runs in ISR context: only post an event */
static void ring_protect_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	bt_fsm_post_event(BT_EVT_RING_PROTECT);
}

K_TIMER_DEFINE(ring_protect_timer, ring_protect_expired, NULL);

/* A stopped or restarted timer has time left / no time left: used to ignore
 * stale BT_EVT_RING_PROTECT events from an earlier run */
static bool ring_protect_expired_now(void)
{
	return k_timer_remaining_ticks(&ring_protect_timer) == 0;
}

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

/* Send dialed_number_reg as Make_Call. Digits are pulse counts: 10 = '0'. */
static void bt_fsm_make_call(void)
{
	uint8_t digits[NUMBER_ARRAY_SIZE];
	char number[NUMBER_ARRAY_SIZE];
	uint8_t len;
	uint8_t n = 0;
	int ret;

	len = fsm_get_dialed_number(digits, sizeof(digits));

	for (uint8_t i = 0; i < len; i++) {
		if (digits[i] >= 1 && digits[i] <= 9) {
			number[n++] = '0' + digits[i];
		} else if (digits[i] == 10) {
			number[n++] = '0';
		} else {
			LOG_WRN("invalid digit %u at position %u skipped", digits[i], i);
		}
	}

	/* debug: show which profiles are connected before calling */
	bm83_read_link_status();

	ret = bm83_make_call(number, n);

	/* With a Command_ACK, bt_rx_thread has already posted the result */
	if (ret == -ETIMEDOUT || (ret < 0 && ret != -EIO)) {
		bt_fsm_post_event(BT_EVT_CALL_ACK_FAIL);
	}
}

/* Answer the incoming call: ring mode off, accept call */
static void bt_fsm_answer_call(void)
{
	k_timer_stop(&ring_protect_timer);
	slic_ring_mode_set(false);
	bm83_accept_call();
}

/* Change state and execute the entry action of the new state */
static void bt_fsm_enter_state(bt_fsm_state_t new_state)
{
	LOG_INF("state %s -> %s",
		state_names[current_state], state_names[new_state]);
	current_state = new_state;

	switch (new_state) {
	case BT_IDLE:
		k_timer_stop(&ring_protect_timer);
		ring_stop();
		slic_ring_mode_set(false);
		break;
	case BT_MAKE_CALL:
		bt_fsm_make_call();
		break;
	case BT_RING_MODE:
		slic_ring_mode_set(true);
		k_timer_start(&ring_protect_timer, K_MSEC(RING_PROTECT_MS), K_NO_WAIT);
		break;
	case BT_RING_BELL:
		ring_start();
		break;
	case BT_STOP_RING:
		ring_stop();
		k_timer_start(&ring_protect_timer, K_MSEC(RING_PROTECT_MS), K_NO_WAIT);
		break;
	default:
		break;
	}
}

/* ---- state handlers ---------------------------------------------------- */

static void state_bt_idle(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_DIALCMPL:
		bt_fsm_enter_state(BT_MAKE_CALL);
		break;
	case BT_EVT_CALL_INCOMING:
		bt_fsm_enter_state(BT_RING_MODE);
		break;
	default:
		break;
	}
}

static void state_bt_make_call(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_MAIN_IDLE:
		bt_fsm_enter_state(BT_IDLE);
		break;
	case BT_EVT_CALL_ACK_OK:
		bt_fsm_enter_state(BT_CALLER_WAIT);
		break;
	case BT_EVT_CALL_ACK_FAIL:
		bt_fsm_enter_state(BT_NOHF);
		break;
	default:
		break;
	}
}

static void state_bt_caller_wait(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_MAIN_IDLE:
		/* handset on hook while the phone is dialling: end the call */
		bm83_end_call();
		bt_fsm_enter_state(BT_IDLE);
		break;
	case BT_EVT_CALL_OUTGOING:
		bt_fsm_enter_state(BT_OUT_ESTABLISHED);
		break;
	default:
		break;
	}
}

static void state_bt_nohf(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_MAIN_IDLE:
		bt_fsm_enter_state(BT_IDLE);
		break;
	default:
		break;
	}
}

static void state_bt_out_established(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_MAIN_IDLE:
		/* handset on hook: end the call on the phone */
		bm83_end_call();
		bt_fsm_enter_state(BT_IDLE);
		break;
	case BT_EVT_CALL_IDLE:
		/* the call has ended (e.g. the other side hung up) */
		bt_fsm_enter_state(BT_IDLE);
		break;
	default:
		break;
	}
}

static void state_bt_ring_mode(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_RING_PROTECT:
		if (ring_protect_expired_now()) {
			bt_fsm_enter_state(BT_RING_BELL);
		}
		break;
	case BT_EVT_CALL_IDLE:
		/* caller hung up before ringing started */
		bt_fsm_enter_state(BT_IDLE);
		break;
	case BT_EVT_OFFHOOK:
		/* handset lifted before ringing started: answer at once */
		bt_fsm_answer_call();
		bt_fsm_enter_state(BT_OUT_ESTABLISHED);
		break;
	default:
		break;
	}
}

static void state_bt_ring_bell(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_OFFHOOK:
		/* handset lifted: stop ringing */
		bt_fsm_enter_state(BT_STOP_RING);
		break;
	case BT_EVT_CALL_IDLE:
		/* caller hung up before the handset was lifted */
		bt_fsm_enter_state(BT_IDLE);
		break;
	default:
		break;
	}
}

static void state_bt_stop_ring(bt_fsm_event_t ev)
{
	switch (ev) {
	case BT_EVT_RING_PROTECT:
		if (ring_protect_expired_now()) {
			/* ringer settled: ring mode off, answer the incoming call */
			bt_fsm_answer_call();
			bt_fsm_enter_state(BT_OUT_ESTABLISHED);
		}
		break;
	case BT_EVT_CALL_IDLE:
		/* caller hung up while the ringer was stopping */
		bt_fsm_enter_state(BT_IDLE);
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

	/* After power up bt_fsm starts in BT_IDLE */
	current_state = BT_IDLE;
	LOG_INF("bt_fsm_thread started, state %s", state_names[current_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&bt_fsm_event_q, &ev, K_FOREVER);

		switch (current_state) {
		case BT_IDLE:
			state_bt_idle(ev);
			break;
		case BT_MAKE_CALL:
			state_bt_make_call(ev);
			break;
		case BT_CALLER_WAIT:
			state_bt_caller_wait(ev);
			break;
		case BT_NOHF:
			state_bt_nohf(ev);
			break;
		case BT_OUT_ESTABLISHED:
			state_bt_out_established(ev);
			break;
		case BT_RING_BELL:
			state_bt_ring_bell(ev);
			break;
		case BT_RING_MODE:
			state_bt_ring_mode(ev);
			break;
		case BT_STOP_RING:
			state_bt_stop_ring(ev);
			break;
		default:
			LOG_ERR("invalid state %d, back to bt_idle", current_state);
			bt_fsm_enter_state(BT_IDLE);
			break;
		}
	}
}
