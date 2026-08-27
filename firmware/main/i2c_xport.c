/* i2c_xport.c — uniform I²C transport over bus0 (HW) and bus1 (bit-bang).
 * See i2c_xport.h. Thin dispatch: bus0 → IDF i2c_master on a per-device
 * handle; bus1 → i2c_bb (address per call). */

#include "i2c_xport.h"

#include "esp_log.h"
#include "i2c_bb.h"
#include "i2c_bus.h"

static const char *TAG = "i2c_xport";

esp_err_t i2c_xport_open(i2c_xport_t *x, cb_bus_t bus, uint8_t addr,
                         uint32_t scl_hz, uint32_t scl_wait_us) {
    if (!x)
        return ESP_ERR_INVALID_ARG;
    x->bus = bus;
    x->addr = addr;
    x->dev = NULL;

    if (bus == CB_BUS1) {
        /* Bit-bang: no per-device handle, just make sure the pads are up. */
        if (!i2c_bus1_ensure()) {
            ESP_LOGW(TAG, "bus1 ensure failed (addr 0x%02x)", addr);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    i2c_master_bus_handle_t hw = i2c_bus_get();
    if (!hw) {
        ESP_LOGE(TAG, "bus0 not available (addr 0x%02x)", addr);
        return ESP_FAIL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_hz ? scl_hz : 100000,
        .scl_wait_us = scl_wait_us,   /* 0 = driver default */
    };
    esp_err_t e = i2c_master_bus_add_device(hw, &cfg, &x->dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bus0 add_device 0x%02x: %s", addr, esp_err_to_name(e));
        x->dev = NULL;
    }
    return e;
}

void i2c_xport_close(i2c_xport_t *x) {
    if (!x)
        return;
    if (x->bus == CB_BUS0 && x->dev) {
        i2c_master_bus_rm_device(x->dev);
        x->dev = NULL;
    }
}

esp_err_t i2c_xport_tx(const i2c_xport_t *x, const uint8_t *buf, size_t len, int tmo_ms) {
    if (!x)
        return ESP_ERR_INVALID_STATE;
    if (x->bus == CB_BUS1)
        return i2c_bb_transmit(x->addr, buf, len);
    if (!x->dev)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(x->dev, buf, len, tmo_ms);
}

esp_err_t i2c_xport_rx(const i2c_xport_t *x, uint8_t *buf, size_t len, int tmo_ms) {
    if (!x)
        return ESP_ERR_INVALID_STATE;
    if (x->bus == CB_BUS1)
        return i2c_bb_receive(x->addr, buf, len);
    if (!x->dev)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_receive(x->dev, buf, len, tmo_ms);
}

esp_err_t i2c_xport_txrx(const i2c_xport_t *x, const uint8_t *wbuf, size_t wlen,
                         uint8_t *rbuf, size_t rlen, int tmo_ms) {
    if (!x)
        return ESP_ERR_INVALID_STATE;
    if (x->bus == CB_BUS1)
        return i2c_bb_transmit_receive(x->addr, wbuf, wlen, rbuf, rlen);
    if (!x->dev)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(x->dev, wbuf, wlen, rbuf, rlen, tmo_ms);
}

bool i2c_xport_probe(const i2c_xport_t *x, int tmo_ms) {
    if (!x)
        return false;
    if (x->bus == CB_BUS1)
        return i2c_bb_probe(x->addr);
    i2c_master_bus_handle_t hw = i2c_bus_get();
    return hw && i2c_master_probe(hw, x->addr, tmo_ms) == ESP_OK;
}

esp_err_t i2c_xport_recover(const i2c_xport_t *x) {
    if (!x)
        return ESP_ERR_INVALID_STATE;
    if (x->bus == CB_BUS1) {
        int sda = -1, scl = -1;
        i2c_bb_pins(&sda, &scl);
        if (sda < 0 || scl < 0)
            return ESP_ERR_INVALID_STATE;
        return i2c_bb_reinit(sda, scl);
    }
    return i2c_bus0_recover();
}
