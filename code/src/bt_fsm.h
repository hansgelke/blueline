/*
 * blueline - bt_fsm.h
 * Bluetooth state machine running in bt_fsm_thread.
 */

#ifndef BT_FSM_H
#define BT_FSM_H

/* RING_PROTECT timer: settle time around ring mode switching */
#define RING_PROTECT_MS   50

/* bt_fsm states */
typedef enum {
	BT_IDLE = 0,
	BT_MAKE_CALL,
	BT_CALLER_WAIT,
	BT_NOHF,
	BT_OUT_ESTABLISHED,
	BT_RING_BELL,
	BT_RING_MODE,
	BT_STOP_RING,
	BT_STATE_COUNT
} bt_fsm_state_t;

/* Events delivered to bt_fsm */
typedef enum {
	BT_EVT_MAIN_IDLE = 0,	/* fsm_main entered STATE_S0_IDLE */
	BT_EVT_DIALCMPL,	/* dialcmpl_timer expired while fsm_main in STATE_S1_OFFHOOK */
	BT_EVT_CALL_ACK_OK,	/* Command_ACK of Make_Call with status 0x00 */
	BT_EVT_CALL_ACK_FAIL,	/* Command_ACK of Make_Call with status != 0x00, or no ACK */
	BT_EVT_CALL_OUTGOING,	/* Call_Status: outgoing call (0x03) or call active (0x04) */
	BT_EVT_CALL_IDLE,	/* Call_Status: idle (0x00), the call has ended */
	BT_EVT_CALL_INCOMING,	/* Call_Status: incoming call (0x02) */
	BT_EVT_OFFHOOK,		/* fsm_main: s0_idle -> s1_offhook (handset lifted) */
	BT_EVT_RING_PROTECT,	/* ring_protect_timer expired */
	BT_EVT_COUNT
} bt_fsm_event_t;

/* Entry point of bt_fsm_thread (signature as required by k_thread_create). */
void bt_fsm_main(void *p1, void *p2, void *p3);

/* Hand an event to bt_fsm. ISR-safe, never blocks. */
void bt_fsm_post_event(bt_fsm_event_t ev);

/* Returns the current bt_fsm state. */
bt_fsm_state_t bt_fsm_get_state(void);

#endif /* BT_FSM_H */
