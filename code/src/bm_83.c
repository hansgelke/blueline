/*
 * blueline - bm_83.c
 * Microchip BM83 Bluetooth module: control signals, power-up sequence and
 * commands sent over the UART (bt_uart.c).
 *
 * Power-up sequence (bm83_init):
 *   bt_rst_n = low, bt_mfb = low
 *   100 ms -> bt_mfb   = high
 *    20 ms -> bt_rst_n = high (BM83 out of reset)
 *   500 ms
 *   MMI action 0x51 (power on button press)
 *   MMI action 0x52 (power on button release)
 *   Read_BTM_Version (firmware version)
 *   MMI action 0x5D (enter pairing mode)
 *   bt_mfb = low
 *
 * MFB handshake: in host MCU mode MFB is the BM83 UART_RX_IND input
 * ("UART data from the MCU follows"), see Audio UART Command Set 4.1/4.4/4.5.
 *   command:    MFB high, >= 3 ms, send, wait for Command_ACK (max 200 ms),
 *               MFB low
 *   Event_ACK:  MFB high, >= 3 ms, send, MFB low (no reply)
 * Commands (main thread) and Event_ACKs (bt_rx_thread) can overlap, so MFB
 * is reference counted: high with the first user, low after the last one.
 *
 * The BM83 must be programmed with a host MCU mode configuration (UART
 * command interface enabled), otherwise it ignores all UART commands.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "bm_83.h"
#include "bt_uart.h"

LOG_MODULE_REGISTER(bm_83, LOG_LEVEL_INF);

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* MMI action parameter: database index */
#define BM83_MMI_DATABASE_INDEX  0x00

/* ---- MFB handshake ----------------------------------------------------- */

K_MUTEX_DEFINE(mfb_mutex);
static uint8_t mfb_users;	/* senders that currently need MFB high */
static int64_t mfb_rise_ticks;	/* uptime when MFB went high */

/* MFB high (if not already) without waiting for the lead time */
static void bm83_mfb_raise(void)
{
	k_mutex_lock(&mfb_mutex, K_FOREVER);
	if (mfb_users++ == 0) {
		gpio_pin_set(gpio0, BT_MFB_PIN, 1);
		mfb_rise_ticks = k_uptime_ticks();
	}
	k_mutex_unlock(&mfb_mutex);
}

/* MFB high and wait until it has been high for BM83_MFB_LEAD_MS */
static void bm83_mfb_get(void)
{
	int64_t ready;
	int64_t now;

	bm83_mfb_raise();

	k_mutex_lock(&mfb_mutex, K_FOREVER);
	ready = mfb_rise_ticks + k_ms_to_ticks_ceil64(BM83_MFB_LEAD_MS);
	k_mutex_unlock(&mfb_mutex);

	now = k_uptime_ticks();
	if (now < ready) {
		k_sleep(K_TICKS(ready - now));
	}
}

/* Release MFB: low again when no other sender needs it */
static void bm83_mfb_put(void)
{
	k_mutex_lock(&mfb_mutex, K_FOREVER);
	if (mfb_users > 0 && --mfb_users == 0) {
		gpio_pin_set(gpio0, BT_MFB_PIN, 0);
	}
	k_mutex_unlock(&mfb_mutex);
}

/* ---- Command_ACK ------------------------------------------------------- */

/* Only one command at a time waits for its Command_ACK */
K_MUTEX_DEFINE(cmd_mutex);
K_SEM_DEFINE(cmd_ack_sem, 0, 1);
static bool cmd_ack_pending;
static uint8_t cmd_ack_id;	/* opcode of the command waiting for its ACK */
static uint8_t cmd_ack_status;

void bm83_command_ack_received(uint8_t cmd_id, uint8_t status)
{
	if (cmd_ack_pending && cmd_id == cmd_ack_id) {
		cmd_ack_status = status;
		cmd_ack_pending = false;
		k_sem_give(&cmd_ack_sem);
	}
}

/* ---- commands ---------------------------------------------------------- */

/* Build a frame for opcode and params */
static int bm83_build(uint8_t *frame, size_t size, uint8_t opcode,
		      const uint8_t *params, uint8_t num_params)
{
	int len;

	len = bt_uart_build_frame(frame, size, opcode, params, num_params);
	if (len < 0) {
		LOG_ERR("command 0x%02X: cannot build frame (%d)", opcode, len);
	}
	return len;
}

/* Send a command with the MFB handshake and wait for its Command_ACK.
 * Must not be called from bt_rx_thread (it delivers the Command_ACK).
 * Returns 0 on "command complete", -EIO for another ACK status,
 * -ETIMEDOUT if no Command_ACK arrives within BM83_CMD_ACK_TIMEOUT_MS. */
