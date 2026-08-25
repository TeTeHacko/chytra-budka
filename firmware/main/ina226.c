/* ina226.c — INA226 driver, instance-based. See ina226.h.
 *
 * Register map (all 16-bit big-endian):
 *   0x00 Config        — averaging, conv. time, mode
 *   0x01 Shunt voltage — signed, 2.5 µV / LSB (full-scale ±81.92 mV)
 *   0x02 Bus voltage   — unsigned, 1.25 mV / LSB
 *   0xFE Manufacturer ID = 0x5449 ('TI')
 *
 * We do NOT use the on-chip Power/Current registers: doing the math in the
 * host removes the calibration-register precision foot-gun and lets us
 * change R_SHUNT_OHM without flashing a new calibration value.
 *
 * The register path runs over an i2c_xport_t, so one instance serves
 * either bus. */
#include "ina226.h"

#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_xport.h"
#include "log_throttle.h"

static const char *TAG = "ina226";

/* Shunt resistor on the solar input. 0.1 Ω gives ±819 mA full scale. */
#ifndef INA226_R_SHUNT_OHM
#define INA226_R_SHUNT_OHM 0.1f
#endif

#define REG_CONFIG  0x00
#define REG_SHUNT   0x01
#define REG_BUS     0x02
#define REG_MFR_ID  0xFE

/* avg=16, vbus_ct=1.1 ms, vshunt_ct=1.1 ms, mode=shunt+bus continuous */
#define CONFIG_DEFAULT 0x4527
#define I2C_TMO_MS 100

struct ina226 {
    i2c_xport_t x;
    bool ready;
    float c_v, c_i, c_p;   /* last good read, for ina226_get_cached() */
    bool  c_valid;
};

static esp_err_t read_reg(ina226_t *n, uint8_t reg, uint16_t *out) {
    uint8_t rx[2] = {0};
    esp_err_t e = i2c_xport_txrx(&n->x, &reg, 1, rx, 2, I2C_TMO_MS);
    if (e != ESP_OK)
        return e;
    *out = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

static esp_err_t write_reg(ina226_t *n, uint8_t reg, uint16_t val) {
    uint8_t tx[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_xport_tx(&n->x, tx, 3, I2C_TMO_MS);
}

ina226_t *ina226_create(cb_bus_t bus, uint8_t addr) {
    ina226_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    if (i2c_xport_open(&n->x, bus, addr, 100000, 0) != ESP_OK) {
        free(n);
        return NULL;
    }
    (void)ina226_probe(n);   /* absence is fine — registry retries later */
    return n;
}

esp_err_t ina226_probe(ina226_t *n) {
    if (!n)
        return ESP_ERR_INVALID_ARG;
    if (n->ready)
        return ESP_OK;

    /* Pre-probe bus recover + retry loop. If a slave wedged mid-transaction
     * during boot (MAX17048's known stuck-state, or a brown-out), the first
     * read can NACK even when the chip is healthy. 3 rounds with a bus
     * recover between covers the realistic transient cases. */
    uint16_t mfr = 0;
    esp_err_t e = ESP_FAIL;
    for (int round = 0; round < 3; round++) {
        (void)i2c_xport_recover(&n->x);
        vTaskDelay(pdMS_TO_TICKS(50));
        e = read_reg(n, REG_MFR_ID, &mfr);
        if (e == ESP_OK && mfr == 0x5449)
            break;
        ESP_LOGW(TAG, "probe round %d (bus%d 0x%02x): mfr=0x%04x (%s)",
                 round + 1, (int)n->x.bus, n->x.addr, mfr, esp_err_to_name(e));
    }
    if (e != ESP_OK || mfr != 0x5449) {
        ESP_LOGW(TAG, "INA226 not detected at bus%d 0x%02x (mfr=0x%04x, %s)",
                 (int)n->x.bus, n->x.addr, mfr, esp_err_to_name(e));
        return ESP_ERR_NOT_FOUND;
    }

    if (write_reg(n, REG_CONFIG, CONFIG_DEFAULT) != ESP_OK) {
        ESP_LOGW(TAG, "config write failed");
        return ESP_FAIL;
    }

    n->ready = true;
    ESP_LOGI(TAG, "INA226 ready (bus%d 0x%02x, R_shunt=%.3f Ω)",
             (int)n->x.bus, n->x.addr, INA226_R_SHUNT_OHM);
    return ESP_OK;
}

bool ina226_ready(ina226_t *n) { return n && n->ready; }

esp_err_t ina226_read(ina226_t *n, float *bus_v, float *current_a, float *power_w) {
    if (!n || !n->ready)
        return ESP_ERR_INVALID_STATE;

    uint16_t shunt_raw = 0, bus_raw = 0;
    esp_err_t e = read_reg(n, REG_SHUNT, &shunt_raw);
    if (e != ESP_OK) {
        LOG_THROTTLED_W(TAG, 60000, "shunt read failed: %s", esp_err_to_name(e));
        return e;
    }
    e = read_reg(n, REG_BUS, &bus_raw);
    if (e != ESP_OK) {
        LOG_THROTTLED_W(TAG, 60000, "bus read failed: %s", esp_err_to_name(e));
        return e;
    }

    int16_t s = (int16_t)shunt_raw;
    float vshunt = (float)s * 2.5e-6f;             /* V */
    float vbus   = (float)bus_raw * 1.25e-3f;      /* V */
    float i      = vshunt / INA226_R_SHUNT_OHM;    /* A */
    float p      = vbus * i;                        /* W */

    n->c_v = vbus;
    n->c_i = i;
    n->c_p = p;
    n->c_valid = true;
    if (bus_v)     *bus_v     = vbus;
    if (current_a) *current_a = i;
    if (power_w)   *power_w   = p;
    return ESP_OK;
}

bool ina226_get_cached(ina226_t *n, float *bus_v, float *current_a, float *power_w) {
    if (!n || !n->c_valid)
        return false;
    if (bus_v)     *bus_v     = n->c_v;
    if (current_a) *current_a = n->c_i;
    if (power_w)   *power_w   = n->c_p;
    return true;
}
