/*
 * blueline - tone.h
 * CMX865A Telecom Signalling Device on SPI0 (C-BUS): call progress tones
 * in the handset, and the tone state machine tone_fsm (tone_thread).
 */

#ifndef TONE_H
#define TONE_H

#include <stdint.h>
#include <zephyr/sys/util.h>

#include "ring.h"

/* C-BUS register addresses (CMX865A datasheet 5.11) */
#define CMX_GENERAL_RESET     0x01   /* command, address only, no data */
#define CMX_GENERAL_CONTROL   0xE0   /* 16 bit, write only */
#define CMX_TX_MODE           0xE1   /* 16 bit, write only */
#define CMX_RX_MODE           0xE2   /* 16 bit, write only */
#define CMX_STATUS            0xE6   /* 16 bit, read only */
#define CMX_PROGRAMMING       0xE8   /* 16 bit, write only */

/* General Control Register bits */
#define CMX_GC_TXAN_OFF       BIT(15) /* 1 = TXAN disconnected (always) */
#define CMX_GC_TXA_OFF        BIT(14) /* 1 = TXA disconnected (tone_on = false) */
#define CMX_GC_PWR            BIT(8)  /* 1 = powered up, 0 = powersave */
#define CMX_GC_RST            BIT(7)  /* 1 = internal circuitry in reset */

/* Status Register bits */
#define CMX_STATUS_PROG_FLAG  BIT(13) /* 1 = Programming Register may be written */

/* Tx Mode Register: DTMF/Tones mode (b15-12 = 0001), Tx level 0 dB
 * (b11-9 = 111), b4 = 0 and b3-0 = 1100: programmed tone pair TA */
#define CMX_TX_MODE_TONE_TA   0x1E0C
#define CMX_TX_MODE_DISABLED  0x0000  /* transmitter disabled: oscillators off */

/* Programming Register: first word of the transmit tone pair sequence */
#define CMX_PROG_TX_TONES     0x8000

/* Tone limits (datasheet 5.11.8) */
#define CMX_TONE_FREQ_MAX_HZ  3400.0  /* frequency value = f * 3.414 */
#define CMX_TONE_LEVEL_MAX    16383   /* level value at vol = 1.0 */

/* Timing */
#define CMX_RESET_MS          100   /* Pwr + Rst high -> Rst low (datasheet: >= 20 ms) */
#define CMX_PROG_TIMEOUT_US   2000  /* Programming Flag back to 1 (datasheet: ~150 us) */

/* tone_fsm states */
typedef enum {
	TONE_IDLE = 0,	/* TXA disconnected, oscillators off */
	TONE_DIAL,	/* dial tone: melody = ger_dial, cadence running */
	TONE_MUTE,	/* TXA disconnected, oscillators off */
	TONE_BUSSY,	/* busy tone: melody = ger_enga, cadence running */
	TONE_STATE_COUNT
} tone_fsm_state_t;

/* Events delivered to tone_fsm (fsm_main and bm_83.c) */
typedef enum {
	TONE_EVT_MAIN_IDLE = 0,	/* fsm_main entered s0_idle */
	TONE_EVT_OFFHOOK,	/* fsm_main entered s1_offhook */
	TONE_EVT_OPEN,		/* fsm_main s1_offhook -> s2_open */
	TONE_EVT_CALL_IDLE,	/* Call_Status 0x00: call ended (bm_83.c) */
	TONE_EVT_CALL_INCOMING,	/* Call_Status 0x02: incoming call (bm_83.c) */
	TONE_EVT_COUNT
} tone_fsm_event_t;

/* Initialise the CMX865A: General Reset command, General Control = Pwr +
 * Rst (TXA and TXAN disconnected), after CMX_RESET_MS Rst cleared (Pwr
 * stays set). No tone is started.
 * Blocks for about 100 ms. Returns 0 on success, negative errno otherwise. */
int cmx_init(void);

/* Program and start the oscillators (transmit tone pair TA):
 * freq_1, freq_2 in Hz (max 3400), 0 = that oscillator off; both 0 = Tx
 * disabled. vol: level of both oscillators, 0.0 .. 1.0 (max).
 * Does not change TXA (see tone_play_note()). Not reprogrammed if the
 * values are already running. Call with tone_mutex held (tone.c only).
 * Returns 0 on success, negative errno otherwise. */
int start_tone(double freq_1, double freq_2, double vol);

/* Play one cadence segment (called by generate_ring() for each segment):
 * tone_on = true: oscillators as freq_1/freq_2/vol, TXA connected;
 * tone_on = false: TXA disconnected. */
void tone_play_note(const note_t *note);

/* TXA disconnected and oscillators off (ring_stop(), tone_idle, tone_mute). */
void tone_off(void);

/* Hand an event to tone_fsm. Never blocks. */
void tone_fsm_post_event(tone_fsm_event_t ev);

/* Entry point of tone_thread: the tone state machine. */
void tone_fsm_main(void *p1, void *p2, void *p3);

#endif /* TONE_H */
