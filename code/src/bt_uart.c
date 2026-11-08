/*
 * blueline - bt_uart.c
 * UART1 link to the Microchip BM83 Bluetooth module (115200 baud, 8N1,
 * no flow control, TX = GPIO4, RX = GPIO5).
 *
 * Transmit: bt_uart_build_frame() builds a frame, bt_uart_send() sends it
 * (mutex protected, it is used by several threads).
 * The BM83 commands themselves are in bm_83.c.
 *
 * Receive: the UART interrupt bt_uart_isr collects the bytes of one frame
 * and hands the complete raw frame to bt_rx_thread through bt_rx_q.
 * bt_rx_thread checks the checksum first, then stores header, opcode and
 * parameters in rx_buffer (rest filled with 0x00) and prints them together
 * with the BM83 event name of the opcode. Every event except Command_ACK is
 * acknowledged with Event_ACK (bm83_event_ack()).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "bt_uart.h"
#include "bm_83.h"

LOG_MODULE_REGISTER(bt_uart, LOG_LEVEL_INF);

static const struct device *const bt_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* Complete raw frames: filled by bt_uart_isr, emptied by bt_rx_thread */
K_MSGQ_DEFINE(bt_rx_q, BT_UART_RX_BUF_SIZE, 4, 1);

/* Receive buffer array: header, opcode, parameters, rest 0x00 */
static uint8_t rx_buffer[BT_UART_RX_BUF_SIZE];

/* ---- checksum ---------------------------------------------------------- */

/* checksum = 1 + ~(length(h) + length(l) + opcode + parameters).
 * bytes[] starts at length(h), n = number of bytes to add up. */
static uint8_t bt_uart_checksum(const uint8_t *bytes, size_t n)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < n; i++) {
		sum += bytes[i];
	}

	return (uint8_t)(1 + ~sum);
}

/* ---- transmit ---------------------------------------------------------- */

int bt_uart_build_frame(uint8_t *frame, size_t size, uint8_t opcode,
			const uint8_t *params, uint8_t num_params)
{
	uint16_t length = 1 + num_params;	/* opcode + parameters */
	size_t frame_len = 4 + num_params + 1;

	if (num_params > BT_FRAME_MAX_PARAMS || size < frame_len) {
		return -EINVAL;
	}

	frame[0] = BT_FRAME_HEADER;
	frame[1] = length >> 8;
	frame[2] = length & 0xFF;
	frame[3] = opcode;
	for (uint8_t i = 0; i < num_params; i++) {
		frame[4 + i] = params[i];
	}
	/* checksum over length(h) .. last parameter */
	frame[frame_len - 1] = bt_uart_checksum(&frame[1], frame_len - 2);

	return frame_len;
}

/* bt_uart_send() is called from several threads (main: commands,
 * bt_rx_thread: Event_ACK); the mutex keeps the frames from mixing */
K_MUTEX_DEFINE(bt_tx_mutex);

void bt_uart_send(const uint8_t *data, size_t len)
{
	if (IS_ENABLED(BT_UART_TX_DISABLE)) {
		return;		/* TX pin is tristate */
	}

	k_mutex_lock(&bt_tx_mutex, K_FOREVER);
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(bt_uart, data[i]);
	}
	k_mutex_unlock(&bt_tx_mutex);
}

/* ---- receive ----------------------------------------------------------- */

