/* nvs_stub.c — host-side stubs for ESP-IDF nvs.h that fail loudly on
 * call. Native tests must avoid exercising NVS paths. */
#include "esp_shim.h"
#include "nvs.h"

#include <stdio.h>

#define NOPE(name) do { fprintf(stderr, "native build: " name " called — " \
                                          "stub never succeeds\n"); \
                        return ESP_FAIL; } while (0)

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out) {
    (void)ns; (void)mode; (void)out; NOPE("nvs_open");
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len) {
    (void)h; (void)key; (void)buf; (void)len; NOPE("nvs_get_blob");
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len) {
    (void)h; (void)key; (void)buf; (void)len; NOPE("nvs_set_blob");
}
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *out) {
    (void)h; (void)key; (void)out; NOPE("nvs_get_i64");
}
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) {
    (void)h; (void)key; (void)v; NOPE("nvs_set_i64");
}
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; NOPE("nvs_erase_all"); }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; NOPE("nvs_commit"); }

const char *esp_err_to_name(esp_err_t e) {
    (void)e;
    return "(native stub)";
}
