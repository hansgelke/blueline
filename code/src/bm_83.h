/*
 * blueline - bm_83.h
 * Microchip BM83 Bluetooth module: control signals, power-up sequence and
 * commands sent over the UART (bt_uart.c).
 */

#ifndef BM_83_H
#define BM_83_H

#include <stdint.h>
#include <zephyr/sys/util.h>

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
#define BM83_CMD_MAKE_CALL       0x00
#define BM83_CMD_MMI_ACTION      0x02
#define BM83_CMD_READ_BTM_VERSION  0x08
#define BM83_CMD_READ_LINK_STATUS  0x0D
#define BM83_CMD_EVENT_ACK         0x14
#define BM83_CMD_SET_OVERALL_GAIN  0x23

/* Event opcodes */
#define BM83_EVT_COMMAND_ACK       0x00
#define BM83_EVT_BTM_STATUS        0x01
#define BM83_EVT_CALL_STATUS       0x02
#define BM83_EVT_LINK_STATUS_REPLY 0x1E

/* BTM_Status states that start the gain setting */
#define BM83_STATUS_HF_LINK        0x05   /* HF/HS link established */
#define BM83_STATUS_A2DP_LINK      0x06   /* A2DP link established */

/* Call_Status states */
#define BM83_CALL_IDLE             0x00   /* idle, no call */
#define BM83_CALL_INCOMING         0x02   /* incoming call */
#define BM83_CALL_OUTGOING         0x03   /* outgoing call */
#define BM83_CALL_ACTIVE           0x04   /* call active */

/* Database index of all commands with a Data_Base_Index parameter */
#define BM83_DATABASE_INDEX        0x00

/* Make_Call: maximum length of the phone number (ASCII digits) */
#define BM83_PHONE_NUMBER_MAX      19

/* Set_Overall_Gain: mask bits, type, levels */
#define BM83_GAIN_MASK_A2DP        BIT(0)
#define BM83_GAIN_MASK_HF          BIT(1)
#define BM83_GAIN_MASK_LINE_IN     BIT(2)
#define BM83_GAIN_TYPE_ABS_LEVEL   0x03   /* set absolute gain level 0..15 */
#define BM83_GAIN_LEVEL_MAX        0x0F

/* Gain levels set after the phone has connected (0..15) */
#define BM83_A2DP_GAIN_LEVEL       BM83_GAIN_LEVEL_MAX
#define BM83_HF_GAIN_LEVEL         BM83_GAIN_LEVEL_MAX

/* Command_ACK status */
#define BM83_ACK_COMMAND_COMPLETE  0x00

/* MMI actions */
#define BM83_MMI_POWER_ON_PRESS    0x51
#define BM83_MMI_POWER_ON_RELEASE  0x52
#define BM83_MMI_ENTER_PAIRING     0x5D   /* fast enter pairing mode (from non-off) */
#define BM83_MMI_ACCEPT_CALL       0x04   /* accept an incoming call */
#define BM83_MMI_END_CALL          0x06   /* end call / transfer audio to phone */

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

/* Set_Overall_Gain (opcode 0x23): data_base_index 0x00, mask (BM83_GAIN_MASK_*),
 * type (e.g. BM83_GAIN_TYPE_ABS_LEVEL), gain for A2DP, HF and line-in.
 * The phone may later change the gain again (AVRCP absolute volume). */
int bm83_set_overall_gain(uint8_t mask, uint8_t type, uint8_t a2dp,
			  uint8_t hf, uint8_t line_in);

/* Read_Link_Status (opcode 0x0D): the reply (event 0x1E) is decoded and
 * printed by bm83_handle_event(): device state and, per database, which
 * profiles (A2DP, AVRCP, HF, SPP) are connected. */
int bm83_read_link_status(void);

/* End the active call: MMI action 0x06 "End call / transfer audio to phone". */
int bm83_end_call(void);

/* Accept the incoming call: MMI action 0x04 "Accept an incoming call". */
int bm83_accept_call(void);

/* Make_Call (opcode 0x00): data_base_index 0x00, phone number as ASCII
 * digits (len <= BM83_PHONE_NUMBER_MAX). The Command_ACK result is also
 * posted to bt_fsm (BT_EVT_CALL_ACK_OK / BT_EVT_CALL_ACK_FAIL) by
 * bt_rx_thread. */
int bm83_make_call(const char *number, uint8_t len);

/* Called by bt_rx_thread for every received event except Command_ACK:
 * starts the gain setting when a link is established, forwards Call_Status
 * (incoming / outgoing / active / idle) to bt_fsm, prints Read_Link_Status_Reply. params[] are the event parameters. */
void bm83_handle_event(uint8_t opcode, const uint8_t *params, uint8_t num_params);

/* Called by bt_rx_thread when a link to the phone is established: sets
 * BM83_A2DP_GAIN_LEVEL / BM83_HF_GAIN_LEVEL from the system workqueue
 * (bt_rx_thread itself must not send commands). */
void bm83_link_established(void);

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
