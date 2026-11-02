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

- `main.c / main.h`: system start. Calls `pico_io_init()` and `bt_uart_init()`, then creates all threads.
  Thread stack sizes and priorities are defined in `main.h`. New threads are added here.
- `fsm_main.c / fsm_main.h`: the state machine, running in `fsm_thread`.
- `pico_io.c / pico_io.h`: GPIO initialisation, the common GPIO ISR `gpio_int`, and LED control.
- `bt_uart.c / bt_uart.h`: UART1 link to the BM83 Bluetooth module (frame build/send,
  receive ISR, frame check). Everything UART-related goes here.

New source files must be added to `CMakeLists.txt`.

## Hardware

| Signal            | GPIO | Config                         | Logic                          |
|-------------------|------|--------------------------------|--------------------------------|
| `SHK_FALLING_PIN` | 12   | input, internal pull-up        | IRQ on falling edge            |
| `SHK_RISING_PIN`  | 11   | input, internal pull-up        | IRQ on rising edge             |
| `RING_MODE_PIN`   | 7    | output (SLIC)                  | low after init                 |
| `PDOWN_N_PIN`     | 3    | output (SLIC)                  | high after init                |
| `POT_FWD_PIN`     | 2    | output (SLIC)                  | high after init                |
| LED               | 25   | output (Pico 2 user LED)       | high = on                      |
| UART1 TX (BM83)   | 4    | UART1, 115200 8N1, no flow ctl | RP2350 → BM83                  |
| UART1 RX (BM83)   | 5    | UART1, 115200 8N1, no flow ctl | BM83 → RP2350                  |

UART1 is set up in the board overlay (`boards/rpi_pico2_rp2350a_m33.overlay`).
The console stays on UART0 (GPIO0/1).
The stock board's i2c0 (GPIO4/5) and i2c1 (GPIO6/7) are disabled in the overlay because
those pins are used for UART1 and RING_MODE.

## Threads

- `fsm_thread`: runs `fsm_main()`, priority 5, stack 2048.
- `bt_rx_thread`: runs `bt_rx_main()`, priority 6, stack 1024. Checks and prints BM83 frames.
- `bt_tx_thread`: runs `bt_tx_test_main()`, priority 7, stack 1024. Test only: `tx_test()`
  sends one frame per second forever. Will be replaced by the real transmit function.
- More threads will follow, all running in parallel.

## Interrupts

- All bank-0 GPIOs share one NVIC interrupt (IO_IRQ_BANK0). The Zephyr GPIO driver
  owns the NVIC handler and calls `gpio_int` with a bit mask of the pins that fired.
- `gpio_int` is the single common ISR for all GPIO interrupt pins. To add a pin:
  add it to `GPIO_INT_PIN_MASK` and add an `if (pins & BIT(n))` branch in `gpio_int`.
- The ISR contains no FSM logic. It only starts timers or posts events with
  `fsm_post_event()` (ISR-safe, K_NO_WAIT) into the `fsm_event_q` message queue.
- shk events (in `pico_io.c`): an edge starts a one-shot timer of `SHK_CHECK_MS`
  (10 ms); its expiry function checks the pin level and posts the event:
  - `EVT_SHK_INACTIVE`: falling edge on `SHK_FALLING_PIN`, pin still low after 10 ms.
  - `EVT_SHK_ACTIVE`: rising edge on `SHK_RISING_PIN`, pin still high after 10 ms.
- No other debounce. Do not add one on your own.

## State machine

The FSM is event-driven: `fsm_thread` blocks on `fsm_event_q` (K_FOREVER), with no polling.
Each state has its own handler function. State changes go through `fsm_enter_state()`,
which runs the entry action of the new state.

Timers (in `fsm_main.c`, one-shot, the expiry function only posts an event):

| Timer          | Timeout | Event                |
|----------------|---------|----------------------|
| `pulse_timer`  | 50 ms   | `EVT_PULSE_TIMEOUT`  |
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
  `number_queue` and `dialcmpl_timer` is (re)started.
- `number_queue` holds up to `NUMBER_QUEUE_SIZE` (19) digits; further digits are dropped.
  Its pointer goes back to the first digit on `s0_idle` → `s1_offhook` and on
  `EVT_DIALCMPL` (handled in every state, before the state handler).
- What else `EVT_DIALCMPL` does will be defined later.

After power-up the FSM starts in `s0_idle`. More states will be added later.

## BM83 UART (bt_uart.c)

Frame (both directions): `0xAA | length(h) | length(l) | opcode | params (0..20) | checksum`

- `length = 1 (opcode) + number of parameter bytes`
- `checksum = 1 + ~(length(h) + length(l) + opcode + params)` (8 bit)
- Bytes between frames (e.g. `0x00`) are ignored until the next `0xAA`.

Transmit is modular: `bt_uart_build_frame()` builds the frame incl. checksum,
`bt_uart_send()` sends bytes (blocking `uart_poll_out`). `tx_test()` only combines them
for testing: opcode `0x02`, params `0xAA 0x55 0xAA 0x55` → `AA 00 05 02 AA 55 AA 55 FB`.

Receive: the UART1 RX interrupt `bt_uart_isr` collects one frame (length-driven) and
puts the raw frame into `bt_rx_q`. `bt_rx_thread` checks the checksum first; only if it
is correct, `rx_buffer[32]` is filled with header, opcode, params, rest `0x00`, and
checksum, opcode and params are printed. A wrong checksum prints an error. Frames with
a length of 0 or more than 21 are dropped in the ISR.

## Code style

- Zephyr style: tabs for indentation, K&R braces, logging with `LOG_MODULE_REGISTER` per file.
- Comments and identifiers in English.
