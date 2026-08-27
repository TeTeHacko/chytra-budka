/* tls_enroll.h — ECDSA P-256 keygen + X.509 CSR builder for the
 * MQTT-based per-device cert enrollment (HTTPS.md "Enrollment
 * protocol"). The MQTT publish/wait/persist flow lives elsewhere
 * (tls_enroll_mqtt.c, future); this file is the pure-crypto core
 * that's amenable to native testing on a host with mbedTLS 3.x.
 *
 * mbedtls_pk_context owns the EC keypair through its lifetime;
 * tls_enroll_keypair_free() releases it. CSR PEM is written into a
 * caller-provided buffer (no malloc inside) — typical CSR size on
 * P-256 with our SAN set is ~600 B, allocate 1024 B for headroom.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mbedtls/pk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mbedtls_pk_context pk;
    bool ready;
} tls_enroll_keypair_t;

/* Subject + SAN inputs to the CSR builder. All pointers must remain
 * valid until tls_enroll_build_csr() returns. SAN fields may be NULL
 * to skip that entry. */
typedef struct {
    const char *cn;             /* required, e.g. "cb-ex01.lan" */
    const char *san_dns_fqdn;   /* same as cn, also added to SAN — optional */
    const char *san_dns_short;  /* "cb-ex01" (no domain) — optional */
    const char *san_dns_mdns;   /* "cb-ex01.local" — optional */
    const char *san_ip_str;     /* dotted-quad IPv4 string — optional */
} tls_enroll_csr_subject_t;

/* Generate a fresh ECDSA P-256 keypair into `kp`. The keypair lives
 * in kp->pk until tls_enroll_keypair_free() is called. Uses
 * mbedtls_ctr_drbg seeded from the platform's entropy source
 * (esp_random TRNG on ESP32-S3; /dev/urandom on Linux host tests).
 *
 * ~3-5 s on first boot — the underlying mbedtls_ecp_gen_keypair() is
 * CPU-bound but unavoidable. Subsequent enrollments reuse the cached
 * keypair from NVS via tls_enroll_keypair_load(). */
esp_err_t tls_enroll_generate_keypair(tls_enroll_keypair_t *kp);

/* Export the keypair as SEC1 DER for NVS storage. The DER form is
 * ~100 B for P-256 (vs ~250 B PEM). Pass a 256-byte buffer; the
 * actual length goes into *out_len. */
esp_err_t tls_enroll_keypair_export_der(const tls_enroll_keypair_t *kp,
                                         uint8_t *out_der, size_t out_sz,
                                         size_t *out_len);

/* Load a keypair from previously-exported SEC1 DER (i.e. from NVS).
 * The DER is parsed and stored into kp->pk. */
esp_err_t tls_enroll_keypair_load_der(tls_enroll_keypair_t *kp,
                                       const uint8_t *der, size_t der_len);

/* Free the keypair context. Safe to call on a zero-init or already-
 * freed kp (no-op). After this call kp->ready is false. */
void tls_enroll_keypair_free(tls_enroll_keypair_t *kp);

/* Build a PEM-encoded X.509 CSR from `kp` + `subj`, hash SHA-256,
 * signed with the keypair's private key. Writes NUL-terminated PEM
 * into out_pem; *out_len receives the byte length (not including the
 * NUL). Typical output: ~600 B. The CSR includes a subjectAltName
 * extension built from the non-NULL fields in subj. */
esp_err_t tls_enroll_build_csr(const tls_enroll_keypair_t *kp,
                                const tls_enroll_csr_subject_t *subj,
                                char *out_pem, size_t out_sz,
                                size_t *out_len);

/* Validate a signer-returned cert PEM against the firmware-embedded
 * sub-CA, verify the cert's public key matches our keypair (so a
 * forged-response MITM can't slip in somebody else's cert), then
 * persist key + cert + expiry + canonical SAN fingerprint into the
 * tls_store NVS namespace.
 *
 * On success kp is consumed — the function calls tls_enroll_keypair_free
 * internally so the caller can drop the variable. On failure kp is
 * left intact (caller decides whether to retry or abandon).
 *
 * `current_device_id` / `current_domain` / `current_ip` feed the
 * tls_store_compute_san_fp helper — what we hash for the staleness
 * check must match what the signer put in the cert's SAN. The signer
 * does the same canonicalisation on its side from the CSR's SAN
 * extension, so as long as device + signer agree on the canonical
 * form the fingerprint round-trips.
 *
 * Returns ESP_OK on success; on failure the NVS store stays untouched
 * (atomic save guarantees that). */
esp_err_t tls_enroll_validate_and_persist(tls_enroll_keypair_t *kp,
                                           const uint8_t *cert_pem,
                                           size_t cert_pem_len,
                                           const char *current_device_id,
                                           const char *current_domain,
                                           const char *current_ip_str);

/* End-to-end enrollment: generate keypair, build CSR for the current
 * device identity (device_id + DHCP-discovered domain + WiFi IP),
 * submit it to the signer, validate the response chain + pubkey match,
 * persist into NVS.
 *
 * Transport is picked from net_store: when the effective enroll_url is
 * set the CSR goes as an HTTPS POST to the manager (TOFU — a 202
 * "pending approval" response returns ESP_ERR_NOT_FINISHED and persists
 * the keypair so the retry re-POSTs the SAME CSR; approval is pinned to
 * its pubkey); otherwise the legacy MQTT cmd/enroll round-trip runs
 * (requires MQTT connected → ESP_ERR_INVALID_STATE if not).
 *
 * timeout_ms gates the signer round-trip. Returns ESP_OK on success —
 * the next boot will load the persisted cert. Progress is mirrored to
 * the retained <id>/state/enroll topic (requested/pending/issued/
 * denied/failed) whenever MQTT is up. */