/* BM83 UART event names, indexed by opcode (NULL = not defined) */
static const char *const bm83_event_names[] = {
	[0x00] = "Command_ACK",
	[0x01] = "BTM_Status",
	[0x02] = "Call_Status",
	[0x03] = "Caller_ID",
	[0x04] = "SMS_Received_Indication",
	[0x05] = "Missed_Call_Indication",
	[0x06] = "Phone_Max_Battery_Level",
	[0x07] = "Phone_Current_Battery_Level",
	[0x08] = "Roaming_Status",
	[0x09] = "Phone_Max_Signal_Strength_Level",
	[0x0A] = "Phone_Current_Signal_Strength_Level",
	[0x0B] = "Phone_Service_Status",
	[0x0C] = "BTM_Battery_Status",
	[0x0D] = "BTM_Charging_Status",
	[0x0E] = "Reset_To_Default",
	[0x0F] = "Report_HF_Gain_Level",
	[0x10] = "EQ_Mode_Indication",
	[0x17] = "Read_Linked_Device_Information_Reply",
	[0x18] = "Read_BTM_Version_Reply",
	[0x19] = "Call_List_Report",
	[0x1A] = "AVC_Specific_Rsp",
	[0x1B] = "BTM_Utility_Req",
	[0x1C] = "Vendor_AT_Cmd_Reply",
	[0x1D] = "Report_Vendor_AT_Event",
	[0x1E] = "Read_Link_Status_Reply",
	[0x1F] = "Read_Paired_Device_Record_Reply",
	[0x20] = "Read_Local_BD_Address_Reply",
	[0x21] = "Read_Local_Device_Name_Reply",
	[0x22] = "Report_SPP/iAP_Data",
	[0x23] = "Report_Link_Back_Status",
	[0x24] = "REPORT_RING_TONE_STATUS",
	[0x25] = "User_Confrim_SSP_Req",
	[0x26] = "Report_AVRCP_Vol_Ctrl",
	[0x27] = "Report_Input_Signal_Level",
	[0x28] = "Report_iAP_Info",
	[0x29] = "REPORT_AVRCP_ABS_VOL_CTRL",
	[0x2A] = "Report_Voice_Prompt_Status",
	[0x2D] = "Report_Type_Codec",
	[0x2E] = "Report_Type_BTM_Setting",
	[0x2F] = "Report_MCU_Update_Reply",
	[0x30] = "Report_BTM_Initial_Status",
	[0x31] = "LE_ANCS_Service_Event",
	[0x32] = "LE_Signaling_Event",
	[0x33] = "Report_nSPK_Link_Status",
	[0x34] = "Report_nSPK_Vendor_Event",
	[0x35] = "Report_nSPK_Audio_Setting",
	[0x36] = "Report_Sound_Effect_Status",
	[0x37] = "Report_Vendor_EEPROM_Data",
	[0x38] = "REPORT_IC_VERSION_INFO",
	[0x39] = "REPORT_LE_GATT_EVENT",
	[0x3A] = "Report_BTM_Link_Mode",
	[0x3B] = "DSP_Dedicated_Event",
	[0x3C] = "Report_nSPK_MISC_Event",
	[0x3D] = "Report_nSPK_Exchange_Link_Info",
	[0x3E] = "Report_Customized_Information",
	[0x3F] = "Report_CSB_CLK",
	[0x40] = "Report_Read_Feature_List_Reply",
	[0x41] = "REPORT_TEST_RESULT_REPLY",
	[0x42] = "Report_Read_EEPROM_Data",
	[0x43] = "PBAPC_Event",
	[0x44] = "AVRCP_Browsing_Event",
};

/* Name of a BM83 event opcode, "unknown" if not in the list */
static const char *bm83_event_name(uint8_t opcode)
{
	if (opcode < ARRAY_SIZE(bm83_event_names) && bm83_event_names[opcode] != NULL) {
		return bm83_event_names[opcode];
	}
	return "unknown";
}

/* Raw frame being collected by the ISR: header, length, opcode, params, checksum */
static uint8_t rx_raw[BT_UART_RX_BUF_SIZE];
static uint8_t rx_pos;
static uint8_t rx_frame_len;

/* Feed one received byte into the frame collector (ISR context) */
static void bt_uart_rx_byte(uint8_t byte)
{
	if (rx_pos == 0) {
		/* wait for the header, ignore fill bytes (0x00) between frames */
		if (byte == BT_FRAME_HEADER) {
			rx_raw[rx_pos++] = byte;
		}
		return;
	}

	rx_raw[rx_pos++] = byte;

	if (rx_pos == 3) {
		uint16_t length = (rx_raw[1] << 8) | rx_raw[2];

		if (length < 1 || length > 1 + BT_FRAME_MAX_PARAMS) {
			LOG_WRN("rx: invalid length %u, frame dropped", length);
			rx_pos = 0;
			return;
		}
		/* header + length(h) + length(l) + length + checksum */
		rx_frame_len = 3 + length + 1;
	} else if (rx_pos > 3 && rx_pos == rx_frame_len) {
		if (k_msgq_put(&bt_rx_q, rx_raw, K_NO_WAIT) != 0) {
			LOG_WRN("rx: queue full, frame dropped");
		}
		rx_pos = 0;
	}
}

