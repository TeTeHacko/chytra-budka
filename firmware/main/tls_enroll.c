/* tls_enroll.c — see tls_enroll.h. */

#include "tls_enroll.h"
#include "config.h"
#include "tls_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
/* mbedtls 4.x (IDF v6.0.1 ships 4.0.0, master/v6.1.0 ships 4.1.0) builds
 * key material through PSA Crypto. We generate the EC keypair with
 * psa_generate_key() and wrap it into a transparent PK context via
 * mbedtls_pk_copy_from_psa() — both are public API on 4.0 AND 4.1, so we
 * need no private mbedtls headers and no MBEDTLS_ALLOW_PRIVATE_ACCESS.
 * (4.1.0 removed the legacy mbedtls_pk_setup() / mbedtls_pk_ec() accessors
 * that the old 3.x-style keygen relied on — see the v6.1 build break.)
 * PSA owns its RNG internally (HW-backed on ESP32-S3), so there is no
 * f_rng callback to supply; the remaining legacy calls we use
 * (mbedtls_pk_parse_key, mbedtls_x509write_csr_pem) already dropped their
 * f_rng/p_rng tail args in 4.0. */
#include "psa/crypto.h"
#include "mbedtls/error.h"
#include "mbedtls/oid.h"
#include "mbedtls/pem.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"

/* tls_enroll_run() — wires the keygen + CSR + MQTT round-trip + persist
 * pipeline together using runtime identity (device_id, DHCP domain,
 * WiFi IP). Only compiled in ESP-IDF builds; the native test harness
 * links tls_enroll.c standalone for the pure-crypto path. */
#ifdef ESP_PLATFORM
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "esp_system.h"   /* esp_restart */
#  include "esp_http_client.h"
#  include "esp_crt_bundle.h"
#  include "mqtt.h"
#  include "device_id.h"
#  include "net_store.h"    /* enroll_url read-through (HTTPS transport pick) */
#  include "wifi_mgr.h"
#  include "diag.h"         /* diag_mark_ota_valid (commit pending OTA pre-reboot) */
#  define CB_TLS_ENROLL_RUN_AVAILABLE 1

/* Re-enroll when the leaf has less than this left before notAfter. The
 * signer issues ~90 d leaves (HTTPS.md), so a 30 d window yields a rolling
 * ~60 d renewal cadence — long before expiry strands HTTPS. */
#  define CB_TLS_RENEW_WINDOW_S (30LL * 24 * 3600)
#endif

#ifdef ESP_PLATFORM
/* Sub-CA PEM embedded by EMBED_TXTFILES (main/CMakeLists.txt). The
 * linker emits a NUL-terminated byte array and a length symbol; the
 * NUL terminator lets mbedtls_x509_crt_parse consume the buffer as a
 * C string. The "pem_end" symbol is one past the trailing NUL; we
 * use its difference from start as length.
 *
 * Only declared in the firmware build — the native test harness
 * compiles this file without the linker-provided symbols (no
 * EMBED_TXTFILES). validate_and_persist (the only consumer) is
 * gated below by the same ESP_PLATFORM. */
extern const uint8_t budka_sub_ca_pem_start[] __asm__("_binary_budka_sub_ca_pem_start");
extern const uint8_t budka_sub_ca_pem_end[]   __asm__("_binary_budka_sub_ca_pem_end");
#endif

static const char *TAG = "tls_enroll";

#ifdef ESP_PLATFORM
const char *tls_enroll_embedded_subca_pem(size_t *out_len) {
    if (out_len) {
        *out_len = (size_t)(budka_sub_ca_pem_end - budka_sub_ca_pem_start);
    }
    return (const char *)budka_sub_ca_pem_start;
}
#endif

esp_err_t tls_enroll_generate_keypair(tls_enroll_keypair_t *kp) {
    if (!kp) return ESP_ERR_INVALID_ARG;

    /* Idempotent on a fresh kp: re-init is fine. If caller passed an
     * already-populated kp, we leak the old context — refuse loudly
     * rather than silently overwriting. */
    if (kp->ready) {
        ESP_LOGE(TAG, "generate_keypair: kp already populated; "
                      "call tls_enroll_keypair_free first");
        return ESP_ERR_INVALID_STATE;
    }

    /* PSA must be initialised before psa_generate_key. Idempotent — IDF's
     * mbedtls port normally inits it during startup; calling again is a
     * cheap no-op that returns PSA_SUCCESS. */
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)st);
        return ESP_FAIL;
    }

    /* Curve choice: secp256r1 (NIST P-256). 256-bit security level, widely
     * supported, has hardware MPI acceleration on ESP32-S3, matches the
     * sub-CA's curve. Don't use P-384 — slower with no meaningful security
     * benefit at our threat model.
     *
     * Usage flags: SIGN_HASH so the same key can sign the CSR; EXPORT so
     * pk_copy_from_psa can lift the private key material into a transparent
     * PK context (which tls_enroll_keypair_export_der then serialises to
     * NVS via mbedtls_pk_write_key_der). */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    st = psa_generate_key(&attr, &key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key: %d", (int)st);
        return ESP_FAIL;
    }

    /* Wrap the PSA key into a transparent PK context — copy_from_psa
     * exports + re-imports the key material, so kp->pk owns a standalone
     * copy. Destroy the PSA key slot right after: kp->pk no longer needs
     * it and we don't want a dangling handle in the PSA key store. */
    mbedtls_pk_init(&kp->pk);
    int ret = mbedtls_pk_copy_from_psa(key_id, &kp->pk);
    psa_destroy_key(key_id);
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_copy_from_psa: -0x%04x", -ret);
        mbedtls_pk_free(&kp->pk);
        return ESP_FAIL;
    }

    kp->ready = true;
    ESP_LOGI(TAG, "generated EC P-256 keypair (PSA)");
    return ESP_OK;
}

