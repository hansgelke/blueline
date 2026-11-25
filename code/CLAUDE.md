# blueline

Firmware for a Raspberry Pi Pico 2 (SoC RP2350), running Zephyr RTOS.
The project is developed step by step; implement only what is asked for, nothing extra.

## Build & flash

- Board target: `rpi_pico2/rp2350a/m33`
- `west` is not on the PATH. Activate the Zephyr environment first:
  `source ~/zephyrproject/.venv/bin/activate` (Zephyr base: `~/zephyrproject/zephyr`)
- Build (run from `code/`, output goes to `code/build/`):
  `west build -b rpi_pico2/rp2350a/m33 -d build .`
- Rebuild from scratch: add `-p always`
- Flash: copy `build/zephyr/zephyr.uf2` to the Pico in BOOTSEL mode (drive `RP2350`).
  `west flash` uses OpenOCD + a debug probe and does not work with the installed
  OpenOCD 0.12.0 (no `target/rp2350.cfg`).

## File layout (src/)

Every `.c` file has a matching `.h` file.

- `main.c / main.h`: system start. Calls `slic_init()` and `ring_init()` first (RING_MODE low, POT_FWD
  PWM at 100 % as early as possible), then `pico_io_init()` and `bt_uart_init()`, creates all
  threads, then calls `bm83_init()` last (so `bt_rx_thread` already shows the BM83 responses).
  Thread stack sizes and priorities are defined in `main.h`. New threads are added here.
- `fsm_main.c / fsm_main.h`: the main state machine, running in `fsm_thread`.
- `bt_fsm.c / bt_fsm.h`: the Bluetooth state machine, running in `bt_fsm_thread`.
- `pico_io.c / pico_io.h`: shk GPIO initialisation, the common GPIO ISR `gpio_int`, and LED control.
- `bt_uart.c / bt_uart.h`: UART1 link to the BM83 Bluetooth module (frame build/send,
  receive ISR, frame check, BM83 event names). Everything UART-related goes here.
- `ring.c / ring.h`: ringer PWM on POT_FWD (GP2), ring cadence generator `generate_ring()`
  (`ring_thread`), cadence type `note_t`, cadence tables (e.g. `gb_ring`), current cadence
  `melody`, and the bell state machine `bell_fsm` (`bell_thread`).
- `slic.c / slic.h`: SLIC (AG1171, J4) control. `slic_init()`: RING_MODE (GP7) low,
  POT_PDOWN_N (GP3) tristate (driven low = SLIC powered down when `BT_UART_TX_DISABLE` = 1).
  `slic_ring_mode_set()` switches RING_MODE (used by `bell_fsm`).
  All SLIC-related code goes here.
- `tone.c / tone.h`: CMX865A Telecom Signalling Device on SPI0 (C-BUS): call progress tones in the
  handset and the tone state machine `tone_fsm` (`tone_thread`). `cmx_init()` (called in `main.c` after
  `bt_uart_init()`, ~100 ms): General Reset (0x01), General Control (0xE0) = Pwr (b8) + Rst (b7) with
  TXA (b14) and TXAN (b15) disconnected, 100 ms, Rst cleared. No tone at start-up.
  `tone_play_note()` is called by `generate_ring()` for every cadence segment (same `note_t` tables as the
  ringing): `freq_1` / `freq_2` = the two oscillators (Tx tone pair TA, Programming Register 0xE8,
  word 0x8000 + 16 words, frequency value = Hz × 3.414, 0 = oscillator off), `vol` = level of both
  (1.0 = 16383 = max), `tone_on` = TXA connected (b14 = 0) / disconnected (b14 = 1). TXAN is always
  disconnected. Tx Mode (0xE1) = 0x1E0C (tone pair TA, 0 dB) or 0x0000 (oscillators off). TA is only
  reprogrammed when frequency or level change. `tone_off()`: TXA disconnected, oscillators off
  (also called by `ring_stop()`). A failure of `cmx_init()` is only logged.
