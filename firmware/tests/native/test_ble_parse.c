/* test_ble_parse.c — host unit tests for the pure BLE meter decoders.
 * No NimBLE / ESP-IDF; ble_parse.c is plain libc. (BLE.md, phase 1.) */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ble_parse.h"

static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int feq(float a, float b) { return fabsf(a - b) < 0.005f; }

static int test_uc96_valid(void) {
    /* V=5.00, I=1.50, cap=1.234Ah, Wh=2.50, temp=25, runtime=1h2m3s=3723s */
    uint8_t f[36] = {0};
    f[0] = 0xFF; f[1] = 0x55; f[2] = 0x01;            /* magic + report */
    f[4] = 0x00; f[5] = 0x01; f[6] = 0xF4;            /* 500  → 5.00 V  */
    f[7] = 0x00; f[8] = 0x00; f[9] = 0x96;            /* 150  → 1.50 A  */
    f[10] = 0x00; f[11] = 0x04; f[12] = 0xD2;         /* 1234 → 1.234 Ah */
    f[13] = 0x00; f[14] = 0x00; f[15] = 0x00; f[16] = 0xFA; /* 250 → 2.50 Wh */
    f[21] = 0x00; f[22] = 0x19;                       /* temp 25 */
    f[23] = 0x00; f[24] = 0x01; f[25] = 0x02; f[26] = 0x03; /* 1h2m3s */

    ble_uc96_reading_t r;
    CHECK(ble_parse_uc96(f, sizeof(f), &r));
    CHECK(feq(r.voltage_v, 5.00f));
    CHECK(feq(r.current_a, 1.50f));
    CHECK(feq(r.power_w, 7.50f));
    CHECK(feq(r.capacity_ah, 1.234f));
    CHECK(feq(r.energy_wh, 2.50f));
    CHECK(r.temperature_c == 25);
    CHECK(r.runtime_s == 3723u);
    return 0;
}

static int test_uc96_rejects(void) {
    uint8_t f[36] = {0};
    f[0] = 0xFF; f[1] = 0x55; f[2] = 0x01;
    ble_uc96_reading_t r;
    CHECK(!ble_parse_uc96(f, 35, &r));        /* wrong length */
    f[1] = 0x56; CHECK(!ble_parse_uc96(f, 36, &r)); /* bad magic */
    f[1] = 0x55; f[2] = 0x02;
    CHECK(!ble_parse_uc96(f, 36, &r));        /* not a report frame */
    CHECK(!ble_parse_uc96(NULL, 36, &r));     /* null */
    return 0;
}

static int test_bthome_basic(void) {
    /* v2, unencrypted (0x40): temp 23.45, humidity 48.00, battery 85 */
    uint8_t d[] = {0x40, 0x02, 0x29, 0x09, 0x03, 0xC0, 0x12, 0x01, 0x55};
    ble_bthome_reading_t r;
    CHECK(ble_parse_bthome(d, sizeof(d), &r));
    CHECK(r.temp_present && feq(r.temp_c, 23.45f));
    CHECK(r.humidity_present && feq(r.humidity_pct, 48.00f));
    CHECK(r.battery_present && r.battery_pct == 85);
    CHECK(!r.voltage_present);
    return 0;
}

static int test_bthome_negative_temp(void) {
    /* temp -5.00 °C → -500 = 0xFE0C, LE bytes 0x0C 0xFE */
    uint8_t d[] = {0x40, 0x02, 0x0C, 0xFE};
    ble_bthome_reading_t r;
    CHECK(ble_parse_bthome(d, sizeof(d), &r));
    CHECK(r.temp_present && feq(r.temp_c, -5.00f));
    return 0;
}

static int test_bthome_voltage(void) {
    /* voltage 3.000 V → 3000 = 0x0BB8, LE 0xB8 0x0B */
    uint8_t d[] = {0x40, 0x0C, 0xB8, 0x0B};
    ble_bthome_reading_t r;
    CHECK(ble_parse_bthome(d, sizeof(d), &r));
    CHECK(r.voltage_present && feq(r.voltage_v, 3.000f));
    return 0;
}

static int test_bthome_rejects_and_stops(void) {
    ble_bthome_reading_t r;
    /* encrypted (bit0 set) → reject */
    uint8_t enc[] = {0x41, 0x02, 0x29, 0x09};
    CHECK(!ble_parse_bthome(enc, sizeof(enc), &r));
    /* wrong version (bits5-7 != 2): 0x00 → v0 */
    uint8_t v0[] = {0x00, 0x02, 0x29, 0x09};
    CHECK(!ble_parse_bthome(v0, sizeof(v0), &r));
    /* unknown object id stops the walk but keeps what was decoded:
     * battery first, then unknown 0x99 → temp after it is NOT reached. */
    uint8_t mixed[] = {0x40, 0x01, 0x55, 0x99, 0x02, 0x29, 0x09};
    CHECK(ble_parse_bthome(mixed, sizeof(mixed), &r));
    CHECK(r.battery_present && r.battery_pct == 85);
    CHECK(!r.temp_present);
    /* empty / null */
    CHECK(!ble_parse_bthome(NULL, 4, &r));
    CHECK(!ble_parse_bthome(mixed, 0, &r));
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_uc96_valid();
    rc |= test_uc96_rejects();
    rc |= test_bthome_basic();
    rc |= test_bthome_negative_temp();
    rc |= test_bthome_voltage();
    rc |= test_bthome_rejects_and_stops();
    if (rc == 0) printf("  ok — %d checks\n", g_checks);
    return rc;
}
