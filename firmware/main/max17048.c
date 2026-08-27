/* max17048.c — MAX17048 fuel-gauge driver, instance-based. See max17048.h.
 *
 * Generalised from the original battery.c bus0 implementation: the same
 * probe+retry+budget loop now runs over an i2c_xport_t, so a bus0 (HW) and
 * a bus1 (bit-bang) instance share one tuned, clone-tolerant read path. */

#include "max17048.h"

#include <stdlib.h>

#include "cb_time.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_xport.h"
#include "log_throttle.h"

static const char *TAG = "max17048";

/* Wall-clock budget per read_reg call. Worst-case clone behaviour with the
 * 30-round × 10-probe loop would block ~12 s; three back-to-back telemetry
 * reads would then exceed the 30 s task watchdog. 2 s/call keeps headroom. */
#define READ_BUDGET_US (2LL * 1000 * 1000)

/* After this many consecutive failed reads past init, mark the gauge
 * un-ready so future calls return the sentinel immediately without touching
 * the bus. Above the observed clone ACK rate (~1/3) so transient NACKs don't
 * kill it. */
#define MAX_CONSECUTIVE_FAILS 8

struct max17048 {
    i2c_xport_t x;
    bool ready;
    int  consecutive_fails;
};

static esp_err_t write_reg(max17048_t *m, uint8_t reg, uint16_t val) {
    uint8_t tx[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_xport_tx(&m->x, tx, 3, 100);
}

esp_err_t max17048_read_reg(max17048_t *m, uint8_t reg, uint16_t *out) {
    if (!m)
        return ESP_ERR_INVALID_STATE;
    uint8_t rx[2] = {0};
    /* Plain bounded read. I²C is ACK/NACK — the gauge answers or it doesn't.
     * We do NOT hard-reset the shared bus0 between tries: a bus recovery is for
     * a genuinely stuck bus (ESP-IDF I2C docs), not one device's missed read —
     * doing it per-round here thrashed the OLED/SHT41/BMP388 on the same bus. A
     * couple of quick retries cover a transient; sustained failure = absent. */
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        e = i2c_xport_txrx(&m->x, &reg, 1, rx, 2, 100);
        if (e == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (e != ESP_OK) {
        LOG_THROTTLED_W(TAG, 60000, "read reg 0x%02x (bus%d): %s",
                        reg, (int)m->x.bus, esp_err_to_name(e));
        if (m->ready) {
            m->consecutive_fails++;
            if (m->consecutive_fails >= MAX_CONSECUTIVE_FAILS) {
                ESP_LOGE(TAG, "MAX17048 (bus%d) degraded: %d consecutive failures, "
                         "marking un-ready until reboot/re-probe",
                         (int)m->x.bus, m->consecutive_fails);
                m->ready = false;
            }
        }
        return e;
    }
    m->consecutive_fails = 0;
    *out = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

max17048_t *max17048_create(cb_bus_t bus, uint8_t addr) {
    max17048_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    /* scl_wait_us 20000: headroom for clone pull-up rise time (bus0 only). */
    if (i2c_xport_open(&m->x, bus, addr, 100000, 20000) != ESP_OK) {
        free(m);
        return NULL;
    }
    (void)max17048_probe(m);   /* absence is fine — caller retries later */
    return m;
}

esp_err_t max17048_probe(max17048_t *m) {
    if (!m)
        return ESP_ERR_INVALID_ARG;
    if (m->ready)
        return ESP_OK;

    /* Pre-init recover: un-wedge a slave that latched the bus mid-transaction
     * (boot scan / prior power cycle) before we start talking. */
    (void)i2c_xport_recover(&m->x);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Soft reset (0xFE=0x5400) — some clones only wake their I²C after a
     * register write. ACK checking stays on: the VERSION read below is the
     * real presence test. Tps=1 ms + margin. */
    (void)write_reg(m, 0xFE, 0x5400);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* VERSION (0x08) = 0x001x for genuine MAX17048/49. Stronger than VCELL:
     * the chip ACKs to any address (documented Maxim bug), so the fixed
     * prefix is the only reliable way to reject a phantom ACK. */
    uint16_t version = 0;
    esp_err_t e = max17048_read_reg(m, 0x08, &version);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "probe (bus%d) VERSION read failed: %s", (int)m->x.bus, esp_err_to_name(e));
        return ESP_ERR_NOT_FOUND;
    }
    if ((version & 0xFFF0) != 0x0010) {
        ESP_LOGW(TAG, "probe (bus%d): VERSION=0x%04x (expected 0x001x) — not a MAX17048",
                 (int)m->x.bus, version);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t vcell = 0;
    (void)max17048_read_reg(m, 0x02, &vcell);

    /* QuickStart — re-models the cell from a single VCELL reading. */
    if (write_reg(m, 0x06, 0x4000) != ESP_OK)
        ESP_LOGW(TAG, "QuickStart write failed (continuing)");

    m->ready = true;
    ESP_LOGI(TAG, "MAX17048 ready (bus%d, VERSION=0x%04x, VCELL≈%.3f V)",
             (int)m->x.bus, version, vcell * 78.125e-6f);
    return ESP_OK;
}

bool max17048_ready(max17048_t *m) { return m && m->ready; }

float max17048_soc(max17048_t *m) {
    if (!m || !m->ready)
        return -1.0f;
    uint16_t v;
    if (max17048_read_reg(m, 0x04, &v) != ESP_OK)
        return -1.0f;
    return (float)v / 256.0f;
}

float max17048_vbat(max17048_t *m) {
    if (!m || !m->ready)
        return -1.0f;
    uint16_t v;
    if (max17048_read_reg(m, 0x02, &v) != ESP_OK)
        return -1.0f;
    return (float)v * 78.125e-6f;
}

float max17048_crate(max17048_t *m) {
    if (!m || !m->ready)
        return 0.0f;
    uint16_t v;
    if (max17048_read_reg(m, 0x16, &v) != ESP_OK)
        return 0.0f;
    return (float)(int16_t)v * 0.208f;
}