- `bm_83.c / bm_83.h`: BM83 control signals (`bt_rst_n`, `bt_mfb`), power-up sequence
  `bm83_init()` and BM83 commands (`bm83_mmi_action()`).

New source files must be added to `CMakeLists.txt`.

## Hardware

| Signal            | GPIO | Config                         | Logic                          |
|-------------------|------|--------------------------------|--------------------------------|
| `SHK_FALLING_PIN` | 12   | input, no internal pulls       | IRQ on falling edge            |
| `SHK_RISING_PIN`  | 11   | input, no internal pulls       | IRQ on rising edge             |
| `RING_MODE_PIN`   | 7    | output (SLIC, `slic.c`)        | low after init, set by `bell_fsm` |
| `POT_PDOWN_N_PIN` | 3    | input, no pulls (tristate)     | to AG1171 pin 14 via D7 (cathode at GP3), AG1171 internal pull-up, `slic.c` |
| `POT_FWD_PIN`     | 2    | PWM1 slice 1 A (SLIC, `ring.c`) | 25 Hz, duty 100 % = high       |
| `BT_RST_N_PIN`    | 6    | output (BM83 reset)            | low after init, active low     |
| `BT_MFB_PIN`      | 8    | output (BM83 MFB)              | low after init                 |
| LED               | 25   | output (Pico 2 user LED)       | high = on                      |
| SPI0 RX (CMX865A) | 16   | SPI0 MISO                      | CMX865A REPLY DATA             |
| CS0 (CMX865A)     | 17   | GPIO chip select (`cs-gpios`)  | CMX865A CSN, active low        |
| SPI0 SCK (CMX865A)| 18   | SPI0, 1 MHz, mode 3            | CMX865A SERIAL CLOCK           |
| SPI0 TX (CMX865A) | 19   | SPI0 MOSI                      | CMX865A COMMAND DATA           |
| UART1 TX (BM83)   | 4    | UART1, 115200 8N1, no flow ctl | RP2350 → BM83                  |
| UART1 RX (BM83)   | 5    | UART1, 115200 8N1, no flow ctl | BM83 → RP2350                  |

UART1 is set up in the board overlay (`boards/rpi_pico2_rp2350a_m33.overlay`).
The console stays on UART0 (GPIO0/1).
The stock board's i2c0 (GPIO4/5) and i2c1 (GPIO6/7) are disabled in the overlay because
those pins are used for UART1 and RING_MODE.

## Threads

- `fsm_thread`: runs `fsm_main()`, priority 5, stack 2048.
- `bt_fsm_thread`: runs `bt_fsm_main()`, priority 6, stack 1024.
- `bt_rx_thread`: runs `bt_rx_main()`, priority 6, stack 1024. Checks and prints BM83 frames.
- `ring_thread`: runs `generate_ring()`, priority 7, stack 1024. Blocked until `bt_fsm` enters
  `BT_RING_BELL` (`ring_start()`), so the cadence is synchronised with that entry. Plays the cadence
  `melody` (`note_t` table, max `RING_MAX_SEGMENTS` = 8 segments, `skip` = true on the last one, then
  it restarts; durations in µs, `k_usleep()`) and reports each segment's `ringer_on` to `bell_fsm`.
  `ring_stop()` (leaving `BT_RING_BELL`) ends the cadence at once and reports `ringer_on` = false.
- `bell_thread`: runs `bell_fsm_main()`, priority 4, stack 1024. The bell state machine.
- `tone_thread`: runs `tone_fsm_main()`, priority 6, stack 1024. The tone state machine.

## Tone (tone.c)

`tone_fsm` (own thread, events in `tone_event_q`). From `fsm_main` (`fsm_enter_state()`):
`TONE_EVT_MAIN_IDLE` (entered `s0_idle`), `TONE_EVT_OFFHOOK` (entered `s1_offhook`), `TONE_EVT_OPEN`
(entered `s2_open`, only possible from `s1_offhook`). From `bm83_handle_event()`: `TONE_EVT_CALL_IDLE`
(`Call_Status` 0x00, the call has ended, e.g. the other party hung up; the BM83 reports this, it does
not send an MMI "end call") and `TONE_EVT_CALL_INCOMING` (`Call_Status` 0x02, incoming call).

