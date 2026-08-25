/* wifi_mgr.c — event-driven WiFi STA with exponential reconnect backoff.
 *
 * The simple "call esp_wifi_connect() inside STA_DISCONNECTED" pattern is
 * fine on a bench (host is always up) but a disaster in the field: when
 * the AP goes away the radio hammers reconnect attempts forever, burning
 * battery and starving other tasks for CPU. We schedule reconnect via a
 * one-shot esp_timer with exponential backoff (500 ms → 60 s cap). On
 * successful association the backoff resets.
 *
 * Init errors are surfaced rather than asserted — caller decides whether
 * to reboot, soft-degrade, or sleep + retry. */
#include "wifi_mgr.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "device_id.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/dhcp.h"     /* struct dhcp */
#include "lwip/pbuf.h"     /* pbuf_copy_partial */

/* lwIP doesn't define a symbolic name for option 15 (Domain Name) —
 * the prot/dhcp.h table stops at the options lwIP's own state machine
 * cares about (subnet/router/dns/hostname/ntp/lease). RFC 2132 §3.17. */
#define DHCP_OPT_DOMAIN_NAME 15
#include "app_config.h"    /* wifi_listen_iv knob */
#include "audiofx.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "wifi_store.h"

static const char *TAG = "wifi";

/* SSID actually applied this boot (from wifi_store_get_effective). Used by
 * the STA_START log and the MQTT status echo. The password is never
 * cached here — only the driver holds it. */
static char s_active_ssid[WIFI_STORE_SSID_CAP] = {0};

/* WiFi onboarding: a per-boot random AP password (set by
 * wifi_mgr_use_random_ap_pass before init) that overrides AP_PASS_DEFAULT when
 * no operator-custom AP creds exist. Empty = use the compiled default. */
static char s_onboard_pass[WIFI_STORE_PASS_CAP] = {0};
/* The SSID + password the AP was last configured with this boot (operator-
 * custom, onboarding-random, or default) — cached by build_ap_config so the
 * OLED onboarding QR can encode the exact join credentials. */
static char s_ap_ssid[WIFI_STORE_SSID_CAP] = {0};
static char s_ap_pass[WIFI_STORE_PASS_CAP] = {0};

#define BIT_CONNECTED   BIT0

#define BACKOFF_INITIAL_MS   500
#define BACKOFF_CAP_MS       60000

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static esp_netif_t       *s_netif          = NULL;
static esp_netif_t       *s_ap_netif       = NULL;
static bool               s_softap_active  = false;
static uint32_t           s_backoff_ms     = BACKOFF_INITIAL_MS;
static uint32_t           s_disconnect_n   = 0;
/* Set while wifi_mgr_scan() temporarily borrows the STA interface (AP-only →
 * APSTA) for a scan: suppresses the STA_START auto-connect + reconnect backoff
 * so an unconfigured STA doesn't spam connect attempts during onboarding. */
static volatile bool      s_scan_borrow_sta = false;

/* IP cached as dotted-quad string. Refreshed in the GOT_IP handler so
 * hot-path callers (mqtt_publish_photo_event) don't have to grab the
 * netif lock via esp_netif_get_ip_info() on every shot. Cleared on
 * STA_DISCONNECTED so a stale IP doesn't survive a roam. */
static char s_ip_str[20] = {0};

/* Domain name captured from DHCP option 15. Updated on every fresh
 * OFFER/ACK that carries it; otherwise stays empty and the accessor
 * falls back to CB_DOMAIN_FALLBACK. 64 B is well past the practical
 * max for a residential domain ("doma.local", "chata.lan", …). */
static char s_dhcp_domain[64] = {0};

static void schedule_reconnect(void);

