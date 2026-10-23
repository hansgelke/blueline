# blueline

Firmware for a Raspberry Pi Pico 2 (SoC RP2350), running Zephyr RTOS.
The project is developed step by step; implement only what is asked for, nothing extra.

## Build & flash

- Board target: `rpi_pico2/rp2350a/m33`
- `west` is not on the PATH. Activate the Zephyr environment first:
  `source ~/zephyrproject/.venv/bin/activate` (Zephyr base: `~/zephyrproject/zephyr`)
- Build (run from `code_blueline/`, output goes to `code_blueline/build/`):
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

| Signal | GPIO | Config                         | Logic                          |
|--------|------|--------------------------------|--------------------------------|
| Button | 15   | input, internal pull-up        | high = released, low = pushed  |
| LED    | 25   | output (Pico 2 user LED)       | high = on                      |

## Threads

- `fsm_thread`: runs `fsm_main()`, priority 5, stack 2048.
- More threads will follow, all running in parallel.

## Interrupts

- All bank-0 GPIOs share one NVIC interrupt (IO_IRQ_BANK0). The Zephyr GPIO driver
  owns the NVIC handler and calls `gpio_int` with a bit mask of the pins that fired.
- `gpio_int` is the single common ISR for all GPIO interrupt pins. To add a pin:
  add it to `GPIO_INT_PIN_MASK` and add an `if (pins & BIT(n))` branch in `gpio_int`.
- The button interrupt fires on the falling edge (button pushed). Release does nothing.
- The ISR contains no FSM logic. It only posts events with `fsm_post_event()`
  (ISR-safe, K_NO_WAIT) into the `fsm_event_q` message queue.
- **No debounce.** The button is assumed to switch cleanly. A dedicated debounce
  will be added later by the developer; do not add one on your own.

## State machine

The FSM is event-driven: `fsm_thread` blocks on `fsm_event_q` (K_FOREVER), with no polling.
Each state has its own handler function. State changes go through `fsm_enter_state()`,
which runs the entry action of the new state.

States (C names cannot start with a digit, so `1_idle` becomes `STATE_1_IDLE`):

| State       | Entry action | Event `FSM_EV_SHK_PRESSED` |
|-------------|--------------|-------------------------------|
| `1_idle`    | LED off      | go to `2_offhook`             |
| `2_offhook` | LED on       | go to `1_idle`                |

After power-up the FSM starts in `1_idle`. More states will be added later.

## Code style

- Zephyr style: tabs for indentation, K&R braces, logging with `LOG_MODULE_REGISTER` per file.
- Comments and identifiers in English.
