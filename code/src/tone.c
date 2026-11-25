/*
 * blueline - tone.c
 * CMX865A Telecom Signalling Device (DTMF / tone generator) on SPI0.
 *
 * C-BUS (CMX865A datasheet 5.11): one address byte, followed by 0, 1 or 2
 * data bytes from the host (write) or reply bytes from the CMX865A (read),
 * msb first. Command data is clocked in on the rising clock edge, reply data
 * is valid while the clock is high: SPI mode 3 (CPOL = 1, CPHA = 1), as
 * recommended in CML's "C-BUS to SPI Interfacing" note. CSN stays low for the
 * whole transaction (GP17 as GPIO chip select, see overlay).
 *
 *   GP16 MISO0 (REPLY DATA)   GP17 CS0 (CSN)
 *   GP18 SCLK0 (SERIAL CLOCK) GP19 MOSI0 (COMMAND DATA)
 *
 * Purpose: call progress tones in the handset. The tones come from the
 * same note_t cadence tables as the ringing (ring.c): generate_ring() hands
 * every segment to tone_play_note():
 *   freq_1, freq_2  frequencies of the two oscillators (transmit tone pair
 *                   TA) in Hz, 0 = oscillator off
 *   vol             level of both oscillators, 1.0 = max
 *   tone_on         true: TXA connected (General Control b14 = 0),
 *                   false: TXA disconnected (b14 = 1)
 * TXAN is always disconnected (General Control b15 = 1).
 *
 * tone_fsm (tone_thread, events from fsm_main):
 *   tone_idle  TXA disconnected, oscillators off
 *              --fsm_main enters s1_offhook, incoming call ringing-->
 *                tone_mute (the call is answered, no dial tone)
 *              --fsm_main enters s1_offhook AND HF link connected-->
 *                tone_dial (without HF link: stays here, no tone)
 *              incoming_call: set by Call_Status 0x02 (incoming), cleared
 *              by Call_Status 0x00 (caller gave up) and on leaving
 *              tone_idle
 *   tone_dial  melody = ger_dial, ring_start() (ringer_on = false in
 *              ger_dial, so the SLIC does not go to ring mode)
 *              exit: ring_stop() (tone off)
 *              --fsm_main s1_offhook -> s2_open-->  tone_mute
 *   tone_mute  TXA disconnected, oscillators off
 *              --Call_Status 0x00 (call ended)-->  tone_bussy
 *   tone_bussy melody = ger_enga, ring_start(); exit: ring_stop()
 *   any state  --fsm_main enters s0_idle-->  tone_idle
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "tone.h"
#include "ring.h"
#include "bm_83.h"

LOG_MODULE_REGISTER(tone, LOG_LEVEL_INF);

#define CMX_NODE DT_NODELABEL(cmx865a)

static const struct spi_dt_spec cmx_spi =
	SPI_DT_SPEC_GET(CMX_NODE, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
			SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA);

/* ---- C-BUS access ------------------------------------------------------ */

/* Send len bytes (address byte + data bytes) in one CSN-low transaction */
static int cmx_write(const uint8_t *data, size_t len)
{
	const struct spi_buf buf = { .buf = (void *)data, .len = len };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	return spi_write_dt(&cmx_spi, &tx);
}

/* Address-only command (General Reset) */
static int cmx_cmd(uint8_t addr)
{
	return cmx_write(&addr, 1);
}

/* 16-bit write register: address, msb, lsb */
static int cmx_write16(uint8_t addr, uint16_t value)
{
	const uint8_t data[3] = { addr, value >> 8, value & 0xFF };

	return cmx_write(data, sizeof(data));
}