static void retry_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "reconnect attempt #%" PRIu32, s_disconnect_n);
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        /* A synchronous esp_wifi_connect() failure (radio not started / busy)
         * doesn't raise STA_DISCONNECTED, so go through schedule_reconnect()
         * — which ESCALATES + caps the backoff — instead of re-arming at the
         * current interval. Otherwise a persistently-failing connect retries
         * at a fixed rate forever, burning power and never backing off. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s — rescheduling (backoff)", esp_err_to_name(e));
        schedule_reconnect();
    }
}

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* Bench-only: hold the STA down (suppress auto-reconnect) until this time,
 * so the SoftAP fallback can be exercised over RF without the good home AP
 * immediately reconnecting + tearing the AP back down. */
static int64_t s_test_hold_until_us = 0;
static bool test_hold_active(void) {
    if (s_test_hold_until_us == 0) return false;
    if (esp_timer_get_time() < s_test_hold_until_us) return true;
    s_test_hold_until_us = 0;
    return false;
}
void wifi_mgr_test_hold_sta_down(int seconds) {
    s_test_hold_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGW(TAG, "DEBUG: holding STA down %d s for SoftAP RF test", seconds);
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
}
#endif

static void schedule_reconnect(void) {
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
    if (test_hold_active()) {
        ESP_LOGI(TAG, "reconnect suppressed (SoftAP test hold)");
        return;
    }
#endif
    esp_timer_stop(s_retry_timer);  /* idempotent if not running */
    ESP_LOGI(TAG, "scheduling reconnect in %" PRIu32 " ms", s_backoff_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
    /* Double for next time, capped. */
    uint32_t next = s_backoff_ms * 2;
    if (next > BACKOFF_CAP_MS) next = BACKOFF_CAP_MS;
    s_backoff_ms = next;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        if (s_scan_borrow_sta) {
            ESP_LOGI(TAG, "STA up for scan only — auto-connect suppressed");
            return;
        }
        ESP_LOGI(TAG, "STA start, connecting to '%s'", s_active_ssid);
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_scan_borrow_sta) return;  /* scan-borrow: no reconnect backoff */
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        status_led_wifi_connected(false);
        s_ip_str[0] = '\0';  /* drop cached IP — next GOT_IP refreshes */
        s_disconnect_n++;
        /* Log every disconnect for the first 5, then every 20th — keeps
         * the bench loud but doesn't spam the log when the AP is gone for
         * an hour. */
        if (s_disconnect_n <= 5 || (s_disconnect_n % 20) == 0) {
            ESP_LOGW(TAG, "disconnected (#%" PRIu32 "), next retry in %" PRIu32 " ms",
                     s_disconnect_n, s_backoff_ms);
        }
        schedule_reconnect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP: " IPSTR " (after %" PRIu32 " disconnects)",
             IP2STR(&evt->ip_info.ip), s_disconnect_n);
    /* Refresh cached IP string for wifi_mgr_get_ip_str() callers. */
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
    /* One-shot log: which domain ended up effective. Either DHCP option 15
     * filled s_dhcp_domain by this point (lwip_dhcp_on_extra_option runs
     * during OFFER/ACK before GOT_IP fires), or we fall back. tls_enroll
     * will build the FQDN from whatever wifi_mgr_get_domain() returns. */
    ESP_LOGI(TAG, "domain: '%s' (%s)", wifi_mgr_get_domain(),
             s_dhcp_domain[0] ? "from DHCP option 15" : "fallback (DHCP omitted)");
    /* Reset backoff and stop any pending retry — we're connected. */
    s_backoff_ms = BACKOFF_INITIAL_MS;
    esp_timer_stop(s_retry_timer);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
    status_led_wifi_connected(true);
}

static esp_err_t bring_up_ap(bool keep_sta, char *ssid_out, size_t cap);