esp_err_t tls_enroll_keypair_export_der(const tls_enroll_keypair_t *kp,
                                         uint8_t *out_der, size_t out_sz,
                                         size_t *out_len) {
    if (!kp || !kp->ready || !out_der || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* mbedtls_pk_write_key_der writes BACKWARDS into the buffer (returns
     * length written, data is at the *end* of the buffer). Standard
     * mbedtls quirk — we copy to the front for storage convenience. */
    uint8_t tmp[256];  /* P-256 SEC1 DER fits comfortably in ~120 B */
    int ret = mbedtls_pk_write_key_der(
        (mbedtls_pk_context *)&kp->pk, tmp, sizeof(tmp));
    if (ret < 0) {
        ESP_LOGE(TAG, "pk_write_key_der: -0x%04x", -ret);
        return ESP_FAIL;
    }
    size_t n = (size_t)ret;
    if (n > out_sz) {
        ESP_LOGE(TAG, "key DER (%zu B) doesn't fit in out_sz=%zu", n, out_sz);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_der, tmp + (sizeof(tmp) - n), n);
    *out_len = n;
    return ESP_OK;
}

esp_err_t tls_enroll_keypair_load_der(tls_enroll_keypair_t *kp,
                                       const uint8_t *der, size_t der_len) {
    if (!kp || !der || der_len == 0) return ESP_ERR_INVALID_ARG;
    if (kp->ready) {
        ESP_LOGE(TAG, "load_der: kp already populated");
        return ESP_ERR_INVALID_STATE;
    }

    /* mbedtls 4.0 dropped the f_rng/p_rng args — PSA Crypto handles
     * blinding internally for parse_key now. 7-arg form (3.x) →
     * 5-arg here. */
    mbedtls_pk_init(&kp->pk);
    int ret = mbedtls_pk_parse_key(
        &kp->pk, der, der_len,
        /*pwd*/ NULL, /*pwdlen*/ 0);

    if (ret != 0) {
        ESP_LOGE(TAG, "pk_parse_key: -0x%04x", -ret);
        mbedtls_pk_free(&kp->pk);
        return ESP_FAIL;
    }
    kp->ready = true;
    return ESP_OK;
}

void tls_enroll_keypair_free(tls_enroll_keypair_t *kp) {
    if (!kp) return;
    if (kp->ready) {
        mbedtls_pk_free(&kp->pk);
        kp->ready = false;
    }
}

/* Parse a dotted-quad IPv4 string into 4 bytes. Returns 0 on success,
 * -1 on parse error. Local impl to avoid pulling in lwIP inet_aton in
 * the native test build. */
static int parse_ipv4(const char *s, uint8_t out[4]) {
    if (!s) return -1;
    unsigned a, b, c, d;
    int matched = sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
    if (matched != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

/* SAN entries are emitted as a chained mbedtls_x509_san_list. Each
 * node carries a name type (DNS / IP / …) and a value pointer; the
 * list is terminated by node.next = NULL. mbedtls walks it and emits
 * one ASN.1 SAN extension covering all entries. The strings are
 * referenced by pointer — they MUST stay alive until the csr write
 * call returns. */
typedef struct san_arena {
    mbedtls_x509_san_list nodes[5];  /* fqdn + short + mdns + ip + slack */
    uint8_t ip_bytes[4];
    int n_used;
} san_arena_t;

static void san_arena_init(san_arena_t *a) {
    memset(a, 0, sizeof(*a));
}

static void san_arena_push_dns(san_arena_t *a, const char *s) {
    if (!s || a->n_used >= (int)(sizeof(a->nodes) / sizeof(a->nodes[0]))) return;
    mbedtls_x509_san_list *n = &a->nodes[a->n_used];
    n->node.type = MBEDTLS_X509_SAN_DNS_NAME;
    n->node.san.unstructured_name.p = (unsigned char *)s;
    n->node.san.unstructured_name.len = strlen(s);
    /* Chain to the previous node (if any). */
    if (a->n_used > 0) a->nodes[a->n_used - 1].next = n;
    n->next = NULL;
    a->n_used++;
}

static void san_arena_push_ip(san_arena_t *a, const uint8_t bytes[4]) {
    if (a->n_used >= (int)(sizeof(a->nodes) / sizeof(a->nodes[0]))) return;
    memcpy(a->ip_bytes, bytes, 4);
    mbedtls_x509_san_list *n = &a->nodes[a->n_used];
    n->node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    n->node.san.unstructured_name.p = a->ip_bytes;
    n->node.san.unstructured_name.len = 4;
    if (a->n_used > 0) a->nodes[a->n_used - 1].next = n;
    n->next = NULL;
    a->n_used++;
}

esp_err_t tls_enroll_build_csr(const tls_enroll_keypair_t *kp,
                                const tls_enroll_csr_subject_t *subj,
                                char *out_pem, size_t out_sz,
                                size_t *out_len) {
    if (!kp || !kp->ready || !subj || !subj->cn || !out_pem || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);

    /* Subject DN — just CN. Add O / OU here if signer policy ever
     * requires them; today our sub-CA only checks CN against the
     * topic prefix. */
    char dn[128];
    int dn_n = snprintf(dn, sizeof(dn), "CN=%s", subj->cn);
    if (dn_n <= 0 || dn_n >= (int)sizeof(dn)) {
        mbedtls_x509write_csr_free(&csr);
        return ESP_ERR_INVALID_ARG;
    }
    int ret = mbedtls_x509write_csr_set_subject_name(&csr, dn);
    if (ret != 0) {
        ESP_LOGE(TAG, "csr_set_subject_name: -0x%04x", -ret);
        goto fail;
    }

    mbedtls_x509write_csr_set_key(&csr, (mbedtls_pk_context *)&kp->pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);

    /* Build SAN list — every non-NULL string in subj becomes one
     * subjectAltName entry. The order matters only for human reading;
     * relying-party libraries treat the SAN as an unordered set. */
    san_arena_t arena;
    san_arena_init(&arena);
    san_arena_push_dns(&arena, subj->san_dns_fqdn);
    san_arena_push_dns(&arena, subj->san_dns_short);
    san_arena_push_dns(&arena, subj->san_dns_mdns);
    if (subj->san_ip_str) {
        uint8_t ip4[4];
        if (parse_ipv4(subj->san_ip_str, ip4) == 0) {
            san_arena_push_ip(&arena, ip4);
        } else {
            ESP_LOGW(TAG, "san_ip_str='%s' didn't parse as IPv4 — skipping",
                     subj->san_ip_str);
        }
    }

    if (arena.n_used > 0) {
        ret = mbedtls_x509write_csr_set_subject_alternative_name(
            &csr, &arena.nodes[0]);
        if (ret != 0) {
            ESP_LOGE(TAG, "csr_set_subject_alternative_name: -0x%04x", -ret);
            goto fail;
        }
    }

    /* Write PEM into the caller's buffer. mbedtls_x509write_csr_pem
     * returns 0 on success and fills out_pem with NUL-terminated PEM.
     * mbedtls 4.0 dropped the f_rng/p_rng args (PSA handles randomness
     * for the signature internally). */
    memset(out_pem, 0, out_sz);
    ret = mbedtls_x509write_csr_pem(
        &csr, (unsigned char *)out_pem, out_sz);
    if (ret != 0) {
        ESP_LOGE(TAG, "csr_pem: -0x%04x", -ret);
        goto fail;
    }
    *out_len = strlen(out_pem);

    mbedtls_x509write_csr_free(&csr);
    return ESP_OK;

fail:
    mbedtls_x509write_csr_free(&csr);
    return ESP_FAIL;
}

#ifdef ESP_PLATFORM
/* ── validate cert chain + persist ──────────────────────────────────── */

/* Compare the public-key components of two mbedtls_pk_context's.
 * mbedtls_pk_check_pair compares private vs public via signature
 * roundtrip but we want pub-only comparison: signer returns a cert
 * carrying a pubkey, we extract our own pubkey from kp->pk, both
 * should encode to identical DER bytes if the signer signed THIS
 * device's CSR (not some other device's). Encoding to DER and
 * memcmp'ing is the simplest portable comparison. */
static bool pk_pubkey_matches(const mbedtls_pk_context *a,
                              const mbedtls_pk_context *b) {
    uint8_t pa[256], pb[256];
    int na = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)a, pa, sizeof(pa));
    int nb = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)b, pb, sizeof(pb));
    if (na <= 0 || nb <= 0 || na != nb) return false;
    /* write_pubkey_der writes BACKWARDS — data is at the tail. */
    return memcmp(pa + (sizeof(pa) - na), pb + (sizeof(pb) - nb), na) == 0;
}

