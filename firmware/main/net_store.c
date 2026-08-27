/* net_store.c — see net_store.h. */

#include "net_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h" /* MQTT_HOST/PORT, MQTT_USER/PASSWORD, OTA_URL defaults */
#include "esp_log.h"
#include "flat_json.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net_store";
static const char *NS  = "net_cfg";

/* NVS key names — all ≤15 chars. */
#define K_CAND  "cand"        /* flat-JSON candidate record */
#define K_GOOD  "good"        /* flat-JSON known-good record */
#define K_TRIES "cand_tries"  /* u8 cross-boot candidate boot counter */

/* Record blobs hold ≤9 short fields; 640 B is comfortable and fits the
 * 4 KB MQTT inbound buffer with margin. */
#define BLOB_CAP 640

/* Field tables. Broker fields require the candidate ladder; live fields
 * merge straight into the good record. */
static const char *BROKER_FIELDS[] = {"mqtt_uri", "mqtt_auth", "mqtt_user",
                                      "mqtt_pass", NULL};
static const char *LIVE_FIELDS[]   = {"ota_url", "relay_url", "stream_url",
                                      "relay_tok", "enroll_url", NULL};

const char *net_store_src_str(net_cfg_src_t s) {
    switch (s) {
        case NET_CFG_CANDIDATE: return "candidate";
        case NET_CFG_GOOD:      return "good";
        default:                return "default";
    }
}

const char *net_store_auth_str(net_auth_t a) {
    switch (a) {
        case NET_AUTH_MTLS: return "mtls";
        case NET_AUTH_BOTH: return "both";
        default:            return "userpass";
    }
}

static bool auth_from_str(const char *s, net_auth_t *out) {
    if (strcmp(s, "userpass") == 0) { *out = NET_AUTH_USERPASS; return true; }
    if (strcmp(s, "mtls") == 0)     { *out = NET_AUTH_MTLS;     return true; }
    if (strcmp(s, "both") == 0)     { *out = NET_AUTH_BOTH;     return true; }
    return false;
}

/* Bounded copy with explicit truncation — the "%.*s" precision keeps
 * -Wformat-truncation quiet where the source buffer is wider than the
 * destination field (values were already length-checked at set time). */
static void cp(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%.*s", (int)cap - 1, src);
}

static bool read_blob(nvs_handle_t h, const char *key, char *buf, size_t cap) {
    size_t len = cap;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = 0;
        return false;
    }
    return buf[0] != 0;
}

/* Validate "mqtt://host[:port]" or "mqtts://host[:port]". */
static esp_err_t validate_mqtt_uri(const char *uri) {
    const char *host;
    if (strncmp(uri, "mqtts://", 8) == 0) {
        host = uri + 8;
    } else if (strncmp(uri, "mqtt://", 7) == 0) {
        host = uri + 7;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (!host[0] || host[0] == ':' || host[0] == '/') return ESP_ERR_INVALID_ARG;
    const char *colon = strchr(host, ':');
    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port < 1 || port > 65535) return ESP_ERR_INVALID_ARG;
    }
    if (strlen(uri) >= NET_STORE_URI_CAP) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

/* Serialize a merged record: for each known field, take the value from
 * `over` (new fields) if present, else from `base` (existing blob). Only
 * non-empty values are written. Escaping: values come from fj_str (already
 * unescaped) and our own commands; forbid '"' and '\' instead of escaping —
 * no legitimate endpoint contains them. */
static esp_err_t merge_records(const char *base, const char *over,
                               char *out, size_t cap) {
    size_t o = 0;
    out[o++] = '{';
    bool first = true;
    const char *const *tables[2] = {BROKER_FIELDS, LIVE_FIELDS};
    for (int t = 0; t < 2; t++) {
        for (const char *const *f = tables[t]; *f; f++) {
            char val[NET_STORE_URI_CAP];
            bool have = (over && fj_str(over, *f, val, sizeof(val)) && val[0]) ||
                        (base && fj_str(base, *f, val, sizeof(val)) && val[0]);
            if (!have) continue;
            if (strpbrk(val, "\"\\")) {
                ESP_LOGW(TAG, "field %s contains quote/backslash — rejected", *f);
                return ESP_ERR_INVALID_ARG;
            }
            int n = snprintf(out + o, cap - o, "%s\"%s\":\"%s\"",
                             first ? "" : ",", *f, val);
            if (n < 0 || (size_t)n >= cap - o) return ESP_ERR_NO_MEM;
            o += (size_t)n;
            first = false;
        }
    }
    if (o + 2 > cap) return ESP_ERR_NO_MEM;
    out[o++] = '}';
    out[o] = 0;
    return ESP_OK;
}

