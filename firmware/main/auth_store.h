/* auth_store.h — NVS-backed, runtime-reconfigurable HTTP Basic-auth
 * (web-admin) credentials, mirroring wifi_store's reconfigurable-creds model.
 *
 * The web admin password used to be compile-time only (HTTP_BASIC_USER/PASS in
 * secrets.h), so a fielded board stuck on the default could only be re-secured
 * by reflashing. This store lets the operator set real credentials at runtime
 * via /config or the cmd/auth MQTT topic; the gate resolves the effective set
 * each request (NVS override → compile-time default), so a change applies live
 * with no reboot.
 *
 * Like wifi_store, the credentials live in their OWN NVS namespace ("auth")
 * that nothing else touches, and the password is NEVER published to MQTT/HA —
 * it is not an app_config schema key (which would echo it to the broker).
 *
 * Length caps keep the decoded "user:pass" within the gate's base64 decode
 * buffer (see basic_auth_gate in http_server.c): user ≤ 31, pass ≤ 63. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_STORE_USER_CAP 32  /* incl. NUL → 31 usable */
#define AUTH_STORE_PASS_CAP 64  /* incl. NUL → 63 usable */

/* Load the effective creds into an in-RAM cache (reads NVS once). MUST be
 * called once at boot from an INTERNAL-stack task (app_main) before the HTTP
 * server starts — auth_store_get_effective() then serves from the cache with
 * no flash access, which is what makes it safe to call from a PSRAM-stacked
 * HTTP worker (a flash read there trips the cache-disable stack assert). */
void auth_store_init(void);

/* Resolve the credentials the gate should check against this request: the NVS
 * override if set, else the compile-time HTTP_BASIC_USER/PASS. Always fills
 * both (NUL-terminated). Served from the RAM cache — no flash access. */
void auth_store_get_effective(char *user, size_t ucap, char *pass, size_t pcap);

/* Read only the operator-set override (no compile-time fallback).
 * *is_custom=true with values when set; false (empty strings) otherwise. */
esp_err_t auth_store_get(char *user, size_t ucap, char *pass, size_t pcap,
                         bool *is_custom);

/* Set the override. user 1..31, pass 1..63 (both required). Passing BOTH
 * empty clears the override (revert to the compile-time default). Applied
 * live — the gate re-reads on the next request, no reboot needed. */
esp_err_t auth_store_set(const char *user, const char *pass);

/* Drop the override (factory reset). Equivalent to auth_store_set("",""). */
esp_err_t auth_store_erase(void);

#ifdef __cplusplus
}
#endif
