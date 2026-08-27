/* wifi_store.h — NVS-backed, reconfigurable WiFi credentials with a
 * brick-safe candidate/known-good ladder.
 *
 * WiFi creds are NOT in the app_config schema on purpose: every schema
 * key is auto-published to HA discovery + echoed retained to
 * <id>/state/cfg/<key>, which would leak the WPA2 password to the MQTT
 * broker. This module keeps creds in their own NVS namespace ("wifi_cfg")
 * that nothing else touches, and the password is never published anywhere.
 *
 * Three tiers, applied by wifi_store_get_effective() at boot:
 *   1. CANDIDATE — a newly-submitted set being tried this boot. The
 *      verify-before-commit logic in main.cpp promotes it to known-good
 *      once the device reaches IP + MQTT, or auto-reverts if it doesn't.
 *   2. GOOD      — the last set proven healthy.
 *   3. DEFAULT   — the compile-time WIFI_SSID/WIFI_PASSWORD from
 *      secrets.h. This is the immutable floor: a full erase or a failed
 *      first-ever provision always lands back here, so the board is never
 *      left with no creds at all.
 *
 * The store either holds a coherent record or falls through to the next
 * tier — wifi_store_set_candidate() commits all keys in one nvs_commit so
 * a power loss mid-write can't strand a torn candidate (mirrors
 * tls_store_save()). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 802.11: SSID ≤ 32 bytes, WPA2 PSK ≤ 63 chars. +1 for NUL. */
#define WIFI_STORE_SSID_CAP 33
#define WIFI_STORE_PASS_CAP 64

typedef enum {
    WIFI_CREDS_DEFAULT = 0, /* compile-time secrets.h fallback */
    WIFI_CREDS_GOOD,        /* known-good set from NVS */
    WIFI_CREDS_CANDIDATE,   /* candidate pending verification */
} wifi_creds_src_t;

/* Idempotent — ensures the "wifi_cfg" namespace exists. Safe to call
 * before any get/set. Returns ESP_OK even on a fresh (empty) device. */
esp_err_t wifi_store_init(void);

/* Resolve which creds to use THIS boot, applying the candidate → good →
 * default ladder. Always fills ssid/pass (falls to secrets.h if NVS is
 * empty) and reports the winning tier in *src (may be NULL). */
esp_err_t wifi_store_get_effective(char *ssid, size_t ssid_cap,
                                   char *pass, size_t pass_cap,
                                   wifi_creds_src_t *src);

/* Stage a new credential set as the candidate, preserving the current
 * known-good untouched. Writes all keys + a single nvs_commit; returns an
 * error (and changes nothing) if the commit fails — callers MUST check the
 * return before rebooting so they never reboot into a no-op. ssid 1..32
 * chars; pass 0 (open) or 8..63 chars. */
esp_err_t wifi_store_set_candidate(const char *ssid, const char *pass);

/* Candidate proved healthy → make it the new known-good and drop the
 * candidate. No-op-safe if there is no candidate. */
esp_err_t wifi_store_promote_candidate(void);

/* Candidate failed → drop it, leaving known-good (or default) in charge. */
esp_err_t wifi_store_revert_candidate(void);

/* True when a candidate is staged and pending verification. */
bool wifi_store_has_candidate(void);

/* True when a known-good (NVS-promoted) credential set exists — i.e. there is
 * something safe to revert a failed candidate TO. False during initial
 * onboarding (after a factory reset, or a clean-clone build), where a revert
 * would instead strand the box in the unprovisioned AP portal. Lets the
 * candidate-verify commit on association alone when there's no safer fallback. */
bool wifi_store_has_known_good(void);

/* True when this boot has a usable STA target to try: a pending candidate, a
 * known-good NVS set, or a real (non-placeholder, non-empty) compile-time
 * WIFI_SSID (fleet pre-provisioning). False on an unprovisioned clean-clone
 * build (WiFi creds left as the secrets.h.example placeholder / blank) — the
 * boot path uses this to come up directly in the AP provisioning portal
 * instead of failing STA and waiting out the SoftAP recovery trigger. */
bool wifi_store_have_sta_target(void);

/* Erase the whole "wifi_cfg" namespace → next boot falls back to the
 * compile-time secrets.h default (reset tier a). Also clears any
 * operator-set AP creds and the ap_only flag (so a BOOT-button factory
 * reset is the guaranteed way out of AP-only mode). */
esp_err_t wifi_store_erase(void);

/* ── SoftAP / AP-only config ───────────────────────────────────────────
 * The recovery SoftAP (and the full AP-only mode) use these creds. When
 * unset, wifi_mgr uses the defaults (SSID cb-<suffix> per AP_SSID_FMT,
 * pass AP_PASS_DEFAULT — random per-boot on an unprovisioned first boot).
 * Stored in the same "wifi_cfg" namespace. */

/* Read operator-set AP creds. Returns ESP_OK with *is_custom=true and the
 * stored values when set; *is_custom=false (ssid/pass untouched) when the
 * operator hasn't overridden them — caller uses its derived default. */
esp_err_t wifi_store_get_ap(char *ssid, size_t scap, char *pass, size_t pcap,
                            bool *is_custom);
/* Set operator AP creds. ssid 1..32, pass "" (open) or 8..63. Passing
 * BOTH empty clears the override (revert to derived default). */
esp_err_t wifi_store_set_ap(const char *ssid, const char *pass);

/* Full AP-only operating mode (sticky across reboots). When true, the
 * device boots as an access point only — NO station, hence no MQTT / OTA /
 * remote recovery. Cleared by wifi_store_set_ap_only(false) (e.g. from the
 * local web page) or a factory erase (BOOT-button). */
bool      wifi_store_is_ap_only(void);
esp_err_t wifi_store_set_ap_only(bool on);

#ifdef __cplusplus
}
#endif