static bool json_has_any(const char *json, const char *const *fields) {
    char tmp[NET_STORE_URI_CAP];
    for (const char *const *f = fields; *f; f++) {
        if (fj_str(json, *f, tmp, sizeof(tmp))) return true;
    }
    return false;
}

esp_err_t net_store_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; /* first boot */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    char blob[BLOB_CAP];
    if (read_blob(h, K_CAND, blob, sizeof(blob))) {
        uint8_t tries = 0;
        (void)nvs_get_u8(h, K_TRIES, &tries);
        tries++;
        if (tries > CONFIG_CHYTRA_BUDKA_NET_CAND_MAX_TRIES) {
            /* Cross-boot backstop: candidate keeps crashing/rebooting the
             * board before the verify window elapses → drop it now, before
             * mqtt_init() would use it again. */
            ESP_LOGE(TAG, "candidate survived %u boots without promotion — "
                          "auto-reverting", (unsigned)(tries - 1));
            (void)nvs_erase_key(h, K_CAND);
            (void)nvs_erase_key(h, K_TRIES);
        } else {
            (void)nvs_set_u8(h, K_TRIES, tries);
            ESP_LOGW(TAG, "endpoint candidate pending (boot attempt %u/%d)",
                     (unsigned)tries, CONFIG_CHYTRA_BUDKA_NET_CAND_MAX_TRIES);
        }
        (void)nvs_commit(h);
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t net_store_get_effective(net_cfg_t *out, net_cfg_src_t *src) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* Tier c: compile-time floor. */
    snprintf(out->mqtt_uri, sizeof(out->mqtt_uri), "%s://%s:%d",
             MQTT_SCHEME, MQTT_HOST, MQTT_PORT);
    if (!auth_from_str(MQTT_AUTH_DEFAULT, &out->mqtt_auth)) {
        out->mqtt_auth = NET_AUTH_USERPASS;   /* unreachable unless the define rots */
    }
    snprintf(out->mqtt_user, sizeof(out->mqtt_user), "%s", MQTT_USER);
    snprintf(out->mqtt_pass, sizeof(out->mqtt_pass), "%s", MQTT_PASSWORD);
    snprintf(out->ota_url, sizeof(out->ota_url), "%s", OTA_URL);
    /* Enrollment defaults to the manager's HTTPS endpoint; an empty value here
     * would send a fresh board to the retired legacy MQTT signer instead. */
    snprintf(out->enroll_url, sizeof(out->enroll_url), "%s", ENROLL_URL);
    /* relay_url/stream_url/relay_tok default "" — use sites fall back to their
     * compile defaults. relay stays plain LAN: audio has no TLS transport yet
     * (no cb::TlsTransport), so pointing it at the stack would break it. */

    net_cfg_src_t s = NET_CFG_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        char blob[BLOB_CAP];
        /* good first, candidate overlays it */
        for (int pass = 0; pass < 2; pass++) {
            const char *key = pass == 0 ? K_GOOD : K_CAND;
            if (!read_blob(h, key, blob, sizeof(blob))) continue;
            char val[NET_STORE_URI_CAP];
            bool broker_touched = false;
            if (fj_str(blob, "mqtt_uri", val, sizeof(val)) && val[0]) {
                cp(out->mqtt_uri, sizeof(out->mqtt_uri), val);
                broker_touched = true;
            }
            if (fj_str(blob, "mqtt_auth", val, sizeof(val)) && val[0]) {
                (void)auth_from_str(val, &out->mqtt_auth);
                broker_touched = true;
            }
            if (fj_str(blob, "mqtt_user", val, sizeof(val)) && val[0]) {
                cp(out->mqtt_user, sizeof(out->mqtt_user), val);
                broker_touched = true;
            }
            if (fj_str(blob, "mqtt_pass", val, sizeof(val)) && val[0]) {
                cp(out->mqtt_pass, sizeof(out->mqtt_pass), val);
                broker_touched = true;
            }
            if (fj_str(blob, "ota_url", val, sizeof(val)) && val[0])
                cp(out->ota_url, sizeof(out->ota_url), val);
            if (fj_str(blob, "relay_url", val, sizeof(val)) && val[0])
                cp(out->relay_url, sizeof(out->relay_url), val);
            if (fj_str(blob, "stream_url", val, sizeof(val)) && val[0])
                cp(out->stream_url, sizeof(out->stream_url), val);
            if (fj_str(blob, "relay_tok", val, sizeof(val)) && val[0])
                cp(out->relay_tok, sizeof(out->relay_tok), val);
            if (fj_str(blob, "enroll_url", val, sizeof(val)) && val[0])
                cp(out->enroll_url, sizeof(out->enroll_url), val);
            if (broker_touched) {
                s = pass == 0 ? NET_CFG_GOOD : NET_CFG_CANDIDATE;
            } else if (pass == 0 && s == NET_CFG_DEFAULT) {
                /* good record exists but has no broker fields — broker still
                 * on default; keep s as-is. */
            }
        }
        nvs_close(h);
    }

    if (src) *src = s;
    return ESP_OK;
}