HF link: `bm83_hf_connected()` is true after `BTM_Status` 0x05 (HF/HS link established) and false
after 0x07 (HF link disconnected), 0x11 (ACL disconnected) or 0x00 (power off) and at power-up.

| State       | Entry action | Transitions |
|-------------|--------------|-------------|
| `tone_idle` | TXA disconnected, oscillators off | `TONE_EVT_OFFHOOK` with an incoming call ringing → `tone_mute` (call answered, no dial tone); `TONE_EVT_OFFHOOK` and `bm83_hf_connected()` → `tone_dial`; without HF link it stays in `tone_idle` (handset silent). Incoming call ringing: `TONE_EVT_CALL_INCOMING` received, not yet followed by `TONE_EVT_CALL_IDLE` (caller gave up) |
| `tone_dial` | `melody = ger_dial`, `ring_start()` (ringer_on = false: no ring mode); exit: `ring_stop()` | `TONE_EVT_OPEN` → `tone_mute` |
| `tone_mute` | TXA disconnected, oscillators off | `TONE_EVT_CALL_IDLE` → `tone_bussy` |
| `tone_bussy` | `melody = ger_enga`, `ring_start()` (ringer_on = false); exit: `ring_stop()` | – |

`TONE_EVT_MAIN_IDLE` takes every state to `tone_idle`. `bt_fsm` sets `melody = gb_ring` before its
`ring_start()` in `BT_RING_BELL`.

## Bell (ring.c)

POT_FWD_PIN (GP2) is the output of PWM1 (slice 1 channel A, pinctrl `pwm_ch1a_default` in the
overlay) from power-up on, 25 Hz. Duty 100 % = constantly high, 50 % = bell rings. Duty changes take
effect at the end of the current PWM period (double-buffered, up to 40 ms). GP2 must never be
configured as a GPIO (that would disconnect it from the PWM).

`bell_fsm` (own thread, events in `bell_event_q`, `BELL_TIMER`):

| State          | RING_MODE | POT_FWD | Entry / transitions |
|----------------|-----------|---------|---------------------|
| `BELL_OFF`     | low       | 100 %   | → `BELL_RM_ON` when `ringer_on` AND NOT `shk_active` |
| `BELL_RM_ON`   | high      | 100 %   | start `BELL_TIMER` 50 ms; expired → `BELL_PWM_ON` |
| `BELL_PWM_ON`  | high      | 50 %    | → `BELL_PWM_OFF` when NOT `ringer_on` OR `shk_active` |
| `BELL_PWM_OFF` | high      | 100 %   | start `BELL_TIMER` 90 ms; expired → `BELL_OFF` |

- `ringer_on`: `ringer_on` of the current cadence segment (`BELL_EVT_RINGER_ON/OFF` from `ring_thread`;
  false after `ring_stop()`).
- `shk_active`: last debounced shk event was `EVT_SHK_ACTIVE`. `fsm_main` forwards every
  `EVT_SHK_ACTIVE` / `EVT_SHK_INACTIVE` as `BELL_EVT_SHK_ACTIVE/INACTIVE`; the shk pins are not read.
- Conditions are re-checked on every input change and on entering `BELL_OFF` / `BELL_PWM_ON`.
- RING_MODE is high at least 50 ms before and 90 ms after the PWM runs at 50 %.
- More threads will follow, all running in parallel.

## Interrupts

- All bank-0 GPIOs share one NVIC interrupt (IO_IRQ_BANK0). The Zephyr GPIO driver
  owns the NVIC handler and calls `gpio_int` with a bit mask of the pins that fired.
- `gpio_int` is the single common ISR for all GPIO interrupt pins. To add a pin:
  add it to `GPIO_INT_PIN_MASK` and add an `if (pins & BIT(n))` branch in `gpio_int`.
