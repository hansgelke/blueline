/*
 * blueline - ring.c
 * Ring cadence generator (ring_thread) and bell state machine (bell_thread).
 *
 * Ring cadence (generate_ring, ring_thread):
 *   Blocked until bt_fsm enters BT_RING_BELL (ring_start()), so the cadence
 *   starts with the entry into BT_RING_BELL. It walks through melody[] (max
 *   RING_MAX_SEGMENTS segments, skip = true on the last one, then it starts
 *   again) and reports each segment's ringer_on to bell_fsm. ring_stop()
 *   (bt_fsm leaves BT_RING_BELL) ends the cadence at once and reports
 *   ringer_on = false.
 *
 * Bell state machine (bell_fsm, bell_thread):
 *
 *   BELL_OFF      RING_MODE low,  POT_FWD 100 %
 *                 --ringer_on AND NOT shk_active-->  BELL_RM_ON
 *   BELL_RM_ON    RING_MODE high, POT_FWD 100 %, BELL_TIMER 50 ms
 *                 --BELL_TIMER expired-->            BELL_PWM_ON
 *   BELL_PWM_ON   RING_MODE high, POT_FWD 50 % (bell rings)
 *                 --NOT ringer_on OR shk_active-->   BELL_PWM_OFF
 *   BELL_PWM_OFF  RING_MODE high, POT_FWD 100 %, BELL_TIMER 90 ms
 *                 --BELL_TIMER expired-->            BELL_OFF
 *
 *   ringer_on:  ringer_on of the current cadence segment (false when the
 *               cadence is stopped)
 *   shk_active: last debounced shk event from fsm_main was EVT_SHK_ACTIVE
 *               (the shk pins are never read here)
 *   Conditions are checked on every input change and on entering BELL_OFF
 *   and BELL_PWM_ON, so a change during a BELL_TIMER run is not lost.
 *
 * POT_FWD_PIN (GP2) is the output of PWM1 (slice 1 channel A, pinctrl in
 * the overlay) from power-up on, 25 Hz. Duty 100 % = constantly high.
 * Duty changes take effect at the end of the current PWM period (<= 40 ms).
 *
 * freq_1, freq_2, vol and tone_on are not used yet.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "ring.h"
#include "slic.h"

LOG_MODULE_REGISTER(ring, LOG_LEVEL_INF);

static const struct device *const ring_pwm = DEVICE_DT_GET(DT_NODELABEL(pwm));

/* British ring cadence: 0.4 s on, 0.2 s off, 0.4 s on, 2 s off */
static note_t gb_ring[4] = {
	/* durat    freq_1  freq_2  vol   tone_on ringer_on skip  */
	{  400000,  450.0,  400.0,  0.5,  true,   true,     false },
	{  200000,  450.0,  400.0,  0.5,  false,  false,    false },
	{  400000,  450.0,  400.0,  0.5,  true,   true,     false },
	{ 2000000,  450.0,  400.0,  0.5,  false,  false,    true  },
};

/* Cadence played by generate_ring() */
note_t *melody = gb_ring;

/* ---- ringer PWM -------------------------------------------------------- */

/* POT_FWD duty: 50 % (bell rings) or 100 % (constantly high) */
static int ring_pwm_set(bool ringing)
{
	uint32_t pulse = ringing ? RING_PWM_PERIOD_NS / 2 : RING_PWM_PERIOD_NS;

	return pwm_set(ring_pwm, RING_PWM_CHANNEL, RING_PWM_PERIOD_NS, pulse,
		       PWM_POLARITY_NORMAL);
}

int ring_init(void)
{
	int ret;

	if (!device_is_ready(ring_pwm)) {
		LOG_ERR("pwm not ready");
		return -ENODEV;
	}

	ret = ring_pwm_set(false);
	if (ret < 0) {
		LOG_ERR("ringer PWM config failed (%d)", ret);
		return ret;
	}

	LOG_INF("PWM1 %d Hz on POT_FWD (GP2), duty 100 %%", RING_PWM_FREQ_HZ);
	return 0;
}

/* ---- ring cadence (ring_thread) ---------------------------------------- */

/* Bit of ring_event: set while bt_fsm is in BT_RING_BELL */
#define RING_ENABLE  BIT(0)

K_EVENT_DEFINE(ring_event);

/* Protects "check RING_ENABLE + report ringer_on" against ring_stop() */
K_MUTEX_DEFINE(ring_mutex);

static k_tid_t ring_tid;

void ring_start(void)
{
	LOG_INF("ring cadence start");
	k_event_post(&ring_event, RING_ENABLE);
}

void ring_stop(void)
{
	k_mutex_lock(&ring_mutex, K_FOREVER);
	k_event_clear(&ring_event, RING_ENABLE);
	bell_fsm_post_event(BELL_EVT_RINGER_OFF);
	k_mutex_unlock(&ring_mutex);

	/* end the current segment's sleep early */
	if (ring_tid != NULL) {
		k_wakeup(ring_tid);
	}
	LOG_INF("ring cadence stop");
}

