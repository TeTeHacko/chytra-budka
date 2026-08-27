/* ina226.h — TI INA226 power monitor (solar V/I/P) over I²C, instance-based.
 *
 * Wired between solar panel + and the charger IN+ with a 0.1 Ω current-sense
 * shunt. Bus voltage measures the panel side, shunt voltage across the
 * resistor; current = V_shunt / R_shunt; P = V_bus * I.
 *
 * Address pins (A1, A0): GND/GND = 0x40 (firmware default; no collision with
 * MAX17048 0x36 or SHT41 0x44). An instance binds to an I²C transport (bus0
 * HW or bus1 bit-bang) at create time. Absent ⇒ reads return an error and
 * the registry / solar publish skip it.
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

typedef struct ina226 ina226_t;

/* Allocate an instance bound to (bus, addr) and probe + configure. Always
 * returns a usable handle once the transport opens (detection may have
 * failed — ina226_ready() reports it and ina226_probe() retries), or NULL
 * if the bus is unavailable / OOM. */
ina226_t *ina226_create(cb_bus_t bus, uint8_t addr);

/* (Re)probe: verify the TI manufacturer id and write the default config.
 * Idempotent; cheap no-op once ready. */
esp_err_t ina226_probe(ina226_t *n);

bool ina226_ready(ina226_t *n);

/* All values come from the same conversion cycle (~1.1 ms typical). Caches
 * the result for ina226_get_cached(). */
esp_err_t ina226_read(ina226_t *n, float *bus_v, float *current_a, float *power_w);

/* Last cached ina226_read() values without touching the bus. False if none
 * yet — meant for the registry / solar publish, which read the cache. */
bool ina226_get_cached(ina226_t *n, float *bus_v, float *current_a, float *power_w);

#ifdef __cplusplus
}
#endif
