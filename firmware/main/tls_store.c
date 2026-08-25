/* tls_store.c — see tls_store.h. */

/* Predef BEFORE any mbedtls header (private_access.h is one-shot). */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "tls_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
/* mbedtls 4.0 (IDF v6.0.1): legacy sha256 API gated behind
 * MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS in mbedtls/private/sha256.h.
 * MBEDTLS_ALLOW_PRIVATE_ACCESS define at the top of this file (before
 * tls_store.h) opens the gate via private_access.h. */
#include "mbedtls/oid.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "tls_store";
static const char *NS  = "tls";

/* NVS key names — all ≤15 chars (NVS_KEY_NAME_MAX_SIZE - 1). The
 * key_too_long sanity guard in app_config_init catches violations
 * for the schema-driven knobs; we mirror the discipline here.
 *
 * K_CERT's bytes used to be a leaf-only DER (format version 1); now
 * they hold a leaf+sub-CA PEM bundle (format version 2). The K_FMT_VER
 * key disambiguates — a v1 record looks structurally identical at
 * the NVS layer, so without the version check a v2 firmware loading
 * a v1 record would hand bare DER to mbedtls and the chain delivery
 * would silently fall back to leaf-only. Keeping the key name as
 * "cert_der" preserves NVS layout across the version bump; the contents
 * are what differ. */
#define K_KEY      "key_der"      /* 7 chars */
#define K_CERT     "cert_der"     /* 8 — bytes are PEM bundle in v2 */
#define K_EXPIRY   "expiry"       /* 6 */
#define K_SAN_FP   "san_fp"       /* 6 */
#define K_FMT_VER  "fmt_ver"      /* 7 */
#define K_PEND_KEY "pend_key"     /* 8 — pending-enrollment keypair DER */

esp_err_t tls_store_init(void) {
    /* Opening the namespace on every call is cheap (NVS keeps a handle
     * cache) — but the actual nvs_flash_init() happens in app_main
     * before any consumer here is called. We don't keep a long-lived
     * handle: each operation opens, does its work, closes. Matches the
     * pattern in app_config.c and avoids leaking a handle when the
     * caller forgets to deinit. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist yet — first boot. Create it by doing
         * a no-op write + close. Subsequent reads will succeed with
         * not_found at the per-key level, which tls_store_load surfaces
         * to the caller as "no cert yet". */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    return ESP_OK;
}

bool tls_store_has_cert(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    /* Check all five keys exist AND format version matches current
     * firmware expectation. If any is missing OR the on-disk format
     * is older than the firmware understands we treat the whole store
     * as absent — partial state shouldn't happen (save is atomic) but
     * a torn flash from an old buggy firmware could leave one, so be
     * defensive. Stale format → boot-enroll path runs and re-enrolls
     * under the current schema. */
    size_t klen = 0, clen = 0;
    int64_t exp = 0;
    size_t fp_len = 0;
    uint8_t fmt_ver = 0;
    bool ok =
        nvs_get_u8  (h, K_FMT_VER, &fmt_ver)       == ESP_OK &&
        fmt_ver == CB_TLS_FORMAT_VER &&
        nvs_get_blob(h, K_KEY,    NULL, &klen)    == ESP_OK && klen > 0 &&
        nvs_get_blob(h, K_CERT,   NULL, &clen)    == ESP_OK && clen > 0 &&
        nvs_get_i64 (h, K_EXPIRY, &exp)            == ESP_OK &&
        nvs_get_blob(h, K_SAN_FP, NULL, &fp_len)  == ESP_OK &&
        fp_len == TLS_STORE_SAN_FP_LEN;
    nvs_close(h);
    return ok;
}

esp_err_t tls_store_load(tls_store_blob_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    out->key_len  = TLS_STORE_KEY_DER_MAX;
    out->cert_len = TLS_STORE_CERT_PEM_MAX;

    err = nvs_get_blob(h, K_KEY, out->key_der, &out->key_len);
    if (err != ESP_OK) goto done;

    err = nvs_get_blob(h, K_CERT, out->cert_pem, &out->cert_len);
    if (err != ESP_OK) goto done;

    err = nvs_get_i64(h, K_EXPIRY, &out->expiry_unix);
    if (err != ESP_OK) goto done;

    size_t fp_len = TLS_STORE_SAN_FP_LEN;
    err = nvs_get_blob(h, K_SAN_FP, out->san_fp, &fp_len);
    if (err != ESP_OK) goto done;
    if (fp_len != TLS_STORE_SAN_FP_LEN) {
        ESP_LOGW(TAG, "san_fp wrong length (%zu) — treating as missing", fp_len);
        err = ESP_ERR_NVS_INVALID_LENGTH;
    }

done:
    nvs_close(h);
    return err;
}