esp_err_t wifi_mgr_init(bool ap_mode) {
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    esp_err_t e;
    if ((e = esp_netif_init()) != ESP_OK) return e;
    if ((e = esp_event_loop_create_default()) != ESP_OK) return e;

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_cb,
        .name     = "wifi_retry",
    };
    if ((e = esp_timer_create(&timer_args, &s_retry_timer)) != ESP_OK) return e;

    /* AP bring-up (no station): either the sticky operator-selected AP-only
     * mode, or the caller forcing it for the unprovisioned first-boot portal
     * (no usable STA target). Both skip the STA netif + creds below. */
    const bool ap_only = wifi_store_is_ap_only() || ap_mode;
    if (!ap_only) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) return ESP_ERR_NO_MEM;
        esp_netif_set_hostname(s_netif, device_id());
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) return e;

    if ((e = esp_event_handler_instance_register(
             WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL)) != ESP_OK)
        return e;
    if ((e = esp_event_handler_instance_register(
             IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL, NULL)) != ESP_OK)
        return e;

    if (ap_only) {
        ESP_LOGW(TAG,
                 "AP-ONLY MODE — no station; MQTT / OTA / remote recovery "
                 "DISABLED. Exit via the local web /config toggle or a "
                 "BOOT-button factory reset.");
        char apssid[WIFI_STORE_SSID_CAP] = {0};
        if ((e = bring_up_ap(/*keep_sta*/ false, apssid, sizeof(apssid))) != ESP_OK) return e;
        if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return e;
        if ((e = esp_wifi_start()) != ESP_OK) return e;
        snprintf(s_active_ssid, sizeof(s_active_ssid), "%s", apssid);
        ESP_LOGW(TAG, "AP-only UP: ssid='%s' — http://" AP_IP "/", apssid);
        return ESP_OK;
    }

    /* Credentials are no longer hard-compiled: wifi_store resolves the
     * effective set (candidate → known-good NVS → compile-time secrets.h
     * default). secrets.h remains the immutable floor so the board can
     * never end up with no creds. See wifi_store.h. */
    char ssid[WIFI_STORE_SSID_CAP] = {0};
    char pass[WIFI_STORE_PASS_CAP] = {0};
    wifi_creds_src_t src = WIFI_CREDS_DEFAULT;
    wifi_store_get_effective(ssid, sizeof(ssid), pass, sizeof(pass), &src);
    snprintf(s_active_ssid, sizeof(s_active_ssid), "%s", ssid);

    wifi_config_t wcfg = {0};
    /* memcpy (not strncpy) so a full 32-char SSID fills the field without a
     * required NUL, and to avoid -Werror=stringop-truncation. wcfg is
     * zero-initialised, so a shorter ssid/pass stays NUL-terminated. */
    size_t sl = strnlen(ssid, sizeof(wcfg.sta.ssid));
    memcpy(wcfg.sta.ssid, ssid, sl);
    size_t pl = strnlen(pass, sizeof(wcfg.sta.password));
    memcpy(wcfg.sta.password, pass, pl);
    /* Open networks (empty PSK) need OPEN authmode; otherwise WPA2-PSK
     * minimum. The candidate path allows an empty password for open APs. */
    wcfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* PMF (Protected Management Frames) policy: capable=true advertises
     * support so a WPA3-SAE / WPA2-PSK+PMF AP will negotiate the
     * stronger management-frame protection. required=false keeps us
     * compatible with legacy APs that still don't speak PMF — the IoT
     * VLAN currently has both. Flip required to true only after the
     * deployment AP is confirmed PMF-capable; otherwise the radio sits
     * in the exponential reconnect loop with no useful error. */
    wcfg.sta.pmf_cfg.capable    = true;
    wcfg.sta.pmf_cfg.required   = false;
    /* AP selection: the default (zero-init) is WIFI_FAST_SCAN, which connects to
     * the FIRST AP it finds with this SSID — whichever answers first by channel
     * order, NOT the strongest. With multiple APs on one SSID (a mesh / several
     * APs around a site, e.g. the chata) that routinely latches onto a far,
     * weak AP → marginal RSSI, retransmits, dropouts, slow reconnect (and on a
     * solar unit, wasted TX power). Scan ALL channels and pick the STRONGEST
     * BSSID by RSSI instead. Costs ~1-2 s extra at connect (full scan) — cheap
     * for a fixed install, and the stable link pays it back many times over. */
    wcfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wcfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* NO rssi threshold: BY_SIGNAL already associates with the STRONGEST AP, and
     * a floor here only risks rejecting it too. An earlier -82 floor did exactly
     * that — in a site where every AP was weaker than -82 the bench couldn't
     * associate at all (HIL provisioning failed, board offline). Connect to the
     * best available, however weak; the watchdog/reconnect handle a dead link. */

    /* Listen interval (beacons skipped between modem wakes). Only honoured under
     * WIFI_PS_MAX_MODEM = Safe mode; other modes use MIN_MODEM/NONE. Higher = deeper
     * doze, lower power, higher downlink latency. Applied here at connect, so the
     * wifi_listen_iv knob takes effect on the next reconnect/reboot. Schema clamps
     * 1..10, default 3 (= IDF default). See app_config.c. */
    wcfg.sta.listen_interval = (uint16_t)app_config_get_int("wifi_listen_iv");

    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return e;
    if ((e = esp_wifi_set_config(WIFI_IF_STA, &wcfg)) != ESP_OK) return e;
    if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return e;
    if ((e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM)) != ESP_OK) return e;
    if ((e = esp_wifi_start()) != ESP_OK) return e;

    ESP_LOGI(TAG, "init done (initial backoff %d ms, cap %d ms)",
             BACKOFF_INITIAL_MS, BACKOFF_CAP_MS);
    return ESP_OK;
}

