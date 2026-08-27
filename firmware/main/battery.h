/* battery.h — MAX17048 fuel-gauge driver (I²C master).
 *
 * Single-shot init consumes the SHARED I²C bus (i2c_bus_get()), adds the
 * MAX17048 device, and issues a QuickStart so the gauge re-models the cell.
 * Subsequent reads are stateless register fetches.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns ESP_OK on success, ESP_ERR_NOT_FOUND if device does not respond. */
esp_err_t battery_init(void);

/* True after battery_init() succeeded. Reads return -1.0f when not ready. */
bool battery_ready(void);

/* State of charge in percent (0..100). Returns -1.0f on error. */
float battery_soc(void);

/* Cell voltage in volts. Returns -1.0f on error. */
float battery_vbat(void);

/* Charge rate %/h, signed (positive = charging). Returns 0.0f on error. */
float battery_charge_rate(void);

/* True when the board appears to be on external power (no fuel gauge present →
 * USB/mains board, or the gauge reports clearly-positive charge rate). Used by
 * the OTA SOC gate to bypass the battery threshold on a mains/USB unit. */
bool battery_on_external_power(void);

#ifdef __cplusplus
}
#endif