- The ISR contains no FSM logic. It only starts timers or posts events with
  `fsm_post_event()` (ISR-safe, K_NO_WAIT) into the `fsm_event_q` message queue.
- shk events (in `pico_io.c`): `SHK_RISING_PIN` (GP11) and `SHK_FALLING_PIN` (GP12) are
  connected together outside the Pico; one pin detects rising edges, the other falling edges.
  An edge starts a one-shot timer of `SHK_CHECK_MS` (20 ms); its expiry function checks the
  pin level and posts the event:
  - `EVT_SHK_INACTIVE`: falling edge on `SHK_FALLING_PIN`, pin still low after 20 ms.
  - `EVT_SHK_ACTIVE`: rising edge on `SHK_RISING_PIN`, pin still high after 20 ms.
- This edge + 20 ms level check is the shk debounce. Only after the check is a high or low
  level valid. **Never use `SHK_RISING_PIN` / `SHK_FALLING_PIN` directly in the logic**
  (no `gpio_pin_get()` on them in the FSMs); use only `EVT_SHK_ACTIVE` / `EVT_SHK_INACTIVE`.
- No other debounce. Do not add one on your own.

## State machine

The FSM is event-driven: `fsm_thread` blocks on `fsm_event_q` (K_FOREVER), with no polling.
Each state has its own handler function. State changes go through `fsm_enter_state()`,
which runs the entry action of the new state.

Timers (in `fsm_main.c`, one-shot, the expiry function only posts an event):

| Timer          | Timeout | Event                |
|----------------|---------|----------------------|
| `pulse_timer`  | 100 ms  | `EVT_PULSE_TIMEOUT`  |
| `hangup_timer` | 100 ms  | `EVT_HANGUP_TIMEOUT` |
| `dialcmpl_timer` | 3 s   | `EVT_DIALCMPL`       |

A timer counts as expired when `k_timer_remaining_ticks() == 0`. Timeout events are
only acted on if the timer is really expired, so stale events from a restarted timer
are ignored.

States (`s0_idle` is `STATE_S0_IDLE` in C, etc.):

| State        | Entry action                  | Transitions                                                                |
|--------------|-------------------------------|----------------------------------------------------------------------------|
| `s0_idle`    | LED off                       | `EVT_SHK_ACTIVE` → `s1_offhook`                                            |
| `s1_offhook` | LED on                        | `EVT_SHK_INACTIVE` → `s2_open`                                             |
| `s2_open`    | LED off, start `hangup_timer` | `EVT_SHK_ACTIVE`, timer not expired → `s3_closed`; timer expired → `s0_idle` |
| `s3_closed`  | LED off, start `pulse_timer`  | `EVT_SHK_INACTIVE`, timer not expired → `s2_open`; timer expired → `s1_offhook` |

If a shk event arrives after the state's timer has expired but before its timeout
event is processed, the FSM takes the timeout transition first and then handles the
shk event in the new state.

Dial pulses and dialled digits:

- `pulse_counter` counts how often `s3_closed` is entered (entry action).
- `s1_offhook` → `s2_open`: `pulse_counter` is cleared and `dialcmpl_timer` is stopped,
  so `EVT_DIALCMPL` only comes 3 s after the last digit.
- `s3_closed` → `s1_offhook` (`fsm_s3_to_s1()`): `pulse_counter` is stored in
  `number_array` at `number_array_pointer`, the pointer is incremented and
  `dialcmpl_timer` is (re)started.
- `number_array` holds up to `NUMBER_ARRAY_SIZE` (19) digits; further digits are dropped.
- `s0_idle` → `s1_offhook`: `number_array` is cleared, `number_array_pointer` = 0.
- `EVT_DIALCMPL` (handled in every state, before the state handler): `number_array` is
  copied to `dialed_number_reg` (`dialed_number_len` = number of digits) and displayed,
  then `number_array` is cleared and `number_array_pointer` = 0. If `fsm_main` is in
  `s1_offhook`, `BT_EVT_DIALCMPL` is posted to `bt_fsm`.