bool wifi_mgr_is_connected(void) {
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms) {
    if (!s_events) return false;
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED, pdFALSE, pdFALSE, ticks);
    return (bits & BIT_CONNECTED) != 0;
}

const char *wifi_mgr_get_ssid(void) {
    return s_active_ssid;
}

bool wifi_mgr_softap_active(void) {
    return s_softap_active;
}

int wifi_mgr_ap_sta_count(void) {
    if (!s_softap_active) return 0;
    wifi_sta_list_t list = {0};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

void wifi_mgr_use_random_ap_pass(void) {
    /* 12 chars from an unambiguous lowercase+digit alphabet (no 0/o/1/l) —
     * ~60 bits of entropy, trivial to retype if the QR scan fails, and free of
     * WIFI: QR meta characters so it needs no escaping. esp_fill_random is
     * hardware-seeded; for a locally-displayed, per-boot onboarding secret
     * that is more than sufficient. The value is never logged. */
    static const char A[] = "abcdefghijkmnpqrstuvwxyz23456789";
    uint8_t r[12];
    esp_fill_random(r, sizeof(r));
    char p[sizeof(r) + 1];
    for (size_t i = 0; i < sizeof(r); i++)
        p[i] = A[r[i] % (sizeof(A) - 1)];
    p[sizeof(r)] = '\0';
    snprintf(s_onboard_pass, sizeof(s_onboard_pass), "%s", p);
    ESP_LOGI(TAG, "onboarding: generated a %u-char random AP password",
             (unsigned)sizeof(r));
}

bool wifi_mgr_get_ap_creds(char *ssid, size_t scap, char *pass, size_t pcap) {
    if (!s_ap_ssid[0]) return false;   /* no AP configured this boot */
    if (ssid) snprintf(ssid, scap, "%s", s_ap_ssid);
    if (pass) snprintf(pass, pcap, "%s", s_ap_pass);
    return true;
}

/* Fill `ap` from operator-set AP creds (wifi_store) or the fixed default
 * (SSID cb-<suffix> per AP_SSID_FMT, pass AP_PASS_DEFAULT). The default pass is a
 * known public string on purpose — see config.h; the /config UI warns whenever
 * it's in use. The chosen SSID is copied into ssid_out for logging/status. */
static void build_ap_config(wifi_config_t *ap, char *ssid_out, size_t cap) {
    char ssid[WIFI_STORE_SSID_CAP], pass[WIFI_STORE_PASS_CAP];
    bool custom = false;
    wifi_store_get_ap(ssid, sizeof(ssid), pass, sizeof(pass), &custom);
    if (!custom) {
        snprintf(ssid, sizeof(ssid), AP_SSID_FMT, device_id_suffix());
        /* Onboarding: a per-boot random password (wifi_mgr_use_random_ap_pass)
         * supersedes the public default so an unprovisioned board isn't
         * reachable on the well-known credential. The operator gets it from
         * the on-screen QR. Operator-custom AP creds (above) still win. */
        snprintf(pass, sizeof(pass), "%s",
                 s_onboard_pass[0] ? s_onboard_pass : AP_PASS_DEFAULT);
    }
    /* Cache the effective creds for the OLED onboarding QR (so it encodes the
     * exact SSID + password clients must use, without re-deriving the ladder). */
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", pass);
    memset(ap, 0, sizeof(*ap));
    size_t slen = strnlen(ssid, sizeof(ap->ap.ssid));
    memcpy(ap->ap.ssid, ssid, slen);
    ap->ap.ssid_len = (uint8_t)slen;
    size_t pl = strnlen(pass, sizeof(ap->ap.password));
    memcpy(ap->ap.password, pass, pl);
    /* Empty pass → open AP; else WPA2-PSK. */
    ap->ap.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Don't *require* PMF (802.11w) so non-PMF clients can still associate.
     * NOTE: in IDF v6 `pmf_cfg.capable` is DEPRECATED + forced — the AP
     * always offers PMF to any client that advertises it, so PMF can't be
     * turned off AP-side. A compliant client completes the SA-Query and
     * connects fine. A broken-PMF client (observed: the RTL8188EUS/aircrack
     * USB dongle used for bench RF tests advertises PMF but never answers
     * SA-Query) gets disassociated (reason 209 SA_QUERY_TIMEOUT) before
     * DHCP — that is the DONGLE's bug, not ours; phones are fine. */
    ap->ap.pmf_cfg.required = false;
    ap->ap.max_connection = AP_MAX_CONN;
    ap->ap.channel = AP_CHANNEL;
    if (ssid_out) snprintf(ssid_out, cap, "%s", ssid);
}

/* Configure the AP netif: relocate it to AP_IP (off 192.168/16 — see config.h)
 * and advertise the captive-portal URI (opt 114) so a joining phone auto-opens
 * the /wifi portal. The DHCP lease pool is derived from AP_IP by dhcps.
 *
 * Ordering matters. Changing the netif IP needs dhcps STOPPED, so we stop →
 * set IP + option → start it EXPLICITLY. We must NOT rely on the AP_START
 * auto-start here: esp_netif only auto-starts dhcps from its post-create INIT
 * state (esp_netif_lwip.c:1257); once we've stopped it the state is STOPPED, so
 * AP_START would skip it and every client would hang on "obtaining IP address"
 * (the bug this path had before, masked because the bench RTL8188EUS dongle died
 * at PMF SA-Query before DHCP). The explicit start below puts it back to STARTED
 * with the new address, and AP_START then leaves the running server alone. */
static void ap_configure_netif(void) {
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip = {0};
    esp_netif_str_to_ip4(AP_IP, &ip.ip);
    esp_netif_str_to_ip4(AP_IP, &ip.gw);
    esp_netif_str_to_ip4(AP_NETMASK, &ip.netmask);
    esp_err_t e = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (e != ESP_OK) ESP_LOGE(TAG, "AP set_ip_info(%s): %s", AP_IP, esp_err_to_name(e));
    /* Advertise ourselves as the DNS server (DHCP option 6) so a joining
     * client sends its OS connectivity-check lookups to our captive DNS
     * responder (dns_hijack) — required for the portal to auto-open instead of
     * the operator having to type the AP IP. Must be set with dhcps stopped
     * (it is, above). 0x02 = OFFER_DNS (lwip dhcpserver.h). */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4(AP_IP, &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 0x02;  /* OFFER_DNS */
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    static const char CP_URI[] = "http://" AP_IP "/wifi";
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)CP_URI, sizeof(CP_URI) - 1);
    if ((e = esp_netif_dhcps_start(s_ap_netif)) != ESP_OK)
        ESP_LOGE(TAG, "AP dhcps_start: %s — clients won't get an IP", esp_err_to_name(e));
}