esp_err_t tls_store_save(const tls_store_blob_t *in) {
    if (!in || in->key_len == 0 || in->key_len > TLS_STORE_KEY_DER_MAX ||
        in->cert_len == 0 || in->cert_len > TLS_STORE_CERT_PEM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    /* Set all five keys, then commit once. NVS's transactional model:
     * uncommitted writes live in RAM until nvs_commit; if any
     * intermediate set fails we close without commit and the existing
     * stored values stay intact. The format-version byte is written
     * LAST (before commit) so a torn flash mid-save leaves the
     * version stale and the next boot's tls_store_has_cert() rejects
     * the partial record — re-enroll cleanly. */
    err = nvs_set_blob(h, K_KEY, in->key_der, in->key_len);
    if (err != ESP_OK) goto fail;
    err = nvs_set_blob(h, K_CERT, in->cert_pem, in->cert_len);
    if (err != ESP_OK) goto fail;
    err = nvs_set_i64(h, K_EXPIRY, in->expiry_unix);
    if (err != ESP_OK) goto fail;
    err = nvs_set_blob(h, K_SAN_FP, in->san_fp, TLS_STORE_SAN_FP_LEN);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u8(h, K_FMT_VER, CB_TLS_FORMAT_VER);
    if (err != ESP_OK) goto fail;

    err = nvs_commit(h);
    if (err != ESP_OK) goto fail;

    nvs_close(h);
    ESP_LOGI(TAG, "saved cert (key=%zu B, cert=%zu B, expiry=%lld)",
             in->key_len, in->cert_len, (long long)in->expiry_unix);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "save: %s — discarding uncommitted writes",
             esp_err_to_name(err));
    nvs_close(h);
    return err;
}

esp_err_t tls_store_erase(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* nvs_erase_all wipes the whole namespace in one shot. Cheaper
     * than four nvs_erase_key calls and matches the "all or nothing"
     * model save/load use. */
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "erased tls namespace");
    }
    return err;
}

esp_err_t tls_store_save_pending_key(const uint8_t *der, size_t len) {
    if (!der || len == 0 || len > TLS_STORE_KEY_DER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, K_PEND_KEY, der, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved pending-enroll key (%zu B)", len);
    } else {
        ESP_LOGE(TAG, "save pending key: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t tls_store_load_pending_key(uint8_t *out_der, size_t cap,
                                     size_t *out_len) {
    if (!out_der || !out_len || cap == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    *out_len = cap;
    err = nvs_get_blob(h, K_PEND_KEY, out_der, out_len);
    nvs_close(h);
    return err;
}

bool tls_store_has_pending_key(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, K_PEND_KEY, NULL, &len) == ESP_OK && len > 0;
    nvs_close(h);
    return ok;
}

esp_err_t tls_store_clear_pending_key(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, K_PEND_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  /* already clear */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t tls_store_get_expiry(int64_t *out_unix) {
    if (!out_unix) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_i64(h, K_EXPIRY, out_unix);
    nvs_close(h);
    return err;
}

esp_err_t tls_store_get_san_fp(uint8_t out_fp[TLS_STORE_SAN_FP_LEN]) {
    if (!out_fp) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = TLS_STORE_SAN_FP_LEN;
    err = nvs_get_blob(h, K_SAN_FP, out_fp, &len);
    nvs_close(h);
    if (err == ESP_OK && len != TLS_STORE_SAN_FP_LEN) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    return err;
}

/* ── canonical SAN fingerprint ─────────────────────────────────────────
 *
 * The same string format is used on both sides of the enrollment
 * trust boundary (device + signer) — keep it stable. Documented
 * format in tls_store_compute_san_fp's doc comment. */

typedef struct {
    char buf[64];
} san_line_t;

static int san_line_cmp(const void *a, const void *b) {
    return strcmp(((const san_line_t *)a)->buf,
                  ((const san_line_t *)b)->buf);
}

void tls_store_compute_san_fp(const char *device_id,
                               const char *domain,
                               const char *ip_str,
                               uint8_t out_fp[TLS_STORE_SAN_FP_LEN]) {
    /* Up to 4 SAN entries: dns:<short>, dns:<short>.<domain>,
     * dns:<short>.local, ip:<dotted_quad>. Fewer if domain/ip empty. */
    san_line_t lines[4];
    int n = 0;

    if (device_id && device_id[0]) {
        snprintf(lines[n].buf, sizeof(lines[n].buf), "dns:%s", device_id);
        n++;
        snprintf(lines[n].buf, sizeof(lines[n].buf), "dns:%s.local", device_id);
        n++;
        if (domain && domain[0]) {
            snprintf(lines[n].buf, sizeof(lines[n].buf), "dns:%s.%s",
                     device_id, domain);
            n++;
        }
    }
    if (ip_str && ip_str[0]) {
        snprintf(lines[n].buf, sizeof(lines[n].buf), "ip:%s", ip_str);
        n++;
    }

    /* Sort lexicographically — fingerprint is order-independent. */
    qsort(lines, n, sizeof(lines[0]), san_line_cmp);

    /* Join with '\n' separator and hash. No trailing newline (so the
     * hash of "" is the no-SAN case, distinct from a single "\n"
     * line). */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, /*is224*/ 0);
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            mbedtls_sha256_update(&ctx, (const unsigned char *)"\n", 1);
        }
        mbedtls_sha256_update(&ctx, (const unsigned char *)lines[i].buf,
                              strlen(lines[i].buf));
    }
    mbedtls_sha256_finish(&ctx, out_fp);
    mbedtls_sha256_free(&ctx);
}

bool tls_store_cert_has_client_auth(void) {
    tls_store_blob_t *blob = calloc(1, sizeof(*blob));
    if (!blob) return false;
    bool ok = false;
    if (tls_store_load(blob) == ESP_OK && blob->cert_len > 0) {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        /* cert_len includes the trailing NUL — exactly what parse wants
         * for PEM input. Only the FIRST cert (the leaf) matters for EKU;
         * parse loads the whole bundle but `crt` heads the chain. */
        if (mbedtls_x509_crt_parse(&crt, blob->cert_pem, blob->cert_len) == 0) {
            ok = mbedtls_x509_crt_check_extended_key_usage(
                     &crt, MBEDTLS_OID_CLIENT_AUTH,
                     MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)) == 0;
        }
        mbedtls_x509_crt_free(&crt);
    }
    free(blob);
    return ok;
}