esp_err_t tls_enroll_run(uint32_t timeout_ms);

/* Pointer to the firmware-embedded issuing sub-CA PEM (budka_sub_ca.pem,
 * EMBED_TXTFILES). NUL-terminated; *out_len (may be NULL) gets the byte length
 * INCLUDING the trailing NUL — exactly what esp-tls / mbedtls_x509_crt_parse
 * want for a PEM buffer. Used as the broker trust anchor for mqtts+mTLS
 * (mqtt.c): the internal broker presents a sub-CA-signed leaf, so the device
 * pins its OWN sub-CA rather than the public CA bundle. Only defined in the
 * firmware build (the embedded symbols don't exist in the native test link). */
const char *tls_enroll_embedded_subca_pem(size_t *out_len);

/* HTTPS enrollment transport: POST the CSR PEM to `url` (the manager's
 * /api/v1/enroll). Server trust = esp_crt_bundle (public CA) unless the
 * bench-only CHYTRA_BUDKA_TLS_INSECURE build skips verification.
 *   ESP_OK               — 200, cert PEM (leaf + chain) in cert_buf,
 *                          NUL-terminated, *out_len set (excl. NUL)
 *   ESP_ERR_NOT_FINISHED — 202 TOFU-pending / 429 rate-limited: re-POST
 *                          the SAME CSR later
 *   ESP_ERR_NOT_ALLOWED  — 403 operator denied
 *   ESP_FAIL             — transport error / 4xx validation / 5xx */
esp_err_t tls_enroll_https_request(const char *url,
                                   const char *csr_pem, size_t csr_len,
                                   uint8_t *cert_buf, size_t cert_buf_sz,
                                   size_t *out_len);

/* True when the effective net_store config has an enroll_url — i.e. the
 * HTTPS transport (which needs only IP + SNTP, not MQTT) would be used. */
bool tls_enroll_https_configured(void);

/* True when the effective config wants mTLS toward the broker
 * (mqtt_auth=mtls/both), HTTPS enrollment is configured, and the stored
 * cert lacks the clientAuth EKU — i.e. a legacy serverAuth-only leaf
 * must be reissued before the mTLS rung can carry the session. Gated on
 * enroll_url so boards still pointed at the legacy MQTT signer (which
 * only issues serverAuth leaves) can't re-enroll in a loop. */
bool tls_enroll_needs_client_auth(void);

/* Operator-forced re-enrollment (cmd/cert {"renew":true}): bypasses the
 * boot quick-path's "cert matches env" skip once and runs the async
 * enroll pipeline (same as tls_enroll_retry_async, reboots on success).
 * This is the field-usable migration step that gets a clientAuth leaf
 * BEFORE the broker candidate is flipped to mtls. */
esp_err_t tls_enroll_force_async(uint32_t timeout_ms);

/* Boot-time gate: ensure NVS holds a cert whose SAN fingerprint matches
 * the current environment (device_id + DHCP-discovered domain + WiFi
 * IP). The function compares stored vs current fingerprint and skips
 * enrollment when they match — no MQTT touch, no signer round-trip, no
 * wait. When they DON'T match (fresh device, IP renumber, domain
 * change, factory reset) it waits up to 15 s for MQTT + SNTP (cert
 * chain verify needs real wall clock — without SNTP the leaf appears
 * "not yet valid" and verify fails) then calls tls_enroll_run.
 *
 * Returns ESP_OK on the no-op path AND on a successful enrollment.
 * Returns ESP_ERR_INVALID_STATE when MQTT/SNTP didn't come up in time
 * (operator can retry via /debug/tls_enroll, or next boot tries again).
 * Returns whatever tls_enroll_run returned on enrollment failure.
 *
 * Caller is expected to invoke this AFTER mqtt_init() but BEFORE
 * http_server_start() so the HTTP server picks up the freshly
 * persisted cert and comes up on HTTPS without a reboot. */
esp_err_t tls_boot_enroll_if_needed(uint32_t enroll_timeout_ms);

/* Deferred-retry enrollment, non-blocking. When tls_boot_enroll_if_needed
 * deferred (MQTT/SNTP weren't up inside its boot window — common on a weak
 * link where MQTT takes a minute+), the main loop calls this once MQTT+SNTP
 * are actually up and there's still no cert. Spawns the enroll pipeline on
 * its own task and reboots on success so the clean boot serves HTTPS; on
 * failure the task just exits so the caller can retry on a cooldown. Returns
 * ESP_OK once the task is spawned, ESP_ERR_NO_MEM if it couldn't be. Does NOT
 * block the caller (so it's safe to call from the watchdog-fed main loop). */
esp_err_t tls_enroll_retry_async(uint32_t timeout_ms);

/* True when a cert exists and is within the proactive-renewal window of its
 * notAfter (and the clock is SNTP-synced). The main loop ORs this into the
 * deferred-enroll trigger so an aging cert is renewed before it expires —
 * without it the board served the cert until notAfter and then lost HTTPS
 * permanently (there was no renewal path at all). Returns false on an unsynced
 * clock or when there's no cert. */
bool tls_enroll_cert_due_for_renewal(void);

/* True when a cert exists, we currently HAVE an IP, and the cert's stored SAN
 * fingerprint no longer matches the current device id + domain + IP — i.e. the
 * environment genuinely renumbered and the cert needs reissuing. Returns false
 * when there's no IP yet (so a check that ran before GOT_IP can't raise a
 * spurious "drift"). The main loop ORs this into the enroll trigger so a real
 * IP/domain change re-enrolls even on a weak link where the boot-path check
 * had no IP and deferred. */
bool tls_enroll_san_drifted(void);

#ifdef __cplusplus
}
#endif