/* 16-bit read register: address out, then msb and lsb in */
static int cmx_read16(uint8_t addr, uint16_t *value)
{
	uint8_t tx_data[3] = { addr, 0x00, 0x00 };
	uint8_t rx_data[3];
	const struct spi_buf tx_buf = { .buf = tx_data, .len = sizeof(tx_data) };
	const struct spi_buf rx_buf = { .buf = rx_data, .len = sizeof(rx_data) };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx = { .buffers = &rx_buf, .count = 1 };
	int ret;

	ret = spi_transceive_dt(&cmx_spi, &tx, &rx);
	if (ret == 0) {
		*value = ((uint16_t)rx_data[1] << 8) | rx_data[2];
	}
	return ret;
}

/* Wait until the Programming Flag (Status b13) is 1. Writing the
 * Programming Register clears it; the CMX865A sets it again when the word
 * has been processed (~150 us). */
static int cmx_wait_prog_flag(void)
{
	uint16_t status = 0;
	int ret;

	for (int t = 0; t < CMX_PROG_TIMEOUT_US; t += 10) {
		ret = cmx_read16(CMX_STATUS, &status);
		if (ret < 0) {
			return ret;
		}
		if (status & CMX_STATUS_PROG_FLAG) {
			return 0;
		}
		k_busy_wait(10);
	}

	LOG_ERR("programming flag not set (status 0x%04X)", status);
	return -ETIMEDOUT;
}

/* Write one word to the Programming Register (only while the flag is 1) */
static int cmx_program_word(uint16_t word)
{
	int ret = cmx_wait_prog_flag();

	if (ret < 0) {
		return ret;
	}
	return cmx_write16(CMX_PROGRAMMING, word);
}

/* ---- tone -------------------------------------------------------------- */

/* Protects the CMX865A registers and the shadow values below
 * (ring_thread: tone_play_note(), ring_stop() / tone_fsm: tone_off()) */
K_MUTEX_DEFINE(tone_mutex);

static bool cmx_ready;		/* cmx_init() succeeded */
static uint16_t gc_value;	/* last value written to General Control */
static bool osc_on;		/* Tx Mode = tone pair TA */
static uint16_t prog_words[4];	/* TA as programmed: f1, level1, f2, level2 */

/* Datasheet 5.11.8: frequency value = f (Hz) * 3.414, f max 3400 Hz */
static uint16_t cmx_freq_value(double freq)
{
	if (freq <= 0.0) {
		return 0;
	}
	if (freq > CMX_TONE_FREQ_MAX_HZ) {
		freq = CMX_TONE_FREQ_MAX_HZ;
	}
	return (uint16_t)(freq * 3.414 + 0.5);
}

/* Level value 0 .. 16383 (0x3FFF): vol 1.0 = max */
static uint16_t cmx_level_value(double vol)
{
	if (vol <= 0.0) {
		return 0;
	}
	if (vol > 1.0) {
		vol = 1.0;
	}
	return (uint16_t)(vol * CMX_TONE_LEVEL_MAX + 0.5);
}

/* General Control: Pwr, TXAN always disconnected, TXA as requested */
static int cmx_txa_set(bool connected)
{
	uint16_t value = CMX_GC_PWR | CMX_GC_TXAN_OFF |
			 (connected ? 0 : CMX_GC_TXA_OFF);
	int ret;

	if (value == gc_value) {
		return 0;
	}
	ret = cmx_write16(CMX_GENERAL_CONTROL, value);
	if (ret == 0) {
		gc_value = value;
		LOG_INF("TXA %s", connected ? "connected" : "disconnected");
	}
	return ret;
}

/* Oscillators off: Tx Mode = transmitter disabled */
static int cmx_osc_off(void)
{
	int ret;

	if (!osc_on) {
		return 0;
	}
	ret = cmx_write16(CMX_TX_MODE, CMX_TX_MODE_DISABLED);
	if (ret == 0) {
		osc_on = false;
	}
	return ret;
}