/* Convert mbedtls_x509_time to unix epoch via mktime(). The fields
 * are 1-indexed months in EXIF / TIFF order; mktime() expects
 * 0-indexed months and (year - 1900). */
static int64_t x509_time_to_unix(const mbedtls_x509_time *t) {
    struct tm tm = {0};
    tm.tm_year = t->year - 1900;
    tm.tm_mon  = t->mon - 1;
    tm.tm_mday = t->day;
    tm.tm_hour = t->hour;
    tm.tm_min  = t->min;
    tm.tm_sec  = t->sec;
    /* X.509 timestamps are UTC. mktime() assumes local time. Use
     * timegm() to convert without TZ offset — POSIX-flavoured ESP-IDF
     * exposes it. */
    return (int64_t)timegm(&tm);
}

esp_err_t tls_enroll_validate_and_persist(tls_enroll_keypair_t *kp,
                                           const uint8_t *cert_pem,
                                           size_t cert_pem_len,
                                           const char *current_device_id,
                                           const char *current_domain,
                                           const char *current_ip_str) {
    if (!kp || !kp->ready || !cert_pem || cert_pem_len == 0 ||
        !current_device_id) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse the signer's response cert. mbedtls_x509_crt_parse handles
     * both PEM (when the input is NUL-terminated and starts with
     * "-----BEGIN") and DER. We pass +1 to length to include the
     * trailing NUL the caller should have appended; if they didn't,
     * mbedtls returns INVALID_FORMAT and we bail cleanly. */
    mbedtls_x509_crt leaf;
    mbedtls_x509_crt_init(&leaf);
    int ret = mbedtls_x509_crt_parse(&leaf, cert_pem, cert_pem_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "leaf parse: -0x%04x", -ret);
        mbedtls_x509_crt_free(&leaf);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse the embedded sub-CA chain. EMBED_TXTFILES guarantees the
     * trailing NUL; length passed to mbedtls_x509_crt_parse must
     * include it. */
    mbedtls_x509_crt sub_ca;
    mbedtls_x509_crt_init(&sub_ca);
    size_t sub_ca_len = budka_sub_ca_pem_end - budka_sub_ca_pem_start;
    ret = mbedtls_x509_crt_parse(&sub_ca, budka_sub_ca_pem_start, sub_ca_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "embedded sub-CA parse: -0x%04x — firmware build bug?",
                 -ret);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&sub_ca);
        return ESP_FAIL;
    }

    /* Chain verification — leaf must chain to embedded sub-CA. We
     * pass sub-CA as the trust anchor (root isn't needed device-side;
     * we only trust certs the sub-CA issued, never anything higher
     * in the chain — sub-CA was already signed by root at provisioning
     * time, that's not relitigated here). */
    uint32_t flags = 0;
    ret = mbedtls_x509_crt_verify(&leaf, &sub_ca, /*ca_crl*/ NULL,
                                   /*cn*/ NULL, &flags,
                                   /*f_vrfy*/ NULL, /*p_vrfy*/ NULL);
    if (ret != 0) {
        char flagstr[256];
        mbedtls_x509_crt_verify_info(flagstr, sizeof(flagstr), "  ", flags);
        ESP_LOGE(TAG, "chain verify failed: -0x%04x flags=0x%x\n%s",
                 -ret, (unsigned)flags, flagstr);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&sub_ca);
        return ESP_ERR_INVALID_ARG;
    }

    /* Pubkey match — leaf cert must carry OUR pubkey, not somebody
     * else's. This is the defence against an attacker who knows our
     * device id and tries to publish a "valid" sub-CA-signed cert
     * (with their pubkey) to our state/cert topic. They can't pass
     * this check without owning our private key. */
    if (!pk_pubkey_matches(&leaf.pk, &kp->pk)) {
        ESP_LOGE(TAG, "cert pubkey doesn't match our keypair — "
                      "REJECTING (possible forged-response attack?)");
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&sub_ca);
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract expiry. notAfter is what matters for renewal; notBefore
     * is informational. */
    int64_t expiry = x509_time_to_unix(&leaf.valid_to);

    /* Compute the canonical SAN fingerprint from the device's CURRENT
     * identity — not from the cert's SAN. The fingerprint is what
     * tls_check_environment will compare against after a reboot; if
     * IP / domain change between now and then, the comparison fails
     * and re-enrollment kicks in. */
    tls_store_blob_t blob = {0};
    esp_err_t e = tls_enroll_keypair_export_der(
        kp, blob.key_der, sizeof(blob.key_der), &blob.key_len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "export key DER for persistence: 0x%x", e);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&sub_ca);
        return e;
    }

    /* Build a PEM bundle "leaf || sub-CA" for the cert store. esp-https
     * server's TLS Certificate message walks mbedtls's crt linked list
     * (mbedtls_ssl_write_certificate / mbedtls_x509_crt_parse loops
     * over multiple -----BEGIN CERTIFICATE----- blocks → crt->next),
     * so once we hand it the bundle the client receives BOTH certs.
     * That lets relying parties (HA, openssl) trust just the root and
     * build the chain from what the server presents — no per-device
     * sub-CA trust import needed. */
    {
        const size_t cap = sizeof(blob.cert_pem);
        size_t leaf_pem_len = 0;
        int ret = mbedtls_pem_write_buffer(
            "-----BEGIN CERTIFICATE-----\n",
            "-----END CERTIFICATE-----\n",
            leaf.raw.p, leaf.raw.len,
            blob.cert_pem, cap, &leaf_pem_len);
        if (ret != 0) {
            ESP_LOGE(TAG, "pem_write_buffer(leaf): -0x%04x (need %zu B)",
                     -ret, leaf_pem_len);
            mbedtls_x509_crt_free(&leaf);
            mbedtls_x509_crt_free(&sub_ca);
            return ESP_ERR_NO_MEM;
        }
        /* mbedtls_pem_write_buffer NUL-terminates and returns len
         * INCLUDING the NUL — strip it so the sub-CA append lands
         * straight after the END CERTIFICATE line. */
        size_t leaf_pem_bytes = (leaf_pem_len > 0 && blob.cert_pem[leaf_pem_len - 1] == 0)
                                ? leaf_pem_len - 1 : leaf_pem_len;

        /* Embedded sub-CA is already NUL-terminated PEM. Append it
         * (including its trailing NUL) so the final bundle ends with
         * NUL — mbedtls's PEM parser needs that to detect end-of-input. */
        const size_t sub_ca_total = (size_t)(budka_sub_ca_pem_end - budka_sub_ca_pem_start);
        if (leaf_pem_bytes + sub_ca_total > cap) {
            ESP_LOGE(TAG, "PEM bundle (%zu leaf + %zu sub-CA) > storage cap (%zu)",
                     leaf_pem_bytes, sub_ca_total, cap);
            mbedtls_x509_crt_free(&leaf);
            mbedtls_x509_crt_free(&sub_ca);
            return ESP_ERR_NO_MEM;
        }
        memcpy(blob.cert_pem + leaf_pem_bytes,
               budka_sub_ca_pem_start, sub_ca_total);
        blob.cert_len = leaf_pem_bytes + sub_ca_total;  /* incl. final NUL */
    }
    blob.expiry_unix = expiry;
    tls_store_compute_san_fp(current_device_id, current_domain,
                              current_ip_str, blob.san_fp);

    e = tls_store_save(&blob);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tls_store_save: 0x%x", e);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&sub_ca);
        return e;
    }

    ESP_LOGI(TAG, "persisted cert: expiry=%lld key=%zu B bundle=%zu B (leaf+sub-CA PEM)",
             (long long)expiry, blob.key_len, blob.cert_len);

    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&sub_ca);
    tls_enroll_keypair_free(kp);
    return ESP_OK;
}
#endif /* ESP_PLATFORM */

