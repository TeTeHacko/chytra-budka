/* tls_store.h — NVS-backed persistence for the device's TLS keypair +
 * cert + expiry + SAN fingerprint. Namespace "tls" in the default NVS
 * partition. HTTPS.md "NVS storage layout" is the design reference.
 *
 * The store either holds a complete record (all four keys present and
 * coherent) or nothing — tls_store_save() commits all four in one
 * nvs_commit so a power loss mid-write can't leave a torn record. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer caps — the actual blob lengths are stored alongside the
 * bytes. SEC1 DER for EC P-256 fits in ~120 B; the cert payload is a
 * NUL-terminated PEM bundle (leaf cert followed by issuing sub-CA
 * cert, each in their own -----BEGIN/END CERTIFICATE----- block) so
 * the TLS handshake can send the full chain. Leaf PEM ~700 B + sub-CA
 * PEM ~1400 B + NUL ≈ 2.1 KB; cap at 2400 B with headroom for a
 * future schema bump. */
#define TLS_STORE_KEY_DER_MAX    256
#define TLS_STORE_CERT_PEM_MAX   2400
#define TLS_STORE_SAN_FP_LEN     32     /* SHA-256 of canonical SAN list */

/* On-disk format version — bumped whenever the bytes of any field
 * change meaning (not just buffer sizing). tls_store_has_cert() rejects
 * records carrying a smaller version so a firmware that knows about
 * v2 (leaf+sub-CA PEM bundle) doesn't try to interpret a v1 record
 * (leaf-only DER) as if it were the new format. Version bump triggers
 * a one-shot re-enrollment on the boot path — zero operator touch.
 *
 *   v1 — leaf X.509 DER only (legacy, never sent in chain)
 *   v2 — leaf PEM + sub-CA PEM bundle (chain-capable, but cbd-enroll
 *        omitted Authority Key Identifier — Python ssl rejects with
 *        "Missing Authority Key Identifier")
 *   v3 — leaf carries SubjectKeyIdentifier + AuthorityKeyIdentifier
 *        extensions (RFC 5280 §4.2.1.1; required by modern Python
 *        ssl module / HA 2024.12+) */
#define CB_TLS_FORMAT_VER        3

typedef struct {
    uint8_t  key_der[TLS_STORE_KEY_DER_MAX];
    size_t   key_len;
    /* PEM bundle: NUL-terminated, two `-----BEGIN CERTIFICATE-----`
     * blocks (leaf first, sub-CA second). cert_len INCLUDES the
     * trailing NUL so the bytes are ready to hand directly to
     * mbedtls_x509_crt_parse / esp_https_server. */
    uint8_t  cert_pem[TLS_STORE_CERT_PEM_MAX];
    size_t   cert_len;
    int64_t  expiry_unix;                    /* notAfter, unix epoch */
    uint8_t  san_fp[TLS_STORE_SAN_FP_LEN];   /* see tls_store_compute_san_fp */
} tls_store_blob_t;

/* Idempotent — opens the "tls" NVS namespace if not already. Returns
 * ESP_OK even when the namespace is empty (a fresh device); callers
 * use tls_store_has_cert() to distinguish. */
esp_err_t tls_store_init(void);

/* True when all four NVS keys are present. Doesn't validate cert
 * contents or expiry — just storage presence. */
bool tls_store_has_cert(void);

/* Read all four fields into `out`. Returns ESP_ERR_NVS_NOT_FOUND if
 * the store is empty; partial-write recovery isn't a thing (save is
 * atomic), so any present key implies all four are. */
esp_err_t tls_store_load(tls_store_blob_t *out);

/* Atomic write of all four fields. On any internal failure the
 * namespace is left untouched (we open in a transactional pattern —
 * write all keys, then commit once). */
esp_err_t tls_store_save(const tls_store_blob_t *in);

/* Erase all four keys. Used by the env-staleness path (IP or domain
 * mismatch in SAN → drop the cert and re-enroll). */
esp_err_t tls_store_erase(void);

/* True when the stored leaf carries the TLS Web Client Authentication EKU —
 * i.e. it is usable as the MQTT mTLS client identity. Legacy leaves issued
 * by the MQTT signer are serverAuth-only and return false, which is the
 * re-enroll trigger once the effective config asks for mtls. False when no
 * cert is stored or it doesn't parse. */
bool tls_store_cert_has_client_auth(void);

/* Quick-path getters that skip loading the full blob. Used by the
 * boot-time staleness check to avoid copying ~1 KB just to compare
 * 32 bytes / 8 bytes. */
esp_err_t tls_store_get_expiry(int64_t *out_unix);
esp_err_t tls_store_get_san_fp(uint8_t out_fp[TLS_STORE_SAN_FP_LEN]);

/* Pending-enrollment keypair (SEC1 DER), separate from the committed
 * record above. The HTTPS enrollment flow needs it: the manager's TOFU
 * approval pins the pubkey of the PENDING CSR, so every retry of a
 * 202-pending enrollment must re-POST a CSR built from the SAME keypair
 * — a fresh key per attempt would look like a re-key and bounce back to
 * the approval queue forever. Saved on the first 202, loaded on retries,
 * cleared once the cert is issued+persisted. tls_store_erase() (factory
 * reset) wipes it too, which is exactly the manager's re-key semantics. */
esp_err_t tls_store_save_pending_key(const uint8_t *der, size_t len);
/* ESP_ERR_NVS_NOT_FOUND when no pending key is stored. */
esp_err_t tls_store_load_pending_key(uint8_t *out_der, size_t cap,
                                     size_t *out_len);
esp_err_t tls_store_clear_pending_key(void);
/* True while a TOFU enrollment is awaiting operator approval — the main
 * loop uses it to keep the deferred-enroll retry alive until the manager
 * resolves the request (issue → cleared on persist; deny → cleared on
 * the 403). */
bool tls_store_has_pending_key(void);

/* Compute the canonical SAN fingerprint for the current device
 * environment. The fingerprint is the SHA-256 of a sorted-and-joined
 * list of "type:value" lines, one per SAN entry:
 *
 *   dns:<short>\n
 *   dns:<short>.<domain>\n
 *   dns:<short>.local\n
 *   ip:<dotted_quad>\n
 *
 * Lines are sorted lexicographically before hashing so the result is
 * independent of insertion order. The same canonicalisation runs
 * device-side (here) and signer-side (Python helper in enroll.py)
 * — keep them in lock-step.
 *
 * `domain` may be empty (when CB_DOMAIN_FALLBACK is "" AND DHCP didn't
 * supply one), in which case the FQDN-style line is omitted.
 * `ip_str` may be NULL/empty (pre-WiFi-up call), in which case the IP
 * line is omitted. */
void tls_store_compute_san_fp(const char *device_id,
                               const char *domain,
                               const char *ip_str,
                               uint8_t out_fp[TLS_STORE_SAN_FP_LEN]);

#ifdef __cplusplus
}
#endif
