/* i2c_bus.h — shared I²C master buses owned by the firmware.
 *
 * Bus0 (main) is the HW I²C NUM_0 controller on D4/D5 — shared between
 * the battery fuel gauge (MAX17048 @ 0x36) and SHT41 ambient sensor
 * (@ 0x44). HW NUM_1 is owned by the camera SCCB, so it's unavailable.
 *
 * Bus1 (diagnostic) lives on D6/D7 via *bit-banged* I²C in i2c_bb.[ch].
 * Used for a quarantined SHT41 or a misbehaving MAX17048 clone; opt-in,
 * lazy-init on the first probe / read.
 *
 * Idempotent: i2c_bus_get() initializes on first call, returns the same
 * handle thereafter; NULL on init failure. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

i2c_master_bus_handle_t i2c_bus_get(void);

/* Recover a wedged bus0: issue the IDF bus reset (9 SCL pulses + STOP via
 * the controller) so a slave that latched SDA low lets go. Use after a
 * driver exhausts its own retries — bus0 (unlike the bit-bang bus1) has no
 * recovery built into its read paths, so a single stuck slave (e.g. a
 * MAX17048 clone) could otherwise wedge SHT41 — the *required* sensor —
 * indefinitely. Returns ESP_OK on a successful reset, an error (or
 * ESP_ERR_INVALID_STATE if bus0 isn't up) otherwise. The IDF i2c_master
 * driver serialises this against in-flight transactions internally, so it
 * is safe to call while other bus0 drivers are active. */
esp_err_t i2c_bus0_recover(void);

/* Initialise the bit-bang bus1 pins (idempotent). Callers that want to
 * read SHT41 / probe a device on bus1 should call this first; the
 * diagnostic scan does it transparently. Returns true on success. */
bool i2c_bus1_ensure(void);

/* Sweep 0x08..0x77 on both I²C buses, write a human-readable report to `out`.
 * Format:
 *   "bus0 D4/D5 (GPIO5/GPIO6): found: 0x44\n"
 *   "  0x36 — MAX17048 (battery)   MISSING\n"
 *   "  0x40 — INA226 (solar)        MISSING\n"
 *   "  0x44 — SHT41 (temp/RH)       OK\n"
 *   "bus1 D6/D7 (GPIO43/GPIO44): found: (nothing)\n"
 *   "  0x36 — MAX17048 quarantine   MISSING\n"
 *   "  0x44 — SHT41 secondary       MISSING\n"
 *   "  0x6C — variant: MAX17041/44 (driver expects 0x36)\n"
 * Returns number of devices that responded on all buses. */
int i2c_bus_scan_report(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