#ifdef CB_TLS_ENROLL_RUN_AVAILABLE

/* CSR + cert response buffer sizes. Moved to heap (see tls_enroll_run)
 * because the httpd worker stack (8 KB by default) plus the mbedtls
 * X.509 + PSA keygen call chain already eats most of the frame — adding
 * 5 KB of stack locals here overflowed and panicked inside _lock_release
 * on the bench. Heap is plenty cheap for a one-shot enrollment. */
#define CB_ENROLL_CSR_BUF_SZ   2048
#define CB_ENROLL_CERT_BUF_SZ  3072

/* One-shot bypass of the boot quick-path's "cert matches env" skip —
 * armed by tls_enroll_force_async (cmd/cert {"renew":true}), consumed
 * by the next tls_boot_enroll_impl run. */
static volatile bool s_force_reenroll = false;

bool tls_enroll_https_configured(void) {
    net_cfg_t nc;
    if (net_store_get_effective(&nc, NULL) != ESP_OK) return false;
    return nc.enroll_url[0] != 0;
}

bool tls_enroll_needs_client_auth(void) {
    if (!tls_store_has_cert()) return false;  /* absent-cert path covers it */
    net_cfg_t nc;
    if (net_store_get_effective(&nc, NULL) != ESP_OK) return false;
    if (!nc.enroll_url[0]) return false;              /* legacy signer can't fix it */
    if (nc.mqtt_auth == NET_AUTH_USERPASS) return false;  /* config doesn't want mTLS */
    return !tls_store_cert_has_client_auth();
}