static int bm83_send_cmd(uint8_t opcode, const uint8_t *params, uint8_t num_params)
{
	uint8_t frame[BT_FRAME_MAX_LEN];
	int len;
	int ret;

	len = bm83_build(frame, sizeof(frame), opcode, params, num_params);
	if (len < 0) {
		return len;
	}

	k_mutex_lock(&cmd_mutex, K_FOREVER);

	k_sem_reset(&cmd_ack_sem);
	cmd_ack_id = opcode;
	cmd_ack_pending = true;

	bm83_mfb_get();
	bt_uart_send(frame, len);
	ret = k_sem_take(&cmd_ack_sem, K_MSEC(BM83_CMD_ACK_TIMEOUT_MS));
	bm83_mfb_put();

	cmd_ack_pending = false;

	if (ret != 0) {
		LOG_WRN("command 0x%02X: no Command_ACK within %d ms",
			opcode, BM83_CMD_ACK_TIMEOUT_MS);
		ret = -ETIMEDOUT;
	} else if (cmd_ack_status != BM83_ACK_COMMAND_COMPLETE) {
		LOG_WRN("command 0x%02X: Command_ACK status 0x%02X",
			opcode, cmd_ack_status);
		ret = -EIO;
	}

	k_mutex_unlock(&cmd_mutex);
	return ret;
}

int bm83_mmi_action(uint8_t action)
{
	const uint8_t params[] = { BM83_MMI_DATABASE_INDEX, action };

	LOG_INF("tx: MMI action 0x%02X", action);
	return bm83_send_cmd(BM83_CMD_MMI_ACTION, params, sizeof(params));
}

int bm83_enter_pairing(void)
{
	LOG_INF("tx: enter pairing mode");
	return bm83_mmi_action(BM83_MMI_ENTER_PAIRING);
}

int bm83_read_btm_version(uint8_t type)
{
	const uint8_t params[] = { type };

	LOG_INF("tx: Read_BTM_Version type 0x%02X", type);
	return bm83_send_cmd(BM83_CMD_READ_BTM_VERSION, params, sizeof(params));
}

int bm83_event_ack(uint8_t event_id)
{
	const uint8_t params[] = { event_id };
	uint8_t frame[BT_FRAME_MAX_LEN];
	int len;

	len = bm83_build(frame, sizeof(frame), BM83_CMD_EVENT_ACK,
			 params, sizeof(params));
	if (len < 0) {
		return len;
	}

	/* the BM83 does not answer an Event_ACK: MFB low right after sending */
	bm83_mfb_get();
	bt_uart_send(frame, len);
	bm83_mfb_put();

	LOG_DBG("tx: Event_ACK 0x%02X", event_id);
	return 0;
}

/* ---- init -------------------------------------------------------------- */

int bm83_init(void)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}

	/* bt_rst_n: output, low (BM83 held in reset) */
	ret = gpio_pin_configure(gpio0, BT_RST_N_PIN, GPIO_OUTPUT_LOW);
	if (ret < 0) {
		LOG_ERR("bt_rst_n pin config failed (%d)", ret);
		return ret;
	}

	/* bt_mfb: output, low */
	ret = gpio_pin_configure(gpio0, BT_MFB_PIN, GPIO_OUTPUT_LOW);
	if (ret < 0) {
		LOG_ERR("bt_mfb pin config failed (%d)", ret);
		return ret;
	}

	/* MFB stays high from here through the power-up commands */
	k_msleep(BM83_MFB_HIGH_DELAY_MS);
	bm83_mfb_raise();

	k_msleep(BM83_RST_HIGH_DELAY_MS);
	gpio_pin_set(gpio0, BT_RST_N_PIN, 1);

	k_msleep(BM83_CMD_DELAY_MS);

	ret = bm83_mmi_action(BM83_MMI_POWER_ON_PRESS);
	if (ret == 0) {
		ret = bm83_mmi_action(BM83_MMI_POWER_ON_RELEASE);
	}
	if (ret == 0) {
		ret = bm83_read_btm_version(BM83_VERSION_FW);
	}
	if (ret == 0) {
		ret = bm83_enter_pairing();
	}

	/* end of the power-up sequence: MFB low unless an Event_ACK is going out */
	bm83_mfb_put();

	if (ret < 0) {
		return ret;
	}

	LOG_INF("BM83 initialised (GPIO%d bt_rst_n / GPIO%d bt_mfb)",
		BT_RST_N_PIN, BT_MFB_PIN);
	return 0;
}