- Digits are raw pulse counts: a dialled 0 is stored as 10.

After power-up the FSM starts in `s0_idle`. More states will be added later.

## Bluetooth state machine (bt_fsm.c)

Second FSM, same structure as `fsm_main`: `bt_fsm_thread` blocks on `bt_fsm_event_q`,
one handler per state, state changes through `bt_fsm_enter_state()`.

Events:

- `BT_EVT_MAIN_IDLE` (`fsm_main`): posted whenever `fsm_main` enters `s0_idle`.
- `BT_EVT_DIALCMPL` (`fsm_main`): `dialcmpl_timer` expired while `fsm_main` is in `s1_offhook`.
- `BT_EVT_CALL_ACK_OK` / `BT_EVT_CALL_ACK_FAIL` (`bt_rx_thread`, `bm83_command_ack_received()`):
  `Command_ACK` of `Make_Call` with status 0x00 / not 0x00. `BT_EVT_CALL_ACK_FAIL` is also posted
  by `bt_fsm` itself if no `Command_ACK` arrives (200 ms). The ACK result is posted from
  `bt_rx_thread` so it is queued before the `Call_Status` event that follows the ACK.
- `BT_EVT_CALL_OUTGOING` (`bt_rx_thread`, `bm83_handle_event()`): `Call_Status` (0x02) reports
  outgoing call (0x03) or call active (0x04).
- `BT_EVT_CALL_IDLE` (`bt_rx_thread`, `bm83_handle_event()`): `Call_Status` reports idle (0x00),
  the call has ended.
- `BT_EVT_CALL_INCOMING` (`bt_rx_thread`, `bm83_handle_event()`): `Call_Status` reports incoming
  call (0x02).
- `BT_EVT_OFFHOOK` (`fsm_main`): `s0_idle` → `s1_offhook` on `EVT_SHK_ACTIVE` (handset lifted).
  `bt_fsm` never reads the shk pins; it only gets this forwarded event.

| State                | Entry action | Transitions |
|----------------------|--------------|-------------|
| `BT_IDLE`            | – | `BT_EVT_DIALCMPL` → `BT_MAKE_CALL`; `BT_EVT_CALL_INCOMING` → `BT_RING_BELL` |
| `BT_MAKE_CALL`       | `Make_Call` (0x00) with `dialed_number_reg` as ASCII | `BT_EVT_MAIN_IDLE` → `BT_IDLE`; `BT_EVT_CALL_ACK_OK` → `BT_CALLER_WAIT`; `BT_EVT_CALL_ACK_FAIL` → `BT_NOHF` |
| `BT_CALLER_WAIT`     | – | `BT_EVT_MAIN_IDLE` → `BT_IDLE`, sending MMI action 0x06 "End call"; `BT_EVT_CALL_OUTGOING` → `BT_OUT_ESTABLISHED` |
| `BT_NOHF`            | – | `BT_EVT_MAIN_IDLE` → `BT_IDLE` |
| `BT_OUT_ESTABLISHED` | – | `BT_EVT_MAIN_IDLE` → `BT_IDLE`, sending MMI action 0x06 "End call" (`bm83_end_call()`, `AA 00 03 02 00 06 F5`); `BT_EVT_CALL_IDLE` → `BT_IDLE` (no command) |
| `BT_RING_BELL`       | `ring_start()` | `BT_EVT_OFFHOOK` → `ring_stop()`, → `BT_OUT_ESTABLISHED`, sending MMI action 0x04 "Accept an incoming call" (`bm83_accept_call()`, `AA 00 03 02 00 04 F7`); `BT_EVT_CALL_IDLE` → `ring_stop()`, → `BT_IDLE` (caller hung up before answering) |

`BT_EVT_MAIN_IDLE` has the highest priority: every state except `BT_IDLE` and `BT_RING_BELL` goes to
`BT_IDLE` on it. In `BT_RING_BELL` the handset is on hook (`fsm_main` already in `s0_idle`).