esp_err_t tls_enroll_https_request(const char *url,
                                   const char *csr_pem, size_t csr_len,
                                   uint8_t *cert_buf, size_t cert_buf_sz,
                                   size_t *out_len) {
    if (!url || !url[0] || !csr_pem || csr_len == 0 ||
        !cert_buf || cert_buf_sz < 2 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
#if !CONFIG_CHYTRA_BUDKA_TLS_INSECURE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
#if CONFIG_CHYTRA_BUDKA_TLS_INSECURE
    ESP_LOGW(TAG, "enroll POST: server cert verification DISABLED (bench build)");
#endif
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Content-Type", "application/x-pem-file");

    /* open/write/read (not perform) — we need the response BODY, and on
     * 200 it's the cert PEM, ~2.5 KB. */
    esp_err_t err = esp_http_client_open(client, (int)csr_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enroll POST: open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int w = esp_http_client_write(client, csr_pem, (int)csr_len);
    if (w != (int)csr_len) {
        ESP_LOGE(TAG, "enroll POST: short write (%d/%zu)", w, csr_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    (void)esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);

    size_t total = 0;
    while (total + 1 < cert_buf_sz) {
        int r = esp_http_client_read(client, (char *)cert_buf + total,
                                     (int)(cert_buf_sz - 1 - total));
        if (r <= 0) break;
        total += (size_t)r;
    }
    cert_buf[total] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    switch (code) {
    case 200:
        if (total == 0 || total + 1 >= cert_buf_sz) {
            /* Empty body or buffer-filling response — either way not a
             * cert we can trust to be complete. */
            ESP_LOGE(TAG, "enroll POST: 200 but body %zu B (cap %zu)",
                     total, cert_buf_sz);
            return ESP_FAIL;
        }
        *out_len = total;
        return ESP_OK;
    case 202:
        ESP_LOGI(TAG, "enroll POST: 202 pending operator approval");
        return ESP_ERR_NOT_FINISHED;
    case 429:
        ESP_LOGW(TAG, "enroll POST: 429 rate-limited — retrying later");
        return ESP_ERR_NOT_FINISHED;
    case 403:
        ESP_LOGE(TAG, "enroll POST: 403 DENIED by operator: %.120s",
                 (const char *)cert_buf);
        return ESP_ERR_NOT_ALLOWED;
    default:
        ESP_LOGE(TAG, "enroll POST: HTTP %d: %.120s",
                 code, (const char *)cert_buf);
        return ESP_FAIL;
    }
}

esp_err_t tls_enroll_run(uint32_t timeout_ms) {
    const char *id     = device_id();
    const char *domain = wifi_mgr_get_domain();

    char ip[20] = {0};
    bool have_ip = wifi_mgr_get_ip_str(ip, sizeof(ip));

    /* Build the CN — prefer "<id>.<domain>" when DHCP supplied a domain,
     * fall back to the bare hostname otherwise. This is what the signer
     * will write into the cert's Subject; the SAN list is what relying
     * parties actually match on, but a meaningful CN helps debugging
     * with `openssl x509 -text`.
     *
     * Buffer sized for the worst case: device_id (20 chars) + '.' +
     * domain (up to 63 chars from DHCP option 15 buffer) + NUL = 85.
     * Round up to 128 to leave slack. snprintf truncation here would
     * silently desync the on-device SAN fingerprint from the cert
     * (signer-side OK, validate_and_persist fails) — fail-loud below
     * if it ever happens. */
    char fqdn[128];
    int fqdn_n;
    if (domain && domain[0]) {
        fqdn_n = snprintf(fqdn, sizeof(fqdn), "%s.%s", id, domain);
    } else {
        fqdn_n = snprintf(fqdn, sizeof(fqdn), "%s", id);
    }
    if (fqdn_n < 0 || (size_t)fqdn_n >= sizeof(fqdn)) {
        ESP_LOGE(TAG, "enroll_run: FQDN truncated (id=%s domain=%s)",
                 id, domain ? domain : "");
        return ESP_ERR_INVALID_ARG;
    }
    char mdns[64];
    snprintf(mdns, sizeof(mdns), "%s.local", id);

    char    *csr_pem  = calloc(1, CB_ENROLL_CSR_BUF_SZ);
    uint8_t *cert_pem = calloc(1, CB_ENROLL_CERT_BUF_SZ);
    if (!csr_pem || !cert_pem) {
        free(csr_pem); free(cert_pem);
        ESP_LOGE(TAG, "enroll_run: OOM allocating buffers");
        return ESP_ERR_NO_MEM;
    }

    net_cfg_t nc = {0};
    net_store_get_effective(&nc, NULL);
    const bool https = (nc.enroll_url[0] != 0);

    /* Keypair: on the HTTPS/TOFU transport a 202-pending retry must
     * re-POST a CSR carrying the SAME pubkey (approval pins it), so a
     * pending keypair persisted by an earlier attempt is reloaded here.
     * No pending key (or an unparsable one) → generate fresh. */
    tls_enroll_keypair_t kp = {0};
    esp_err_t e = ESP_FAIL;
    if (https) {
        uint8_t der[256];
        size_t der_len = 0;
        if (tls_store_load_pending_key(der, sizeof(der), &der_len) == ESP_OK) {
            e = tls_enroll_keypair_load_der(&kp, der, der_len);
            if (e == ESP_OK) {
                ESP_LOGI(TAG, "enroll_run: reusing pending-enroll keypair");
            } else {
                ESP_LOGW(TAG, "enroll_run: pending key unparsable — regenerating");
                tls_store_clear_pending_key();
            }
        }
    }
    if (e != ESP_OK) {
        e = tls_enroll_generate_keypair(&kp);
    }
    if (e != ESP_OK) {
        free(csr_pem); free(cert_pem);
        return e;
    }

    tls_enroll_csr_subject_t subj = {
        .cn            = fqdn,
        .san_dns_fqdn  = (domain && domain[0]) ? fqdn : NULL,
        .san_dns_short = id,
        .san_dns_mdns  = mdns,
        .san_ip_str    = have_ip ? ip : NULL,
    };

    size_t csr_len = 0;
    e = tls_enroll_build_csr(&kp, &subj, csr_pem, CB_ENROLL_CSR_BUF_SZ, &csr_len);
    if (e != ESP_OK) {
        tls_enroll_keypair_free(&kp);
        free(csr_pem); free(cert_pem);
        return e;
    }
    ESP_LOGI(TAG, "enroll_run: CSR ready (%zu B, cn=%s, transport=%s)",
             csr_len, fqdn, https ? "https" : "mqtt-legacy");
    mqtt_publish_enroll_state(https
        ? "{\"status\":\"requested\",\"transport\":\"https\"}"
        : "{\"status\":\"requested\",\"transport\":\"mqtt-legacy\"}");

    /* The cert PEM the signer returns is ~1.2 KB for a leaf with our
     * SAN set (~2.5 KB when the HTTPS manager appends the sub-CA).
     * CB_ENROLL_CERT_BUF_SZ gives headroom; the buffer is NUL-terminated
     * by both transports, hence the +1 passed to validate_and_persist
     * below. */
    size_t cert_len = 0;
    if (https) {
        e = tls_enroll_https_request(nc.enroll_url, csr_pem, csr_len,
                                     cert_pem, CB_ENROLL_CERT_BUF_SZ,
                                     &cert_len);
        if (e == ESP_ERR_NOT_FINISHED) {
            /* TOFU pending (or rate-limited): persist the keypair so the
             * retry re-POSTs the SAME CSR — approval pins its pubkey. */
            uint8_t der[256];
            size_t der_len = 0;
            if (tls_enroll_keypair_export_der(&kp, der, sizeof(der),
                                              &der_len) == ESP_OK) {
                tls_store_save_pending_key(der, der_len);
            }
            mqtt_publish_enroll_state("{\"status\":\"pending\"}");
        } else if (e == ESP_ERR_NOT_ALLOWED) {
            /* Operator denied THIS key. Drop it so the retry loop stops;
             * a later cmd/cert renew starts a fresh TOFU round (new key
             * → new pending request the operator can approve). */
            tls_store_clear_pending_key();
            mqtt_publish_enroll_state("{\"status\":\"denied\"}");
        } else if (e != ESP_OK) {
            mqtt_publish_enroll_state(
                "{\"status\":\"failed\",\"reason\":\"transport\"}");
        }
    } else {
        e = mqtt_enroll_request(csr_pem, csr_len,
                                cert_pem, CB_ENROLL_CERT_BUF_SZ, &cert_len,
                                timeout_ms);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "enroll_run: MQTT round-trip failed: 0x%x", e);
            mqtt_publish_enroll_state(
                "{\"status\":\"failed\",\"reason\":\"mqtt\"}");
        }
    }
    if (e != ESP_OK) {
        tls_enroll_keypair_free(&kp);
        free(csr_pem); free(cert_pem);
        return e;
    }

    /* validate_and_persist consumes kp on success; on failure kp is
     * left intact so the caller can decide to retry or drop. We drop
     * here — a verification failure means somebody on the wire
     * tampered with the response, retrying immediately won't help. */
    e = tls_enroll_validate_and_persist(
        &kp, cert_pem, cert_len + 1 /* include NUL for PEM parser */,
        id, (domain && domain[0]) ? domain : "",
        have_ip ? ip : NULL);
    free(csr_pem); free(cert_pem);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "enroll_run: validate_and_persist failed: 0x%x", e);
        mqtt_publish_enroll_state(
            "{\"status\":\"failed\",\"reason\":\"validate\"}");
        tls_enroll_keypair_free(&kp);
        return e;
    }

    tls_store_clear_pending_key();
    mqtt_publish_enroll_state(tls_store_cert_has_client_auth()
        ? "{\"status\":\"issued\",\"client_auth\":true}"
        : "{\"status\":\"issued\",\"client_auth\":false}");
    ESP_LOGI(TAG, "enroll_run: success — reboot to start HTTPS server");
    return ESP_OK;
}

