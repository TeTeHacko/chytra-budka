/* auth_store.c — see auth_store.h. */

#include "auth_store.h"

#include <stdio.h>
#include <string.h>

#include "config.h" /* HTTP_BASIC_USER / HTTP_BASIC_PASS compile-time defaults */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "auth_store";
static const char *NS  = "auth";

#define K_USER "user"
#define K_PASS "pass"

/* In-RAM cache of the EFFECTIVE creds (override → else compile default).
 *
 * WHY THE CACHE EXISTS — the gate (basic_auth_enabled) resolves creds on EVERY
 * request, and some HTTP handlers run on tasks whose stack is in PSRAM
 * (xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM) — e.g. the /capture
 * worker, given a PSRAM stack to survive BLE-on internal-DRAM fragmentation).
 * A per-request NVS read is a flash op, which disables the PSRAM cache; with
 * the calling task's stack in PSRAM that trips
 * `esp_task_stack_is_sane_cache_disabled()` → panic (coredump-confirmed under
 * concurrent /capture load with auth enabled). Resolving from this RAM cache
 * touches no flash, so the gate is safe on any task. NVS is read only in
 * auth_store_init() (app_main, internal stack) and refreshed in
 * auth_store_set() (the mqtt task, internal stack). */
static char s_eff_user[AUTH_STORE_USER_CAP];
static char s_eff_pass[AUTH_STORE_PASS_CAP];
static SemaphoreHandle_t s_lock;
static bool s_loaded;

/* Read a NUL-terminated string key into buf (cap incl. NUL). Returns true on
 * success with a non-empty value; clears buf and returns false otherwise. */
static bool read_str(nvs_handle_t h, const char *key, char *buf, size_t cap) {
    if (!buf || cap == 0) return false;
    size_t len = cap;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = 0;
        return false;
    }
    return buf[0] != 0;
}

esp_err_t auth_store_get(char *user, size_t ucap, char *pass, size_t pcap,
                         bool *is_custom) {
    if (is_custom) *is_custom = false;
    if (!user || !pass || ucap == 0 || pcap == 0) return ESP_ERR_INVALID_ARG;
    user[0] = 0;
    pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    /* Require BOTH keys — a half-written record (only user, or only pass) is
     * treated as "no override" so the gate never compares against a blank. */
    bool have = read_str(h, K_USER, user, ucap) && read_str(h, K_PASS, pass, pcap);
    nvs_close(h);
    if (!have) {
        user[0] = 0;
        pass[0] = 0;
    } else if (is_custom) {
        *is_custom = true;
    }
    return ESP_OK;
}

/* Recompute the effective creds from NVS (+ compile-time floor) into the RAM
 * cache. DOES a flash read — MUST be called from an internal-stack task only
 * (app_main / the mqtt task), never from a PSRAM-stacked HTTP worker. */
static void refresh_cache(void) {
    char user[AUTH_STORE_USER_CAP], pass[AUTH_STORE_PASS_CAP];
    bool custom = false;
    auth_store_get(user, sizeof(user), pass, sizeof(pass), &custom);
    if (!custom) {
        snprintf(user, sizeof(user), "%s", HTTP_BASIC_USER);
        snprintf(pass, sizeof(pass), "%s", HTTP_BASIC_PASS);
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_eff_user, sizeof(s_eff_user), "%s", user);
    snprintf(s_eff_pass, sizeof(s_eff_pass), "%s", pass);
    s_loaded = true;
    if (s_lock) xSemaphoreGive(s_lock);
}

void auth_store_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    refresh_cache();
}

void auth_store_get_effective(char *user, size_t ucap, char *pass, size_t pcap) {
    if (!user || !pass || ucap == 0 || pcap == 0) return;
    /* Serve from the RAM cache — NO flash access, so this is safe to call from
     * a PSRAM-stacked task (see the cache comment above). If init somehow has
     * not run yet, fall back to the compile-time default (still no flash). */
    if (s_loaded && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(user, ucap, "%s", s_eff_user);
        snprintf(pass, pcap, "%s", s_eff_pass);
        xSemaphoreGive(s_lock);
    } else {
        snprintf(user, ucap, "%s", HTTP_BASIC_USER);
        snprintf(pass, pcap, "%s", HTTP_BASIC_PASS);
    }
}

esp_err_t auth_store_set(const char *user, const char *pass) {
    bool clear = (!user || !user[0]) && (!pass || !pass[0]);
    if (!clear) {
        size_t ul = user ? strlen(user) : 0;
        size_t pl = pass ? strlen(pass) : 0;
        /* Both required; cap so the decoded "user:pass" fits the gate's
         * base64 decode buffer (see basic_auth_gate). */
        if (ul < 1 || ul >= AUTH_STORE_USER_CAP ||
            pl < 1 || pl >= AUTH_STORE_PASS_CAP) {
            ESP_LOGW(TAG, "reject web-admin creds: bad user/pass length "
                          "(user 1..%d, pass 1..%d)",
                     AUTH_STORE_USER_CAP - 1, AUTH_STORE_PASS_CAP - 1);
            return ESP_ERR_INVALID_ARG;
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (clear) {
        nvs_erase_key(h, K_USER);  /* NOT_FOUND is fine — ignored */
        nvs_erase_key(h, K_PASS);
    } else {
        err = nvs_set_str(h, K_USER, user);
        if (err == ESP_OK) err = nvs_set_str(h, K_PASS, pass);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        refresh_cache(); /* applied live — runs on the caller (mqtt task, internal stack) */
        ESP_LOGI(TAG, "web-admin creds %s",
                 clear ? "cleared (using compile default)" : "set");
    } else {
        ESP_LOGE(TAG, "set: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t auth_store_erase(void) {
    return auth_store_set(NULL, NULL);
}