void generate_ring(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int note_idx;
	bool skip_flag;

	ring_tid = k_current_get();
	LOG_INF("ring_thread started");

	while (1) {
		/* blocked until bt_fsm enters BT_RING_BELL */
		k_event_wait(&ring_event, RING_ENABLE, false, K_FOREVER);

		/* new run of the cadence */
		note_idx = 0;
		skip_flag = false;

		while ((note_idx < RING_MAX_SEGMENTS) && !skip_flag) {
			k_mutex_lock(&ring_mutex, K_FOREVER);
			if (k_event_test(&ring_event, RING_ENABLE) == 0) {
				/* ring_stop(): ringer_on already reported false */
				k_mutex_unlock(&ring_mutex);
				break;
			}
			skip_flag = melody[note_idx].skip;
			bell_fsm_post_event(melody[note_idx].ringer_on ?
					    BELL_EVT_RINGER_ON : BELL_EVT_RINGER_OFF);
			k_mutex_unlock(&ring_mutex);

			/* keep this segment for its duration (ring_stop() wakes us) */
			k_usleep(melody[note_idx].duration);

			note_idx = note_idx + 1;
		}
	}
}

/* ---- bell state machine (bell_thread) ---------------------------------- */

/* Event queue: filled by ring_thread, fsm_main and BELL_TIMER */
K_MSGQ_DEFINE(bell_event_q, sizeof(bell_fsm_event_t), 16, 4);

static bell_fsm_state_t bell_state;
static bool ringer_on;		/* ringer_on of the current cadence segment */
static bool shk_active;		/* last debounced shk event was EVT_SHK_ACTIVE */

static const char *const bell_state_names[BELL_STATE_COUNT] = {
	[BELL_OFF]     = "bell_off",
	[BELL_RM_ON]   = "bell_rm_on",
	[BELL_PWM_ON]  = "bell_pwm_on",
	[BELL_PWM_OFF] = "bell_pwm_off",
};

void bell_fsm_post_event(bell_fsm_event_t ev)
{
	/* K_NO_WAIT: safe to call from an ISR; event is dropped if the queue is full */
	if (k_msgq_put(&bell_event_q, &ev, K_NO_WAIT) != 0) {
		LOG_WRN("bell event queue full, event %d dropped", ev);
	}
}

/* BELL_TIMER expiry runs in ISR context: only post an event */
static void bell_timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	bell_fsm_post_event(BELL_EVT_TIMER);
}

K_TIMER_DEFINE(bell_timer, bell_timer_expired, NULL);

/* Ignore stale BELL_EVT_TIMER events from an earlier (stopped) timer run */
static bool bell_timer_expired_now(void)
{
	return k_timer_remaining_ticks(&bell_timer) == 0;
}

static void bell_enter_state(bell_fsm_state_t new_state);

/* Transition conditions */
static bool bell_start_condition(void)
{
	return ringer_on && !shk_active;
}

static bool bell_stop_condition(void)
{
	return !ringer_on || shk_active;
}

/* Change state and execute the entry action of the new state */
static void bell_enter_state(bell_fsm_state_t new_state)
{
	LOG_INF("bell %s -> %s", bell_state_names[bell_state],
		bell_state_names[new_state]);
	bell_state = new_state;

	switch (new_state) {
	case BELL_OFF:
		ring_pwm_set(false);
		slic_ring_mode_set(false);
		/* the cadence may already ask for the next ring */
		if (bell_start_condition()) {
			bell_enter_state(BELL_RM_ON);
		}
		break;
	case BELL_RM_ON:
		ring_pwm_set(false);
		slic_ring_mode_set(true);
		k_timer_start(&bell_timer, K_MSEC(BELL_RM_ON_MS), K_NO_WAIT);
		break;
	case BELL_PWM_ON:
		slic_ring_mode_set(true);
		ring_pwm_set(true);
		/* ringer_on / shk may have changed during BELL_RM_ON */
		if (bell_stop_condition()) {
			bell_enter_state(BELL_PWM_OFF);
		}
		break;
	case BELL_PWM_OFF:
		ring_pwm_set(false);
		slic_ring_mode_set(true);
		k_timer_start(&bell_timer, K_MSEC(BELL_PWM_OFF_MS), K_NO_WAIT);
		break;
	default:
		break;
	}
}

void bell_fsm_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bell_fsm_event_t ev;

	/* After power up: BELL_OFF (RING_MODE low, POT_FWD 100 % set by init) */
	bell_state = BELL_OFF;
	LOG_INF("bell_thread started, state %s", bell_state_names[bell_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&bell_event_q, &ev, K_FOREVER);

		/* update the inputs */
		switch (ev) {
		case BELL_EVT_RINGER_ON:
			ringer_on = true;
			break;
		case BELL_EVT_RINGER_OFF:
			ringer_on = false;
			break;
		case BELL_EVT_SHK_ACTIVE:
			shk_active = true;
			break;
		case BELL_EVT_SHK_INACTIVE:
			shk_active = false;
			break;
		default:
			break;
		}

		switch (bell_state) {
		case BELL_OFF:
			if (bell_start_condition()) {
				bell_enter_state(BELL_RM_ON);
			}
			break;
		case BELL_RM_ON:
			if (ev == BELL_EVT_TIMER && bell_timer_expired_now()) {
				bell_enter_state(BELL_PWM_ON);
			}
			break;
		case BELL_PWM_ON:
			if (bell_stop_condition()) {
				bell_enter_state(BELL_PWM_OFF);
			}
			break;
		case BELL_PWM_OFF:
			if (ev == BELL_EVT_TIMER && bell_timer_expired_now()) {
				bell_enter_state(BELL_OFF);
			}
			break;
		default:
			LOG_ERR("invalid bell state %d, back to bell_off", bell_state);
			bell_enter_state(BELL_OFF);
			break;
		}
	}
}
