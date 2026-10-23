/*
 * blueline - fsm_main.h
 * State machine running in fsm_thread.
 */

#ifndef FSM_MAIN_H
#define FSM_MAIN_H

/* FSM states. C identifiers cannot start with a digit, so "1_idle" becomes STATE_1_IDLE. */
typedef enum {
	STATE_1_IDLE = 0,
	STATE_2_OFFHOOK,
	STATE_COUNT
} fsm_state_t;

/* Events delivered to the FSM (e.g. from interrupt service routines) */
typedef enum {
	FSM_EV_SHK_PRESSED = 0,
	FSM_EV_COUNT
} fsm_event_t;

/* Entry point of fsm_thread (signature as required by k_thread_create). */
void fsm_main(void *p1, void *p2, void *p3);

/* Hand an event to the FSM. ISR-safe, never blocks. */
void fsm_post_event(fsm_event_t ev);

/* Returns the current state (for debugging / other threads later). */
fsm_state_t fsm_get_state(void);

#endif /* FSM_MAIN_H */