/* Bring the AP up. keep_sta=true → APSTA (recovery fallback; STA keeps
 * trying underneath). false → AP only (full AP-only mode, no station). */
static esp_err_t bring_up_ap(bool keep_sta, char *ssid_out, size_t cap) {
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_ERR_NO_MEM;
    ap_configure_netif();
    wifi_config_t ap;
    build_ap_config(&ap, ssid_out, cap);
    esp_err_t e;
    if ((e = esp_wifi_set_mode(keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP)) != ESP_OK) return e;
    if ((e = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK) return e;
    s_softap_active = true;
    return e;
}

esp_err_t wifi_mgr_start_softap(void) {
    if (s_softap_active) return ESP_OK;
    char ssid[WIFI_STORE_SSID_CAP] = {0};
    esp_err_t e = bring_up_ap(/*keep_sta*/ true, ssid, sizeof(ssid));
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "SoftAP fallback UP: ssid='%s' — join + open http://" AP_IP "/wifi", ssid);
        audiofx_ap();
    }
    return e;
}

esp_err_t wifi_mgr_stop_softap(void) {
    if (!s_softap_active) return ESP_OK;
    s_softap_active = false;
    /* Tear down the AP dhcps server so it isn't left bound on a now
     * station-only radio after the recovery AP drops. The netif itself is
     * kept (NULL-checked, reused by the next bring_up_ap → ap_configure_netif
     * restarts dhcps), so this doesn't leak across repeated recovery cycles. */
    if (s_ap_netif) esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "stop_softap: set_mode(STA) %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "SoftAP fallback DOWN — back to STA only");
    return e;
}