/* Internal worker — runs on the dedicated enrollment task. The public
 * tls_boot_enroll_if_needed wraps this in a 12 KB task because the
 * main task stack (5 KB) plus the mbedtls SHA-512 entropy + X.509
 * verify chain (4-6 KB cumulative) overflows when called inline. */
/* True when a cert exists, the clock is SNTP-synced, and the leaf is within
 * CB_TLS_RENEW_WINDOW_S of notAfter — i.e. due for proactive renewal. Before
 * this, NOTHING read the persisted expiry (tls_store_get_expiry had zero
 * callers), so the board would serve the cert right up to notAfter and then
 * silently lose HTTPS forever — a guaranteed self-strand at a fixed future
 * date on the OTA-only field box. Clock-gated so an unsynced boot (time≈0)
 * can't trigger spurious renewal churn. */
bool tls_enroll_cert_due_for_renewal(void) {
    if (!tls_store_has_cert()) return false;
    time_t now = time(NULL);
    if (now <= (time_t)CB_CLOCK_SYNCED_EPOCH) return false;  /* clock not synced yet */
    int64_t exp = 0;
    if (tls_store_get_expiry(&exp) != ESP_OK) return false;
    return (exp - (int64_t)now) < CB_TLS_RENEW_WINDOW_S;
}

