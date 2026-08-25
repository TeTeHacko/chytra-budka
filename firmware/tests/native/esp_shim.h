/* Minimal stubs to compile ESP-IDF firmware sources on a Linux host.
 * Included via -include into native test compilations of firmware code
 * that drags in esp_err.h / esp_log.h. Keep this small — every shim
 * is one more place the on-device and on-host behaviors can diverge. */
#pragma once

#include <stdio.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107

/* %s for tag because firmware sources use `static const char *TAG`,
 * not a literal — the real esp_log.h is a printf wrapper that takes
 * the tag as a runtime arg, so this matches the on-device path. */
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I [%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W [%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E [%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
