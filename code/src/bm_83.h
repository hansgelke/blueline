/*
 * blueline - bm_83.h
 * Microchip BM83 Bluetooth module: control signals, power-up sequence and
 * commands sent over the UART (bt_uart.c).
 */

#ifndef BM_83_H
#define BM_83_H

#include <stdint.h>

/* Pin assignment */
#define BT_RST_N_PIN   6   /* output, BM83 reset, active low */
#define BT_MFB_PIN     8   /* output, BM83 multi-function button */

/* Power-up sequence timing */
#define BM83_MFB_HIGH_DELAY_MS   100   /* init -> bt_mfb high */
#define BM83_RST_HIGH_DELAY_MS   20    /* bt_mfb high -> bt_rst_n high */
#define BM83_CMD_DELAY_MS        500   /* bt_rst_n high -> first command */

/* MFB handshake (Audio UART Command Set 4.4/4.5) */
#define BM83_MFB_LEAD_MS          3     /* bt_mfb high -> UART data (> 2 ms) */
#define BM83_CMD_ACK_TIMEOUT_MS   200   /* command -> Command_ACK */

/* Command opcodes */
#define BM83_CMD_MMI_ACTION      0x02
#define BM83_CMD_READ_BTM_VERSION  0x08
#define BM83_CMD_EVENT_ACK         0x14

/* Event opcodes */
#define BM83_EVT_COMMAND_ACK       0x00

/* Command_ACK status */
#define BM83_ACK_COMMAND_COMPLETE  0x00

/* MMI actions */
#define BM83_MMI_POWER_ON_PRESS    0x51
#define BM83_MMI_POWER_ON_RELEASE  0x52
#define BM83_MMI_ENTER_PAIRING     0x5D   /* fast enter pairing mode (from non-off) */

/* Read_BTM_Version types (reply: event 0x18 Read_BTM_Version_Reply) */
#define BM83_VERSION_UART          0x00
#define BM83_VERSION_FW            0x01

/* Initialise bt_rst_n and bt_mfb, run the power-up sequence, power the
 * BM83 on with MMI actions 0x51/0x52 and request its firmware version
 * (Read_BTM_Version) and put it into pairing mode. bt_mfb is low afterwards.
 * Blocks for about 650 ms.
 * Returns 0 on success, negative errno otherwise. */
int bm83_init(void);

/* Commands below are sent with the MFB handshake and wait for their
 * Command_ACK (max BM83_CMD_ACK_TIMEOUT_MS). They must not be called from
 * bt_rx_thread. Return 0 on "command complete", -EIO for another ACK status,
 * -ETIMEDOUT without ACK, other negative errno on errors. */

/* Send an MMI action command (opcode 0x02, data_base_index 0x00, action). */
int bm83_mmi_action(uint8_t action);

/* Put the BM83 into pairing mode (discoverable), MMI action 0x5D with a
 * pairing tone. Result: BTM_Status 0x01 (pairing), later 0x03 (pairing
 * successful) or 0x04 (pairing failed). */
int bm83_enter_pairing(void);

/* Send a Read_BTM_Version command (opcode 0x08) with the given type
 * (BM83_VERSION_UART or BM83_VERSION_FW). */
int bm83_read_btm_version(uint8_t type);

/* Acknowledge a received BM83 event (Event_ACK, opcode 0x14, param event_id).
 * Without it the BM83 waits 800 ms before it sends the next event
 * (Audio UART Command Set 4.5.2). Command_ACK events are not acknowledged.
 * Sent with the MFB handshake, does not wait for a reply.
 * Returns 0 on success, negative errno otherwise. */
int bm83_event_ack(uint8_t event_id);

/* Called by bt_rx_thread for every received Command_ACK: wakes up the
 * command waiting for it. */
void bm83_command_ack_received(uint8_t cmd_id, uint8_t status);

#endif /* BM_83_H */
