/*
 * blueline - main.h
 * Thread configuration.
 */

#ifndef MAIN_H
#define MAIN_H

/* fsm_thread */
#define FSM_THREAD_STACK_SIZE  2048
#define FSM_THREAD_PRIORITY    5

/* bt_fsm_thread: Bluetooth state machine */
#define BT_FSM_THREAD_STACK_SIZE  1024
#define BT_FSM_THREAD_PRIORITY    6

/* bt_rx_thread: checks and prints frames received from the BM83 */
#define BT_RX_THREAD_STACK_SIZE  1024
#define BT_RX_THREAD_PRIORITY    6

/* ring_thread: ring cadence generator */
#define RING_THREAD_STACK_SIZE   1024
#define RING_THREAD_PRIORITY     7

#endif /* MAIN_H */
