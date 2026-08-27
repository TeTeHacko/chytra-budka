/* bmp388.c — minimal Bosch BMP388 pressure/temp driver, instance-based.
 *
 * Register map + float compensation are straight from the BMP388
 * datasheet (rev 1.x) / Bosch BMP3 reference API. Float path with a fixed
 * config (NORMAL mode, OSR ×4 press / ×1 temp, IIR off). The measurement
 * path runs over an i2c_xport_t, so one instance serves either bus. */

#include "bmp388.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_xport.h"

static const char *TAG = "bmp388";

/* registers */
#define REG_CHIP_ID  0x00
#define REG_DATA     0x04   /* press[0..2], temp[3..5] */
#define REG_PWR_CTRL 0x1B
#define REG_OSR      0x1C
#define REG_ODR      0x1D
#define REG_CONFIG   0x1F
#define REG_CALIB    0x31   /* 21 calibration bytes */
#define REG_CMD      0x7E
#define CHIP_ID_BMP388 0x50
#define SOFT_RESET   0xB6
#define I2C_TMO_MS   50

struct bmp388 {
    i2c_xport_t x;
    bool ready;
    float c_t, c_p;        /* last good read, for bmp388_get_cached() */
    bool  c_valid;
    /* Quantized float calibration coefficients (datasheet §9.1). */
    struct {
        double t1, t2, t3;
        double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
    } C;
};

static esp_err_t rd(bmp388_t *b, uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_xport_txrx(&b->x, &reg, 1, buf, n, I2C_TMO_MS);
}
static esp_err_t wr(bmp388_t *b, uint8_t reg, uint8_t val) {
    uint8_t bb[2] = {reg, val};
    return i2c_xport_tx(&b->x, bb, 2, I2C_TMO_MS);
}

static void load_calib(bmp388_t *b, const uint8_t *d) {
    uint16_t t1 = (uint16_t)(d[1] << 8 | d[0]);
    uint16_t t2 = (uint16_t)(d[3] << 8 | d[2]);
    int8_t   t3 = (int8_t)d[4];
    int16_t  p1 = (int16_t)(d[6] << 8 | d[5]);
    int16_t  p2 = (int16_t)(d[8] << 8 | d[7]);
    int8_t   p3 = (int8_t)d[9];
    int8_t   p4 = (int8_t)d[10];
    uint16_t p5 = (uint16_t)(d[12] << 8 | d[11]);
    uint16_t p6 = (uint16_t)(d[14] << 8 | d[13]);
    int8_t   p7 = (int8_t)d[15];
    int8_t   p8 = (int8_t)d[16];
    int16_t  p9 = (int16_t)(d[18] << 8 | d[17]);
    int8_t   p10 = (int8_t)d[19];
    int8_t   p11 = (int8_t)d[20];

    b->C.t1 = (double)t1 * 256.0;                 /* /2^-8  */
    b->C.t2 = (double)t2 / 1073741824.0;          /* /2^30  */
    b->C.t3 = (double)t3 / 281474976710656.0;     /* /2^48  */
    b->C.p1 = ((double)p1 - 16384.0) / 1048576.0; /* (-2^14)/2^20 */
    b->C.p2 = ((double)p2 - 16384.0) / 536870912.0;   /* /2^29 */
    b->C.p3 = (double)p3 / 4294967296.0;          /* /2^32  */
    b->C.p4 = (double)p4 / 137438953472.0;        /* /2^37  */
    b->C.p5 = (double)p5 * 8.0;                    /* /2^-3  */
    b->C.p6 = (double)p6 / 64.0;                   /* /2^6   */
    b->C.p7 = (double)p7 / 256.0;                  /* /2^8   */
    b->C.p8 = (double)p8 / 32768.0;                /* /2^15  */
    b->C.p9 = (double)p9 / 281474976710656.0;      /* /2^48  */
    b->C.p10 = (double)p10 / 281474976710656.0;    /* /2^48  */
    b->C.p11 = (double)p11 / 36893488147419103232.0; /* /2^65 */
}

static double comp_temp(bmp388_t *b, uint32_t raw) {
    double d1 = (double)raw - b->C.t1;
    double d2 = d1 * b->C.t2;
    return d2 + (d1 * d1) * b->C.t3;   /* °C */
}