int start_tone(double freq_1, double freq_2, double vol)
{
	/* TA: oscillator 1 = freq_1, oscillator 2 = freq_2. An oscillator with
	 * frequency 0 is off (frequency and level 0). */
	uint16_t ta[4] = { 0 };
	uint16_t words[17] = { 0 };
	int ret;

	ta[0] = cmx_freq_value(freq_1);
	ta[1] = (ta[0] != 0) ? cmx_level_value(vol) : 0;
	ta[2] = cmx_freq_value(freq_2);
	ta[3] = (ta[2] != 0) ? cmx_level_value(vol) : 0;

	/* both oscillators off */
	if (ta[0] == 0 && ta[2] == 0) {
		return cmx_osc_off();
	}

	/* already running with these values: nothing to do (no click) */
	if (osc_on && memcmp(ta, prog_words, sizeof(ta)) == 0) {
		return 0;
	}

	/* Transmit tone pairs: word 1 = 0x8000, then TA..TD, each tone 1
	 * frequency, tone 1 level, tone 2 frequency, tone 2 level. Only TA is
	 * used, TB..TD are programmed as "no tone". */
	words[0] = CMX_PROG_TX_TONES;
	memcpy(&words[1], ta, sizeof(ta));

	for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
		ret = cmx_program_word(words[i]);
		if (ret < 0) {
			return ret;
		}
	}

	/* wait until the last word has been processed before changing Tx Mode */
	ret = cmx_wait_prog_flag();
	if (ret < 0) {
		return ret;
	}

	ret = cmx_write16(CMX_TX_MODE, CMX_TX_MODE_TONE_TA);
	if (ret < 0) {
		return ret;
	}

	memcpy(prog_words, ta, sizeof(ta));
	osc_on = true;
	LOG_INF("tone: %d Hz + %d Hz, vol %d %%", (int)freq_1, (int)freq_2,
		(int)(vol * 100.0));
	return 0;
}

void tone_play_note(const note_t *note)
{
	if (!cmx_ready) {
		return;
	}

	k_mutex_lock(&tone_mutex, K_FOREVER);
	if (note->tone_on) {
		start_tone(note->freq_1, note->freq_2, note->vol);
		cmx_txa_set(true);
	} else {
		cmx_txa_set(false);
	}
	k_mutex_unlock(&tone_mutex);
}

void tone_off(void)
{
	if (!cmx_ready) {
		return;
	}

	k_mutex_lock(&tone_mutex, K_FOREVER);
	cmx_txa_set(false);
	cmx_osc_off();
	k_mutex_unlock(&tone_mutex);
}

/* ---- init -------------------------------------------------------------- */

int cmx_init(void)
{
	int ret;

	if (!spi_is_ready_dt(&cmx_spi)) {
		LOG_ERR("spi0 not ready");
		return -ENODEV;
	}

	/* General Reset: clears all registers, device in powersave */
	ret = cmx_cmd(CMX_GENERAL_RESET);
	if (ret < 0) {
		LOG_ERR("General Reset failed (%d)", ret);
		return ret;
	}

	/* Powerup (b8) and Reset (b7) set: crystal and VBIAS start up.
	 * TXA and TXAN disconnected. */
	ret = cmx_write16(CMX_GENERAL_CONTROL, CMX_GC_PWR | CMX_GC_RST |
			  CMX_GC_TXA_OFF | CMX_GC_TXAN_OFF);
	if (ret < 0) {
		LOG_ERR("General Control write failed (%d)", ret);
		return ret;
	}

	k_msleep(CMX_RESET_MS);

	/* Reset (b7) cleared, Powerup stays set: normal operation */
	gc_value = CMX_GC_PWR | CMX_GC_TXA_OFF | CMX_GC_TXAN_OFF;
	ret = cmx_write16(CMX_GENERAL_CONTROL, gc_value);
	if (ret < 0) {
		LOG_ERR("General Control write failed (%d)", ret);
		return ret;
	}

	osc_on = false;		/* Tx Mode cleared by the reset */
	cmx_ready = true;
	LOG_INF("CMX865A initialised (SPI0: GP16 MISO, GP17 CS, GP18 SCLK, GP19 MOSI), TXA/TXAN disconnected");
	return 0;
}

