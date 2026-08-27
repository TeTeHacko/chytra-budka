/* ble_store.h — NVS-backed allowlist of BLE devices (UC96 meters + BTHome
 * peripherals), keyed by MAC, with an optional friendly name.
 *
 * Connection model is ALLOWLIST: ble.c only GATT-connects a UC96 — and only
 * ingests/publishes a BTHome sensor — whose MAC is saved here. Scanning still
 * surfaces every device in range (for the web UI), but a device must be saved
 * before the firmware acts on it, so a neighbour's UC96 is never touched.
 *
 * "Saved" == an entry exists for that MAC. The value is the friendly name,
 * which may be empty (saved-but-unnamed → the UI shows the MAC). Names are NOT
 * in the app_config schema on purpose: that schema is fixed/compile-time and
 * every key auto-publishes to HA discovery + retained cfg echo. This is a
 * dynamic per-MAC map in its own NVS namespace ("ble_dev"), mirroring the
 * wifi_store/auth_store split. Each save is one nvs_commit (torn-write-safe). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device id = MAC as 12 lowercase hex chars, no separators (matches ble.c's
 * meter_id_str()). +1 for NUL. Friendly name kept short for the UI/NVS key. */
#define BLE_STORE_ID_LEN   12
#define BLE_STORE_ID_CAP   (BLE_STORE_ID_LEN + 1)
#define BLE_STORE_NAME_CAP 33

/* Idempotent — ensures the "ble_dev" namespace exists. Safe before any get. */
esp_err_t ble_store_init(void);

/* True iff a device with this id is in the allowlist. id must be 12 hex chars. */
bool ble_store_is_saved(const char *id);

/* Read the friendly name for a saved device into out (cap incl. NUL). Returns
 * true when the device is saved (out holds its name, "" if unnamed); false +
 * out[0]=0 when not saved. */
bool ble_store_get_name(const char *id, char *out, size_t cap);

/* Add a device to the allowlist (or rename an existing one). name may be ""
 * (saved without a label) or up to BLE_STORE_NAME_CAP-1 printable chars; it's
 * sanitised (control chars dropped). One nvs_commit. id must be 12 hex chars. */
esp_err_t ble_store_save(const char *id, const char *name);

/* Remove a device from the allowlist. No-op-safe if it isn't saved. */
esp_err_t ble_store_forget(const char *id);

/* Number of saved devices. */
int ble_store_count(void);

/* Fetch the idx-th saved device (0-based, order is NVS-iterator order, stable
 * within a boot). Fills id_out (BLE_STORE_ID_CAP) + name_out (BLE_STORE_NAME_CAP).
 * Returns true on success, false if idx is out of range. */
bool ble_store_list(int idx, char *id_out, size_t id_cap,
                    char *name_out, size_t name_cap);

#ifdef __cplusplus
}
#endif
