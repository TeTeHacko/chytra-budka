/* Native-test stub for ESP-IDF's nvs.h. Provides just enough type +
 * symbol declarations to let tls_store.c compile on the host — none
 * of the NVS operations actually work. Host tests only exercise
 * non-NVS functions (currently just tls_store_compute_san_fp); the
 * NVS-touching paths are covered by HIL on the bench.
 *
 * Implementations are weak symbols in nvs_stub.c that always return
 * ESP_FAIL — any accidental host-side call will fail loudly rather
 * than silently no-op. */
#pragma once

#include "esp_shim.h"
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;

#define NVS_READONLY  0
#define NVS_READWRITE 1

#define ESP_ERR_NVS_NOT_FOUND       0x1102
#define ESP_ERR_NVS_INVALID_LENGTH  0x1107

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len);
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *out);
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);

const char *esp_err_to_name(esp_err_t e);
