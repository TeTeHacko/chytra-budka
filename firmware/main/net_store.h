/* net_store.h — NVS-backed, runtime-reconfigurable network endpoints with a
 * brick-safe candidate/known-good ladder (clone of the wifi_store pattern —
 * see wifi_store.h for the rationale; NOTES.md "Config portability" is the
 * limitation this module removes).
 *
 * Endpoints are NOT in the app_config schema on purpose: schema keys are
 * auto-published to HA discovery + echoed retained, which would leak broker
 * credentials/tokens, and the schema's "apply now" contract is wrong for
 * connectivity-critical values. This module keeps everything in its own NVS
 * namespace ("net_cfg") as two flat-JSON blobs:
 *
 *   cand   candidate record being tried this boot (verify-before-commit:
 *          main.cpp promotes it once MQTT holds NET_CAND_VERIFY_S, or
 *          auto-reverts after NET_CAND_VERIFY_TIMEOUT_S / MAX_TRIES boots)
 *   good   last record proven healthy
 *
 * plus a cross-boot "cand_tries" counter. Resolution is PER FIELD: effective
 * value = candidate ?? good ?? compile-time default (config.h/secrets.h), so
 * a record may override just the broker and leave OTA on the default.
 *
 * Apply semantics (the critical split — see the migration plan):
 *   broker fields (mqtt_uri/mqtt_auth/mqtt_user/mqtt_pass) → staged as
 *     candidate, reboot required, verify ladder guards the flip;
 *   everything else (ota_url, relay_url, stream_url, relay_tok, enroll_url)
 *     → merged straight into the good record, read through at each use site,
 *     always correctable over MQTT.
 *
 * mqtt_uri accepts mqtt:// (plaintext, legacy broker) and mqtts:// (TLS;
 * broker cert verified via esp_crt_bundle — public CA e.g. Let's Encrypt —
 * unless the bench-only CHYTRA_BUDKA_TLS_INSECURE build skips it).
 * mqtt_auth=mtls/both present the enrollment leaf as the client identity
 * when it carries the clientAuth EKU (see mqtt.c / tls_store). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_STORE_URI_CAP  128
#define NET_STORE_CRED_CAP 64
#define NET_STORE_TOK_CAP  96

typedef enum {
    NET_AUTH_USERPASS = 0,
    NET_AUTH_MTLS     = 1,
    NET_AUTH_BOTH     = 2,
} net_auth_t;

typedef enum {
    NET_CFG_DEFAULT = 0,  /* compile-time config.h/secrets.h floor */
    NET_CFG_GOOD,         /* known-good NVS record */
    NET_CFG_CANDIDATE,    /* candidate pending verification */
} net_cfg_src_t;

typedef struct {
    char       mqtt_uri[NET_STORE_URI_CAP];   /* "mqtt://host:1883" */
    net_auth_t mqtt_auth;
    char       mqtt_user[NET_STORE_CRED_CAP];
    char       mqtt_pass[NET_STORE_CRED_CAP];
    char       ota_url[NET_STORE_URI_CAP];
    char       relay_url[NET_STORE_URI_CAP];  /* "" = compile RELAY_* */
    char       stream_url[NET_STORE_URI_CAP]; /* "" = push video unconfigured */
    char       relay_tok[NET_STORE_TOK_CAP];  /* "" = compile RELAY_AUTH */
    char       enroll_url[NET_STORE_URI_CAP]; /* "" = legacy MQTT enrollment */
} net_cfg_t;

/* Idempotent init. ALSO enforces the cross-boot candidate backstop: bumps
 * cand_tries when a candidate is pending and auto-reverts it once tries
 * exceed CONFIG_CHYTRA_BUDKA_NET_CAND_MAX_TRIES — call once, early, BEFORE
 * mqtt_init(). */
esp_err_t net_store_init(void);

/* Resolve the effective record THIS boot (candidate → good → default,
 * per field). Always succeeds; *src (may be NULL) reports the tier that won
 * for the BROKER fields. */
esp_err_t net_store_get_effective(net_cfg_t *out, net_cfg_src_t *src);

/* Stage broker-affecting fields (merged over the good record) as the
 * candidate. `json` is the flat {"mqtt_uri":...,...} object. Returns
 * ESP_ERR_NOT_SUPPORTED for mqtts/mTLS on this build, ESP_ERR_INVALID_ARG
 * on validation failure — nothing staged; callers must not reboot then. */
esp_err_t net_store_set_candidate_json(const char *json);

/* Merge live (non-broker) fields straight into the good record. Broker
 * fields present in `json` are ignored with a warning. */
esp_err_t net_store_set_live_json(const char *json);

esp_err_t net_store_promote_candidate(void);
esp_err_t net_store_revert_candidate(void);
bool      net_store_has_candidate(void);
bool      net_store_has_known_good(void);

/* Erase the whole namespace → compile defaults (cmd/endpoint {"clear":true}
 * and factory reset). */
esp_err_t net_store_erase(void);

const char *net_store_src_str(net_cfg_src_t s);
const char *net_store_auth_str(net_auth_t a);

#ifdef __cplusplus
}
#endif
