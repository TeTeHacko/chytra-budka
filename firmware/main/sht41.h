/* sht41.h — Sensirion SHT4x driver, instance-based.
 *
 * One driver, any bus: an instance binds to an I²C transport (bus0 HW or
 * bus1 bit-bang) at create time, so the same code reads an on-board sensor
 * and an external one. (This replaced the old sht41_* / sht41_ext_* split
 * that duplicated the whole measurement path per bus.)
 *
 * Instances are owned by the sensor registry (sensors.c); other code reaches
 * them through cb_sensor_* helpers, not directly.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "i2c_xport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sht41 sht41_t;

/* Allocate an instance bound to (bus, addr) and run an initial probe.
 * Always returns a usable handle once the transport opens (probe may have
 * failed — sht41_ready() reports that and sht41_probe() retries later),
 * or NULL only if the bus itself is unavailable / out of memory. */
sht41_t *sht41_create(cb_bus_t bus, uint8_t addr);

/* (Re)probe: confirm a real SHT4x answers with a CRC-valid measurement and
 * mark the instance ready. Idempotent; cheap no-op once ready. Used both at
 * create and by the registry's periodic recovery of a hot-plugged sensor. */
esp_err_t sht41_probe(sht41_t *s);

/* True once a CRC-valid measurement has confirmed the sensor. */
bool sht41_ready(sht41_t *s);

/* Fresh high-precision T+RH measurement (~12 ms). Retries, and on repeated
 * failure recovers the bus once and retries a final time before giving up.
 * Updates the instance cache on success. */
esp_err_t sht41_read(sht41_t *s, float *t_c, float *rh_pct);

/* Last successful measurement without touching the bus. False if none yet. */
bool sht41_get_cached(sht41_t *s, float *t_c, float *rh_pct);

#ifdef __cplusplus
}
#endif
