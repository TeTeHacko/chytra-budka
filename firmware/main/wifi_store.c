/* wifi_store.c — see wifi_store.h. */

#include "wifi_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config.h" /* WIFI_SSID / WIFI_PASSWORD compile-time defaults */
#include "secret_helpers.h" /* secret_is_placeholder() */
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_store";
static const char *NS  = "wifi_cfg";

/* NVS key names — all ≤15 chars (NVS_KEY_NAME_MAX_SIZE - 1). Mirrors the
 * discipline in tls_store.c / app_config.c. */
#define K_SSID       "ssid"        /* 4 — known-good SSID */
#define K_PASS       "pass"        /* 4 — known-good password */
#define K_CAND_SSID  "cand_ssid"   /* 9 — candidate SSID */
#define K_CAND_PASS  "cand_pass"   /* 9 — candidate password */
#define K_STATE      "state"       /* 5 — st_t below */
#define K_GEN        "gen"         /* 3 — monotonic generation counter */
#define K_AP_SSID    "ap_ssid"     /* 7 — operator-set SoftAP SSID */
#define K_AP_PASS    "ap_pass"     /* 7 — operator-set SoftAP password */
#define K_AP_ONLY    "ap_only"     /* 7 — full AP-only mode flag (u8) */

/* state byte. Only CANDIDATE_PENDING needs to be distinguished; the
 * known-good tier is detected by an actual non-empty ssid key. */
enum { ST_NONE = 0, ST_GOOD = 1, ST_CANDIDATE_PENDING = 2 };

/* ssid 1..32 chars; password open (0) or WPA2 8..63 chars. */
static bool valid_ssid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n >= 1 && n <= 32;
}
static bool valid_pass(const char *p) {
    size_t n = p ? strlen(p) : 0;
    return n == 0 || (n >= 8 && n <= 63);
}

static uint8_t read_state(nvs_handle_t h) {
    uint8_t st = ST_NONE;
    (void)nvs_get_u8(h, K_STATE, &st);
    return st;
}

/* Read a NUL-terminated string key into buf (cap incl. NUL). Returns true
 * on success with a non-empty value. */
static bool read_str(nvs_handle_t h, const char *key, char *buf, size_t cap) {
    if (!buf || cap == 0) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err != ESP_OK) {
        buf[0] = 0;
        return false;
    }
    return buf[0] != 0;
}

esp_err_t wifi_store_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist yet — first boot. get_effective() will
         * fall through to the compile-time default. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    return ESP_OK;
}

bool wifi_store_has_candidate(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t st = read_state(h);
    char tmp[WIFI_STORE_SSID_CAP];
    bool have = (st == ST_CANDIDATE_PENDING) && read_str(h, K_CAND_SSID, tmp, sizeof(tmp));
    nvs_close(h);
    return have;
}

bool wifi_store_has_known_good(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char tmp[WIFI_STORE_SSID_CAP];
    bool have = read_str(h, K_SSID, tmp, sizeof(tmp));
    nvs_close(h);
    return have;
}

bool wifi_store_have_sta_target(void) {
    /* A candidate is on trial this boot — that's a target. */
    if (wifi_store_has_candidate()) return true;
    /* Known-good set provisioned via the web/MQTT path?
     *
     * FAIL-SAFE: this predicate gates the AP-vs-STA boot decision. A FALSE
     * answer drops the board into the unprovisioned AP portal, which has NO
     * timeout and waits for a physical operator — a terrible place to land by
     * accident on an OTA-only field box. So a TRANSIENT NVS error (open fails,
     * or get_str returns anything other than NOT_FOUND) must NOT be read as
     * "no creds": we bias toward STA (remotely recoverable), and only fall
     * through to the compile-time default when NVS positively reports the
     * known-good key ABSENT (ESP_ERR_NVS_NOT_FOUND). */
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NS, NVS_READONLY, &h);
    if (oe == ESP_OK) {
        size_t len = 0;
        esp_err_t e = nvs_get_str(h, K_SSID, NULL, &len);
        nvs_close(h);
        if (e == ESP_OK && len > 1) return true;  /* known-good present */
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "have_sta_target: K_SSID read %s — assuming STA target "
                     "(fail toward remotely-recoverable, not stranded AP)",
                     esp_err_to_name(e));
            return true;
        }
        /* e == ESP_ERR_NVS_NOT_FOUND → genuinely no known-good; fall through. */
    } else if (oe != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "have_sta_target: nvs_open %s — assuming STA target",
                 esp_err_to_name(oe));
        return true;
    }
    /* Compile-time floor: a real (non-empty, non-placeholder) WIFI_SSID means
     * the operator pre-provisioned this build for their fleet → STA-first as
     * before. An empty or "your-…"/"placeholder-…" SSID (a clean-clone build
     * with the WiFi creds left unfilled) means there is no usable STA target,
     * so the caller boots straight into the AP provisioning portal instead of
     * burning ~10 min failing to associate to a network that isn't there. */
    return WIFI_SSID[0] != '\0' && !secret_is_placeholder(WIFI_SSID);
}

