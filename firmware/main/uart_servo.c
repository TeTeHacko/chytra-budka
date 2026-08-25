/* uart_servo.c — see uart_servo.h. */

#include "uart_servo.h"

#include <stdatomic.h>

#include "app_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_servo";

/* Use UART2 — UART0 is the USB CDC console (idf monitor) and UART1
 * carries the bootloader log via the chip's strapping pins. UART2 is
 * the only HW UART left, but the GPIO matrix lets it route to any
 * pad so the pin map can choose freely. */
#define UART_PORT UART_NUM_2

#define RX_BUF_BYTES 512
#define TX_BUF_BYTES 256

static atomic_bool s_ready  = ATOMIC_VAR_INIT(false);
static atomic_int  s_tx_pin = ATOMIC_VAR_INIT(-1);
static atomic_int  s_rx_pin = ATOMIC_VAR_INIT(-1);

esp_err_t uart_servo_init(void) {
    if (atomic_load(&s_ready)) return ESP_OK;

    int tx = app_config_pin_for_first("uart_tx");
    int rx = app_config_pin_for_first("uart_rx");
    if (tx < 0 && rx < 0) {
        ESP_LOGI(TAG, "no GPIO mapped to uart_tx / uart_rx in pin map — module idle");
        return ESP_ERR_NOT_FOUND;
    }
    if (tx < 0 || rx < 0) {
        ESP_LOGW(TAG, "pin map half-broken (tx=%d rx=%d) — UART not initialised",
                 tx, rx);
        return ESP_ERR_INVALID_STATE;
    }

    int32_t baud = app_config_get_int("uart_baud");
    if (baud < 9600 || baud > 1000000) baud = 1000000;

    uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t e = uart_driver_install(UART_PORT, RX_BUF_BYTES, TX_BUF_BYTES,
                                      /*queue size*/ 0, /*queue*/ NULL,
                                      /*intr flags*/ 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(e));
        return e;
    }
    e = uart_param_config(UART_PORT, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "param_config: %s", esp_err_to_name(e));
        uart_driver_delete(UART_PORT);
        return e;
    }
    e = uart_set_pin(UART_PORT, tx, rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_pin tx=%d rx=%d: %s", tx, rx, esp_err_to_name(e));
        uart_driver_delete(UART_PORT);
        return e;
    }

    atomic_store(&s_tx_pin, tx);
    atomic_store(&s_rx_pin, rx);
    atomic_store(&s_ready, true);
    ESP_LOGI(TAG, "ready on UART2 (tx=GPIO%d rx=GPIO%d, baud=%" PRId32 ")",
             tx, rx, baud);
    return ESP_OK;
}

bool uart_servo_ready(void) { return atomic_load(&s_ready); }

int uart_servo_write(const uint8_t *buf, size_t len) {
    if (!atomic_load(&s_ready) || !buf || len == 0) return -1;
    int n = uart_write_bytes(UART_PORT, buf, len);
    if (n < 0) return -1;
    /* Best-effort drain so a caller that expects a response sees the
     * device's reply rather than racing the TX FIFO. 100 ms upper
     * bound — anything slower than that is the host's UART misconfig,
     * not the line. */
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
    return n;
}

int uart_servo_read(uint8_t *buf, size_t max, uint32_t timeout_ms) {
    if (!atomic_load(&s_ready) || !buf || max == 0) return -1;
    int n = uart_read_bytes(UART_PORT, buf, max, pdMS_TO_TICKS(timeout_ms));
    return n;  /* uart_read_bytes returns count or -1 */
}

void uart_servo_pins(int *tx_out, int *rx_out) {
    if (tx_out) *tx_out = atomic_load(&s_tx_pin);
    if (rx_out) *rx_out = atomic_load(&s_rx_pin);
}
