/*
 * blueline - ring.h
 * Ring cadence generator (ring_thread) and bell state machine (bell_thread).
 */

#ifndef RING_H
#define RING_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* Maximum number of segments of one cadence */
#define RING_MAX_SEGMENTS  8

/* Ringer PWM on POT_FWD_PIN (GP2 = PWM slice 1 channel A = Zephyr channel 2) */
#define RING_PWM_CHANNEL   2
#define RING_PWM_FREQ_HZ   25
#define RING_PWM_PERIOD_NS (NSEC_PER_SEC / RING_PWM_FREQ_HZ)

/* Test feature: 1 = the bell rings constantly from power-up on (ringer_on
 * is always true, the cadence and ring_stop() are ignored). The handset
 * (shk) does not stop the bell either, so the shk line can be checked with
 * a scope while the bell rings. 0 = normal operation. */
#define RING_TEST_CONTINUOUS  0

/* BELL_TIMER durations */
#define BELL_RM_ON_MS      50   /* BELL_RM_ON:   RING_MODE high -> PWM on */
#define BELL_PWM_OFF_MS    90   /* BELL_PWM_OFF: PWM off -> RING_MODE low */

/* One segment of a ring cadence: audio tone and mechanical bell.
 * Only duration, ringer_on and skip are used so far. */
typedef struct {
	/* Duration in microseconds */
	uint32_t duration;
	/* Frequency 1 in Hz */
	double freq_1;
	/* Frequency 2 in Hz */
	double freq_2;
	/* Volume of the tone */
	double vol;
	/* Turn tone on */
	bool tone_on;
	/* Turn AC on for the ringer (input of bell_fsm) */
	bool ringer_on;
	/* Last segment, start again at the beginning */
	bool skip;
} note_t;

/* bell_fsm states */
typedef enum {
	BELL_OFF = 0,	/* RING_MODE low,  POT_FWD 100 % */
	BELL_RM_ON,	/* RING_MODE high, POT_FWD 100 %, BELL_TIMER 50 ms */
	BELL_PWM_ON,	/* RING_MODE high, POT_FWD 50 % (bell rings) */
	BELL_PWM_OFF,	/* RING_MODE high, POT_FWD 100 %, BELL_TIMER 90 ms */
	BELL_STATE_COUNT
} bell_fsm_state_t;

/* Events delivered to bell_fsm */
typedef enum {
	BELL_EVT_RINGER_ON = 0,	/* cadence segment with ringer_on = true */
	BELL_EVT_RINGER_OFF,	/* cadence segment with ringer_on = false, or ring_stop() */
	BELL_EVT_SHK_ACTIVE,	/* fsm_main received EVT_SHK_ACTIVE */
	BELL_EVT_SHK_INACTIVE,	/* fsm_main received EVT_SHK_INACTIVE */
	BELL_EVT_TIMER,		/* BELL_TIMER expired */
	BELL_EVT_COUNT
} bell_fsm_event_t;

/* Cadence played by generate_ring() */
extern note_t *melody;

/* Initialise the ringer PWM on GP2: 25 Hz, duty 100 % (POT_FWD high).
 * Returns 0 on success, negative errno otherwise. */
int ring_init(void);

/* bt_fsm entered BT_RING_BELL: generate_ring() starts the cadence. */
void ring_start(void);

/* bt_fsm left BT_RING_BELL: generate_ring() stops and blocks again,
 * ringer_on becomes false (bell_fsm turns the bell off). */
void ring_stop(void);

/* Entry point of ring_thread: plays melody[] while bt_fsm is in BT_RING_BELL. */
void generate_ring(void *p1, void *p2, void *p3);

/* Hand an event to bell_fsm. ISR-safe, never blocks. */
void bell_fsm_post_event(bell_fsm_event_t ev);

/* Entry point of bell_thread: the bell state machine. */
void bell_fsm_main(void *p1, void *p2, void *p3);

#endif /* RING_H */
