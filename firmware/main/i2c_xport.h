/* i2c_xport.h — uniform I²C transport handle over both physical buses.
 *
 * The ESP32-S3 has two HW I²C controllers and both are claimed: NUM_0 is
 * the main shared bus0 (D4/D5) and NUM_1 is owned by the camera SCCB. The
 * diagnostic bus1 (D6/D7) therefore has no HW controller and is bit-banged
 * in i2c_bb.[ch]. The two transports have completely different APIs:
 * bus0 goes through the IDF i2c_master driver (per-device handle), bus1
 * through i2c_bb (address per call).
 *
 * This handle hides that split behind one set of tx/rx/txrx/probe/recover
 * ops, so a single sensor driver can be pointed at either bus by opening a
 * transport on it — instead of being written twice (the old sht41 vs
 * sht41_ext duplication). A driver instance just embeds an i2c_xport_t.
 *
 * Not thread-safe per handle: the bit-bang bus serialises in software and
 * the caller (the single telemetry owner via cb_sensors_refresh) is the
 * only writer. Honour that — see sensors.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which physical I²C bus a transport (and thus a sensor instance) lives on.
 * bus0 = HW i2c_master (D4/D5), bus1 = bit-bang (D6/D7). The sole definition
 * — sensors.h and everything else includes it from here. */
typedef enum { CB_BUS0 = 0, CB_BUS1 = 1 } cb_bus_t;

typedef struct {
    cb_bus_t bus;
    uint8_t  addr;                  /* 7-bit device address */
    i2c_master_dev_handle_t dev;    /* HW (bus0) per-device handle; NULL on bus1 */
} i2c_xport_t;

/* Open a transport to (bus, addr).
 *  bus0: brings up bus0 if needed and adds an i2c_master device with the
 *        given SCL speed + clock-stretch wait (scl_wait_us = 0 → default).
 *  bus1: ensures the bit-bang pins are configured; `dev` stays NULL and
 *        scl_hz / scl_wait_us are ignored (fixed ~100 kHz bit-bang timing).
 * Does NOT probe the device — the driver verifies identity with a real
 * read (chip-id / mfr-id / CRC), which is what defeats the bus1 false-ACK.
 * Returns ESP_OK on success; the handle is unusable otherwise. */
esp_err_t i2c_xport_open(i2c_xport_t *x, cb_bus_t bus, uint8_t addr,
                         uint32_t scl_hz, uint32_t scl_wait_us);

/* Release a transport. bus0: removes the i2c_master device. bus1: no-op
 * (the bit-bang bus is shared and stateless per device). Safe on a
 * zero/failed handle. */
void i2c_xport_close(i2c_xport_t *x);

/* Single write / read / write-then-read (repeated-START register read).
 * tmo_ms is the per-transaction timeout on bus0; ignored on bus1. All three
 * return ESP_ERR_INVALID_STATE if the handle isn't open. */
esp_err_t i2c_xport_tx(const i2c_xport_t *x, const uint8_t *buf, size_t len, int tmo_ms);
esp_err_t i2c_xport_rx(const i2c_xport_t *x, uint8_t *buf, size_t len, int tmo_ms);
esp_err_t i2c_xport_txrx(const i2c_xport_t *x, const uint8_t *wbuf, size_t wlen,
                         uint8_t *rbuf, size_t rlen, int tmo_ms);

/* Single address-ACK probe. On bus1 the bit-bang probe can false-ACK
 * (SDA rise time) — drivers must still gate on a real identity read, never
 * trust probe alone for presence. tmo_ms is the bus0 probe timeout. */
bool i2c_xport_probe(const i2c_xport_t *x, int tmo_ms);

/* Recover a wedged bus. bus0: issues i2c_bus0_recover() (9-clock reset).
 * bus1: forces a bit-bang re-init (recovery clock burst + idle re-sample).
 * Used by a driver's read path after its own retries are exhausted, so a
 * transient wedge self-heals instead of needing a reboot. */
esp_err_t i2c_xport_recover(const i2c_xport_t *x);

#ifdef __cplusplus
}
#endif
