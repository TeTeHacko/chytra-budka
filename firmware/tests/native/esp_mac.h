/* Minimal esp_mac.h stub for the native host build of device_id.c.
 * The real header is ESP-IDF-only; device_id.c just needs esp_read_mac()
 * + ESP_MAC_WIFI_STA. We return a FIXED MAC with a NEUTRAL tail ab:cd:ef so
 * the derived id is the deterministic "cb-abcdef" the tests assert. (Not a
 * real board id on purpose — the public-export scrub rewrites real ids in
 * text but cannot see these byte literals, which would desync the asserts.) */
#pragma once

#include <stdint.h>

#include "esp_shim.h"   /* esp_err_t, ESP_OK, ESP_FAIL */

typedef enum { ESP_MAC_WIFI_STA = 0 } esp_mac_type_t;

static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    (void)type;
    if (!mac)
        return ESP_FAIL;
    mac[0] = 0x24; mac[1] = 0x6f; mac[2] = 0x28;   /* Espressif OUI */
    mac[3] = 0xab; mac[4] = 0xcd; mac[5] = 0xef;   /* tail → suffix "abcdef" */
    return ESP_OK;
}