bool net_store_has_candidate(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char blob[BLOB_CAP];
    bool have = read_blob(h, K_CAND, blob, sizeof(blob));
    nvs_close(h);
    return have;
}

bool net_store_has_known_good(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char blob[BLOB_CAP];
    bool have = read_blob(h, K_GOOD, blob, sizeof(blob));
    nvs_close(h);
    return have;
}

esp_err_t net_store_set_candidate_json(const char *json) {
    if (!json || !json_has_any(json, BROKER_FIELDS)) return ESP_ERR_INVALID_ARG;

    char val[NET_STORE_URI_CAP];
    if (fj_str(json, "mqtt_uri", val, sizeof(val))) {
        esp_err_t verr = validate_mqtt_uri(val);
        if (verr != ESP_OK) return verr;
    }
    if (fj_str(json, "mqtt_auth", val, sizeof(val))) {
        net_auth_t a;
        if (!auth_from_str(val, &a)) return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char base[BLOB_CAP] = "";
    (void)read_blob(h, K_GOOD, base, sizeof(base));
    char merged[BLOB_CAP];
    err = merge_records(base[0] ? base : NULL, json, merged, sizeof(merged));
    if (err == ESP_OK) err = nvs_set_str(h, K_CAND, merged);
    if (err == ESP_OK) err = nvs_set_u8(h, K_TRIES, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        /* Record logged WITHOUT credential values. */
        char uri[NET_STORE_URI_CAP] = "(unchanged)";
        (void)fj_str(json, "mqtt_uri", uri, sizeof(uri));
        ESP_LOGW(TAG, "endpoint candidate staged (mqtt_uri=%s) — verify ladder "
                      "runs after reboot", uri);
    } else {
        ESP_LOGE(TAG, "set_candidate: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_store_set_live_json(const char *json) {
    if (!json || !json_has_any(json, LIVE_FIELDS)) return ESP_ERR_INVALID_ARG;
    if (json_has_any(json, BROKER_FIELDS)) {
        /* Callers split broker/live; getting here means a mixed payload
         * slipped through — refuse rather than silently half-apply. */
        ESP_LOGW(TAG, "set_live: payload contains broker fields — refused");
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char base[BLOB_CAP] = "";
    (void)read_blob(h, K_GOOD, base, sizeof(base));
    char merged[BLOB_CAP];
    err = merge_records(base[0] ? base : NULL, json, merged, sizeof(merged));
    if (err == ESP_OK) err = nvs_set_str(h, K_GOOD, merged);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "live endpoint fields applied to known-good record");
    } else {
        ESP_LOGE(TAG, "set_live: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_store_promote_candidate(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char cand[BLOB_CAP];
    if (!read_blob(h, K_CAND, cand, sizeof(cand))) {
        nvs_close(h);
        return ESP_OK; /* nothing to promote */
    }
    err = nvs_set_str(h, K_GOOD, cand);
    if (err == ESP_OK) err = nvs_erase_key(h, K_CAND);
    if (err == ESP_OK) { (void)nvs_erase_key(h, K_TRIES); }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "endpoint candidate promoted to known-good");
    } else {
        ESP_LOGE(TAG, "promote: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_store_revert_candidate(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    (void)nvs_erase_key(h, K_CAND);
    (void)nvs_erase_key(h, K_TRIES);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "endpoint candidate reverted (now using %s)",
                 net_store_has_known_good() ? "known-good" : "compile default");
    }
    return err;
}

esp_err_t net_store_erase(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "erased net_cfg namespace — next boot uses compile defaults");
    }
    return err;
}
