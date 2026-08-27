/* battery.c — the board's MAX17048 fuel gauge (bus0).
 *
 * Thin owner of a single bus0 MAX17048 instance. The gauge logic (the
 * clone-tolerant probe+retry+budget read loop, VERSION identity gate,
 * QuickStart, degradation handling) lives in the instance driver max17048.c
 * now, shared with the bus1 quarantine part the registry tracks. This file
 * keeps the historic battery_* API so main.cpp's dedicated SOC / VCELL /
 * charge-rate telemetry path is unchanged. */

#include "battery.h"

#include "config.h"
#include "i2c_xport.h"
#include "max17048.h"

static max17048_t *s_gauge = NULL;

esp_err_t battery_init(void) {
    if (!s_gauge)
        s_gauge = max17048_create(CB_BUS0, MAX17048_ADDR);
    if (!s_gauge)
        return ESP_FAIL;            /* bus0 unavailable */
    return max17048_probe(s_gauge); /* ESP_OK if ready, ESP_ERR_NOT_FOUND otherwise */
}

bool battery_ready(void) { return max17048_ready(s_gauge); }

float battery_soc(void) { return max17048_soc(s_gauge); }

float battery_vbat(void) { return max17048_vbat(s_gauge); }

float battery_charge_rate(void) { return max17048_crate(s_gauge); }

bool battery_on_external_power(void) {
    /* Heuristic for "won't brown out mid-OTA-flash":
     *   - No fuel gauge present  → USB/mains board (e.g. the field unit) → true.
     *   - Charging (CRATE clearly positive) → external source connected → true.
     *   - Otherwise (discharging on battery) → false.
     * The +0.5 %/h floor rejects MAX17048 CRATE noise around zero. A solar unit
     * in darkness reads as on-battery; the OTA SOC gate then protects it. */
    if (!max17048_ready(s_gauge)) return true;
    return max17048_crate(s_gauge) > 0.5f;
}
