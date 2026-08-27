/* wifi_mgr.h — event-driven WiFi STA on top of esp_wifi.
 *
 * Init creates default netif + event loop, registers handlers, and starts
 * STA. Reconnect is automatic on WIFI_EVENT_STA_DISCONNECTED.
 * `wifi_mgr_wait_connected()` blocks until IP is acquired (or timeout).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init the WiFi stack. ap_mode=true forces AP-only bring-up (no station)
 * even when the sticky ap_only flag is unset — used for the unprovisioned
 * first-boot provisioning portal (no usable STA target). ap_mode=false uses
 * the normal STA path unless the sticky ap_only flag is set in wifi_store. */
esp_err_t wifi_mgr_init(bool ap_mode);
bool      wifi_mgr_is_connected(void);

/* Block until got IP, up to `timeout_ms`. 0 = wait forever. */
bool      wifi_mgr_wait_connected(uint32_t timeout_ms);

/* Last RSSI in dBm (or 0 if not connected). */
int       wifi_mgr_rssi(void);

/* BSSID (AP MAC) of the currently-associated AP, formatted "aa:bb:cc:dd:ee:ff"
 * into `out` (needs ≥18 bytes). Returns false (out untouched) when not
 * associated or the info read fails. */
bool      wifi_mgr_get_bssid(char *out, size_t out_cap);

/* The SSID actually applied this boot (resolved by wifi_store: candidate /
 * known-good / compile default). Never returns NULL; returns "" before
 * wifi_mgr_init(). Safe to publish — the password is never exposed. */
const char *wifi_mgr_get_ssid(void);

/* SoftAP recovery fallback. wifi_mgr_start_softap() switches to APSTA and
 * brings up a WPA2 AP named "cb-<suffix>" (AP_SSID_FMT; passphrase =
 * operator-set ap_pass, else AP_PASS_DEFAULT — or the random first-boot
 * password on an unprovisioned board) so an operator can re-provision a
 * board that can't
 * reach its station network — STA keeps running underneath so a recovered
 * home AP still auto-reconnects. Time-bounded by the caller. */
esp_err_t wifi_mgr_start_softap(void);
esp_err_t wifi_mgr_stop_softap(void);
bool      wifi_mgr_softap_active(void);

/* Number of stations currently associated to the SoftAP (0 when the AP is
 * down or on a query error). Lets the OLED swap the onboarding QR for the
 * status page once a client joins, and back when they leave. */
int       wifi_mgr_ap_sta_count(void);

/* WiFi onboarding (bench OLED): replace the well-known public AP default
 * password (AP_PASS_DEFAULT) with a fresh per-boot random one, so an
 * unprovisioned board isn't reachable on a guessable credential — the
 * operator reads it from the on-screen QR instead. MUST be called BEFORE
 * wifi_mgr_init() (it feeds build_ap_config). No effect once a custom AP
 * password is set in wifi_store (operator creds win). */
void wifi_mgr_use_random_ap_pass(void);

/* The SSID + password the SoftAP/AP-only interface was actually configured
 * with this boot (operator-custom, onboarding-random, or the default) — for
 * the OLED onboarding QR. Returns false (outputs untouched) if no AP has been
 * brought up this boot. `pass` is "" for an open AP. */
bool wifi_mgr_get_ap_creds(char *ssid, size_t scap, char *pass, size_t pcap);

/* Write the STA IPv4 address as dotted-quad ("192.0.2.42") into out.
 * Returns false (and leaves out untouched) when not associated or no
 * address has been assigned yet. */
bool      wifi_mgr_get_ip_str(char *out, size_t out_cap);

/* Domain suffix from DHCP option 15 (Domain Name) in the latest lease,
 * or the compile-time CB_DOMAIN_FALLBACK (config.h) when DHCP didn't
 * supply one. Used by tls_enroll.c to build the device's FQDN for the
 * CSR. Never returns NULL; returns "" only if the fallback is also
 * empty (i.e. operator explicitly disabled the fallback). */
const char *wifi_mgr_get_domain(void);

/* One scanned access point. */
typedef struct {
    char    ssid[33];   /* NUL-terminated; empty entries skipped */
    int8_t  rssi;       /* dBm (negative) */
    uint8_t authmode;   /* wifi_auth_mode_t; 0 = open */
} wifi_scan_ap_t;

/* Active scan of nearby APs, for the /config STA SSID picker. Fills up to
 * `max` entries in `out`, de-duplicated by SSID (strongest kept) and sorted
 * by RSSI (strongest first). Returns the number written, or -1 if a scan
 * isn't possible right now (no STA interface — e.g. pure AP-only mode) or it
 * failed. Blocking (~a few seconds, all channels); briefly interrupts an
 * active STA connection, so it's an on-demand operator action only. */
int wifi_mgr_scan(wifi_scan_ap_t *out, int max);

/* Debug: force a disconnect. Equivalent to "kick from AP" — wifi_mgr's
 * normal disconnect handler then drives the exponential backoff retry
 * pipeline (500 ms → 60 s cap, resets to 500 ms on GOT_IP).
 *
 * Defined only when CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS is set;
 * production builds omit it from the binary. Callers must gate their
 * callsite with the same flag (the declaration stays visible to keep
 * compile errors readable). */
void      wifi_mgr_force_disconnect(void);

/* Debug/bench: suppress STA auto-reconnect for `seconds` so the SoftAP
 * fallback stays up over RF for testing (the good home AP would otherwise
 * reconnect immediately and tear it down). DEBUG_ENDPOINTS builds only. */
void      wifi_mgr_test_hold_sta_down(int seconds);

#ifdef __cplusplus
}
#endif
