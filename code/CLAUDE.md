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

- `main.c / main.h`: system start. Calls `pico_io_init()`, then creates all threads.
  Thread stack sizes and priorities are defined in `main.h`. New threads are added here.
- `fsm_main.c / fsm_main.h`: the state machine, running in `fsm_thread`.
- `pico_io.c / pico_io.h`: GPIO initialisation, the common GPIO ISR `gpio_int`, and LED control.

New source files must be added to `CMakeLists.txt`.

## Hardware

| Signal            | GPIO | Config                         | Logic                          |
|-------------------|------|--------------------------------|--------------------------------|
| `SHK_FALLING_PIN` | 15   | input, internal pull-up        | IRQ on falling edge            |
| `SHK_RISING_PIN`  | 14   | input, internal pull-up        | IRQ on rising edge             |
| LED               | 25   | output (Pico 2 user LED)       | high = on                      |

## Threads

- `fsm_thread`: runs `fsm_main()`, priority 5, stack 2048.
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

After power-up the FSM starts in `s0_idle`. More states will be added later.

## Code style

- Zephyr style: tabs for indentation, K&R braces, logging with `LOG_MODULE_REGISTER` per file.
- Comments and identifiers in English.
