/* uart_servo.h — generic UART bus for serial servo controllers.
 *
 * Greenfield module added in Phase 8 of the pin-function-map refactor
 * so the user can experiment with a 16-channel UART servo board
 * (Feetech / Waveshare style) without further firmware changes. The
 * module is intentionally protocol-agnostic — it just opens UART_NUM_2
 * on the pin-map-configured TX + RX pins at the schema-configured
 * baud rate, and exposes raw byte send/receive. The servo protocol
 * (Feetech, Dynamixel, custom) is the operator's concern; once they
 * know what their hardware speaks, they can either:
 *
 *   (a) Build the protocol on the host side via the
 *       POST /debug/uart_servo HTTP endpoint (hex-encoded payload in,
 *       hex-encoded response out — handy for reverse-engineering an
 *       unknown servo board), or
 *   (b) Add a thin protocol layer here once stabilized.
 *
 * Pin map: requires both uart_tx and uart_rx assigned in the pin
 * function map. If only one side is mapped, init refuses to bring up
 * the UART (same half-pair guard as I²C). The ESP32-S3 GPIO matrix
 * can route UART2 to any GPIO, so the user can put TX/RX on any pair
 * of the free XIAO breakouts (D4-D7 are I²C by default; remap those
 * via the pin map to free them for UART).
 *
 * Caveats:
 *   - D6/D7 (GPIO43/44) are the chip's UART1 boot console. Remapping
 *     them to UART2 here loses the USB-serial boot log; the pin-map
 *     setter already warns about this.
 *   - UART2 is the only free hardware UART. UART0 is USB CDC console,
 *     UART1 is bootloader log.
 *   - No protocol-level framing — caller is responsible for protocol
 *     checksums, timing, half-duplex direction enable if applicable. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize UART2 on the pins assigned to uart_tx / uart_rx in the
 * pin function map. Idempotent. Returns:
 *   ESP_OK         — UART is up and ready
 *   ESP_ERR_INVALID_STATE — pin map half-broken (only TX or only RX mapped)
 *   ESP_ERR_NOT_FOUND     — neither TX nor RX mapped (module disabled)
 *   ESP_FAIL              — driver install or pin assignment failed */
esp_err_t uart_servo_init(void);

/* True iff the UART is open and ready for raw I/O. */
bool uart_servo_ready(void);

/* Raw byte write. Returns number of bytes actually sent (≤ len), or
 * -1 on driver error. Blocks up to ~100 ms while the TX FIFO drains. */
int uart_servo_write(const uint8_t *buf, size_t len);

/* Raw byte read. Returns number of bytes received (≤ max), 0 on
 * timeout, or -1 on driver error. */
int uart_servo_read(uint8_t *buf, size_t max, uint32_t timeout_ms);

/* TX/RX GPIO numbers the module is currently bound to, or -1 each
 * when not initialized. For selftest + diagnostic display. */
void uart_servo_pins(int *tx_out, int *rx_out);

#ifdef __cplusplus
}
#endif