bool tls_enroll_san_drifted(void) {
    if (!tls_store_has_cert()) return false;
    char ip[20] = {0};
    if (!wifi_mgr_get_ip_str(ip, sizeof(ip))) return false;  /* no IP → can't judge */
    uint8_t stored[TLS_STORE_SAN_FP_LEN], current[TLS_STORE_SAN_FP_LEN];
    if (tls_store_get_san_fp(stored) != ESP_OK) return false;
    tls_store_compute_san_fp(device_id(), wifi_mgr_get_domain(), ip, current);
    return memcmp(stored, current, TLS_STORE_SAN_FP_LEN) != 0;
}

static esp_err_t tls_boot_enroll_impl(uint32_t enroll_timeout_ms) {
    const char *id     = device_id();
    const char *domain = wifi_mgr_get_domain();
    char ip[20] = {0};
    bool have_ip = wifi_mgr_get_ip_str(ip, sizeof(ip));

    /* One-shot force flag (cmd/cert {"renew":true}) — consume it here so
     * a forced run bypasses the quick path exactly once. A persisted
     * pending-enroll key bypasses it too: that's an in-flight TOFU
     * enrollment which MUST reach the signer again to be resolved —
     * skipping here would return ESP_OK without enrolling, which the
     * deferred-retry task reads as success and reboots on (observed as a
     * ~2 min spurious-reboot loop on the bench). */
    const bool force = s_force_reenroll;
    s_force_reenroll = false;
    const bool pending = tls_store_has_pending_key();
    if (pending) {
        ESP_LOGI(TAG, "boot enroll: pending TOFU enrollment on file — "
                      "re-submitting the same CSR");
    }

    /* Quick path: existing cert with SAN fingerprint that still matches
     * the current id+domain+IP. No MQTT, no wait — boot continues to
     * http_server_start which picks up the cert and goes HTTPS. */
    if (tls_store_has_cert() && !force && !pending) {
        uint8_t stored[TLS_STORE_SAN_FP_LEN];
        if (tls_store_get_san_fp(stored) == ESP_OK) {
            uint8_t current[TLS_STORE_SAN_FP_LEN];
            tls_store_compute_san_fp(id, domain, have_ip ? ip : NULL, current);
            if (memcmp(stored, current, TLS_STORE_SAN_FP_LEN) == 0) {
                if (!tls_enroll_cert_due_for_renewal() &&
                    !tls_enroll_needs_client_auth()) {
                    ESP_LOGI(TAG, "boot enroll: cert matches env (id=%s domain=%s) — skip",
                             id, domain ? domain : "");
                    return ESP_OK;
                }
                if (tls_enroll_needs_client_auth()) {
                    ESP_LOGW(TAG,
                             "boot enroll: config wants mTLS but the leaf has "
                             "no clientAuth EKU — reissuing via HTTPS enroll");
                } else {
                    int64_t exp = 0;
                    (void)tls_store_get_expiry(&exp);
                    ESP_LOGW(TAG,
                             "boot enroll: cert matches env but within renew window "
                             "(expiry=%lld, <%lld s left) — renewing",
                             (long long)exp, (long long)CB_TLS_RENEW_WINDOW_S);
                }
                /* fall through to the link/SNTP wait + tls_enroll_run */
            } else if (!have_ip) {
                /* SAN mismatch but we have NO IP yet (slow DHCP on a weak
                 * link): the stored fp includes the IP, ours here doesn't, so
                 * this "drift" is spurious. Keep serving the existing cert and
                 * let the main-loop tls_enroll_san_drifted() re-check once an
                 * IP is actually up — don't re-enroll on a phantom change. */
                ESP_LOGI(TAG, "boot enroll: SAN check deferred (no IP yet) — keeping cert");
                return ESP_OK;
            } else {
                ESP_LOGW(TAG,
                         "boot enroll: SAN fp drift (id/domain/IP changed since "
                         "last enrollment) — re-enrolling");
            }
        } else {
            ESP_LOGW(TAG, "boot enroll: cert present but SAN fp unreadable — re-enrolling");
        }
    } else {
        ESP_LOGI(TAG, "boot enroll: no cert in NVS — enrolling");
    }

    /* Wait for the signer link + SNTP. mbedtls_x509_crt_verify checks
     * notBefore vs current wall clock; an un-synced device clock
     * (typically ~0) makes the freshly signed leaf look "not yet valid"
     * and verify fails. SNTP usually syncs ~8 s after wifi up; 15 s
     * gives slack.
     *
     * The link the transport needs differs: legacy MQTT enrollment
     * requires a connected broker session, the HTTPS POST only needs an
     * IP — which is what makes it usable while an mtls broker candidate
     * is still failing (the enroll fixes the cert, the ladder retries).
     *
     * Year ≥ 2023 (epoch > 1.7e9) is the same SNTP-synced proxy
     * mqtt_publish_triggered uses. */
    const bool https = tls_enroll_https_configured();
    ESP_LOGI(TAG, "boot enroll: waiting up to 15 s for %s + SNTP",
             https ? "IP" : "MQTT");
    char ipw[20];
    bool link_up = false;
    for (int i = 0; i < 150; i++) {
        link_up = https ? wifi_mgr_get_ip_str(ipw, sizeof(ipw))
                        : mqtt_is_connected();
        if (link_up && time(NULL) > CB_CLOCK_SYNCED_EPOCH) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!link_up || time(NULL) <= CB_CLOCK_SYNCED_EPOCH) {
        ESP_LOGW(TAG,
                 "boot enroll: %s %s, SNTP %s — defer (main loop retries "
                 "once the link is up)",
                 https ? "IP" : "MQTT", link_up ? "up" : "down",
                 time(NULL) > CB_CLOCK_SYNCED_EPOCH ? "synced" : "unsynced");
        return ESP_ERR_INVALID_STATE;
    }

    return tls_enroll_run(enroll_timeout_ms);
}

