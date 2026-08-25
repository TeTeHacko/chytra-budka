/* i2c_bb.c — bit-banged I²C master. See i2c_bb.h. */

#include "i2c_bb.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cb_time.h"

static const char *TAG = "i2c_bb";

/* 20 µs half-clock ≈ 25 kHz. Internal pull-ups are ~45 kΩ on ESP32-S3
 * and with even modest bus capacitance (40-100 pF from wires + pad)
 * the RC rise time is several µs — pushing closer to 100 kHz with
 * only weak pulls makes the SCL/SDA highs miss the slave's 0.7·VDD
 * threshold. For a diagnostic bus with possibly long bench wires, run
 * conservatively; the only consumer (SHT41) is dominated by its 12 ms
 * conversion latency anyway. */
#define HALF_US 20

static int s_sda = -1;
static int s_scl = -1;
static bool s_inited = false;

/* Serialises bit-bang transactions. Unlike the HW i2c_master bus (which the
 * IDF driver locks internally), raw GPIO toggling has NO hardware
 * arbitration — two tasks interleaving start/clock/stop on the same pins
 * corrupt each other's transfers, which showed up as the bus1 SHT41 reading
 * intermittently (telemetry refresh racing the /i2c scan + /sensors). Every
 * public transaction takes this mutex. */
static SemaphoreHandle_t s_lock = NULL;
static inline void bb_take(void) {
    if (s_lock)
        xSemaphoreTake(s_lock, portMAX_DELAY);
}
static inline void bb_give(void) {
    if (s_lock)
        xSemaphoreGive(s_lock);
}

static inline void delay_half(void) { esp_rom_delay_us(HALF_US); }

static inline void sda_set(int level) {
    gpio_set_level((gpio_num_t)s_sda, level);
}
static inline void scl_set(int level) {
    gpio_set_level((gpio_num_t)s_scl, level);
}
static inline int sda_read(void) {
    return gpio_get_level((gpio_num_t)s_sda);
}

static esp_err_t bb_init_force(int sda_pin, int scl_pin);

esp_err_t i2c_bb_init(int sda_pin, int scl_pin) {
    if (s_inited && sda_pin == s_sda && scl_pin == s_scl)
        return ESP_OK;
    return bb_init_force(sda_pin, scl_pin);
}

esp_err_t i2c_bb_reinit(int sda_pin, int scl_pin) {
    return bb_init_force(sda_pin, scl_pin);
}

void i2c_bb_pins(int *sda_pin, int *scl_pin) {
    if (sda_pin)
        *sda_pin = s_inited ? s_sda : -1;
    if (scl_pin)
        *scl_pin = s_inited ? s_scl : -1;
}

void i2c_bb_idle_read(int *sda_level, int *scl_level) {
    if (!s_inited) {
        if (sda_level)
            *sda_level = -1;
        if (scl_level)
            *scl_level = -1;
        return;
    }
    if (sda_level)
        *sda_level = gpio_get_level((gpio_num_t)s_sda);
    if (scl_level)
        *scl_level = gpio_get_level((gpio_num_t)s_scl);
}

/* Drive one line low and sample BOTH lines. On a properly wired bus
 * the line driven low reads 0 and the other line stays at 1 (pulled
 * high). A short between SDA and SCL (stray solder, crossed wire on
 * the same node) pulls the other line down too — and that's a fatal
 * bus condition because START requires SDA falling *while SCL is
 * still high*, which becomes impossible when the two are tied. */
void i2c_bb_short_check(int *sda_to_scl_bleed, int *scl_to_sda_bleed) {
    if (!s_inited) {
        if (sda_to_scl_bleed)
            *sda_to_scl_bleed = -1;
        if (scl_to_sda_bleed)
            *scl_to_sda_bleed = -1;
        return;
    }
    /* Drive SDA low, sample SCL. If shorted, SCL goes low too → bleed=1. */
    sda_set(0);
    esp_rom_delay_us(50);
    if (sda_to_scl_bleed)
        *sda_to_scl_bleed = (gpio_get_level((gpio_num_t)s_scl) == 0) ? 1 : 0;
    sda_set(1);
    esp_rom_delay_us(50);
    /* Drive SCL low, sample SDA. */
    scl_set(0);
    esp_rom_delay_us(50);
    if (scl_to_sda_bleed)
        *scl_to_sda_bleed = (gpio_get_level((gpio_num_t)s_sda) == 0) ? 1 : 0;
    scl_set(1);
}