static void bt_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t byte;

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &byte, 1) == 1) {
		bt_uart_rx_byte(byte);
	}
}

/* Check one raw frame, store it in rx_buffer and print it */
static void bt_uart_rx_frame(const uint8_t *raw)
{
	uint16_t length = (raw[1] << 8) | raw[2];
	uint8_t num_params = length - 1;
	uint8_t checksum = raw[3 + length];
	uint8_t expected = bt_uart_checksum(&raw[1], 2 + length);
	char buf[BT_FRAME_MAX_PARAMS * 5 + 1];	/* "0xAA " per parameter */
	int len = 0;

	/* the checksum is checked before opcode and parameters are used */
	if (checksum != expected) {
		LOG_ERR("rx: checksum error, received 0x%02X, expected 0x%02X",
			checksum, expected);
		return;
	}

	memset(rx_buffer, 0x00, sizeof(rx_buffer));
	rx_buffer[0] = raw[0];			/* header */
	rx_buffer[1] = raw[3];			/* opcode */
	memcpy(&rx_buffer[2], &raw[4], num_params);

	buf[0] = '\0';
	for (uint8_t i = 0; i < num_params; i++) {
		len += snprintk(&buf[len], sizeof(buf) - len, "0x%02X ",
				rx_buffer[2 + i]);
	}

	LOG_INF("rx: %s | checksum 0x%02X | opcode 0x%02X | params %s",
		bm83_event_name(rx_buffer[1]), checksum, rx_buffer[1], buf);

	if (rx_buffer[1] == BM83_EVT_COMMAND_ACK) {
		/* params: command id, status -> wake up the waiting command */
		if (num_params >= 2) {
			bm83_command_ack_received(rx_buffer[2], rx_buffer[3]);
		}
	} else {
		/* acknowledge every other event, so the BM83 sends the next
		 * event right away instead of after its 800 ms timeout */
		bm83_event_ack(rx_buffer[1]);

		/* phone connected (HF or A2DP link): set the output gain */
		if (rx_buffer[1] == BM83_EVT_BTM_STATUS && num_params >= 1 &&
		    (rx_buffer[2] == BM83_STATUS_HF_LINK ||
		     rx_buffer[2] == BM83_STATUS_A2DP_LINK)) {
			bm83_link_established();
		}
	}
}

void bt_rx_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t raw[BT_UART_RX_BUF_SIZE];

	while (1) {
		/* Sleep until the ISR has collected a complete frame */
		k_msgq_get(&bt_rx_q, raw, K_FOREVER);
		bt_uart_rx_frame(raw);
	}
}

/* ---- init -------------------------------------------------------------- */

int bt_uart_init(void)
{
	int ret;

	if (!device_is_ready(bt_uart)) {
		LOG_ERR("uart1 not ready");
		return -ENODEV;
	}

	ret = uart_irq_callback_user_data_set(bt_uart, bt_uart_isr, NULL);
	if (ret < 0) {
		LOG_ERR("uart1 IRQ callback failed (%d)", ret);
		return ret;
	}
	uart_irq_rx_enable(bt_uart);
	if (IS_ENABLED(BT_UART_TX_DISABLE)) {
		/* Take GP4 away from UART1: GPIO input, no pulls = tristate */
		ret = gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio0)),
					 BT_UART_TX_PIN, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("GP%d tristate config failed (%d)", BT_UART_TX_PIN, ret);
			return ret;
		}
		LOG_WRN("uart1 TX disabled, GP%d is tristate (BT_UART_TX_DISABLE)",
			BT_UART_TX_PIN);
	}

	LOG_INF("uart1 (BM83) initialised");
	return 0;
}
