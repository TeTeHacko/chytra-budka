/* sht41.c — Sensirion SHT4x driver (single-shot high-precision), instance-based.
 *
 * Wire protocol (per SHT4x datasheet rev 1.4):
 *   Trigger: write 1 byte command, no register address.
 *     0x94 = soft reset
 *     0xFD = T+RH high precision (~8.2 ms conversion, ~70 µA peak)
 *   Read response: 6 bytes = T_msb T_lsb T_crc RH_msb RH_lsb RH_crc
 *   CRC-8: poly 0x31, init 0xFF, no reflect, no xor-out.
 *
 * Conversion:
 *   T_°C  = -45 + 175 * (T_raw / 65535)
 *   RH_%  =  -6 + 125 * (RH_raw / 65535), clamped [0, 100]
 *
 * One measurement path runs over an i2c_xport_t, so the same code serves a
 * bus0 (HW) and a bus1 (bit-bang) instance — see sht41.h. */

#include "sht41.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "cb_time.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_xport.h"
#include "log_throttle.h"

static const char *TAG = "sht41";

#define CMD_MEAS_HIGH 0xFD

/* SHT4x doesn't clock-stretch, but the extra SCL wait gives headroom for the
 * internal pull-up rise time on AliExpress clone modules (~45 kΩ XIAO
 * internal pullup vs 10 kΩ on Adafruit). Ignored on the bit-bang bus. */
#define SHT41_SCL_HZ      100000
#define SHT41_SCL_WAIT_US 20000

struct sht41 {
    i2c_xport_t x;
    bool  ready;
    float cache_t_c;
    float cache_rh;
    bool  cache_valid;
};

static uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* One trigger→wait→read→CRC cycle over the instance's transport. */
static esp_err_t measure_raw(sht41_t *s, uint16_t *t_raw, uint16_t *rh_raw) {
    uint8_t cmd = CMD_MEAS_HIGH;
    esp_err_t e = i2c_xport_tx(&s->x, &cmd, 1, 100);
    if (e != ESP_OK)
        return e;

    /* SHT4x ≤8.2 ms conversion; sensor NACKs reads until data is ready.
     * 10 ms initial wait (worst-case + RTOS tick), then up to 5 retries. */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t rx[6] = {0};
    for (int tries = 0; tries < 5; tries++) {
        e = i2c_xport_rx(&s->x, rx, sizeof(rx), 100);
        if (e == ESP_OK)
            break;
        cb_delay_ms(5);
    }
    if (e != ESP_OK)
        return e;

    if (crc8(&rx[0], 2) != rx[2] || crc8(&rx[3], 2) != rx[5])
        return ESP_ERR_INVALID_CRC;

    *t_raw = ((uint16_t)rx[0] << 8) | rx[1];
    *rh_raw = ((uint16_t)rx[3] << 8) | rx[4];
    return ESP_OK;
}

static void cook(uint16_t t_raw, uint16_t rh_raw, float *t_c, float *rh_pct) {
    float t = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    float rh = -6.0f + 125.0f * (float)rh_raw / 65535.0f;
    if (rh < 0.0f)
        rh = 0.0f;
    if (rh > 100.0f)
        rh = 100.0f;
    *t_c = t;
    *rh_pct = rh;
}

sht41_t *sht41_create(cb_bus_t bus, uint8_t addr) {
    sht41_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cache_t_c = NAN;
    s->cache_rh = NAN;
    if (i2c_xport_open(&s->x, bus, addr, SHT41_SCL_HZ, SHT41_SCL_WAIT_US) != ESP_OK) {
        free(s);
        return NULL;
    }
    /* Settle after add_device. Datasheet POR is 1 ms but clones are flaky
     * for ~20 ms; soft reset is deliberately skipped (clones NACK it). */
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)sht41_probe(s);   /* absence is fine — registry retries later */
    return s;
}

esp_err_t sht41_probe(sht41_t *s) {
    if (!s)
        return ESP_ERR_INVALID_ARG;
    if (s->ready)
        return ESP_OK;

    uint16_t t_raw, rh_raw;
    esp_err_t e = ESP_FAIL;
    for (int tries = 0; tries < 3; tries++) {
        e = measure_raw(s, &t_raw, &rh_raw);
        if (e == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (e != ESP_OK) {
        LOG_THROTTLED_W(TAG, 60000, "probe (bus%d 0x%02x) failed: %s",
                        (int)s->x.bus, s->x.addr, esp_err_to_name(e));
        return e;
    }

    s->ready = true;
    float t_c, rh;
    cook(t_raw, rh_raw, &t_c, &rh);
    s->cache_t_c = t_c;
    s->cache_rh = rh;
    s->cache_valid = true;
    ESP_LOGI(TAG, "ready (bus%d 0x%02x): T=%.2f°C RH=%.1f%%",
             (int)s->x.bus, s->x.addr, t_c, rh);
    return ESP_OK;
}

bool sht41_ready(sht41_t *s) {
    return s && s->ready;
}

esp_err_t sht41_read(sht41_t *s, float *t_c, float *rh_pct) {
    if (!s || !s->ready)
        return ESP_ERR_INVALID_STATE;

    uint16_t t_raw = 0, rh_raw = 0;
    /* Two tasks racing inside the IDF i2c_master driver can NACK a
     * transaction mid-sequence. Retry the whole measurement; 20 ms backoff
     * is well above conversion time, so each retry is a fresh sample. */
    esp_err_t e = ESP_FAIL;
    for (int tries = 0; tries < 3; tries++) {
        e = measure_raw(s, &t_raw, &rh_raw);
        if (e == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (e != ESP_OK) {
        /* All retries failed. A slave that latched the bus low (a wedged
         * MAX17048 clone has done exactly this on bus0) would otherwise
         * keep this sensor dead forever. Recover the bus once and make a
         * final attempt so a transient wedge self-heals without a reboot.
         * Throttled so a genuinely-absent sensor doesn't spam the log. */
        LOG_THROTTLED_W(TAG, 60000,
                        "read (bus%d 0x%02x) failed after 3 tries (%s) — recovering bus",
                        (int)s->x.bus, s->x.addr, esp_err_to_name(e));
        (void)i2c_xport_recover(&s->x);
        vTaskDelay(pdMS_TO_TICKS(20));
        e = measure_raw(s, &t_raw, &rh_raw);
        if (e != ESP_OK)
            return e;
    }

    float t, rh;
    cook(t_raw, rh_raw, &t, &rh);
    if (t_c)
        *t_c = t;
    if (rh_pct)
        *rh_pct = rh;
    s->cache_t_c = t;
    s->cache_rh = rh;
    s->cache_valid = true;
    return ESP_OK;
}

bool sht41_get_cached(sht41_t *s, float *t_c, float *rh_pct) {
    if (!s || !s->cache_valid)
        return false;
    if (t_c)
        *t_c = s->cache_t_c;
    if (rh_pct)
        *rh_pct = s->cache_rh;
    return true;
}
