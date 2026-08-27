#include "device_id.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_mac.h"

static char s_id[32];
static char s_suffix[12];
static bool s_init = false;

static void init_once(void) {
    if (s_init) return;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        /* Lowercase hex, no separator → a valid DNS hostname label, e.g.
         * "ex01". (An earlier scheme inserted an underscore, which made
         * "<id>.<domain>" an invalid hostname.) Combined id: "cb-ex01". */
        snprintf(s_suffix, sizeof(s_suffix), "%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
        snprintf(s_id, sizeof(s_id), "%s-%s", HOSTNAME, s_suffix);
    } else {
        /* No MAC available — fall back to the project name so the
         * device still boots. Two boards in this state would clash
         * but that's strictly better than not booting at all. */
        snprintf(s_suffix, sizeof(s_suffix), "%s", "unknown");
        snprintf(s_id, sizeof(s_id), "%s", HOSTNAME);
    }
    s_init = true;
}

const char *device_id(void) {
    init_once();
    return s_id;
}

const char *device_id_suffix(void) {
    init_once();
    return s_suffix;
}

void device_url(char *out, size_t cap, const char *domain) {
    if (!out || cap == 0)
        return;
    init_once();
    /* The board's own HTTPS web UI: "https://cb-<suffix>.<domain>/".
     * `domain` is the caller's wifi_mgr_get_domain() (DHCP option 15, else the
     * compile fallback) — passed in so device_id.c keeps no dependency on
     * wifi_mgr. When empty, fall back to the bare hostname (still resolvable
     * via mDNS on the LAN). */
    if (domain && domain[0])
        snprintf(out, cap, "https://%s.%s/", s_id, domain);
    else
        snprintf(out, cap, "https://%s/", s_id);
}