Phone number: `fsm_get_dialed_number()` copies `dialed_number_reg` (pulse counts); `bt_fsm`
converts 1–9 to '1'–'9' and 10 to '0' (other values are skipped with a warning) and sends it
with `bm83_make_call()`: `AA | len | 00 | 00 (database) | ASCII digits (max 19) | checksum`.
All BM83 commands with a `Data_Base_Index` use 0x00 (`BM83_DATABASE_INDEX`).
Before every `Make_Call`, `bt_fsm` sends `Read_Link_Status` (0x0D) for debugging; the reply
(event 0x1E) is printed by `bm83_handle_event()` with device state and, per database (0 and 1),
which profiles are connected (bit 0 A2DP signaling, 1 A2DP stream, 2 AVRCP, 3 HF, 4 SPP).
`Make_Call` status 0x01 "Command disallow" means "No HF connection exist".

After power-up `bt_fsm` starts in `BT_IDLE`.

## BM83 UART (bt_uart.c)

Frame (both directions): `0xAA | length(h) | length(l) | opcode | params (0..20) | checksum`

- `length = 1 (opcode) + number of parameter bytes`
- `checksum = 1 + ~(length(h) + length(l) + opcode + params)` (8 bit)
- Bytes between frames (e.g. `0x00`) are ignored until the next `0xAA`.

Transmit is modular: `bt_uart_build_frame()` builds the frame incl. checksum,
`bt_uart_send()` sends bytes (blocking `uart_poll_out`). BM83 commands in `bm_83.c` use both.

Receive: the UART1 RX interrupt `bt_uart_isr` collects one frame (length-driven) and
puts the raw frame into `bt_rx_q`. `bt_rx_thread` checks the checksum first; only if it
is correct, `rx_buffer[32]` is filled with header, opcode, params, rest `0x00`, and
the BM83 event name (from `bm83_event_names[]`, "unknown" if not listed), checksum,
opcode and params are printed. A wrong checksum prints an error. Frames with
a length of 0 or more than 21 are dropped in the ISR.

## BM83 power-up (bm_83.c)

`bm83_init()` (blocks about 650 ms):

1. `bt_rst_n` (GPIO6) and `bt_mfb` (GPIO8): outputs, low.
2. 100 ms → `bt_mfb` high.
3. 20 ms → `bt_rst_n` high (BM83 out of reset).
4. 500 ms wait (`bt_mfb` still high).
5. MMI action 0x51 (power on button press): `AA 00 03 02 00 51 AA`.
6. MMI action 0x52 (power on button release): `AA 00 03 02 00 52 A9`.
7. Read_BTM_Version 0x08, type 0x01 = firmware version (0x00 = UART version):
   `AA 00 02 08 01 F5`. Reply: event 0x18 `Read_BTM_Version_Reply`, params type, version (2 bytes).
8. MMI action 0x5D (fast enter pairing mode, with tone): `AA 00 03 02 00 5D 9E`.
   BM83 reports `BTM_Status` 0x01 (pairing), then 0x03 (paired) or 0x04 (failed).
9. `bt_mfb` low (each command waits for its `Command_ACK` first).