void i2c_bb_drive_low_check(int *sda_low_readback, int *scl_low_readback) {
    if (!s_inited) {
        if (sda_low_readback)
            *sda_low_readback = -1;
        if (scl_low_readback)
            *scl_low_readback = -1;
        return;
    }
    /* SDA: drive low, settle 50 µs (≥10× RC for the 45 kΩ pull-up + a
     * few tens of pF), sample, release. Then SCL same. We do them
     * sequentially so a slave that ACKs on a START won't confuse the
     * readback — it sees SDA falling with SCL still high (a START)
     * but no address byte follows, so it just sits in addressing
     * limbo until the next bus_recovery / bus_stop runs. Safe because
     * the diag endpoint always runs a reinit afterward. */
    sda_set(0);
    esp_rom_delay_us(50);
    if (sda_low_readback)
        *sda_low_readback = gpio_get_level((gpio_num_t)s_sda);
    sda_set(1);

    scl_set(0);
    esp_rom_delay_us(50);
    if (scl_low_readback)
        *scl_low_readback = gpio_get_level((gpio_num_t)s_scl);
    scl_set(1);
}

static esp_err_t bb_init_force(int sda_pin, int scl_pin) {
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    /* Reset both pins to clear any prior matrix routing (UART, etc).
     * gpio_reset_pin returns the pin to default high-Z input. */
    gpio_reset_pin((gpio_num_t)sda_pin);
    gpio_reset_pin((gpio_num_t)scl_pin);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << sda_pin) | (1ULL << scl_pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config sda=%d scl=%d: %s", sda_pin, scl_pin,
                 esp_err_to_name(e));
        return e;
    }
    s_sda = sda_pin;
    s_scl = scl_pin;
    /* Release both lines so the bus idles HIGH via the pull-ups. */
    sda_set(1);
    scl_set(1);
    /* Generous settle so the pull-ups can charge the bus capacitance
     * to a valid HIGH before we sample. */
    cb_delay_ms(5);

    /* Bus recovery: 9 SCL clock pulses with SDA released. Any slave
     * that wedged mid-byte (saw a partial START while we were doing
     * gpio_reset_pin → gpio_config) will release SDA after seeing
     * enough clocks to complete its byte, and then a proper STOP
     * brings it back to idle. Cheap (~9 * 2 * HALF_US ≈ 360 µs at
     * 25 kHz) and worth doing on every init since the cost of a
     * wedged slave is hours of "why doesn't it ACK". */
    for (int i = 0; i < 9; i++) {
        scl_set(0);
        delay_half();
        scl_set(1);
        delay_half();
    }
    /* STOP condition after the dummy clocks: SDA low while SCL high,
     * then release SDA. */
    scl_set(0);
    sda_set(0);
    delay_half();
    scl_set(1);
    delay_half();
    sda_set(1);
    cb_delay_ms(2);

    /* Sanity: with both lines released and pull-ups working, both
     * reads should be HIGH. If either is stuck LOW, something is
     * shorting the line (slave holding it, wrong pin, no power). */
    int sda_idle = gpio_get_level((gpio_num_t)sda_pin);
    int scl_idle = gpio_get_level((gpio_num_t)scl_pin);
    s_inited = true;
    ESP_LOGI(TAG, "ready sda=GPIO%d scl=GPIO%d ~25kHz idle=(SDA=%d SCL=%d) +recovery",
             sda_pin, scl_pin, sda_idle, scl_idle);
    if (sda_idle == 0 || scl_idle == 0) {
        ESP_LOGW(TAG, "line stuck LOW at idle — slave hold? missing pull-up? wrong pin?");
    }
    return ESP_OK;
}

bool i2c_bb_initialized(void) { return s_inited; }

