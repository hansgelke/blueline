/*
 * blueline - fsm_main.h
 * State machine running in fsm_thread.
 */

#ifndef FSM_MAIN_H
#define FSM_MAIN_H

/* FSM timer timeouts */
#define PULSE_TIMER_MS    500
#define HANGUP_TIMER_MS   500

/* FSM states */
typedef enum {
	STATE_S0_IDLE = 0,
	STATE_S1_OFFHOOK,
	STATE_S2_OPEN,
	STATE_S3_CLOSED,
	STATE_COUNT
} fsm_state_t;

/* Events delivered to the FSM (from ISRs and timer expiry functions) */
typedef enum {
	EVT_SHK_ACTIVE = 0,
	EVT_SHK_INACTIVE,
	EVT_PULSE_TIMEOUT,
	EVT_HANGUP_TIMEOUT,
	FSM_EV_COUNT
} fsm_event_t;

/* Entry point of fsm_thread (signature as required by k_thread_create). */
void fsm_main(void *p1, void *p2, void *p3);

/* Hand an event to the FSM. ISR-safe, never blocks. */
void fsm_post_event(fsm_event_t ev);

/* Returns the current state (for debugging / other threads later). */
fsm_state_t fsm_get_state(void);

#endif /* FSM_MAIN_H */
