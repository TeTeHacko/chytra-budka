/* ble_parse.h — pure decoders for the BLE meter payloads (BLE.md, phase 1).
 *
 * No NimBLE, no ESP-IDF — plain libc so it builds + unit-tests on the host
 * (firmware/tests/native/test_ble_parse.c), the same split as sd_layout.c.
 * ble.c (NimBLE) feeds raw advertisement / GATT-notification bytes in here. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Atorch UC96 USB-C power meter — 36-byte BLE report frame ──
 * Big-endian fields; header 0xFF 0x55, msg type 0x01. Mirrors
 * power-meter/uc96d.py:parse_frame so host + firmware agree byte-for-byte. */
typedef struct {
    float    voltage_v;     /* bus voltage      [4:7]  /100  */
    float    current_a;     /* bus current      [7:10] /100  */
    float    power_w;       /* V * I (derived)               */
    float    capacity_ah;   /* accumulated Ah   [10:13] /1000 */
    float    energy_wh;     /* accumulated Wh   [13:17] /100  */
    float    dminus_v;      /* USB D- line      [17:19] /100  */
    float    dplus_v;       /* USB D+ line      [19:21] /100  */
    int      temperature_c; /* meter temp       [21:23]       */
    uint32_t runtime_s;     /* elapsed time     [23:27]       */
} ble_uc96_reading_t;

/* True + fills *out for a valid 36-byte UC96 report; false (out untouched) on
 * bad length / header / message type. */
bool ble_parse_uc96(const uint8_t *buf, size_t len, ble_uc96_reading_t *out);

/* ── BTHome v2 (unencrypted) advertisement service-data, UUID 0xFCD2 ──
 * The common thermo-hygrometer objects. A field is meaningful only when its
 * *_present flag is set (an advertiser need not include every object). */
typedef struct {
    bool    temp_present;     float   temp_c;        /* obj 0x02 sint16 /100 */
    bool    humidity_present; float   humidity_pct;  /* obj 0x03 uint16 /100 */
    bool    battery_present;  uint8_t battery_pct;   /* obj 0x01 uint8  %    */
    bool    voltage_present;  float   voltage_v;     /* obj 0x0C uint16 /1000 */
} ble_bthome_reading_t;

/* Parse BTHome v2 service data (the bytes AFTER the 0xFCD2 UUID, starting with
 * the device-info byte). Returns true for a recognised UNENCRYPTED v2 payload,
 * filling whichever known objects were present (others skipped by their
 * spec size; an unknown object id stops the walk, keeping what was decoded).
 * Returns false for encrypted payloads, a non-v2 version, or empty input. */
bool ble_parse_bthome(const uint8_t *data, size_t len, ble_bthome_reading_t *out);

#ifdef __cplusplus
}
#endif
