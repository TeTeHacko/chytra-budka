/* ble.h — optional BLE meter subsystem (see firmware/BLE.md).
 *
 * NimBLE central: active scan → connect → read Atorch UC96 power meters
 * (V/I/P/Wh) over GATT and publish per meter to MQTT, with WiFi given absolute
 * RF priority and "no meter present" treated as the normal case. All entry
 * points are safe no-ops when CONFIG_CHYTRA_BUDKA_BLE is off (the NimBLE stack
 * isn't even built) or the `ble_enabled` NVS knob is false. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_parse.h"   /* ble_uc96_reading_t, ble_bthome_reading_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Start BLE if compiled in AND `ble_enabled`=true. Call once after WiFi is up
 * (skipped in crash-loop safe mode). Idempotent. */
void ble_start(void);

/* Live reaction to a `ble_enabled` toggle over cmd/cfg. */
void ble_apply_config(void);

/* True iff BLE is compiled in and the host is running. */
bool ble_running(void);

/* Short human/selftest status: "off" / "scanning,no-uc96" /
 * "uc96 seen <N>s ago" / "uc96 <N> streaming". */
void ble_status(char *out, size_t out_sz);

/* ── web-UI surface ─────────────────────────────────────────────────────
 * Device kinds + a snapshot of every device the scan currently knows about
 * (in-range advertisers + connected meters), for the /ble management page.
 * The web layer cross-references ble_store for the saved/allowlist + names. */
typedef enum { BLE_DEV_OTHER = 0, BLE_DEV_UC96 = 1, BLE_DEV_BTHOME = 2 } ble_dev_kind_t;

typedef struct {
    char    id[13];          /* MAC hex, no colons (ble_store key) */
    char    adv_name[32];    /* advertised name, "" if none */
    int     rssi;            /* last advert RSSI (0 if only connected) */
    uint8_t kind;            /* ble_dev_kind_t */
    int     age_s;           /* seconds since last advert / notify */
    uint8_t conn_state;      /* 0 discovered, 1 connecting, 2 connected, 3 streaming */
    bool    has_uc96;        ble_uc96_reading_t   uc96;    /* last UC96 frame */
    bool    has_bthome;      ble_bthome_reading_t bthome;  /* last BTHome data */
} ble_dev_view_t;

/* Fill out[] (up to max) with the current device snapshot; returns the count.
 * Display-only — values may race a concurrent advert (bounded, never unsafe). */
int  ble_snapshot(ble_dev_view_t *out, int max);

/* Ensure the scan is running (UI "scan" button). No-op if BLE isn't up. */
void ble_request_scan(void);

#ifdef __cplusplus
}
#endif