MFB handshake: in host MCU mode MFB is the BM83 `UART_RX_IND` input ("UART data from the MCU
follows", Audio UART Command Set 4.1/4.4/4.5), as in Microchip's PIC32 demo:

- Command (`bm83_send_cmd()`, main thread): MFB high → ≥ 3 ms (`BM83_MFB_LEAD_MS`) → send →
  wait for the matching `Command_ACK` (max 200 ms, `BM83_CMD_ACK_TIMEOUT_MS`) → MFB low.
  `bt_rx_thread` passes each `Command_ACK` to `bm83_command_ack_received()`, which gives a
  semaphore. Returns -ETIMEDOUT without ACK, -EIO for a status other than "command complete".
- `Event_ACK` (`bt_rx_thread`): MFB high → ≥ 3 ms → send → MFB low (no reply).
- MFB is reference counted (`bm83_mfb_get()/put()`): high with the first sender, low after
  the last, so an Event_ACK cannot drop MFB while a command waits for its ACK.
- During power-up MFB stays high from the "MFB high" step until the last power-up command is
  acknowledged, then goes low.
- Never send commands from `bt_rx_thread`: it would wait for a Command_ACK only it can deliver.

The BM83 must be programmed with a **host MCU mode** configuration (Config Tool: "Host MCU
Mode", UART 115200). The factory/embedded configuration (`SYS_CFG_KEY_OPTION1` = 0xC0) has the
UART command interface disabled: the BM83 then ignores all UART commands and never transmits.
Programming: isUpdate tool over J3 (USB-UART), BM83 in test mode (J5 fitted), Pico in BOOTSEL
(or firmware built with `BT_UART_TX_DISABLE` = 1 in `bt_uart.h`: GP4 tristate, `bm83_init()` skipped,
SLIC powered down with POT_PDOWN_N (GP3) low),
`image num` = 3 with 8051 + DSP + config hex selected together in one file dialog.

BM83 configuration on the module (IS208x Config Tool / UI 1.3.25, based on Microchip's
`IS208x_UI_1.3.23_Demo_Package_Host_Mode_SPP.hex`, firmware MSPKv2 1.03.0406 SPP). These settings
exist only in the Config Tool and on the module, not in this repository:

| Setting | Value | Why |
|---|---|---|
| Operating mode | Host MCU Mode, UART 115200 | UART command interface (demo default: embedded, UART off) |
| CODEC Type | Internal Codec | analog output on AOHPL/AOHPR (demo: I2S, pins 1–5 unconnected) |
| CODEC Function → Speaker Output | Single Ended External Amp | board has DC-blocking caps C25/C26, AOHPM floats; raised the output from ~50 mV RMS to ~530 mV RMS (1.5 Vpp at TP17) |
| Output Volume Scaling Method | 0x0: Codec DAC gain | |
| Digital Mic | Disable | analog mic on MICP1/MICN1 (demo: digital mic) |
| ADC Path | 5 (MIC1 to L), mic bias off | MICBIAS (pin 15) is not connected |
| Audio Gain Table | default (level 15 = 0xFC = 0 dB) | |

The SLIC (AG1171, J4 pin 9 via RV1 and R12 47k) needs about 500 mV RMS audio input.

`bm83_mmi_action(action)` sends opcode `0x02` with params `data_base_index 0x00, action`.
`bm83_read_btm_version(type)` sends opcode `0x08` with param `type`. Both use `bm83_send_cmd()`.

Speaker volume: no gain commands are sent. `Set_Overall_Gain` (0x23, A2DP/HF level 15 on link
established) and `Set_HF_Speaker_Gain_Level` (0x1B, level 0x0F at init and on call active) were
tried and had no audible effect; the phone sets the volume itself (AVRCP absolute volume,
`Report_HF_Gain_Level`). Volume comes from the BM83 configuration ("Speaker Output = Single Ended
External Amp") and the phone.

Event acknowledge: `bt_rx_thread` answers every received event except `Command_ACK` (0x00) with
`bm83_event_ack(opcode)` (Event_ACK 0x14, e.g. `AA 00 02 14 01 E9` for BTM_Status). Without it the
BM83 waits 800 ms before sending the next event (Audio UART Command Set 4.5.2).
`bt_uart_send()` is protected by `bt_tx_mutex` because main (commands) and `bt_rx_thread`
(Event_ACK) both send.
Reference: Microchip IS2083 Turnkey 1.2.4 (`AudioUARTCommandSet_v2.09.pdf`, PIC32 demo source
`BT Common/bt_command_send.c`) in `../doc/BM83 Software/`.

## Code style

- Zephyr style: tabs for indentation, K&R braces, logging with `LOG_MODULE_REGISTER` per file.
- Comments and identifiers in English.