static double comp_press(bmp388_t *b, uint32_t raw, double t) {
    double u = (double)raw;
    double o1 = b->C.p5 + b->C.p6 * t + b->C.p7 * (t * t) + b->C.p8 * (t * t * t);
    double o2 = u * (b->C.p1 + b->C.p2 * t + b->C.p3 * (t * t) + b->C.p4 * (t * t * t));
    double o3 = (u * u) * (b->C.p9 + b->C.p10 * t) + (u * u * u) * b->C.p11;
    return o1 + o2 + o3;            /* Pa */
}

bmp388_t *bmp388_create(cb_bus_t bus, uint8_t addr) {
    bmp388_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    if (i2c_xport_open(&b->x, bus, addr, 100000, 0) != ESP_OK) {
        free(b);
        return NULL;
    }
    (void)bmp388_probe(b);   /* absence is fine — registry retries later */
    return b;
}

esp_err_t bmp388_probe(bmp388_t *b) {
    if (!b)
        return ESP_ERR_INVALID_ARG;
    if (b->ready)
        return ESP_OK;

    /* bus0 emits random false-ACKs — demand a probe streak before trusting
     * the address (same guard as the OLED bring-up). On bus1 the bit-bang
     * probe false-ACKs unconditionally, so skip the pre-filter there and
     * let the chip-id read below be the gate. */
    if (b->x.bus == CB_BUS0) {
        int consec = 0;
        for (int i = 0; i < 20 && consec < 3; i++) {
            if (i2c_xport_probe(&b->x, I2C_TMO_MS))
                consec++;
            else { consec = 0; vTaskDelay(pdMS_TO_TICKS(30)); }
        }
        if (consec < 3) {
            ESP_LOGI(TAG, "no BMP388 at bus%d 0x%02x (absent / wiring flaky)",
                     (int)b->x.bus, b->x.addr);
            return ESP_ERR_NOT_FOUND;
        }
    }

    wr(b, REG_CMD, SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t id = 0;
    if (rd(b, REG_CHIP_ID, &id, 1) != ESP_OK || id != CHIP_ID_BMP388) {
        ESP_LOGW(TAG, "bus%d 0x%02x chip id 0x%02X != 0x50 — not a BMP388",
                 (int)b->x.bus, b->x.addr, id);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t calib[21];
    if (rd(b, REG_CALIB, calib, sizeof(calib)) != ESP_OK) {
        ESP_LOGW(TAG, "calib read failed");
        return ESP_FAIL;
    }
    load_calib(b, calib);

    /* OSR ×4 pressure (osr_p=2), ×1 temp; IIR off; ODR 12.5 Hz; NORMAL. */
    wr(b, REG_OSR, 0x02);
    wr(b, REG_ODR, 0x03);
    wr(b, REG_CONFIG, 0x00);
    if (wr(b, REG_PWR_CTRL, 0x33) != ESP_OK) {   /* press_en|temp_en|mode=normal */
        ESP_LOGW(TAG, "pwr_ctrl write failed");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    b->ready = true;
    float t = NAN, pp = NAN;
    if (bmp388_read(b, &t, &pp))
        ESP_LOGI(TAG, "ready (bus%d 0x%02x): %.2f C  %.1f hPa",
                 (int)b->x.bus, b->x.addr, (double)t, (double)pp);
    else
        ESP_LOGW(TAG, "configured (bus%d 0x%02x) but first read failed",
                 (int)b->x.bus, b->x.addr);
    return ESP_OK;
}

bool bmp388_ready(bmp388_t *b) { return b && b->ready; }

bool bmp388_read(bmp388_t *b, float *temp_c, float *press_hpa) {
    if (!b || !b->ready)
        return false;
    uint8_t d[6];
    if (rd(b, REG_DATA, d, sizeof(d)) != ESP_OK)
        return false;
    uint32_t raw_p = (uint32_t)d[2] << 16 | (uint32_t)d[1] << 8 | d[0];
    uint32_t raw_t = (uint32_t)d[5] << 16 | (uint32_t)d[4] << 8 | d[3];
    double t = comp_temp(b, raw_t);
    double p = comp_press(b, raw_p, t);
    if (!isfinite(t) || !isfinite(p))
        return false;
    b->c_t = (float)t;
    b->c_p = (float)(p / 100.0);
    b->c_valid = true;
    if (temp_c)
        *temp_c = b->c_t;
    if (press_hpa)
        *press_hpa = b->c_p;
    return true;
}

bool bmp388_get_cached(bmp388_t *b, float *temp_c, float *press_hpa) {
    if (!b || !b->c_valid)
        return false;
    if (temp_c)
        *temp_c = b->c_t;
    if (press_hpa)
        *press_hpa = b->c_p;
    return true;
}
