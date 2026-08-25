/* dns_hijack.h — minimal captive-portal DNS responder.
 *
 * A UDP:53 task that answers EVERY inbound A query with one fixed IPv4
 * address (the SoftAP gateway, AP_IP). When the onboarding AP is up, a phone
 * that joins resolves its OS connectivity-check host (connectivitycheck.
 * gstatic.com / captive.apple.com / www.msftconnecttest.com / …) to the box;
 * the HTTP server's captive 404 handler then serves the /wifi form, which
 * trips the OS "sign in to network" portal automatically — no typed URL.
 *
 * Only meaningful while the SoftAP is up. Started by main.cpp in AP mode and
 * torn down with the AP. Non-A queries (AAAA, etc.) get a NODATA reply so the
 * client falls back to IPv4 rather than seeing a malformed answer.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the responder, answering all A queries with `resolve_ip` (dotted
 * quad, e.g. AP_IP). Idempotent — a second call while running is a no-op. */
esp_err_t dns_hijack_start(const char *resolve_ip);

/* Stop the responder; the task exits within ~1 s and frees its socket. */
void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