typedef struct {
    uint32_t          timeout_ms;
    esp_err_t         result;
    SemaphoreHandle_t done;
} boot_enroll_arg_t;

static void boot_enroll_task(void *pv) {
    boot_enroll_arg_t *a = (boot_enroll_arg_t *)pv;
    a->result = tls_boot_enroll_impl(a->timeout_ms);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

esp_err_t tls_boot_enroll_if_needed(uint32_t enroll_timeout_ms) {
    /* 12 KB stack: the PSA ECC keygen path eats a few KB, and
     * x509_crt_verify inside validate_and_persist reaches another ~3-
     * 4 KB through the chain walk + ECDSA verify. 12 KB gives slack.
     * The task is single-shot — exits + frees its TCB after dispatch. */
    boot_enroll_arg_t arg = {
        .timeout_ms = enroll_timeout_ms,
        .result     = ESP_FAIL,
        .done       = xSemaphoreCreateBinary(),
    };
    if (!arg.done) {
        ESP_LOGE(TAG, "boot enroll: OOM allocating done semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t r = xTaskCreate(boot_enroll_task, "tls_boot_enroll",
                                /*stack*/ 12288, &arg, /*prio*/ 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "boot enroll: xTaskCreate failed");
        vSemaphoreDelete(arg.done);
        return ESP_ERR_NO_MEM;
    }

    /* Block app_main until the worker is done — boot is sequential by
     * design here so http_server_start sees the freshly-saved cert. */
    xSemaphoreTake(arg.done, portMAX_DELAY);
    vSemaphoreDelete(arg.done);
    return arg.result;
}

/* Fire-and-forget enrollment for the deferred-retry path. The boot enroll
 * gives MQTT+SNTP only a 15 s window; on a weak-signal link MQTT can take a
 * minute+ to punch through, so the boot enroll defers and — with no retry —
 * the board would stay on plain HTTP forever. The main loop calls this once
 * MQTT+SNTP are actually up; it runs the same enroll pipeline on its own task
 * (the main task stack can't hold the X.509 verify chain) and, on success,
 * reboots so the clean boot's quick-path brings up HTTPS. Non-blocking:
 * returns as soon as the task is spawned (or ESP_ERR_NO_MEM if it isn't), so
 * it never stalls the watchdog-fed main loop. */
static void enroll_retry_task(void *pv) {
    uint32_t timeout_ms = (uint32_t)(uintptr_t)pv;
    esp_err_t e = tls_boot_enroll_impl(timeout_ms);
    if (e == ESP_OK) {
        /* We only get here having just completed an MQTT round-trip with the
         * signer — the control-plane invariant (can still receive a corrective
         * OTA) is therefore PROVEN. So if a freshly-OTA'd image is still
         * PENDING_VERIFY, confirm it BEFORE this reboot: otherwise a reboot
         * earlier than the 180 s mark-valid window would let the bootloader
         * roll back the very image the operator just pushed. */
        diag_mark_ota_valid();
        ESP_LOGW(TAG, "deferred enroll succeeded — rebooting to serve HTTPS");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGW(TAG, "deferred enroll attempt failed (0x%x) — will retry later", e);
    vTaskDelete(NULL);
}

esp_err_t tls_enroll_retry_async(uint32_t timeout_ms) {
    BaseType_t r = xTaskCreate(enroll_retry_task, "tls_enroll_retry",
                               /*stack*/ 12288, (void *)(uintptr_t)timeout_ms,
                               /*prio*/ 5, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t tls_enroll_force_async(uint32_t timeout_ms) {
    s_force_reenroll = true;
    esp_err_t e = tls_enroll_retry_async(timeout_ms);
    if (e != ESP_OK) s_force_reenroll = false;
    return e;
}
#endif /* CB_TLS_ENROLL_RUN_AVAILABLE */
