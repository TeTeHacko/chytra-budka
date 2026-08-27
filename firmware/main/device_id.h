/* device_id.h — Per-board identifier derived from the WiFi STA MAC.
 *
 * Two boards on the same broker need distinct MQTT client_ids, HA
 * discovery device.ids, hostnames, and relay stream paths — otherwise
 * the broker kicks one of them, HA sees them as one device, and the
 * relay overwrites a single stream. Use HOSTNAME ("cb") as the project
 * namespace and append the last 24 bits of the STA MAC (e.g. "ex01")
 * to make the id unique per silicon. The id is a valid DNS hostname
 * (lowercase, hyphen, no underscore) so "<id>.<domain>" resolves cleanly.
 *
 * Both getters are safe to call before WiFi init — esp_read_mac()
 * reads from eFUSE, not the radio. Results are cached after first call. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full id, e.g. "cb-ex01". Used for: MQTT client_id, WiFi/mDNS
 * hostname, HA discovery device.ids, HA discovery topic path, HA entity
 * uniq_id prefix, and as the runtime MQTT topic root for
 * state/cmd/event/diag namespaces. */
const char *device_id(void);

/* MAC-tail suffix only, e.g. "ex01". Used by audio relay path builder
 * and JPEG filename stamper where the project namespace is already
 * implicit. */
const char *device_id_suffix(void);

/* Build the board's own HTTPS web-UI URL into `out`:
 * "https://cb-<suffix>.<domain>/" (or "https://<id>/" when
 * `domain` is NULL/empty). `domain` is passed by the caller (typically
 * wifi_mgr_get_domain()) so this stays free of a wifi_mgr dependency.
 * Used by the OLED web-URL QR and the /oled/qr default. */
void device_url(char *out, size_t cap, const char *domain);

#ifdef __cplusplus
}
#endif