int wifi_mgr_rssi(void) {
    if (!wifi_mgr_is_connected()) return 0;
    wifi_ap_record_t info = {0};
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) return 0;
    return info.rssi;
}

bool wifi_mgr_get_bssid(char *out, size_t out_cap) {
    if (!out || out_cap < 18) return false;
    if (!wifi_mgr_is_connected()) return false;
    wifi_ap_record_t info = {0};
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) return false;
    snprintf(out, out_cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);
    return true;
}

int wifi_mgr_scan(wifi_scan_ap_t *out, int max) {
    if (!out || max <= 0) return -1;
    /* Scanning needs the STA interface up. In pure AP-only / unprovisioned mode
     * (WIFI_MODE_AP — the onboarding portal, exactly where picking your home
     * WiFi is most useful) there's no station, so temporarily borrow one by
     * switching to APSTA for the scan and switching back to AP afterwards. The
     * AP interface (and its dhcps + the client's TCP) survives the mode change;
     * s_scan_borrow_sta suppresses the STA auto-connect so the unconfigured
     * station doesn't spam connect attempts during the borrow. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return -1;
    bool borrowed = false;
    if (mode == WIFI_MODE_AP) {
        s_scan_borrow_sta = true;
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            s_scan_borrow_sta = false;
            return -1;
        }
        borrowed = true;
    } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        return -1;  /* WIFI_MODE_NULL — wifi not started */
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, /*block*/ true);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(e));
        if (borrowed) { esp_wifi_set_mode(WIFI_MODE_AP); s_scan_borrow_sta = false; }
        return -1;
    }
    /* All locals declared up front so the cleanup `goto restore` never jumps
     * over a variable initialisation. */
    int n = -1;
    wifi_ap_record_t *recs = NULL;
    uint16_t found = 0, got = 0;

    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) { n = 0; goto restore; }
    recs = calloc(found, sizeof(*recs));
    if (!recs) {
        uint16_t z = 0;
        esp_wifi_scan_get_ap_records(&z, NULL);  /* drain driver list */
        goto restore;
    }
    got = found;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) goto restore;

    /* De-dup by SSID (keep strongest RSSI), skipping hidden/empty SSIDs. */
    n = 0;
    for (uint16_t i = 0; i < got; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;
        int dup = -1;
        for (int j = 0; j < n; j++) {
            if (strncmp(out[j].ssid, ssid, sizeof(out[j].ssid)) == 0) { dup = j; break; }
        }
        if (dup >= 0) {
            if (recs[i].rssi > out[dup].rssi) {
                out[dup].rssi = recs[i].rssi;
                out[dup].authmode = (uint8_t)recs[i].authmode;
            }
            continue;
        }
        if (n >= max) continue;
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
        out[n].rssi = recs[i].rssi;
        out[n].authmode = (uint8_t)recs[i].authmode;
        n++;
    }
    /* Sort by RSSI descending (strongest first) — insertion sort, n <= max. */
    for (int i = 1; i < n; i++) {
        wifi_scan_ap_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }

restore:
    free(recs);  /* free(NULL) is a no-op */
    if (borrowed) {
        esp_wifi_set_mode(WIFI_MODE_AP);  /* give the STA back; AP stays up */
        s_scan_borrow_sta = false;
    }
    return n;
}

bool wifi_mgr_get_ip_str(char *out, size_t out_cap) {
    if (!out || out_cap < sizeof(s_ip_str)) return false;
    /* Read the cache populated in on_ip(). No netif lock needed on the
     * hot path. s_ip_str is empty when not associated; getters treat
     * that as failure to discourage stale reads. */
    if (s_ip_str[0] == '\0') return false;
    size_t n = strlen(s_ip_str);
    if (n + 1 > out_cap) return false;
    memcpy(out, s_ip_str, n + 1);
    return true;
}

/* lwIP extra-option hook. Enabled via
 * CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_CUSTOM=y in sdkconfig.defaults
 * (without it the weak default kicks in and only logs at DEBUG).
 *
 * Called on the tcpip_thread for every DHCP option in OFFER/ACK that
 * isn't handled by lwIP's built-in parser. We only care about
 * option 15 (Domain Name); everything else gets ignored.
 *
 * Caveat: upstream lwIP's DHCP DISCOVER parameter request list (opt
 * 55) doesn't include option 15. A strict DHCP server (one that only
 * sends explicitly-requested options) won't supply it. dnsmasq + ISC
 * dhcpd send option 15 unconditionally when configured, so the field
 * unit's existing DHCP setup works without firmware-side parameter
 * request list extension. See HTTPS.md "Domain discovery" for the
 * full caveat + fallback chain. */
void lwip_dhcp_on_extra_option(struct dhcp *dhcp, uint8_t state,
                                uint8_t option, uint8_t len,
                                struct pbuf *p, uint16_t offset) {
    (void)dhcp; (void)state;
    if (option != DHCP_OPT_DOMAIN_NAME || len == 0) return;
    size_t n = (len < sizeof(s_dhcp_domain) - 1) ? len
                                                  : sizeof(s_dhcp_domain) - 1;
    pbuf_copy_partial(p, s_dhcp_domain, n, offset);
    s_dhcp_domain[n] = 0;
    ESP_LOGI(TAG, "DHCP option 15 (domain) = '%s'", s_dhcp_domain);
}

const char *wifi_mgr_get_domain(void) {
    if (s_dhcp_domain[0]) return s_dhcp_domain;
#ifdef CB_DOMAIN_FALLBACK
    return CB_DOMAIN_FALLBACK;
#else
    return "";
#endif
}

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
void wifi_mgr_force_disconnect(void) {
    ESP_LOGW(TAG, "DEBUG: forcing disconnect");
    esp_err_t e = esp_wifi_disconnect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(e));
    }
}
#endif