/* ---- tone_fsm (tone_thread) -------------------------------------------- */

K_MSGQ_DEFINE(tone_event_q, sizeof(tone_fsm_event_t), 8, 4);

static tone_fsm_state_t tone_state;
static bool incoming_call;	/* Call_Status 0x02 seen while in tone_idle */

static const char *const tone_state_names[TONE_STATE_COUNT] = {
	[TONE_IDLE] = "tone_idle",
	[TONE_DIAL] = "tone_dial",
	[TONE_MUTE] = "tone_mute",
	[TONE_BUSSY] = "tone_bussy",
};

void tone_fsm_post_event(tone_fsm_event_t ev)
{
	/* K_NO_WAIT: never blocks; event is dropped if the queue is full */
	if (k_msgq_put(&tone_event_q, &ev, K_NO_WAIT) != 0) {
		LOG_WRN("tone event queue full, event %d dropped", ev);
	}
}

/* Change state: exit action of the old state, entry action of the new one */
static void tone_enter_state(tone_fsm_state_t new_state)
{
	LOG_INF("tone %s -> %s", tone_state_names[tone_state],
		tone_state_names[new_state]);

	/* exit action of tone_dial and tone_bussy: stop the cadence
	 * (RING_EVENT), tone off */
	if ((tone_state == TONE_DIAL || tone_state == TONE_BUSSY) &&
	    new_state != tone_state) {
		ring_stop();
	}

	tone_state = new_state;

	switch (new_state) {
	case TONE_IDLE:
	case TONE_MUTE:
		/* TXA disconnected, oscillators off */
		tone_off();
		break;
	case TONE_DIAL:
		/* dial tone: ringer_on = false in ger_dial, no ring mode */
		melody = ger_dial;
		ring_start();
		break;
	case TONE_BUSSY:
		/* busy tone: ringer_on = false in ger_enga, no ring mode */
		melody = ger_enga;
		ring_start();
		break;
	default:
		break;
	}
}

void tone_fsm_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	tone_fsm_event_t ev;

	tone_state = TONE_IDLE;
	LOG_INF("tone_thread started, state %s", tone_state_names[tone_state]);

	while (1) {
		/* Sleep until an event arrives - no polling */
		k_msgq_get(&tone_event_q, &ev, K_FOREVER);

		/* fsm_main entered s0_idle: from any state to tone_idle */
		if (ev == TONE_EVT_MAIN_IDLE) {
			if (tone_state != TONE_IDLE) {
				tone_enter_state(TONE_IDLE);
			}
			continue;
		}

		switch (tone_state) {
		case TONE_IDLE:
			if (ev == TONE_EVT_CALL_INCOMING) {
				incoming_call = true;
			} else if (ev == TONE_EVT_CALL_IDLE) {
				incoming_call = false;
			} else if (ev == TONE_EVT_OFFHOOK) {
				if (incoming_call) {
					/* handset lifted to answer the call */
					incoming_call = false;
					tone_enter_state(TONE_MUTE);
				} else if (bm83_hf_connected()) {
					tone_enter_state(TONE_DIAL);
				} else {
					LOG_INF("no HF link: no dial tone");
				}
			}
			break;
		case TONE_DIAL:
			if (ev == TONE_EVT_OPEN) {
				tone_enter_state(TONE_MUTE);
			}
			break;
		case TONE_MUTE:
			if (ev == TONE_EVT_CALL_IDLE) {
				tone_enter_state(TONE_BUSSY);
			}
			break;
		case TONE_BUSSY:
			break;
		default:
			LOG_ERR("invalid tone state %d, back to tone_idle", tone_state);
			tone_enter_state(TONE_IDLE);
			break;
		}
	}
}
