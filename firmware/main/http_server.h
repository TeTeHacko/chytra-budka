/* http_server.h — local HTTP server for on-bench/LAN access.
 *
 * Endpoints:
 *   GET  /              → tiny status HTML page
 *   GET  /last.jpg      → most recent capture (camera last-JPEG cache)
 *   GET  /capture       → trigger a fresh capture, then return JPEG
 *   GET  /selftest      → re-run self-test, return JSON summary
 *
 * Server is started after WiFi has an IP. Idempotent. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_server_start(void);
void      http_server_stop(void);

/* SoftAP recovery portal. Started by app_main when the WiFi SoftAP
 * fallback engages (STA couldn't connect): a minimal PLAIN-HTTP server on
 * :80 (the AP netif is AP_IP, default 172.31.4.1) serving a GET/POST /wifi credential
 * form (and a captive-portal catch-all that shows the same form). To free
 * :80 it stops the HTTPS→HTTP redirect server if that was running. The
 * submitted creds go through the SAME candidate/verify-before-commit flow
 * as cmd/wifi, so a typo can't brick the board. */
esp_err_t http_softap_portal_start(void);
void      http_softap_portal_stop(void);

/* True after the server started in HTTPS (port 443) mode. Other
 * modules use this to decide between "http://" and "https://" in
 * URLs they publish (e.g. mqtt.c puts the photo URL in MQTT events
 * for HA's sensor.last_photo attributes). Called from any task, no
 * locking — the underlying flag flips once during http_server_start
 * and never again until http_server_stop. */
bool http_server_is_https(void);

#ifdef __cplusplus
}
#endif
