/* ble_parse.c — pure BLE meter decoders. See ble_parse.h / BLE.md. */
#include "ble_parse.h"

#include <string.h>

/* Big-endian unsigned, n ≤ 4 bytes. */
static uint32_t be(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

bool ble_parse_uc96(const uint8_t *buf, size_t len, ble_uc96_reading_t *out) {
    if (!buf || !out || len != 36) return false;
    if (buf[0] != 0xFF || buf[1] != 0x55) return false; /* MAGIC ff 55 */
    if (buf[2] != 0x01) return false;                   /* MSG_TYPE_REPORT */

    ble_uc96_reading_t r;
    r.voltage_v     = be(buf + 4, 3) / 100.0f;
    r.current_a     = be(buf + 7, 3) / 100.0f;
    r.capacity_ah   = be(buf + 10, 3) / 1000.0f;
    r.energy_wh     = be(buf + 13, 4) / 100.0f;
    r.dminus_v      = be(buf + 17, 2) / 100.0f;
    r.dplus_v       = be(buf + 19, 2) / 100.0f;
    r.temperature_c = (int)be(buf + 21, 2);
    r.runtime_s     = be(buf + 23, 2) * 3600u + buf[25] * 60u + buf[26];
    r.power_w       = r.voltage_v * r.current_a;
    *out = r;
    return true;
}

/* BTHome v2 object data sizes (bytes) for the ids we know — used to decode the
 * ones we care about and to skip the rest. An id not in this table can't be
 * skipped safely (BTHome encodes no per-object length), so the walk stops. */
static int bthome_obj_size(uint8_t id) {
    switch (id) {
    case 0x00: return 1; /* packet id      */
    case 0x01: return 1; /* battery %      */
    case 0x02: return 2; /* temperature    */
    case 0x03: return 2; /* humidity       */
    case 0x04: return 3; /* pressure       */
    case 0x05: return 3; /* illuminance    */
    case 0x0C: return 2; /* voltage        */
    default:   return -1;
    }
}

bool ble_parse_bthome(const uint8_t *data, size_t len, ble_bthome_reading_t *out) {
    if (!data || !out || len < 1) return false;

    uint8_t info = data[0];
    if (info & 0x01) return false;        /* encrypted — not supported */
    if ((info >> 5) != 2) return false;   /* BTHome version must be 2 */

    memset(out, 0, sizeof(*out));

    size_t i = 1;
    while (i < len) {
        uint8_t id = data[i];
        int sz = bthome_obj_size(id);
        if (sz < 0) break;                /* unknown id — can't skip, stop */
        if (i + 1 + (size_t)sz > len) break; /* truncated */
        const uint8_t *v = &data[i + 1];
        switch (id) {
        case 0x01: /* battery, uint8 % */
            out->battery_present = true;
            out->battery_pct = v[0];
            break;
        case 0x02: /* temperature, sint16 LE, 0.01 °C */
            out->temp_present = true;
            out->temp_c = (int16_t)(v[0] | (v[1] << 8)) / 100.0f;
            break;
        case 0x03: /* humidity, uint16 LE, 0.01 % */
            out->humidity_present = true;
            out->humidity_pct = (uint16_t)(v[0] | (v[1] << 8)) / 100.0f;
            break;
        case 0x0C: /* voltage, uint16 LE, 0.001 V */
            out->voltage_present = true;
            out->voltage_v = (uint16_t)(v[0] | (v[1] << 8)) / 1000.0f;
            break;
        default:
            break; /* known size, value we don't decode — skip */
        }
        i += 1 + (size_t)sz;
    }
    return true;
}
