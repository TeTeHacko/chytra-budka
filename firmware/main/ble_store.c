/* ble_store.c — see ble_store.h. */

#include "ble_store.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ble_store";
static const char *NS  = "ble_dev";

#define KEY_PREFIX     "d_"            /* + 12 hex = 14 chars (NVS key ≤ 15) */
#define KEY_CAP        (sizeof(KEY_PREFIX) + BLE_STORE_ID_LEN)  /* "d_"+12+NUL */

/* id = exactly 12 hex chars (a MAC, no separators). */
static bool valid_id(const char *id) {
    if (!id || strlen(id) != BLE_STORE_ID_LEN) return false;
    for (int i = 0; i < BLE_STORE_ID_LEN; i++)
        if (!isxdigit((unsigned char)id[i])) return false;
    return true;
}

static void key_for(const char *id, char out[KEY_CAP]) {
    snprintf(out, KEY_CAP, KEY_PREFIX "%s", id);
}

/* Drop control chars; bound to cap-1. Keeps the name safe for NVS + later
 * HTML/MQTT use (display layer still escapes). */
static void sanitize_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c != 0x7f) out[o++] = (char)c;
    }
    out[o] = 0;
}

esp_err_t ble_store_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  /* fresh device */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    return ESP_OK;
}

bool ble_store_is_saved(const char *id) {
    if (!valid_id(id)) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[KEY_CAP];
    key_for(id, key);
    size_t len = 0;
    bool saved = (nvs_get_str(h, key, NULL, &len) == ESP_OK);
    nvs_close(h);
    return saved;
}

bool ble_store_get_name(const char *id, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!valid_id(id) || !out || cap == 0) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[KEY_CAP];
    key_for(id, key);
    size_t len = cap;
    bool saved = (nvs_get_str(h, key, out, &len) == ESP_OK);
    nvs_close(h);
    if (!saved) out[0] = 0;
    return saved;
}

esp_err_t ble_store_save(const char *id, const char *name) {
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    char clean[BLE_STORE_NAME_CAP];
    sanitize_name(name, clean, sizeof(clean));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[KEY_CAP];
    key_for(id, key);
    err = nvs_set_str(h, key, clean);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "saved %s name='%s'", id, clean);
    else
        ESP_LOGW(TAG, "save %s failed: %s", id, esp_err_to_name(err));
    return err;
}

esp_err_t ble_store_forget(const char *id) {
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  /* nothing saved */
    if (err != ESP_OK) return err;
    char key[KEY_CAP];
    key_for(id, key);
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* already gone */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "forgot %s", id);
    return err;
}

int ble_store_count(void) {
    int n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_STR, &it);
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, KEY_PREFIX, strlen(KEY_PREFIX)) == 0) n++;
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    return n;
}

bool ble_store_list(int idx, char *id_out, size_t id_cap,
                    char *name_out, size_t name_cap) {
    if (id_out && id_cap) id_out[0] = 0;
    if (name_out && name_cap) name_out[0] = 0;
    if (idx < 0 || !id_out || id_cap < BLE_STORE_ID_CAP) return false;

    /* Find the idx-th "d_*" key via the iterator. info.key is valid only until
     * the iterator advances/releases, so copy it (buffer sized to the NVS key
     * max so the copy can't truncate). */
    char key[NVS_KEY_NAME_MAX_SIZE + 1] = {0};
    int seen = 0;
    bool found = false;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_STR, &it);
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, KEY_PREFIX, strlen(KEY_PREFIX)) == 0) {
            if (seen == idx) {
                snprintf(key, sizeof(key), "%s", info.key);
                found = true;
                break;
            }
            seen++;
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    if (!found || strlen(key) <= strlen(KEY_PREFIX)) return false;

    /* id is exactly the 12 hex chars after "d_" — fixed length, copy directly. */
    memcpy(id_out, key + strlen(KEY_PREFIX), BLE_STORE_ID_LEN);
    id_out[BLE_STORE_ID_LEN] = 0;
    if (name_out && name_cap) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = name_cap;
            if (nvs_get_str(h, key, name_out, &len) != ESP_OK) name_out[0] = 0;
            nvs_close(h);
        }
    }
    return true;
}