esp_err_t wifi_store_get_effective(char *ssid, size_t ssid_cap,
                                   char *pass, size_t pass_cap,
                                   wifi_creds_src_t *src) {
    if (!ssid || !pass || ssid_cap == 0 || pass_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_creds_src_t s = WIFI_CREDS_DEFAULT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t st = read_state(h);
        if (st == ST_CANDIDATE_PENDING &&
            read_str(h, K_CAND_SSID, ssid, ssid_cap)) {
            /* password may legitimately be empty (open network) */
            if (!read_str(h, K_CAND_PASS, pass, pass_cap)) pass[0] = 0;
            s = WIFI_CREDS_CANDIDATE;
        } else if (read_str(h, K_SSID, ssid, ssid_cap)) {
            if (!read_str(h, K_PASS, pass, pass_cap)) pass[0] = 0;
            s = WIFI_CREDS_GOOD;
        }
        nvs_close(h);
    }

    if (s == WIFI_CREDS_DEFAULT) {
        /* Compile-time floor — always present, always recoverable. */
        snprintf(ssid, ssid_cap, "%s", WIFI_SSID);
        snprintf(pass, pass_cap, "%s", WIFI_PASSWORD);
    }

    if (src) *src = s;
    ESP_LOGI(TAG, "effective creds: ssid='%s' source=%s", ssid,
             s == WIFI_CREDS_CANDIDATE ? "candidate"
             : s == WIFI_CREDS_GOOD    ? "known-good (NVS)"
                                       : "compile default");
    return ESP_OK;
}

esp_err_t wifi_store_set_candidate(const char *ssid, const char *pass) {
    if (!valid_ssid(ssid) || !valid_pass(pass)) {
        ESP_LOGW(TAG, "reject candidate: bad ssid/pass length");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_candidate: nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t gen = 0;
    (void)nvs_get_u32(h, K_GEN, &gen);

    /* Write all keys, then one commit. The state byte is written LAST so a
     * torn flash mid-write leaves state != CANDIDATE_PENDING and the next
     * boot ignores the half-written candidate (falls to known-good). */
    err = nvs_set_str(h, K_CAND_SSID, ssid);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(h, K_CAND_PASS, pass ? pass : "");
    if (err != ESP_OK) goto fail;
    err = nvs_set_u32(h, K_GEN, gen + 1);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u8(h, K_STATE, ST_CANDIDATE_PENDING);
    if (err != ESP_OK) goto fail;

    err = nvs_commit(h);
    if (err != ESP_OK) goto fail;
    nvs_close(h);
    /* SSID logged; password deliberately NOT logged. */
    ESP_LOGI(TAG, "candidate staged: ssid='%s' gen=%" PRIu32, ssid, gen + 1);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "set_candidate: %s — discarding uncommitted writes",
             esp_err_to_name(err));
    nvs_close(h);
    return err;
}

esp_err_t wifi_store_promote_candidate(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (read_state(h) != ST_CANDIDATE_PENDING) {
        nvs_close(h);
        return ESP_OK; /* nothing to promote */
    }

    char ssid[WIFI_STORE_SSID_CAP], pass[WIFI_STORE_PASS_CAP];
    if (!read_str(h, K_CAND_SSID, ssid, sizeof(ssid))) {
        nvs_close(h);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!read_str(h, K_CAND_PASS, pass, sizeof(pass))) pass[0] = 0;

    err = nvs_set_str(h, K_SSID, ssid);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(h, K_PASS, pass);
    if (err != ESP_OK) goto fail;
    (void)nvs_erase_key(h, K_CAND_SSID);
    (void)nvs_erase_key(h, K_CAND_PASS);
    err = nvs_set_u8(h, K_STATE, ST_GOOD);
    if (err != ESP_OK) goto fail;

    err = nvs_commit(h);
    if (err != ESP_OK) goto fail;
    nvs_close(h);
    ESP_LOGI(TAG, "candidate promoted to known-good: ssid='%s'", ssid);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "promote: %s", esp_err_to_name(err));
    nvs_close(h);
    return err;
}

esp_err_t wifi_store_revert_candidate(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(h, K_CAND_SSID);
    (void)nvs_erase_key(h, K_CAND_PASS);

    /* Demote state: GOOD if a known-good ssid still exists, else NONE so
     * get_effective() falls through to the compile default. */
    char tmp[WIFI_STORE_SSID_CAP];
    uint8_t st = read_str(h, K_SSID, tmp, sizeof(tmp)) ? ST_GOOD : ST_NONE;
    err = nvs_set_u8(h, K_STATE, st);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "revert: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "candidate reverted (now using %s)",
                 st == ST_GOOD ? "known-good" : "compile default");
    }
    return err;
}

esp_err_t wifi_store_erase(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "erased wifi_cfg namespace — next boot uses compile default");
    }
    return err;
}

esp_err_t wifi_store_get_ap(char *ssid, size_t scap, char *pass, size_t pcap,
                            bool *is_custom) {
    if (is_custom) *is_custom = false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    bool have = read_str(h, K_AP_SSID, ssid, scap);
    if (have && !read_str(h, K_AP_PASS, pass, pcap)) pass[0] = 0;
    nvs_close(h);
    if (have && is_custom) *is_custom = true;
    return ESP_OK;
}

esp_err_t wifi_store_set_ap(const char *ssid, const char *pass) {
    bool clear = (!ssid || !ssid[0]) && (!pass || !pass[0]);
    if (!clear && (!valid_ssid(ssid) || !valid_pass(pass))) {
        ESP_LOGW(TAG, "reject AP creds: bad ssid/pass length");
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (clear) {
        nvs_erase_key(h, K_AP_SSID);
        nvs_erase_key(h, K_AP_PASS);
    } else {
        err = nvs_set_str(h, K_AP_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, K_AP_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AP creds %s", clear ? "cleared (using default)" : "set");
    }
    return err;
}

bool wifi_store_is_ap_only(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    (void)nvs_get_u8(h, K_AP_ONLY, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t wifi_store_set_ap_only(bool on) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_AP_ONLY, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGW(TAG, "AP-only mode %s", on ? "ENABLED (no STA/MQTT/OTA)" : "disabled");
    return err;
}
