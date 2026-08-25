/* bmp388.h — minimal Bosch BMP388 pressure/temperature driver, instance-based.
 *
 * One driver, any bus: an instance binds to an I²C transport (bus0 HW or
 * bus1 bit-bang) at create time. Soft-detected — absent ⇒ reads return
 * false and the instance is a no-op. Configured in NORMAL mode so a read
 * just grabs the latest conversion from the data registers.
 *
 * Instances are owned by the sensor registry (sensors.c).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "i2c_xport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmp388 bmp388_t;

/* Allocate an instance bound to (bus, addr) and run an initial probe +
 * configure. Always returns a usable handle once the transport opens
 * (detection may have failed — bmp388_ready() reports that and
 * bmp388_probe() retries), or NULL if the bus is unavailable / OOM. */
bmp388_t *bmp388_create(cb_bus_t bus, uint8_t addr);

/* (Re)probe: verify chip id 0x50, load calibration, start NORMAL mode, and
 * mark ready. Idempotent; cheap no-op once ready. */
esp_err_t bmp388_probe(bmp388_t *b);

/* True once a BMP388 has been detected + configured. */
bool bmp388_ready(bmp388_t *b);

/* Read the latest compensated temperature (°C) and pressure (hPa), caching
 * the result. Returns true on a good read; false if absent or the read
 * failed. */
bool bmp388_read(bmp388_t *b, float *temp_c, float *press_hpa);

/* Last cached bmp388_read() values without touching the bus. False if none
 * yet — meant for the registry / UI, which must never add bus traffic. */
bool bmp388_get_cached(bmp388_t *b, float *temp_c, float *press_hpa);

#ifdef __cplusplus
}
#endif
