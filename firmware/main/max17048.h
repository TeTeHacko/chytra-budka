/* max17048.h — Maxim MAX17048 LiPo fuel-gauge driver, instance-based.
 *
 * Register map (big-endian on the wire):
 *   0x02 VCELL   — 16-bit, 78.125 µV / LSB
 *   0x04 SOC     — 16-bit, hi byte = %, lo byte = 1/256 %
 *   0x06 MODE    — write 0x4000 = QuickStart
 *   0x08 VERSION — 0x001x for genuine MAX17048/49 (identity gate)
 *   0x16 CRATE   — 16-bit signed, 0.208 %/h per LSB
 *
 * One driver, any bus: an instance binds to an I²C transport at create time.
 * The read path carries the heavy probe+retry+budget+degradation loop tuned
 * for the buggy AliExpress clones that ACK to any address and only complete
 * a register read intermittently — so the same robustness covers the bus0
 * field gauge (via battery.c) and a bus1 quarantine part (via the registry).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_xport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct max17048 max17048_t;

/* Allocate an instance bound to (bus, addr) and probe (VERSION gate +
 * QuickStart). Always returns a usable handle once the transport opens
 * (detection may have failed — max17048_ready() reports it), or NULL if the
 * bus is unavailable / OOM. */
max17048_t *max17048_create(cb_bus_t bus, uint8_t addr);

/* (Re)probe: VERSION identity check (rejects clones / phantom ACKs) +
 * QuickStart, marks ready. Idempotent; cheap no-op once ready. */
esp_err_t max17048_probe(max17048_t *m);

bool max17048_ready(max17048_t *m);

/* Robust single 16-bit register read (probe-until-warm, retry rounds with a
 * bus recover between, wall-clock budget so a dead clone can't trip the task
 * watchdog, and steady-state degradation to un-ready after repeated fails).
 * `out` is untouched on failure. */
esp_err_t max17048_read_reg(max17048_t *m, uint8_t reg, uint16_t *out);

/* Cooked reads. Sentinels: soc/vbat = -1.0f on failure, crate = 0.0f. */
float max17048_soc(max17048_t *m);
float max17048_vbat(max17048_t *m);
float max17048_crate(max17048_t *m);

#ifdef __cplusplus
}
#endif
