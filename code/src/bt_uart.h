/*
 * blueline - bt_uart.h
 * UART1 link to the Microchip BM83 Bluetooth module: frame build/send and
 * frame receive.
 *
 * Frame format (both directions):
 *   0xAA | length(h) | length(l) | opcode | parameters (0..20) | checksum
 *   length   = 1 (opcode) + number of parameter bytes
 *   checksum = 1 + ~(length(h) + length(l) + opcode + parameters)
 * Bytes between frames (e.g. 0x00) are ignored until the next 0xAA.
 */

#ifndef BT_UART_H
#define BT_UART_H

#include <stddef.h>
#include <stdint.h>

#define BT_FRAME_HEADER      0xAA
#define BT_FRAME_MAX_PARAMS  20

/* Longest frame: header + length(h) + length(l) + opcode + params + checksum */
#define BT_FRAME_MAX_LEN     (4 + BT_FRAME_MAX_PARAMS + 1)

/* UART1 TX pin (RP2350 -> BM83 UART_RXD, pin 29) */
#define BT_UART_TX_PIN       4

/* Programming the BM83 with isUpdate (USB-UART adapter on J3):
 * 1 = GP4 is switched to a GPIO input without pulls (tristate) so the adapter
 *     can drive the BM83 RX line, bt_uart_send() sends nothing, and main()
 *     skips bm83_init() so GP6 (reset) and GP8 (MFB) stay undriven inputs.
 *     Receiving on GP5 still works.
 * 0 = normal operation. */
#define BT_UART_TX_DISABLE   0

/* Size of the receive buffer array */
#define BT_UART_RX_BUF_SIZE  32

/* Initialise UART1 and enable the receive interrupt.
 * Returns 0 on success, negative errno otherwise. */
int bt_uart_init(void);

/* Build a complete frame (header, length, opcode, parameters, checksum) in
 * frame[]. Returns the frame length in bytes, or -EINVAL if num_params is too
 * large or frame[] is too small. */
int bt_uart_build_frame(uint8_t *frame, size_t size, uint8_t opcode,
			const uint8_t *params, uint8_t num_params);

/* Send len bytes on UART1 (blocking). */
void bt_uart_send(const uint8_t *data, size_t len);

/* Entry point of bt_rx_thread: checks and prints received frames. */
void bt_rx_main(void *p1, void *p2, void *p3);

#endif /* BT_UART_H */