static void bb_start(void) {
    sda_set(1);
    scl_set(1);
    delay_half();
    sda_set(0);
    delay_half();
    scl_set(0);
}

static void bb_stop(void) {
    sda_set(0);
    delay_half();
    scl_set(1);
    delay_half();
    sda_set(1);
    delay_half();
}

/* Returns true if slave ACKed. */
static bool bb_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; --i) {
        sda_set((b >> i) & 1);
        delay_half();
        scl_set(1);
        delay_half();
        scl_set(0);
    }
    /* Release SDA, clock one bit, sample for ACK (low = ACK). */
    sda_set(1);
    delay_half();
    scl_set(1);
    delay_half();
    bool ack = (sda_read() == 0);
    scl_set(0);
    return ack;
}

static uint8_t bb_read_byte(bool send_ack) {
    uint8_t b = 0;
    sda_set(1); /* release SDA so slave can drive */
    for (int i = 7; i >= 0; --i) {
        delay_half();
        scl_set(1);
        delay_half();
        if (sda_read())
            b |= (1u << i);
        scl_set(0);
    }
    /* ACK or NACK */
    sda_set(send_ack ? 0 : 1);
    delay_half();
    scl_set(1);
    delay_half();
    scl_set(0);
    sda_set(1);
    return b;
}

bool i2c_bb_probe(uint8_t addr) {
    if (!s_inited)
        return false;
    bb_take();
    bb_start();
    bool ack = bb_write_byte((uint8_t)((addr << 1) | 0));
    bb_stop();
    bb_give();
    return ack;
}

esp_err_t i2c_bb_transmit(uint8_t addr, const uint8_t *buf, size_t len) {
    if (!s_inited)
        return ESP_ERR_INVALID_STATE;
    bb_take();
    esp_err_t r = ESP_OK;
    bb_start();
    if (!bb_write_byte((uint8_t)((addr << 1) | 0))) {
        r = ESP_ERR_NOT_FOUND;
        goto out;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!bb_write_byte(buf[i])) {
            r = ESP_FAIL;
            goto out;
        }
    }
out:
    bb_stop();
    bb_give();
    return r;
}

esp_err_t i2c_bb_receive(uint8_t addr, uint8_t *buf, size_t len) {
    if (!s_inited || len == 0)
        return ESP_ERR_INVALID_STATE;
    bb_take();
    esp_err_t r = ESP_OK;
    bb_start();
    if (!bb_write_byte((uint8_t)((addr << 1) | 1))) {
        r = ESP_ERR_NOT_FOUND;
        goto out;
    }
    for (size_t i = 0; i < len; ++i) {
        bool ack = (i + 1 < len); /* ACK all but final byte */
        buf[i] = bb_read_byte(ack);
    }
out:
    bb_stop();
    bb_give();
    return r;
}

esp_err_t i2c_bb_transmit_receive(uint8_t addr, const uint8_t *wbuf, size_t wlen,
                                  uint8_t *rbuf, size_t rlen) {
    if (!s_inited || rlen == 0)
        return ESP_ERR_INVALID_STATE;
    bb_take();
    esp_err_t r = ESP_OK;
    bb_start();
    if (!bb_write_byte((uint8_t)((addr << 1) | 0))) {
        r = ESP_ERR_NOT_FOUND;
        goto out;
    }
    for (size_t i = 0; i < wlen; ++i) {
        if (!bb_write_byte(wbuf[i])) {
            r = ESP_FAIL;
            goto out;
        }
    }
    /* Repeated START — required before turning the bus around for the
     * read phase. Pull SDA high (release), clock SCL high, then drop
     * SDA while SCL is still high. */
    sda_set(1);
    delay_half();
    scl_set(1);
    delay_half();
    sda_set(0);
    delay_half();
    scl_set(0);

    if (!bb_write_byte((uint8_t)((addr << 1) | 1))) {
        r = ESP_ERR_NOT_FOUND;
        goto out;
    }
    for (size_t i = 0; i < rlen; ++i) {
        bool ack = (i + 1 < rlen);
        rbuf[i] = bb_read_byte(ack);
    }
out:
    bb_stop();
    bb_give();
    return r;
}
