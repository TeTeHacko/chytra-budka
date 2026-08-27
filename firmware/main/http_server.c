/* http_server.c — local HTTP endpoints. See http_server.h. */
#include "http_server.h"

#include "app_config.h"
#include "ble.h"
#include "ble_store.h"
#include "uart_servo.h"

#include <stdio.h>
#include <string.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_main_exports.h"
#include "audio.h"
#include "battery.h"
#include "camera.h"
#include "exif_read.h"
#include "config.h"
#include "glitchtip.h"
#include "secrets.h"
#include "secret_helpers.h"
#include "auth_store.h"
#include "tls_enroll.h"
#include "tls_store.h"
#include "wifi_mgr.h"
#include "wifi_store.h"
#include "i18n.h"
#include "sd_layout.h"
#include "ota.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "device_id.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cb_time.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "i2c_bb.h"
#include "i2c_bus.h"
#include "mqtt.h"
#include "pir.h"
#include "reed.h"
#include "soil.h"
#include "sonar.h"
#include "oled.h"
#include "sd_storage.h"
#include "sensors.h"
#include "text_util.h"
#include "wifi_mgr.h"

static const char *TAG = "http";

/* BLE web-UI helpers shared by root_get (a summary table of connected meters)
 * and the /ble management page, both defined later in this file. */
#define BLE_VIEW_MAX 24
/* Bytes of a JPEG to peek when reading EXIF: the APP1 segment jpeg_stamp.c
 * writes sits right after SOI and is ≤ ~1.1 KB, so 2 KB is ample headroom and
 * avoids copying/parsing the whole (UXGA) frame. */
#define EXIF_PEEK_BYTES 2048

/* Scratch buffer for non-DMA per-request work (EXIF header peeks, SD file
 * reads). Prefer PSRAM so we don't add pressure to the scarce internal DRAM
 * that the BT controller + HW-AES (TLS handshakes) compete for — small plain
 * malloc()s land in internal DRAM (SPIRAM_MALLOC_ALWAYSINTERNAL=4096). Falls
 * back to internal if PSRAM is full. Free with the normal free(). */
static void *scratch_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static void ble_values_html(const ble_dev_view_t *v, char *out, size_t cap);
static void scan_html_escape(const char *s, char *out, size_t cap);
static void scan_url_decode(const char *s, char *out, size_t cap);

static httpd_handle_t s_server       = NULL;
static httpd_handle_t s_redirect_srv = NULL;
/* True after http_server_start() came up over TLS. HSTS + the Basic
 * auth gate are conditional on this — when the board is in the
 * pre-enrollment HTTP fallback (no cert yet), we don't ship HSTS (it
 * would lock the browser into a non-existent HTTPS) and we don't ask
 * for Basic credentials (the wire is plaintext, so collecting them
 * would be worse than letting LAN users in unauthenticated). */
static bool           s_https_active = false;
/* Cert+key blob outlives the httpd config struct because esp_https_server
 * copies the bytes into its own arena at start (see https_server.c). We
 * still keep the blob alive for the lifetime of the server so a future
 * reload path can re-use it. NULL when running plain HTTP. */
static tls_store_blob_t *s_tls_blob  = NULL;

/* True when HTTP_BASIC_USER/PASS in secrets.h aren't placeholders. The
 * gate also requires s_https_active — basic creds over plaintext is
 * worse than no gate, since the password is recoverable by anybody on
 * the LAN.
 *
 * TODO(fail-closed): when HTTPS is active and placeholders are still
 * in secrets.h, log a loud ESP_LOGW at startup (or refuse to register
 * sensitive handlers). Today a default-config production build serves
 * /mic.wav etc. unauthenticated over HTTPS, which contradicts the
 * "paranoid" deployment posture. Tracked in HTTPS.md follow-ups. */
static bool basic_auth_enabled(void) {
    if (!s_https_active) return false;
    /* Effective creds = operator-set NVS override (via /config or cmd/auth)
     * else the compile-time secrets.h default. */
    char user[AUTH_STORE_USER_CAP], pass[AUTH_STORE_PASS_CAP];
    auth_store_get_effective(user, sizeof(user), pass, sizeof(pass));
    return !secret_is_placeholder(user) && !secret_is_placeholder(pass);
}

/* Set Strict-Transport-Security on HTTPS responses. max-age=15552000 =
 * 180 days; includeSubDomains is safe because *.lan / *.lan aren't
 * also served plain HTTP outside the budka fleet. Skip on plain HTTP
 * (pre-enrollment) so a browser doesn't pin to a non-existent HTTPS
 * endpoint. Idempotent — handlers can call this in addition to
 * basic_auth_gate; the order doesn't matter. */
static void apply_hsts(httpd_req_t *req) {
    if (!s_https_active) return;
    httpd_resp_set_hdr(req, "Strict-Transport-Security",
                       "max-age=15552000; includeSubDomains");
}

/* Check HTTP Basic auth on the incoming request. Returns:
 *   ESP_OK   — gate disabled OR credentials valid
 *   ESP_FAIL — 401 sent; caller must return ESP_OK (NOT ESP_FAIL) so
 *              the httpd loop doesn't log a spurious error
 * Constant-time strcmp not used — the credential is short, the LAN
 * threat model isn't a timing-attack side-channel; if that changes,
 * swap for a CRYPTO_memcmp. */
static esp_err_t basic_auth_gate(httpd_req_t *req) {
    if (!basic_auth_enabled()) return ESP_OK;

    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        goto deny;
    }
    /* Expect "Basic <base64>" — anything else (Bearer …, Digest …)
     * gets refused with WWW-Authenticate so the client retries with
     * Basic. Case-insensitive scheme per RFC 7235 §2.1. */
    const char *p = hdr;
    if (strncasecmp(p, "Basic ", 6) != 0) goto deny;
    p += 6;
    while (*p == ' ') p++;

    /* Decode base64 → "USER:PASS". 96 bytes covers a 64-char user +
     * 64-char pass + ':' + slack. Anything longer gets rejected. */
    unsigned char dec[96];
    size_t dec_len = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dec_len,
                              (const unsigned char *)p, strlen(p)) != 0) {
        goto deny;
    }
    dec[dec_len] = 0;
    char *colon = strchr((char *)dec, ':');
    if (!colon) goto deny;
    *colon = 0;
    const char *user = (const char *)dec;
    const char *pass = colon + 1;

    char user_exp[AUTH_STORE_USER_CAP], pass_exp[AUTH_STORE_PASS_CAP];
    auth_store_get_effective(user_exp, sizeof(user_exp), pass_exp, sizeof(pass_exp));
    if (strcmp(user, user_exp) == 0 && strcmp(pass, pass_exp) == 0) {
        return ESP_OK;
    }

deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    /* Realm string just labels the credential prompt in the browser
     * dialog — "chytra-budka" is enough to disambiguate from any other
     * site the LAN user has bookmarks for. */
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"chytra-budka\", charset=\"UTF-8\"");
    apply_hsts(req);
    httpd_resp_send(req, "auth required\n", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

/* One-liner used at the top of sensitive handlers: enforce the gate
 * and apply HSTS in one go. Returns the value the caller should return
 * (ESP_OK to short-circuit on a 401, since the httpd loop interprets
 * ESP_FAIL as "handler failed, drop the socket"). */
#define HTTP_AUTH_OR_RETURN(req) do {                       \
    if (basic_auth_gate((req)) != ESP_OK) return ESP_OK;    \
    apply_hsts((req));                                      \
} while (0)

static void format_mac(char *out, size_t out_sz) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(out, out_sz, "??:??:??:??:??:??");
        return;
    }
    snprintf(out, out_sz, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

/* Uptime in operator-readable form: "3d 4h 12m", collapsing leading zero
 * units down to "4h 12m" / "12m 5s" / "45s" so a fresh boot reads sanely too.
 * (MQTT/EXIF keep raw seconds for machine parsing; this is for the web UI.) */
static void fmt_uptime(char *out, size_t cap, int64_t secs) {
    if (secs < 0) secs = 0;
    int d = (int)(secs / 86400);
    int h = (int)((secs % 86400) / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    if (d)      snprintf(out, cap, "%dd %dh %dm", d, h, m);
    else if (h) snprintf(out, cap, "%dh %dm", h, m);
    else if (m) snprintf(out, cap, "%dm %ds", m, s);
    else        snprintf(out, cap, "%ds", s);
}

/* Status-row glyphs (shared by the merged Status table): green tick when a
 * subsystem is ready, amber em-dash otherwise. */
#define ST_TICK "&#10003;"
#define ST_DASH "&mdash;"

/* Map a pin-slot function name to a small emoji (HTML entity, so the source
 * stays ASCII) for the homepage pin-map. Icons mirror the HA dashboard's MDI
 * choices (ha/chytra-budka-strategy.js) so the two UIs read the same. Match
 * is substring, ordered most- to least-specific — note pir/i2s are checked
 * before the generic "ir"/"i2c" so they don't get the wrong icon. */
static const char *pin_fn_icon(const char *fn) {
    if (!fn || !strcmp(fn, "none")) return "";
    if (strstr(fn, "pir") || strstr(fn, "motion"))  return "&#128694;&#65039; "; /* 🚶 motion  (HA motion-sensor) */
    if (strstr(fn, "reed") || strstr(fn, "door"))   return "&#128682; ";          /* 🚪 door    (HA door)         */
    if (strstr(fn, "cam"))                          return "&#128247; ";          /* 📷 camera  (HA camera)       */
    if (strstr(fn, "mic") || strstr(fn, "pdm")
        || strstr(fn, "i2s") || strstr(fn, "vad")
        || strstr(fn, "audio"))                     return "&#127897;&#65039; ";  /* 🎙 audio   (HA waveform)     */
    if (strstr(fn, "pcm"))                          return "&#128266; ";          /* 🔊 PCM speaker (PDM 1-bit DAC)*/
    if (strstr(fn, "buzzer"))                       return "&#128276; ";          /* 🔔 buzzer  (LEDC beeper)     */
    if (strstr(fn, "ir"))                           return "&#127769; ";          /* 🌙 IR/night(HA weather-night)*/
    /* "oled" BEFORE "led" — oled_pwr contains the substring "led" and would
     * otherwise grab the LED bulb icon. */
    if (strstr(fn, "oled"))                         return "&#128421;&#65039; ";  /* 🖥 OLED power gate           */
    if (strstr(fn, "led") || strstr(fn, "status"))  return "&#128161; ";          /* 💡 LED     (HA led-on)       */
    if (strstr(fn, "servo"))                        return "&#9881;&#65039; ";    /* ⚙ servo   (HA cog-transfer) */
    if (strstr(fn, "button"))                       return "&#128280; ";          /* 🔘 push-button (cycles OLED) */
    if (strstr(fn, "uart") || strstr(fn, "tx")
        || strstr(fn, "rx"))                        return "&#128268; ";          /* 🔌 serial  (HA serial-port)  */
    if (strstr(fn, "i2c") || strstr(fn, "sda")
        || strstr(fn, "scl"))                       return "&#128279; ";          /* 🔗 I²C bus                   */
    return "&#128205; ";                                                          /* 📍 generic GPIO (HA gpio)    */
}

/* Single source of truth for page styling: a dark, mobile-first theme served
 * once at /style.css and <link>ed from every page (homepage, /config, /wifi,
 * replies). Beats inlining the same CSS into each page — one copy in flash,
 * and the browser caches it across the multi-page session. Mobile: 16px form
 * controls (no iOS focus-zoom), 44px tap targets, full-width fields, fluid
 * layout. Keep the :root palette as the canonical theme. */
static const char STYLE_CSS[] =
    ":root{--bg:#0f1115;--fg:#d4d4d4;--muted:#9aa0a6;--accent:#7fc4ff;"
    "--code-bg:#1a1d23;--row-alt:#161922;--border:#262a33;--field:#161922;"
    "--ok:#3fb950;--warn:#d29922;--ok-bg:#0f2f1a;--ok-fg:#7ee787;"
    "--warn-bg:#2d2607;--warn-fg:#e3b341}"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,-apple-system,sans-serif;max-width:42em;margin:0 auto;"
    "padding:1em;color:var(--fg);background:var(--bg);line-height:1.5}"
    "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}"
    "h1{margin-bottom:0}h1 small{color:var(--muted);font-size:.6em;font-weight:normal}"
    /* Unified page header (logo + title + cross-page pill nav with an active-
     * page highlight), shared by /, /config and /ble via send_page_head(). */
    "header.hdr{display:flex;align-items:center;gap:.75em;flex-wrap:wrap;"
    "margin:0 0 1.2em;padding:.4em 0 .7em;border-bottom:1px solid var(--border)}"
    ".brand{display:flex;align-items:center;gap:.55em;color:var(--fg);text-decoration:none}"
    ".brand:hover{text-decoration:none}"
    ".logo{width:34px;height:34px;display:block;flex:none}"
    ".btitle{font-size:1.3em;font-weight:600;line-height:1.05}"
    ".btitle small{color:var(--muted);font-size:.62em;font-weight:normal}"
    ".topnav{margin-left:auto;display:flex;gap:.4em;flex-wrap:wrap}"
    ".topnav a{padding:.42em .9em;border:1px solid var(--border);border-radius:999px;"
    "background:var(--code-bg);color:var(--accent);font-size:.95em;font-weight:500}"
    ".topnav a:hover{text-decoration:none;border-color:var(--accent);background:#1f2a37}"
    ".topnav a.cur{background:var(--accent);color:#0b0d12;border-color:var(--accent)}"
    "@media(max-width:30em){.topnav{width:100%;margin-left:0}.topnav a{flex:1;text-align:center}}"
    "h2{margin-top:1.5em;border-bottom:1px solid var(--border);padding-bottom:.2em}"
    "h3{margin-top:1.3em;border-bottom:1px solid var(--border);padding-bottom:.2em}"
    "code{background:var(--code-bg);padding:.1em .3em;border-radius:3px}"
    "label{display:block;margin:.35em 0}"
    "input,select{width:100%;font-size:16px;padding:.55em;margin-top:.25em;color:var(--fg);"
    "background:var(--field);border:1px solid var(--border);border-radius:8px}"
    "button{font-size:16px;padding:.6em 1em;margin:.2em 0;background:#1f2a37;color:var(--fg);"
    "border:1px solid var(--border);border-radius:8px;cursor:pointer;min-height:44px}"
    "button:hover{background:#2a3441}"
    "small{color:var(--muted)}"
    "details{border:1px solid var(--border);border-radius:8px;margin:.4em 0;padding:0 .7em}"
    "summary{cursor:pointer;padding:.6em 0;font-weight:bold}"
    "details[open]{padding-bottom:.4em}"
    /* Security warning strips: red, one per line (each is its own block div),
     * spaced so stacked warnings read as a list. */
    ".warn-bar{background:#7f1d1d;color:#fee2e2;border:1px solid #ef4444;"
    "border-radius:8px;padding:.6em .8em;margin:0 0 .5em;font-size:.95em}"
    ".warn-bar a{color:#fff;text-decoration:underline;display:block}"
    /* WiFi scan picker list — finger-friendly: tall, well-spaced tap-card rows
     * with a big bold SSID. Don't crowd them; this is tapped on a phone during
     * onboarding where mis-taps mean retyping. */
    "ul.scan{list-style:none;padding:0;margin:.6em 0}"
    "ul.scan li{margin:.5em 0}"
    "ul.scan a{display:flex;gap:.7em;align-items:center;padding:.9em 1em;min-height:60px;"
    "background:#1f2a37;border:1px solid var(--border);border-radius:10px;color:var(--fg);"
    "font-size:1.1em}"
    "ul.scan a:hover{text-decoration:none;border-color:var(--accent)}"
    "ul.scan a:active{background:#2a3441;border-color:var(--accent)}"
    ".bars{font-family:monospace;letter-spacing:1px;color:var(--accent)}"
    ".ssid{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".dbm{opacity:.6;font-size:.8em;white-space:nowrap}"
    ".micgain{margin:.5em 0;display:flex;align-items:center;gap:.5em;flex-wrap:wrap}"
    ".micgain input[type=range]{flex:1;min-width:140px}"
    "table{border-collapse:collapse;width:100%}"
    "td{padding:.25em .6em;vertical-align:top}"
    "td.k{color:var(--muted);width:11em}td.v{font-variant-numeric:tabular-nums}"
    "td.ok{color:var(--ok);width:1.5em;text-align:center;font-weight:bold}"
    "td.warn{color:var(--warn);width:1.5em;text-align:center;font-weight:bold}"
    "tr:nth-child(even){background:var(--row-alt)}"
    "td.size{color:var(--muted);text-align:right;white-space:nowrap}"
    "td.tag{color:var(--muted);font-size:.85em}"
    ".empty{color:var(--muted);padding:1em 0}"
    ".summary{padding:.4em .8em;border-radius:4px;display:inline-block;margin-left:.5em;font-size:.8em}"
    ".s-ok{background:var(--ok-bg);color:var(--ok-fg)}"
    ".s-degraded{background:var(--warn-bg);color:var(--warn-fg)}"
    "audio{width:100%;margin-top:.5em;filter:invert(.9) hue-rotate(180deg)}"
    ".actions{display:flex;gap:.6em;margin:.8em 0;flex-wrap:wrap}"
    /* Action controls are <a> links (work without JS — JS just enhances them
     * into in-page capture/stream); style them identically to buttons. */
    ".actions a,.actions button{flex:1;font-size:1.15em;padding:.7em .5em;min-height:3em;"
    "background:#1f2a37;color:var(--fg);border:1px solid var(--border);border-radius:8px;"
    "cursor:pointer;display:flex;align-items:center;justify-content:center;text-decoration:none}"
    ".actions a:hover{text-decoration:none}"
    /* Photo frame: positioned wrapper that RESERVES the capture aspect-ratio
     * (set inline from the camera framesize — no hardcoded ratio) so the preview
     * doesn't reflow when /last.jpg loads or 404s. A placeholder (.ph) sits
     * behind the image and shows when there's no frame; the caption (.cap)
     * overlays bottom-left (HTML/CSS only, no pixel burn-in — see jpeg_stamp.h). */
    ".frame{position:relative;display:block;width:100%;max-width:100%;margin:.5em 0;"
    "border-radius:4px;overflow:hidden;background:var(--code-bg)}"
    ".frame img{position:relative;z-index:1;display:block;width:100%;height:100%;"
    "object-fit:contain;border-radius:4px}"
    /* Placeholder sits BEHIND the image (z-index 0 vs img's 1) — a positioned
     * .ph would otherwise paint OVER the static img. JS also hides it on load so
     * it never shows through a stream's letterbox bars. */
    ".frame .ph{position:absolute;inset:0;z-index:0;display:flex;align-items:center;"
    "justify-content:center;color:var(--muted);font-size:.95em}"
    /* Caption above the image (z 2 > img 1 > placeholder 0). */
    ".frame .cap{position:absolute;left:0;bottom:0;z-index:2;max-width:100%;margin:0;"
    "background:rgba(0,0,0,.6);color:#fff;padding:.3em .55em;font-size:.8em;"
    "line-height:1.3;border-radius:0 6px 0 4px;font-variant-numeric:tabular-nums}"
    "@media(max-width:48em){td.k{width:auto}table{font-size:.95em}}";

/* GET /style.css — the shared theme. No auth (it's just styling, requested
 * before any login) and cached so it isn't refetched on every page. */
static esp_err_t style_css_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, STYLE_CSS, HTTPD_RESP_USE_STRLEN);
}

/* Embedded brand assets (see CMakeLists.txt EMBED_*). favicon.ico is binary
 * (start/end bound the length); logo.svg rides EMBED_TXTFILES so it's
 * NUL-terminated and serves with HTTPD_RESP_USE_STRLEN. */
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");
extern const char logo_svg_start[] asm("_binary_logo_svg_start");

/* GET /favicon.ico — the embedded site icon. No auth (browsers fetch it before
 * any login); cached a week so it isn't re-requested. */
static esp_err_t favicon_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    return httpd_resp_send(req, (const char *)favicon_ico_start,
                           (ssize_t)(favicon_ico_end - favicon_ico_start));
}

/* GET /logo.svg — the monochrome mark shown in every page's header. No auth,
 * cached a week (the <header> links it from /, /config and /ble). */
static esp_err_t logo_svg_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    return httpd_resp_send(req, logo_svg_start, HTTPD_RESP_USE_STRLEN);
}

/* Persistent top warning bar for insecure DEFAULTS, emitted on every page so
 * the operator can't miss it. Fires when (a) the recovery AP is up on its
 * public default password, and/or (b) web-admin basic-auth is a placeholder
 * (gate off) or the example default we ship. Emits nothing when both are fine.
 * Uses chunked send (works mid-stream on all the page handlers). */
static void send_security_banner(httpd_req_t *req) {
    char aps[WIFI_STORE_SSID_CAP], app[WIFI_STORE_PASS_CAP];
    bool ap_custom = false;
    wifi_store_get_ap(aps, sizeof(aps), app, sizeof(app), &ap_custom);
    bool ap_default = (wifi_mgr_softap_active() || wifi_store_is_ap_only()) && !ap_custom;
    /* Weak web-admin auth: gate effectively off (placeholder creds / no HTTPS)
     * OR still on the public example default. Uses the EFFECTIVE pass so the
     * banner clears once the operator sets a real password via /config or
     * cmd/auth. */
    char eff_u[AUTH_STORE_USER_CAP], eff_p[AUTH_STORE_PASS_CAP];
    auth_store_get_effective(eff_u, sizeof(eff_u), eff_p, sizeof(eff_p));
    /* "cb" is the easy out-of-box default (secrets.h) — flag it as weak so the
     * banner nags until the operator sets a real login. */
    bool auth_weak = !basic_auth_enabled() || strcmp(eff_p, "cb") == 0;
    if (!ap_default && !auth_weak) return;
    /* One red strip PER warning (each its own block div → own line), and each
     * is a link straight to the /config section that fixes it (#ap / #auth —
     * the anchors on those section headings). */
    char bar[512];
    if (ap_default) {
        snprintf(bar, sizeof(bar),
                 "<div class=\"warn-bar\"><a href=\"/config#ap\">&#9888; %s</a></div>",
                 tr(STR_WARN_AP_DEFAULT));
        httpd_resp_sendstr_chunk(req, bar);
    }
    if (auth_weak) {
        snprintf(bar, sizeof(bar),
                 "<div class=\"warn-bar\"><a href=\"/config#auth\">&#9888; %s</a></div>",
                 tr(STR_WARN_AUTH_DEFAULT));
        httpd_resp_sendstr_chunk(req, bar);
    }
}

/* Forward decls — definitions live with the WiFi-scan helpers further down. */
static void scan_html_escape(const char *s, char *out, size_t cap);
static const char *rssi_bars(int rssi);  /* RSSI → 4-cell █/░ signal bar */

/* Shared page chrome for every page (/, /config, /ble) so they read
 * identically: the <head> (+ optional extra head markup, e.g. the BLE
 * auto-refresh meta), opening <body>, the security banner, and a header bar
 * carrying the embedded logo, the title and the cross-page nav. Each page then
 * emits only its own sections. Replaces the per-page ad-hoc <h1>/<h2> + Home
 * link that had drifted apart across the three pages. */
static void send_page_head(httpd_req_t *req, const char *head_extra,
                           const char *active) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* Version-stamp the /style.css + /logo.svg URLs so a firmware update busts
     * the browser cache (they're served with a long max-age; without this an
     * old cached stylesheet renders the new markup unstyled). */
    const char *ver = esp_app_get_description()->version;
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Chytrá Budka %s</title>"
        "<meta name=\"color-scheme\" content=\"dark\">"
        "<link rel=\"icon\" href=\"/favicon.ico\">"
        "<link rel=\"stylesheet\" href=\"/style.css?v=%s\">%s"
        "</head><body>",
        device_id_suffix(), ver, head_extra ? head_extra : "");
    httpd_resp_send_chunk(req, buf, n);
    send_security_banner(req);
    /* Mark the current page's nav pill so it reads as "you are here". */
    const char *cur_home = (active && !strcmp(active, "home")) ? "cur" : "";
    const char *cur_cfg  = (active && !strcmp(active, "config")) ? "cur" : "";
    const char *cur_ble  = (active && !strcmp(active, "ble")) ? "cur" : "";
    n = snprintf(buf, sizeof(buf),
        "<header class=\"hdr\">"
        "<a class=\"brand\" href=\"/\">"
        "<img class=\"logo\" src=\"/logo.svg?v=%s\" alt=\"\" width=\"34\" height=\"34\">"
        "<span class=\"btitle\">Chytrá Budka <small>%s</small></span></a>"
        "<nav class=\"topnav\">"
        "<a class=\"%s\" href=\"/\">%s</a>"
        "<a class=\"%s\" href=\"/config\">%s</a>"
        "<a class=\"%s\" href=\"/ble\">BLE</a>"
        "</nav></header>",
        ver, device_id_suffix(),
        cur_home, tr(STR_NAV_HOME), cur_cfg, tr(STR_SETTINGS), cur_ble);
    httpd_resp_send_chunk(req, buf, n);
}

/* "YYYY:MM:DD HH:MM:SS" → "YYYY-MM-DD HH:MM:SS" (EXIF date uses colons). */
static void exif_reformat_dt(const char *in, char *out, size_t cap) {
    snprintf(out, cap, "%s", in ? in : "");
    if (strlen(out) >= 10) {
        if (out[4] == ':') out[4] = '-';
        if (out[7] == ':') out[7] = '-';
    }
}

/* Compact one-line overlay caption built from a photo's EXIF: capture time,
 * trigger, and a battery (or RSSI) hint. The trigger is HTML-escaped (EXIF is
 * untrusted SD content); the rest are digits/symbols. &nbsp; keeps a value
 * glued to its unit. `out` is empty if there's nothing worth showing. */
static void exif_caption_html(const exif_meta_t *m, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    char dt[24];
    if (m->have_dt) exif_reformat_dt(m->datetime, dt, sizeof(dt));
    else            snprintf(dt, sizeof(dt), "%s", tr(STR_NO_CLOCK));
    int o = snprintf(out, cap, "%s", dt);
    if (o < 0) { out[0] = 0; return; }

    if (m->trigger[0] && (size_t)o < cap) {
        char te[40];
        scan_html_escape(m->trigger, te, sizeof(te));
        o += snprintf(out + o, cap - (size_t)o, " &middot; %.39s", te);
    }
    double vb, soc, rssi;
    if ((size_t)o < cap && exif_json_num(m->user_json, "vbatt", &vb)) {
        if (exif_json_num(m->user_json, "soc", &soc))
            o += snprintf(out + o, cap - (size_t)o,
                          " &middot; %.2f&nbsp;V (%.0f&nbsp;%%)", vb, soc);
        else
            o += snprintf(out + o, cap - (size_t)o, " &middot; %.2f&nbsp;V", vb);
    } else if ((size_t)o < cap && exif_json_num(m->user_json, "rssi", &rssi)) {
        o += snprintf(out + o, cap - (size_t)o, " &middot; %.0f&nbsp;dBm", rssi);
    }
    /* Ambient temperature · humidity (SHT41) · pressure (BMP388) · camera AGC —
     * all from the UserComment telemetry, each shown only when present. */
    double tC, rh, pa, agc;
    if ((size_t)o < cap && exif_json_num(m->user_json, "temp", &tC))
        o += snprintf(out + o, cap - (size_t)o, " &middot; %.1f&nbsp;&deg;C", tC);
    if ((size_t)o < cap && exif_json_num(m->user_json, "humidity", &rh))
        o += snprintf(out + o, cap - (size_t)o, " &middot; %.0f&nbsp;%%", rh);
    if ((size_t)o < cap && exif_json_num(m->user_json, "pressure", &pa))
        o += snprintf(out + o, cap - (size_t)o, " &middot; %.0f&nbsp;hPa", pa);
    if ((size_t)o < cap && exif_json_num(m->user_json, "agc", &agc))
        o += snprintf(out + o, cap - (size_t)o, " &middot; AGC&nbsp;%.0f", agc);
}

static esp_err_t root_get(httpd_req_t *req) {
    apply_hsts(req);
    char mac[20];
    format_mac(mac, sizeof(mac));
    const esp_app_desc_t *app = esp_app_get_description();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    /* SHT41 readings — use the cached value from the most recent
     * telemetry tick instead of triggering a fresh 12 ms blocking I²C
     * read on every page load. Telemetry refreshes the cache once per
     * mode-dependent period (60s in Continuous, 300s in Triggered) so
     * the value here is at most that stale, which is acceptable for an
     * info page and avoids contention with the camera SCCB on I²C0. */
    /* Physical I²C sensor values are rendered below straight from the
     * sensor registry's caches (populated by the telemetry tick) — the
     * HTTP task does NO live I²C read, which avoids adding a concurrent
     * caller on the shared bus (the source of the OLED flush timeouts).
     * Battery (MAX17048) is one of those registry sensors now, so it's
     * rendered in the loop below too — no separate battery row. */

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;

    uint32_t pir_count = pir_motion_count();
    uint32_t audio_bursts = audio_burst_count();
    uint32_t photo_count = camera_capture_count();
    uint32_t heap_free = esp_get_free_heap_size();

    uint64_t sd_free = 0, sd_total = 0;
    sd_storage_stats(&sd_free, &sd_total);

    /* Status rows — the connected sensors (SHT/BMP/battery/solar) render from
     * the CB_SENSORS registry loop below, so no per-sensor bool is needed here;
     * only the always-shown subsystems + the armed-only PIR/reed keep a flag. */
    bool b_sd = sd_storage_ready();
    bool b_cam = camera_ready();
    bool b_pir = pir_ready();
    bool b_reed = reed_ready();
    bool b_mic = audio_ready();
    bool b_audio_task = audio_task_running();
    bool b_cam_worker = camera_worker_running();
    bool b_wifi = wifi_mgr_is_connected();
    bool b_mqtt = mqtt_is_connected();
    /* In full AP-only mode there is no station by design, so STA/MQTT being
     * "down" is the expected state, not a fault. */
    bool b_ap = wifi_store_is_ap_only();

    /* Page exceeds a single 4 KB block, so build it incrementally with
     * httpd_resp_send_chunk(). Each chunk gets its own bounded buffer
     * so a snprintf overflow corrupts at most that chunk, never the
     * whole response.
     *
     * Buffer is heap-allocated rather than on stack — keeps 1 KB out of
     * the handler's stack frame while sht41_read / esp_wifi_sta_get_ap_info
     * / battery_* go deep on the same stack. */
#define CHUNK_SZ 1024
    char *chunk = (char *)malloc(CHUNK_SZ);
    if (!chunk) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM allocating response buffer");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");

/* snprintf returns the would-be length; if a chunk's format expands past
 * CHUNK_SZ it does NOT overflow the buffer, but passing that oversized count
 * to httpd_resp_send_chunk would read PAST the buffer (heap over-read leaking
 * adjacent memory into the response). Clamp to what actually fits and shout
 * in the log so an over-long chunk is caught in dev rather than shipped. */
#define SEND(fmt, ...)                                          \
    do {                                                        \
        int _n = snprintf(chunk, CHUNK_SZ, fmt, ##__VA_ARGS__); \
        if (_n >= CHUNK_SZ) {                                   \
            ESP_LOGW(TAG, "SEND chunk truncated (%d >= %d) — split the literal", \
                     _n, CHUNK_SZ);                             \
            _n = CHUNK_SZ - 1;                                  \
        }                                                       \
        if (_n > 0)                                             \
            httpd_resp_send_chunk(req, chunk, _n);              \
    } while (0)

    send_page_head(req, NULL, "home");

    /* Top action bar — Capture + Stream are the two controls used most when
     * physically positioning the box, so they sit at the very top with big
     * touch targets and a shared preview area right below. /capture triggers a
     * fresh shot and returns it; /stream.mjpg is the live MJPEG view. The
     * cache-buster on /capture forces the browser to re-fetch on every tap.
     * The preview defaults to the last stored frame (/last.jpg) so you see the
     * current framing immediately on load; onerror hides it if no shot yet. */
    /* Reserve the preview box at the camera's CAPTURE aspect-ratio (read from
     * cam_framesize — never hardcoded) so the layout doesn't jump when /last.jpg
     * loads or 404s; a placeholder shows until/unless there's a frame. */
    uint16_t cam_w = 0, cam_h = 0;
    char framestyle[40] = "";
    if (camera_capture_dimensions(&cam_w, &cam_h))
        snprintf(framestyle, sizeof(framestyle), " style=\"aspect-ratio:%u/%u\"",
                 (unsigned)cam_w, (unsigned)cam_h);
    /* Deep-link the static preview to the stored photo's viewer (server-side,
     * no JS) when there's a last-saved frame. The /view param charset is the
     * same alnum + . _ - the filename uses and the bucket is
     * "YYYY-MM-DD"/"boot"/"root", so no escaping is needed. The <a> wraps only
     * the <img>; JS shoot()/stream() still swaps the inner src in place. */
    char view_open[128] = "";
    const char *view_close = "";
    {
        char ld[SD_LAYOUT_DAY_LEN], lf[SD_LAYOUT_LEAF_MAX];
        if (sd_storage_last_photo(ld, sizeof(ld), lf, sizeof(lf))) {
            snprintf(view_open, sizeof(view_open),
                     "<a id=\"viewlink\" href=\"/view?d=%s&amp;f=%s\">", ld, lf);
            view_close = "</a>";
        }
    }
    SEND(
        /* Real <a> links so the page is usable with JavaScript OFF: Capture
         * opens a fresh JPEG, Stream opens the MJPEG. With JS, shoot()/stream()
         * intercept the click (return false) and swap the image in-page instead
         * of navigating. Settings/BLE navigation lives in the header nav now, so
         * the action bar is just the two capture controls. */
        "<div class=\"actions\">"
        "<a id=\"btn-cap\" href=\"/capture\" onclick=\"return shoot()\">&#128247; %s</a>"
        "<a id=\"btn-str\" href=\"/stream.mjpg\" onclick=\"return stream()\">&#127909; %s</a>"
        "</div>"
        "<div class=\"frame\"%s>"
        "<div class=\"ph\">&#128247; %s</div>"
        "%s<img id=\"view\" alt=\"\" src=\"/last.jpg\" "
        "onerror=\"this.style.display='none'\">%s",
        tr(STR_HP_CAPTURE), tr(STR_HP_STREAM),
        framestyle, tr(STR_HP_NOPHOTO), view_open, view_close);
    /* Overlay the last stored frame's capture time + telemetry, read straight
     * from its EXIF (peek the JPEG header only — no full decode). JS swaps the
     * <img> to a live capture/stream on tap and hides this caption (it would be
     * stale against the new frame); the static last-frame view keeps it. */
    {
        uint8_t *hdrbuf = (uint8_t *)scratch_alloc(EXIF_PEEK_BYTES);
        if (hdrbuf) {
            size_t hn = camera_last_jpeg_peek_header(hdrbuf, EXIF_PEEK_BYTES);
            exif_meta_t lm;
            if (hn && exif_read(hdrbuf, hn, &lm)) {
                char capt[256];
                exif_caption_html(&lm, capt, sizeof(capt));
                if (capt[0]) SEND("<div class=\"cap\" id=\"cap\">%s</div>", capt);
            }
            free(hdrbuf);
        }
    }
    SEND("</div>");  /* close .frame */
    /* Live mic right under the photo/stream preview (its natural home — the
     * other "live view" of the box), not buried at the bottom by the
     * endpoints list. */
    /* Live mic + browser-side gain — only when the mic is actually capturing
     * (audio_ready() = i2s channel up AND ≥1 frame read). When the PDM mic is
     * absent / disconnected (no DMA data) there's nothing to stream, so hide
     * the player entirely rather than offer a dead control. The ESP serves the
     * raw PDM WAV at unity (no on-device boost), which is very quiet; route the
     * <audio> through a Web Audio GainNode so a slider can amplify it up to 10x
     * in the browser. No-JS fallback: the <audio controls> still plays at 1x. */
    if (audio_ready()) {
        /* Gain integrated INTO the player: no separate slider. The PDM mic is
         * quiet, so route playback through a fixed 10x Web-Audio boost and let
         * the <audio> element's own volume control set the effective gain — the
         * element's volume (0-1) scales the source ahead of the 10x node, so the
         * player's volume slider spans 0-10x. Default 0.3 ~ 3x (the old default). */
        SEND("<h3>%s</h3>"
             "<audio id=\"mic\" controls preload=\"none\" src=\"/mic.wav\"></audio>"
             "<script>"
             "(function(){var a=document.getElementById('mic'),"
             "AC=window.AudioContext||window.webkitAudioContext;if(!a||!AC)return;"
             "a.volume=0.3;var c,g;"
             "a.addEventListener('play',function(){if(!c){c=new AC();"
             "var sr=c.createMediaElementSource(a);g=c.createGain();g.gain.value=10;"
             "sr.connect(g);g.connect(c.destination);}"
             "if(c.state=='suspended')c.resume();});})();"
             "</script>",
             tr(STR_H_LIVEMIC));
    }
    /* Capture/stream toggle. Capture swaps in a fresh /capture JPEG and, once it
     * loads, re-shows the caption with THAT frame's EXIF (via showCap()/last.json
     * — capture refreshes the last-frame cache). Stream points the <img> at the
     * live MJPEG and hides the caption (live frames have no single timestamp);
     * Stop reloads the latest stored frame (/last.jpg) + its caption rather than
     * clearing the preview. Helpers split across two <script>s (shared globals)
     * to keep each chunk under the SEND buffer. */
    SEND(
        "<script>"
        "var v=document.getElementById('view'),"
        "sb=document.getElementById('btn-str'),st=false;"
        /* Show the placeholder ONLY when no frame is displayed: hide it once the
         * <img> loads (covers the stream-letterbox case too), show it on error. */
        "var ph=v?v.parentNode.querySelector('.ph'):null;"
        "function setPh(s){if(ph)ph.style.display=s?'':'none';}"
        "if(v){v.addEventListener('load',function(){setPh(0);});"
        "v.addEventListener('error',function(){setPh(1);});"
        "if(v.complete&&v.naturalHeight>0)setPh(0);}"
        "function hc(){var c=document.getElementById('cap');if(c)c.style.display='none';}"
        "</script>");
    SEND(
        "<script>"
        "function fmtCap(j){if(!j||j.exif!==true)return '';"
        "var dt=(j.have_dt===false)?'':(j.datetime||'').replace(/^(\\d{4}):(\\d{2}):/,'$1-$2-');"
        "var p=[];if(dt)p.push(dt);if(j.trigger)p.push(j.trigger);"
        "var t=j.telemetry||{};"
        "if(typeof t.vbatt==='number'){var b=t.vbatt.toFixed(2)+'\\u00a0V';"
        "if(typeof t.soc==='number')b+=' ('+Math.round(t.soc)+'\\u00a0%%)';p.push(b);}"
        "else if(typeof t.rssi==='number')p.push(Math.round(t.rssi)+'\\u00a0dBm');"
        "if(typeof t.temp==='number')p.push(t.temp.toFixed(1)+'\\u00a0\\u00b0C');"
        "if(typeof t.humidity==='number')p.push(Math.round(t.humidity)+'\\u00a0%%');"
        "if(typeof t.pressure==='number')p.push(Math.round(t.pressure)+'\\u00a0hPa');"
        "if(typeof t.agc==='number')p.push('AGC\\u00a0'+t.agc);"
        "return p.join(' \\u00b7 ');}"
        "</script>");
    SEND(
        "<script>"
        "function showCap(){fetch('/last.json').then(function(r){return r.json();})"
        ".then(function(j){var x=fmtCap(j);if(!x)return;"
        "var c=document.getElementById('cap');"
        "if(!c){c=document.createElement('div');c.id='cap';c.className='cap';"
        "if(v.parentNode)v.parentNode.appendChild(c);}"
        "c.textContent=x;c.style.display='';}).catch(function(){});}"
        "</script>");
    SEND(
        "<script>"
        "function rst(){st=false;sb.innerHTML='&#127909; %s';}"
        "function shoot(){if(st)rst();v.style.display='block';hc();setPh(0);"
        "v.onload=function(){v.onload=null;showCap();};"
        "v.src='/capture?t='+Date.now();return false;}"
        "function stream(){if(st){v.onload=function(){v.onload=null;showCap();};"
        "v.style.display='block';v.src='/last.jpg?t='+Date.now();rst();}"
        "else{v.onload=null;v.style.display='block';hc();setPh(0);v.src='/stream.mjpg';st=true;"
        "sb.innerHTML='&#9209; %s';}return false;}"
        "</script>",
        tr(STR_HP_STREAM), tr(STR_HP_STOP));

    /* ── Status — one operator-facing table merging the old "Self-test" and
     * "Live sensors": the core subsystems every board has (always shown, ✓ or
     * —), then connected peripherals/sensors with their live reading folded in
     * (absent ones emit no row), then operator counters. Debug internals (tasks,
     * heap, stack, DMA) drop into the collapsed Diagnostics block below. No
     * N/N summary badge — optional sensors are normally absent, so it would
     * permanently read "degraded — some sensors not wired" and mean nothing;
     * the truthful per-check verdict lives in /selftest for a deeper look. */
    SEND("<h2>%s</h2><table>", tr(STR_H_STATUS));

    /* WiFi: the same signal bar + thresholds as the scan list, then RSSI and
     * the joined SSID/BSSID (so the operator sees exactly what it joined). */
    if (b_ap) {
        SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>WiFi</td><td>%s</td></tr>",
             tr(STR_ST_AP_MODE));
    } else if (b_wifi) {
        char esc_ssid[200], bssid[18] = {0};
        scan_html_escape(wifi_mgr_get_ssid(), esc_ssid, sizeof(esc_ssid));
        wifi_mgr_get_bssid(bssid, sizeof(bssid));
        SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>WiFi</td>"
             "<td><span class=\"bars\">%s</span> %d&nbsp;dBm &middot; %s (%s)</td></tr>",
             rssi_bars(rssi), rssi, esc_ssid, bssid[0] ? bssid : "?");
    } else {
        SEND("<tr><td class=\"warn\">" ST_DASH "</td><td>WiFi</td><td>%s</td></tr>",
             tr(STR_ST_NO_ASSOC));
    }
    SEND("<tr><td class=\"%s\">%s</td><td>MQTT</td><td>%s</td></tr>",
         (b_ap || b_mqtt) ? "ok" : "warn", (b_ap || b_mqtt) ? ST_TICK : ST_DASH,
         b_ap ? tr(STR_ST_NA_AP) : b_mqtt ? tr(STR_ST_BROKER_OK) : tr(STR_ST_DISCONNECTED));
    SEND("<tr><td class=\"%s\">%s</td><td>%s</td><td>%s</td></tr>",
         b_cam ? "ok" : "warn", b_cam ? ST_TICK : ST_DASH,
         tr(STR_LBL_CAMERA), b_cam ? tr(STR_ST_READY) : tr(STR_D_SCCB));
    SEND("<tr><td class=\"%s\">%s</td><td>%s</td><td>%s</td></tr>",
         b_mic ? "ok" : "warn", b_mic ? ST_TICK : ST_DASH,
         tr(STR_LBL_MIC), b_mic ? tr(STR_D_MIC_OK) : tr(STR_D_MIC_NO));
    /* (audio_task / cam_worker — the pinned CPU1 tasks — are internals, so they
     * live in the Diagnostics block below, not in the operator Status.) */

    /* microSD: usage bar when mounted (red >90 %, amber >75 %), else "—". */
    if (b_sd && sd_total > 0) {
        uint64_t used = sd_total - sd_free;
        unsigned pct = (unsigned)((used * 100ULL) / sd_total);
        const char *color = (pct >= 90) ? "#cf222e" : (pct >= 75) ? "#bf8700" : "#1a7f37";
        SEND(
            "<tr><td class=\"ok\">" ST_TICK "</td><td>microSD</td><td>"
            "<div style=\"display:flex;align-items:center;gap:0.6em\">"
            "<div style=\"flex:1;height:0.8em;background:#30363d;border-radius:3px;overflow:hidden\">"
            "<div style=\"width:%u%%;height:100%%;background:%s\"></div></div>"
            "<span>%u%% &mdash; %llu / %llu MB %s</span></div></td></tr>",
            pct, color, pct, sd_free / (1024ULL * 1024ULL),
            sd_total / (1024ULL * 1024ULL), tr(STR_S_FREE));
    } else {
        SEND("<tr><td class=\"warn\">" ST_DASH "</td><td>microSD</td><td>%s</td></tr>",
             tr(STR_D_NOT_MOUNTED));
    }

    /* Connected I²C sensors from the registry — one ✓ row each (name + joined
     * channel readings), present-only (absent sensors read nothing → no row).
     * Covers both SHT41s, BMP388, MAX17048 and INA226 on each bus; adding a
     * sensor here makes it appear automatically. Readings come from the
     * telemetry-refreshed caches (no live I²C from the HTTP task). */
    for (size_t si = 0; si < CB_SENSORS_N; si++) {
        const cb_sensor_t *sen = &CB_SENSORS[si];
        char vals[160];
        int o = 0;
        for (size_t ci = 0; ci < sen->n_chans; ci++) {
            const cb_chan_t *c = &sen->chans[ci];
            float v;
            if (c->read(&v) && isfinite(v))
                o += snprintf(vals + o, sizeof(vals) - o, "%s%.*f %s",
                              o ? " &middot; " : "", c->decimals, v, c->unit);
        }
        if (o == 0)
            continue;  /* sensor absent / not reading → no row */
        SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td><td>%s</td></tr>",
             sen->name, vals);
    }
    /* MCU die temperature — a genuine sensor reading (not a debug internal), so
     * it sits with the other sensors rather than in Diagnostics. */
    {
        float mcu_c = diag_mcu_temp_c();
        if (isfinite(mcu_c))
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>%.1f &deg;C</td></tr>", tr(STR_S_MCU_TEMP), mcu_c);
    }

    /* PIR / reed — peripherals that may be absent, so shown only when armed
     * (a stock board carries no permanent "—" for them). */
    if (b_pir) {
        char pir_armed[56];
        snprintf(pir_armed, sizeof(pir_armed), tr(STR_D_PIR_ARMED_FMT),
                 app_config_pin_for_first("pir"));
        SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>PIR</td><td>%s</td></tr>",
             pir_armed);
    }
    if (b_reed) {
        SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
             "<td>%s &middot; %" PRIu32 " %s</td></tr>",
             tr(STR_LBL_REED), reed_is_closed() ? tr(STR_ST_CLOSED) : tr(STR_ST_OPEN),
             reed_event_count(), tr(STR_S_EVENTS));
    }
    /* Grove sensors — same only-when-armed policy as PIR/reed. Values
     * come from the modules' caches (sonar poll task / telemetry-tick
     * soil read); "—" = armed but no valid reading yet. */
    if (sonar_ready()) {
        float cm;
        if (sonar_last_cm(&cm))
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>%.1f cm</td></tr>", tr(STR_LBL_SONAR), cm);
        else if (sonar_is_clear())
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>&infin;</td></tr>", tr(STR_LBL_SONAR));
        else
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>&mdash;</td></tr>", tr(STR_LBL_SONAR));
    }
    if (soil_ready()) {
        float soil_mv, soil_pct;
        if (soil_last(&soil_mv, &soil_pct) && isfinite(soil_pct))
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>%.0f %% &middot; %.0f mV</td></tr>",
                 tr(STR_LBL_SOIL), soil_pct, soil_mv);
        else if (soil_last(&soil_mv, &soil_pct))
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>%.0f mV</td></tr>", tr(STR_LBL_SOIL), soil_mv);
        else
            SEND("<tr><td class=\"ok\">" ST_TICK "</td><td>%s</td>"
                 "<td>&mdash;</td></tr>", tr(STR_LBL_SOIL));
    }

    /* Operator counters (no health glyph — empty first cell). Uptime is
     * human-readable; motion/bursts/photos are lifetime totals. */
    {
        char up[24];
        fmt_uptime(up, sizeof(up), uptime_s);
        SEND(
            "<tr><td></td><td class=\"k\">%s</td><td class=\"v\">%" PRIu32 "</td></tr>"
            "<tr><td></td><td class=\"k\">%s</td><td class=\"v\">%" PRIu32 "</td></tr>"
            "<tr><td></td><td class=\"k\">%s</td><td class=\"v\">%" PRIu32 "</td></tr>"
            "<tr><td></td><td class=\"k\">%s</td><td class=\"v\">%s</td></tr>",
            tr(STR_S_MOTION), pir_count, tr(STR_S_BURSTS), audio_bursts,
            tr(STR_S_PHOTOS), photo_count, tr(STR_S_UPTIME), up);
    }
    SEND("</table>");

    /* ── Diagnostics (collapsed) — internals an operator rarely needs but a
     * dev/HIL might: the two pinned CPU1 tasks (audio + camera worker), heap
     * (now + worst-ever), HTTP-task stack high-water, and the scarce
     * internal/DMA pools. PSRAM can't serve DMA, so the DMA figure — not the
     * PSRAM-inclusive free heap — is what gates the BT/i2s/camera buffers (its
     * exhaustion is what broke the BLE bring-up). ── */
    {
        size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        UBaseType_t main_stack_hwm =
            uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle());
        SEND("<details><summary>%s</summary><table>", tr(STR_H_DIAGNOSTICS));
        SEND("<tr><td class=\"%s\">%s</td><td>%s</td><td>%s</td></tr>"
             "<tr><td class=\"%s\">%s</td><td>%s</td><td>%s</td></tr>",
             b_audio_task ? "ok" : "warn", b_audio_task ? ST_TICK : ST_DASH,
             tr(STR_LBL_AUDIO_TASK), b_audio_task ? tr(STR_D_TASK_OK10) : tr(STR_D_TASK_FAIL),
             b_cam_worker ? "ok" : "warn", b_cam_worker ? ST_TICK : ST_DASH,
             tr(STR_LBL_CAM_WORKER), b_cam_worker ? tr(STR_D_TASK_OK5) : tr(STR_D_TASK_FAIL));
        SEND("<tr><td class=\"k\">%s</td><td class=\"v\">%" PRIu32 " B (%s %zu B)</td></tr>"
             "<tr><td class=\"k\">%s</td><td class=\"v\">%u B</td></tr>"
             "<tr><td class=\"k\">int / DMA free</td>"
             "<td class=\"v\">%u / %u B (max %u)</td></tr>",
             tr(STR_S_FREE_HEAP), heap_free, tr(STR_S_HEAP_MINEVER), heap_min,
             tr(STR_S_HTTP_STACK), (unsigned)(main_stack_hwm * sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        SEND("</table></details>");
    }

    /* ── BLE meters (UC96 / BTHome) — only when one is streaming or a BTHome
     * sensor has data. A separate table under the on-board sensors so the BLE
     * readings (off-board, allowlisted) stay visually distinct. Rows go via
     * sendstr_chunk (not SEND) so the values string sidesteps format-truncation. */
    if (ble_running()) {
        /* Heap, not a ~3 KB stack array (BLE_VIEW_MAX × sizeof(ble_dev_view_t)):
         * this runs on the 8 KB plain-HTTP task and the page already nests deep
         * (send_page_head + the status table + per-row buffers). bn=0 on alloc
         * failure makes the loops/render below no-op; rendered via sendstr_chunk
         * (no early return), so free() always runs. */
        ble_dev_view_t *bv = malloc(sizeof(*bv) * BLE_VIEW_MAX);
        int bn = bv ? ble_snapshot(bv, BLE_VIEW_MAX) : 0, show = 0;
        for (int i = 0; i < bn; i++)
            if (bv[i].conn_state == 3 || bv[i].has_bthome) show++;
        if (show) {
            httpd_resp_sendstr_chunk(req, "<h2>&#128268; BLE meters</h2><table>");
            for (int i = 0; i < bn; i++) {
                if (!(bv[i].conn_state == 3 || bv[i].has_bthome)) continue;
                char nm[BLE_STORE_NAME_CAP], nmesc[80], vv[160];
                if (!ble_store_get_name(bv[i].id, nm, sizeof(nm)) || !nm[0])
                    snprintf(nm, sizeof(nm), "%.12s", bv[i].id);  /* id is 12 hex */
                scan_html_escape(nm, nmesc, sizeof(nmesc));
                ble_values_html(&bv[i], vv, sizeof(vv));
                httpd_resp_sendstr_chunk(req, "<tr><td class=\"k\">");
                httpd_resp_sendstr_chunk(req, nmesc);
                httpd_resp_sendstr_chunk(req, "</td><td class=\"v\">");
                httpd_resp_sendstr_chunk(req, vv);
                httpd_resp_sendstr_chunk(req, "</td></tr>");
            }
            httpd_resp_sendstr_chunk(req, "</table>");
        }
        free(bv);
    }

    /* ── Device meta ──────────────────────────────────────────────────── */
    SEND(
        "<h2>%s</h2><table>"
        "<tr><td class=\"k\">MAC</td><td class=\"v\"><code>%s</code></td></tr>"
        "<tr><td class=\"k\">%s</td><td class=\"v\"><code>%s</code></td></tr>"
        "<tr><td class=\"k\">%s</td><td class=\"v\"><code>%s (IDF %s)</code></td></tr>"
        "</table>",
        tr(STR_H_DEVICE), mac, tr(STR_DEV_ID), device_id(), tr(STR_DEV_VERSION),
        app->version, app->idf_ver);

    /* ── Pin map (runtime, read from app_config) ──────────────────────── */
    SEND("<h2>%s</h2>"
         "<table>"
         "<tr><td class=\"k\">Slot</td><td class=\"k\">GPIO</td>"
         "<td class=\"k\">%s</td></tr>", tr(STR_H_PINMAP), tr(STR_PM_FUNCTION));
    for (int slot = 0; slot < app_config_pin_slot_count(); slot++) {
        int gpio = -1;
        const char *fn = "none";
        app_config_pin_slot_info(slot, &gpio, &fn);
        SEND("<tr><td class=\"v\"><code>D%d</code></td>"
             "<td class=\"v\"><code>GPIO%d</code></td>"
             "<td class=\"v\">%s<code>%s</code></td></tr>",
             slot, gpio, pin_fn_icon(fn), fn);
    }
    SEND("</table>");

    /* ── Endpoints + live mic + live cam ──────────────────────────────── */
    /* Split across multiple SEND chunks — one big literal overflows the
     * CHUNK_SZ (1 KB) format buffer. */
    SEND(
        "<h2>%s</h2><ul>"
        "<li><a href=\"/last.jpg\">/last.jpg</a> &mdash; %s</li>",
        tr(STR_H_ENDPOINTS), tr(STR_EP_LASTJPG));
    SEND(
        "<li><a href=\"/capture\">/capture</a> &mdash; %s</li>"
        "<li><code>/photo?d=&amp;f=</code> &mdash; %s</li>"
        "<li><a href=\"/photos\">/photos</a> &mdash; %s</li>"
        "<li><a href=\"/mic.wav\">/mic.wav</a> &mdash; %s</li>"
        "<li><a href=\"/stream.mjpg\">/stream.mjpg</a> &mdash; %s</li>"
        "<li><a href=\"/selftest\">/selftest</a> &mdash; %s</li>",
        tr(STR_EP_CAPTURE), tr(STR_EP_PHOTO), tr(STR_EP_PHOTOS), tr(STR_EP_MIC),
        tr(STR_EP_STREAM), tr(STR_EP_SELFTEST));
    SEND(
        "<li><a href=\"/i2c\">/i2c</a> &mdash; %s</li>"
        "<li><a href=\"/sensors\">/sensors</a> &mdash; live read of all registered I&sup2;C sensors</li>"
        "<li><a href=\"/oled/qr\">/oled/qr</a> &mdash; show a QR on the OLED (?text=...) for ~90s</li>"
        "<li><a href=\"/oled/logo\">/oled/logo</a> &mdash; upload a custom boot logo (image &rarr; 128&times;64)</li>"
        "<li><a href=\"/sht41/bus1\">/sht41/bus1</a> &mdash; %s</li>"
        "<li><a href=\"/max17048/bus1\">/max17048/bus1</a> &mdash; %s</li>"
        "<li><a href=\"/i2c/bus1_diag\">/i2c/bus1_diag</a> &mdash; %s</li>"
        "</ul>",
        tr(STR_EP_I2C), tr(STR_EP_SHT1), tr(STR_EP_MAX1), tr(STR_EP_I2CDIAG));
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
    SEND(
        "<h2>Debug endpoints</h2><ul>"
        "<li><a href=\"/debug/cores\">/debug/cores</a> &mdash; per-task core + runtime stats</li>"
        "<li><a href=\"/debug/pir\">/debug/pir</a> &mdash; raw PIR pin read + histogram</li>"
        "<li><a href=\"/debug/hang\">/debug/hang</a> &mdash; intentional WDT trigger (crash path test)</li>"
        "<li><a href=\"/debug/wifi_disconnect\">/debug/wifi_disconnect</a> &mdash; force STA disconnect</li>"
        "<li><a href=\"/debug/sd_format\">/debug/sd_format</a> &mdash; reformat SD (destructive)</li>"
        "<li><a href=\"/debug/sd_remount\">/debug/sd_remount</a> &mdash; remount SD without reboot</li>"
        "<li><code>POST /debug/uart_servo</code> &mdash; raw hex on the UART servo bus "
        "(<code>?timeout_ms=&lt;n&gt;</code>)</li>"
        "<li><code>POST /debug/capture</code> &mdash; enqueue a capture with operator-supplied "
        "trigger (body = trigger string, e.g. <code>pir</code>); used by EXIF HIL tests</li>"
        "<li><a href=\"/debug/tls_csr\">/debug/tls_csr</a> &mdash; fresh EC P-256 CSR PEM "
        "(pipe to <code>openssl req -text -noout</code> to inspect)</li>");
    SEND(
        "<li><a href=\"/debug/cam_standby?on=1\">/debug/cam_standby</a> &mdash; OV3660 "
        "software standby toggle (<code>?on=1|0</code>); bench power-measurement spike</li>"
        "</ul>");
#endif
    SEND("</body></html>");  /* live mic moved up under the photo/stream preview */

#undef SEND
#undef CHUNK_SZ
    httpd_resp_send_chunk(req, NULL, 0); /* finish chunked transfer */
    free(chunk);
    return ESP_OK;
}

static esp_err_t last_jpg_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    size_t len = 0;
    uint8_t *buf = camera_last_jpeg_dup(&len);
    if (!buf) {
        /* esp-httpd's send_err emits text/html with no charset; browsers
         * default to ISO-8859-1 (RFC 7231 §3.1.1.5) and an em-dash here
         * shows as mojibake. Plain ASCII keeps the message readable
         * regardless of client charset assumption. */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "no capture yet -- POST /cmd/photo or wait for trigger");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return e;
}

static atomic_bool s_capture_http_busy = false;  /* one async /capture at a time */

/* /capture runs ASYNC: camera_capture_event() blocks ~2-4 s (IR warm, sensor
 * capture, EXIF stamp); running it in the single httpd task stalled concurrent
 * page loads. Offload to a task; cap at one in flight. */
static void capture_task(void *arg) {
    httpd_req_t *req = (httpd_req_t *)arg;
    esp_err_t e = camera_capture_event("http");
    if (e == ESP_OK)
        last_jpg_get(req);  /* send the freshly-cached JPEG (re-checks auth: ok) */
    else
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    atomic_store(&s_capture_http_busy, false);
    httpd_req_async_handler_complete(req);
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack (WithCaps) — free it accordingly */
}

static esp_err_t capture_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    if (!camera_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized");
        return ESP_OK;
    }
    if (atomic_exchange(&s_capture_http_busy, true)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "capture already in progress\n");
        return ESP_OK;
    }
    httpd_req_t *areq = NULL;
    if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
        atomic_store(&s_capture_http_busy, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
        return ESP_OK;
    }
    /* 12 KB stack from PSRAM (WithCaps): BLE-on fragments internal DRAM to a
     * ~2-3 KB largest block so a normal (internal) 12 KB stack can't be
     * allocated; PSRAM has room. The task does camera_capture_event + a TLS
     * send — no flash-cache-disable ops — so a PSRAM stack is safe here. */
    if (xTaskCreatePinnedToCoreWithCaps(capture_task, "cap_http", 12288, areq, 5,
                                        NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        /* PSRAM exhausted (very unlikely) — respond 503 instead of silently
         * completing the async request (which would hang the client). */
        httpd_resp_set_status(areq, "503 Service Unavailable");
        httpd_resp_set_type(areq, "text/plain");
        httpd_resp_sendstr(areq, "capture unavailable (low memory; try with BLE off)\n");
        httpd_req_async_handler_complete(areq);
        atomic_store(&s_capture_http_busy, false);
        return ESP_OK;
    }
    return ESP_OK;
}

/* One-at-a-time gates for the two live streams. The streams are long
 * loops in the httpd task; two concurrent fetches of the same URL
 * (e.g., a refresh that didn't drop the prior connection, or two
 * tabs) leave the second request waiting on the http server while
 * the first holds the task slot. Returning 503 immediately keeps the
 * board healthy. */
static atomic_bool s_mic_busy = false;
static atomic_bool s_mjpg_busy = false;

/* Exported so app_config can check whether MJPEG is currently streaming
 * before deciding whether a cam_ / mjpg_ cfg change should apply right
 * now or be deferred to the next MJPEG-exit / next stream-open. Tying
 * the apply gate to "is the handler actively running" (not to a
 * cached s_active_profile flag in camera.c) avoids the persistent-leak
 * bug where a stream-profile flag survived past handler exit. */
bool http_server_mjpg_is_active(void) {
    return atomic_load(&s_mjpg_busy);
}

/* GET /mic.wav — live PCM stream, chunked Transfer-Encoding. Plays
 * until the client disconnects or the safety cap fires (default 60 s,
 * cap with ?max=<s> up to 3600). The ESP doesn't do DC removal /
 * gain / EQ here — raw int16 from the PDM ring straight to the wire.
 * Any cleanup belongs on the consumer side. */
static esp_err_t mic_wav_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    if (!audio_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mic not capturing yet");
        return ESP_OK;
    }
    if (atomic_exchange(&s_mic_busy, true)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "another mic stream is active\n");
        return ESP_OK;
    }

    float max_secs = 60.0f;  // matches the handler doc; ?max=<s> overrides up to 3600
    char qbuf[24];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[12];
        if (httpd_query_key_value(qbuf, "max", val, sizeof(val)) == ESP_OK) {
            float v = strtof(val, NULL);
            if (v > 0.0f && v <= 3600.0f)
                max_secs = v;
        }
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* 44-byte streaming WAV header (data_len = 0xFFFFFFFF). */
    uint8_t header[44];
    audio_wav_header_streaming(header, audio_sample_rate());
    if (httpd_resp_send_chunk(req, (const char *)header, sizeof(header)) != ESP_OK) {
        atomic_store(&s_mic_busy, false);
        return ESP_OK;
    }

    /* Start at the live edge — don't backfill stale samples from the
     * ring (the snapshot endpoint is /mic.wav?max=... if anyone wants
     * a fixed-length recording later). */
    uint32_t read_pos = audio_ring_write_pos();
    int16_t chunk[1024];  // 64 ms at 16 kHz; 2 KB on stack
    int64_t now0 = esp_timer_get_time();
    int64_t deadline_us = now0 + (int64_t)(max_secs * 1e6f);
    int64_t last_data_us = now0;  // last time the ring actually yielded samples

    /* End the stream if the mic produces nothing for this long. The audio
     * pump can stall or be disabled (VAD off, BLE-on starving the i2s DMA,
     * mic HW fault); without this cap the loop would hold the socket — and
     * the single s_mic_busy slot — open for the full max_secs. Mirrors the
     * "frame_null_cnt → break" pattern in IDF's streaming examples. */
    const int64_t MIC_STALL_US = 2 * 1000000LL;

    while (esp_timer_get_time() < deadline_us) {
        uint32_t next = read_pos;
        size_t got = audio_ring_read(read_pos, chunk, sizeof(chunk) / sizeof(chunk[0]), &next);
        if (got == 0) {
            /* Ring empty (or lock briefly contended). Two things matter here:
             *   1) We MUST yield. pdMS_TO_TICKS(8) rounds to 0 ticks at
             *      CONFIG_FREERTOS_HZ=100, and vTaskDelay(0) does not yield to
             *      lower-priority tasks — so the old code busy-spun at the
             *      httpd-task priority, starved the main loop + IDLE0 on CPU0,
             *      and tripped the task watchdog (root cause of the v0.4.4
             *      `task_wdt` reboots). cb_delay_ms() floors at one tick so it
             *      always yields. See cb_time.h.
             *   2) Bail out if the mic has been silent too long (stall-cap). */
            if (esp_timer_get_time() - last_data_us > MIC_STALL_US) {
                ESP_LOGW(TAG, "/mic.wav: no audio for %llds — ending stream",
                         (long long)(MIC_STALL_US / 1000000));
                break;
            }
            cb_delay_ms(8);  // ≥1 tick (10 ms) — close to real-time, never a spin
            continue;
        }
        last_data_us = esp_timer_get_time();
        read_pos = next;
        if (httpd_resp_send_chunk(req, (const char *)chunk, got * sizeof(int16_t)) != ESP_OK)
            break;  // client disconnected
    }
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked stream
    atomic_store(&s_mic_busy, false);
    return ESP_OK;
}

/* qsort comparator: descending string compare. Filenames are
 * YYYY-MM-DD_HH-MM-SS_<mac>_<tag>.jpg (Europe/Prague local time) so
 * lexicographic descending = chronologically newest first. */
static int photo_name_cmp_desc(const void *a, const void *b) {
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

/* GET /stream.mjpg — multipart MJPEG live preview. Native browser
 * support: just open the URL in any browser, or point VLC at it.
 *
 * The sensor swaps from the "capture" profile (cam_framesize +
 * cam_quality) to the "stream" profile (mjpg_framesize + mjpg_quality)
 * on entry and restores capture profile on every exit path. Stream
 * profile is typically lighter (e.g. SVGA q=18) so the live preview is
 * faster while stills stay sharp. No mid-stream toggling — if VAD/PIR
 * fires a triggered capture during the stream, the resulting JPEG
 * comes out at stream quality; event/photo JSON publishes the actual
 * (framesize, quality) so HA can correlate.
 *
 * 5 min default cap (overridable via ?max=N up to 3600) so a forgotten
 * browser tab doesn't keep the sensor pinned in stream profile.
 *
 * Runs ASYNC: esp-httpd is single-threaded, so this multi-minute loop would
 * monopolize the one httpd task and block EVERY other request until the
 * stream ended (bench-confirmed: a GET / hung ~14 s behind an open stream).
 * httpd_req_async_handler_begin() hands the socket to stream_mjpg_task and
 * frees the httpd worker immediately. s_mjpg_busy caps it at one stream, so
 * at most one extra task + one held socket. */
typedef struct {
    httpd_req_t *req;
    int64_t      deadline_us;
} mjpg_async_t;

static void stream_mjpg_task(void *arg) {
    mjpg_async_t *a = (mjpg_async_t *)arg;
    httpd_req_t *req = a->req;
    int64_t deadline_us = a->deadline_us;
    free(a);

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Framerate", "best-effort");

    char part_hdr[96];
    while (esp_timer_get_time() < deadline_us) {
        /* Standalone heap copy under the sensor mutex (camera_grab_jpeg_copy):
         * serializes the grab against a concurrent /capture and lets us send
         * the slow TLS copy without holding the sensor locked. */
        uint8_t *jpg = NULL;
        size_t jlen = camera_grab_jpeg_copy(&jpg);
        if (!jlen) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int hn = snprintf(part_hdr, sizeof(part_hdr),
                          "\r\n--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %u\r\n\r\n",
                          (unsigned)jlen);
        esp_err_t e1 = httpd_resp_send_chunk(req, part_hdr, hn);
        esp_err_t e2 =
            (e1 == ESP_OK) ? httpd_resp_send_chunk(req, (const char *)jpg, jlen) : ESP_FAIL;
        free(jpg);
        if (e1 != ESP_OK || e2 != ESP_OK)
            break;  // client disconnected
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    httpd_resp_send_chunk(req, NULL, 0);
    /* Restore capture profile + clear busy on every exit, then return the
     * socket to httpd. */
    (void)camera_apply_capture_profile();
    atomic_store(&s_mjpg_busy, false);
    httpd_req_async_handler_complete(req);
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack (WithCaps) */
}

static esp_err_t stream_mjpg_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    if (!camera_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
        return ESP_OK;
    }
    if (atomic_exchange(&s_mjpg_busy, true)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "another mjpg stream is active\n");
        return ESP_OK;
    }

    /* Switch to stream profile. The profile-apply contends with an
     * in-flight capture for the sensor mutex; that hold is now short
     * (capture moved SD/MQTT outside it), but a capture can still be
     * mid-SCCB when the operator opens the stream — exactly the field
     * positioning case (step in front → PIR capture → open stream). So
     * retry a few times before giving up, instead of 500ing on the first
     * collision. ~5×600 ms covers a capture's sensor window with margin. */
    esp_err_t pe = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        pe = camera_apply_stream_profile();
        if (pe == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    if (pe != ESP_OK) {
        atomic_store(&s_mjpg_busy, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream profile apply failed");
        return ESP_OK;
    }

    /* 5 min default cap; ?max=N (<=3600) override. Parse here while we still
     * hold the original request, then hand off to the worker task. */
    int64_t max_us = 300LL * 1000 * 1000;
    char qbuf[24];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[12];
        if (httpd_query_key_value(qbuf, "max", val, sizeof(val)) == ESP_OK) {
            float v = strtof(val, NULL);
            if (v > 0.0f && v <= 3600.0f)
                max_us = (int64_t)(v * 1e6f);
        }
    }

    /* Detach the socket to a worker task so the single httpd task stays free
     * to serve everyone else. On any failure after the profile switch, undo
     * it + clear busy so the server isn't left wedged in stream profile. */
    httpd_req_t *areq = NULL;
    if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
        (void)camera_apply_capture_profile();
        atomic_store(&s_mjpg_busy, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
        return ESP_OK;
    }
    mjpg_async_t *a = (mjpg_async_t *)malloc(sizeof(*a));
    if (!a) {
        httpd_req_async_handler_complete(areq);
        (void)camera_apply_capture_profile();
        atomic_store(&s_mjpg_busy, false);
        return ESP_OK;
    }
    a->req = areq;
    a->deadline_us = esp_timer_get_time() + max_us;
    /* 8 KB stack from PSRAM (WithCaps) — see capture_get: an internal 8 KB stack
     * can't be allocated BLE-on (fragmented to ~2-3 KB); PSRAM has room. ssl_write
     * + camera_grab_jpeg_copy (heap) + snprintf — no flash-cache-disable ops. */
    if (xTaskCreatePinnedToCoreWithCaps(stream_mjpg_task, "mjpg_stream", 8192, a, 5,
                                        NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        /* PSRAM exhausted (very unlikely) — respond 503 instead of hanging. */
        free(a);
        httpd_resp_set_status(areq, "503 Service Unavailable");
        httpd_resp_set_type(areq, "text/plain");
        httpd_resp_sendstr(areq, "stream unavailable (low memory; try with BLE off)\n");
        httpd_req_async_handler_complete(areq);
        (void)camera_apply_capture_profile();
        atomic_store(&s_mjpg_busy, false);
        return ESP_OK;
    }
    return ESP_OK;  /* httpd worker freed immediately; task owns the socket */
}

static esp_err_t selftest_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char json[704];  /* match selftest_run_and_publish()'s buffer */
    selftest_run_and_publish(json, sizeof(json));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, strlen(json));
}

/* ── photo store browsing (date-tree) ───────────────────────────────────────
 * Photos live under /sdcard as day buckets (YYYY-MM-DD/, plus boot/ for
 * pre-clock captures and loose legacy *.jpg as the "root" bucket — see
 * sd_layout.h). The gallery lists buckets first (cheap: one readdir of the
 * card root) and walks a single day's directory only when you open it, so the
 * listing cost no longer scales with the whole-card photo count. A one-entry
 * PSRAM cache keyed on (bucket, sd_storage_listing_gen) keeps paging within a
 * day from re-reading the card; it's invalidated whenever a capture or
 * autoprune changes the set. Still no thumbnails — the browser renders the
 * full JPEG that /photo serves (now cacheable + range-able). */

/* Newest-first ordering of bucket names for the day index. */
static int bucket_cmp_newest_first(const void *a, const void *b) {
    return -sd_layout_bucket_cmp_oldest_first(*(const char *const *)a,
                                              *(const char *const *)b);
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Minimal JSON string-body escaper: backslash, double-quote, and control
 * chars (<0x20) → \uXXXX. Firmware-written filenames never need this (they're
 * [A-Za-z0-9._-]), but a file dropped on the card out-of-band could otherwise
 * break the /photos.json output. Truncates safely if `out` is too small. */
static void json_escape_str(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            int k = snprintf(out + o, cap - o, "\\u%04x", c);
            if (k > 0) o += (size_t)k;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* One-entry listing cache: the sorted (desc) leaf names of a single bucket. */
static SemaphoreHandle_t s_listing_mtx = NULL;
static char     s_lc_bucket[SD_LAYOUT_DAY_LEN] = {0};
static uint32_t s_lc_gen = 0;
static bool     s_lc_valid = false;
static char   **s_lc_names = NULL;
static size_t   s_lc_count = 0;

static void listing_cache_clear_locked(void) {
    if (s_lc_names) {
        for (size_t i = 0; i < s_lc_count; i++) free(s_lc_names[i]);
        free(s_lc_names);
    }
    s_lc_names = NULL;
    s_lc_count = 0;
    s_lc_valid = false;
    s_lc_bucket[0] = '\0';
}

/* (Re)build the cache for `bucket` at the current generation. Lock held. */
static size_t listing_cache_ensure_locked(const char *bucket) {
    uint32_t gen = sd_storage_listing_gen();
    if (s_lc_valid && gen == s_lc_gen && strcmp(bucket, s_lc_bucket) == 0)
        return s_lc_count;
    listing_cache_clear_locked();

    bool is_root = (strcmp(bucket, SD_LAYOUT_ROOT) == 0);
    char dirpath[32];
    if (is_root) snprintf(dirpath, sizeof(dirpath), "%s", SD_MOUNT_POINT);
    else         snprintf(dirpath, sizeof(dirpath), "%s/%s", SD_MOUNT_POINT, bucket);

    DIR *d = opendir(dirpath);
    if (!d) {
        /* Transient SD glitch (vs a genuinely empty/openable dir) — DON'T stamp
         * the cache valid, or a populated day would show empty until the next
         * gen bump. Leave it invalid so the next request retries the open. */
        return 0;
    }
    size_t cap = 128, count = 0;
    char **names = heap_caps_malloc(cap * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!names) names = malloc(cap * sizeof(char *));
    struct dirent *e;
    while (names && (e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (is_root) {
            if (sd_layout_classify(nm) != SD_BUCKET_ROOT_JPG) continue;
        } else {
            size_t nl = strlen(nm);
            if (nl < 4 || strcasecmp(nm + nl - 4, ".jpg") != 0) continue;
        }
        if (count == cap) {
            size_t nc = cap * 2;
            char **g = heap_caps_realloc(names, nc * sizeof(char *), MALLOC_CAP_SPIRAM);
            if (!g) g = realloc(names, nc * sizeof(char *));
            if (!g) break;
            names = g;
            cap = nc;
        }
        size_t nl = strlen(nm);
        char *dup = heap_caps_malloc(nl + 1, MALLOC_CAP_SPIRAM);
        if (!dup) break;
        memcpy(dup, nm, nl + 1);
        names[count++] = dup;
    }
    closedir(d);
    if (names && count > 1)
        qsort(names, count, sizeof(char *), photo_name_cmp_desc);
    /* Stamp the cache valid only after a successful scan (an empty-but-openable
     * dir caches as 0; a failed open above did not get here). */
    s_lc_names = names;
    s_lc_count = count;
    strncpy(s_lc_bucket, bucket, sizeof(s_lc_bucket) - 1);
    s_lc_bucket[sizeof(s_lc_bucket) - 1] = '\0';
    s_lc_gen = gen;
    s_lc_valid = true;
    return count;
}

/* Copy one page's leaf names for `bucket` into out[] (each malloc'd; caller
 * frees). Returns count copied; *total = full bucket count. The cache lock is
 * held across the readdir/sort + slice copy only — never across a network
 * send, so a slow client can't block another listing request for seconds. */
static size_t listing_page(const char *bucket, int page, int per_page,
                           char **out, size_t out_cap, size_t *total) {
    if (total) *total = 0;
    if (!s_listing_mtx) return 0;
    xSemaphoreTake(s_listing_mtx, portMAX_DELAY);
    size_t count = listing_cache_ensure_locked(bucket);
    if (total) *total = count;
    size_t start, end;
    sd_layout_page_slice(count, page, per_page, &start, &end);
    size_t n = 0;
    for (size_t i = start; i < end && n < out_cap; i++) {
        char *dup = dup_str(s_lc_names[i]);
        if (!dup) break;
        out[n++] = dup;
    }
    xSemaphoreGive(s_listing_mtx);
    return n;
}

/* List the buckets present under the card root, newest-first. out[] entries
 * malloc'd (caller frees). Returns count (capped at out_cap). */
static size_t list_buckets(char **out, size_t out_cap) {
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) return 0;
    size_t n = 0;
    bool boot_seen = false, root_seen = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        sd_bucket_kind_t k = sd_layout_classify(e->d_name);
        if (k == SD_BUCKET_DATE) {
            if (n < out_cap) {
                char *dup = dup_str(e->d_name);
                if (dup) out[n++] = dup;
            }
        } else if (k == SD_BUCKET_BOOT) {
            boot_seen = true;
        } else if (k == SD_BUCKET_ROOT_JPG) {
            root_seen = true;
        }
    }
    closedir(d);
    if (boot_seen && n < out_cap) { char *p = dup_str(SD_LAYOUT_BOOT_DIR); if (p) out[n++] = p; }
    if (root_seen && n < out_cap) { char *p = dup_str(SD_LAYOUT_ROOT);     if (p) out[n++] = p; }
    if (n > 1) qsort(out, n, sizeof(char *), bucket_cmp_newest_first);
    return n;
}

/* Human label for a bucket in the gallery (dates shown verbatim). */
static const char *bucket_label(const char *bucket) {
    if (strcmp(bucket, SD_LAYOUT_ROOT) == 0) return tr(STR_PH_ROOT_BUCKET);
    if (strcmp(bucket, SD_LAYOUT_BOOT_DIR) == 0) return tr(STR_PH_BOOT_BUCKET);
    return bucket;
}

#define PHOTOS_PER_PAGE 100
static esp_err_t photos_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (!sd_storage_ready()) {
        char m[220];
        snprintf(m, sizeof(m),
            "<!doctype html><meta charset=utf-8><link rel=stylesheet href=/style.css>"
            "<title>Chytr\xc3\xa1 Budka</title><body><p>%s</p>", tr(STR_PH_SD_UNMOUNTED));
        return httpd_resp_sendstr(req, m);
    }

    /* ?day=<bucket> selects one day; absent → the day index. ?page=N within. */
    char day[SD_LAYOUT_DAY_LEN] = {0};
    int  page = 0;
    {
        char q[48], v[16];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            if (httpd_query_key_value(q, "day", v, sizeof(v)) == ESP_OK &&
                sd_layout_valid_day_param(v)) {
                strncpy(day, v, sizeof(day) - 1);
                day[sizeof(day) - 1] = '\0';
            }
            if (httpd_query_key_value(q, "page", v, sizeof(v)) == ESP_OK) {
                int p = atoi(v);
                if (p > 0) page = p;
            }
        }
    }

    char hdr[1280];

    /* ── Day index: list buckets, newest first (one readdir of the root). ── */
    if (day[0] == '\0') {
        int hn = snprintf(hdr, sizeof(hdr),
                 "<!doctype html><html><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                 "<title>Photos — %s</title>"
                 "<meta name=\"color-scheme\" content=\"dark\">"
                 "<link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
                 "<h1>%s</h1><p><a href=\"/\">%s</a></p><table>",
                 device_id_suffix(), tr(STR_PH_TITLE), tr(STR_PH_BACK));
        httpd_resp_send_chunk(req, hdr, hn);
        send_security_banner(req);

        size_t bcap = 800;
        char **buckets = (char **)malloc(bcap * sizeof(char *));
        size_t bn = buckets ? list_buckets(buckets, bcap) : 0;
        char row[256];
        for (size_t i = 0; i < bn; i++) {
            int rn = snprintf(row, sizeof(row),
                              "<tr><td><a href=\"/photos?day=%s\">%s</a></td></tr>",
                              buckets[i], bucket_label(buckets[i]));
            if (rn > 0) httpd_resp_send_chunk(req, row, rn);
            free(buckets[i]);
        }
        free(buckets);
        if (bn == 0) {
            char empty[120];
            snprintf(empty, sizeof(empty), "<tr><td class=\"empty\">%s</td></tr>",
                     tr(STR_PH_EMPTY));
            httpd_resp_send_chunk(req, empty, strlen(empty));
        }
        httpd_resp_send_chunk(req, "</table></body></html>", 22);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    /* ── One day's photos: readdir only that bucket (cached). ─────────────── */
    int hn = snprintf(hdr, sizeof(hdr),
             "<!doctype html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>%s — %s</title>"
             "<meta name=\"color-scheme\" content=\"dark\">"
             "<link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
             "<h1>%s <small>%s</small></h1>"
             "<p><a href=\"/photos\">%s</a> &middot; <a href=\"/\">%s</a></p><table>",
             bucket_label(day), device_id_suffix(),
             tr(STR_PH_TITLE), bucket_label(day),
             tr(STR_PH_ALL_DAYS), tr(STR_PH_BACK));
    httpd_resp_send_chunk(req, hdr, hn);
    send_security_banner(req);

    char *names[PHOTOS_PER_PAGE];
    size_t total = 0;
    size_t n = listing_page(day, page, PHOTOS_PER_PAGE, names, PHOTOS_PER_PAGE, &total);

    char row[400];
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];
        char path[180];
        if (strcmp(day, SD_LAYOUT_ROOT) == 0)
            snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
        else
            snprintf(path, sizeof(path), "%s/%s/%s", SD_MOUNT_POINT, day, name);
        struct stat st;
        long long size = (stat(path, &st) == 0) ? (long long)st.st_size : 0;
        char trig[16];
        sd_layout_trigger_of(name, trig, sizeof(trig));
        /* HTML-escape the readdir-sourced name + trig: firmware names are
         * charset-safe, but a file placed on the card out-of-band could carry
         * HTML metacharacters and reflect into this auth-gated page. */
        char esc_name[160], esc_trig[40];
        scan_html_escape(name, esc_name, sizeof(esc_name));
        scan_html_escape(trig, esc_trig, sizeof(esc_trig));
        int rn = snprintf(row, sizeof(row),
                          "<tr><td><a href=\"/view?d=%s&amp;f=%s\">%s</a></td>"
                          "<td class=\"tag\">%s</td>"
                          "<td class=\"size\">%lld B</td></tr>",
                          day, esc_name, esc_name, esc_trig, size);
        if (rn > 0) httpd_resp_send_chunk(req, row, rn);
        free(names[i]);
    }
    if (total == 0) {
        char empty[120];
        snprintf(empty, sizeof(empty), "<tr><td class=\"empty\">%s</td></tr>", tr(STR_PH_EMPTY));
        httpd_resp_send_chunk(req, empty, strlen(empty));
    }

    /* Pagination footer (links carry the day). */
    size_t start, end;
    sd_layout_page_slice(total, page, PHOTOS_PER_PAGE, &start, &end);
    size_t total_pages = (total + PHOTOS_PER_PAGE - 1) / PHOTOS_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
    char foot[512];
    int fn = snprintf(foot, sizeof(foot), "</table><p>%zu %s", total, tr(STR_PH_COUNT));
    if (total > 0) {
        fn += snprintf(foot + fn, sizeof(foot) - fn,
                       " &mdash; %s %zu&ndash;%zu (page %d / %zu)",
                       tr(STR_PH_SHOWING), start + 1, end, page + 1, total_pages);
    }
    fn += snprintf(foot + fn, sizeof(foot) - fn, ".</p><p>");
    if (page > 0) {
        fn += snprintf(foot + fn, sizeof(foot) - fn,
                       "<a href=\"/photos?day=%s&amp;page=%d\">%s</a>",
                       day, page - 1, tr(STR_PH_NEWER));
    }
    if (page > 0 && (size_t)(page + 1) < total_pages) {
        fn += snprintf(foot + fn, sizeof(foot) - fn, " &middot; ");
    }
    if ((size_t)(page + 1) < total_pages) {
        fn += snprintf(foot + fn, sizeof(foot) - fn,
                       "<a href=\"/photos?day=%s&amp;page=%d\">%s</a>",
                       day, page + 1, tr(STR_PH_OLDER));
    }
    fn += snprintf(foot + fn, sizeof(foot) - fn, "</p></body></html>");
    httpd_resp_send_chunk(req, foot, fn);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /photos.json — machine-readable listing for a JS/HA client.
 *   no ?day  → {"gen":G,"days":["2026-06-04",…,"boot","root"]}
 *   ?day=X   → {"day":"X","page":P,"per_page":N,"total":T,"pages":PP,
 *               "files":[{"f":"…","trig":"pir","bytes":123},…]}
 * The day index carries no per-day counts on purpose: counting would re-walk
 * every bucket and defeat the whole point of the tree. A client wanting counts
 * fetches a day, whose readdir is bounded. */
static esp_err_t photos_json_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (!sd_storage_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"sd not mounted\"}");
    }

    char day[SD_LAYOUT_DAY_LEN] = {0};
    int page = 0;
    {
        char q[48], v[16];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            if (httpd_query_key_value(q, "day", v, sizeof(v)) == ESP_OK &&
                sd_layout_valid_day_param(v)) {
                strncpy(day, v, sizeof(day) - 1);
                day[sizeof(day) - 1] = '\0';
            }
            if (httpd_query_key_value(q, "page", v, sizeof(v)) == ESP_OK) {
                int p = atoi(v);
                if (p > 0) page = p;
            }
        }
    }

    char buf[256];
    if (day[0] == '\0') {
        int n0 = snprintf(buf, sizeof(buf), "{\"gen\":%" PRIu32 ",\"days\":[",
                          sd_storage_listing_gen());
        httpd_resp_send_chunk(req, buf, n0);
        size_t bcap = 800;
        char **buckets = (char **)malloc(bcap * sizeof(char *));
        size_t bn = buckets ? list_buckets(buckets, bcap) : 0;
        for (size_t i = 0; i < bn; i++) {
            int rn = snprintf(buf, sizeof(buf), "%s\"%s\"", i ? "," : "", buckets[i]);
            httpd_resp_send_chunk(req, buf, rn);
            free(buckets[i]);
        }
        free(buckets);
        httpd_resp_send_chunk(req, "]}", 2);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char *names[PHOTOS_PER_PAGE];
    size_t total = 0;
    size_t n = listing_page(day, page, PHOTOS_PER_PAGE, names, PHOTOS_PER_PAGE, &total);
    size_t total_pages = (total + PHOTOS_PER_PAGE - 1) / PHOTOS_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
    int n0 = snprintf(buf, sizeof(buf),
                      "{\"day\":\"%s\",\"page\":%d,\"per_page\":%d,\"total\":%zu,"
                      "\"pages\":%zu,\"files\":[",
                      day, page, PHOTOS_PER_PAGE, total, total_pages);
    httpd_resp_send_chunk(req, buf, n0);
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];
        char path[180];
        if (strcmp(day, SD_LAYOUT_ROOT) == 0)
            snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
        else
            snprintf(path, sizeof(path), "%s/%s/%s", SD_MOUNT_POINT, day, name);
        struct stat st;
        long long size = (stat(path, &st) == 0) ? (long long)st.st_size : 0;
        char trig[16];
        sd_layout_trigger_of(name, trig, sizeof(trig));
        char esc_name[160], esc_trig[40];
        json_escape_str(name, esc_name, sizeof(esc_name));
        json_escape_str(trig, esc_trig, sizeof(esc_trig));
        int rn = snprintf(buf, sizeof(buf),
                          "%s{\"f\":\"%s\",\"trig\":\"%s\",\"bytes\":%lld}",
                          i ? "," : "", esc_name, esc_trig, size);
        httpd_resp_send_chunk(req, buf, rn);
        free(names[i]);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /photo?d=<bucket>&f=<leaf> — serve one JPEG from the date-tree. d
 * defaults to "root" (legacy flat files); both components are validated
 * (no '/', no "..") so a caller can't escape /sdcard. Photos are immutable,
 * so the response is cacheable with an ETag (conditional GET → 304) and a
 * single Range request returns 206 so a client can resume over a weak link. */
static esp_err_t photo_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char qbuf[128], fname[80], dparam[16] = SD_LAYOUT_ROOT;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK ||
        httpd_query_key_value(qbuf, "f", fname, sizeof(fname)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?f=");
        return ESP_OK;
    }
    {
        char v[16];
        if (httpd_query_key_value(qbuf, "d", v, sizeof(v)) == ESP_OK) {
            if (!sd_layout_valid_day_param(v)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ?d=");
                return ESP_OK;
            }
            strncpy(dparam, v, sizeof(dparam) - 1);
            dparam[sizeof(dparam) - 1] = '\0';
        }
    }
    if (!sd_layout_valid_leaf(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename: alphanumerics + . _ - only");
        return ESP_OK;
    }
    char path[180];
    if (strcmp(dparam, SD_LAYOUT_ROOT) == 0)
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, fname);
    else
        snprintf(path, sizeof(path), "%s/%s/%s", SD_MOUNT_POINT, dparam, fname);

    /* Use low-level open()/read() — stdio fread() on this FATFS port was
     * returning errno=EIO consistently at 32 KB (= 2 × 16 KB cluster)
     * boundary, suggesting stdio's read-ahead buffering interacts
     * badly with the SDIO 1-bit driver. read() doesn't buffer ahead. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not on /sdcard");
        return ESP_OK;
    }
    struct stat st;
    off_t fsize = (fstat(fd, &st) == 0) ? st.st_size : 0;

    /* ETag = leaf + size; a stored photo never changes. */
    char etag[96];
    snprintf(etag, sizeof(etag), "\"%s-%lld\"", fname, (long long)fsize);

    char inm[96];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        close(fd);
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    /* Optional single Range: bytes=A-[B]. */
    off_t start = 0, end = (fsize > 0) ? fsize - 1 : 0;
    bool ranged = false;
    char rng[64];
    if (fsize > 0 &&
        httpd_req_get_hdr_value_str(req, "Range", rng, sizeof(rng)) == ESP_OK &&
        strncmp(rng, "bytes=", 6) == 0) {
        const char *p = rng + 6;
        const char *dash = strchr(p, '-');
        if (dash) {
            long long a = (p != dash) ? atoll(p) : 0;
            long long b = (*(dash + 1)) ? atoll(dash + 1) : (long long)(fsize - 1);
            if (a >= fsize) {
                close(fd);
                char cr[48];
                snprintf(cr, sizeof(cr), "bytes */%lld", (long long)fsize);
                httpd_resp_set_status(req, "416 Range Not Satisfiable");
                httpd_resp_set_hdr(req, "Content-Range", cr);
                return httpd_resp_send(req, NULL, 0);
            }
            if (a < 0) a = 0;
            if (b > fsize - 1) b = fsize - 1;
            if (a <= b) {
                start = (off_t)a;
                end = (off_t)b;
                ranged = true;
            }
        }
    }
    if (ranged) {
        httpd_resp_set_status(req, "206 Partial Content");
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %lld-%lld/%lld",
                 (long long)start, (long long)end, (long long)fsize);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        if (start > 0) lseek(fd, start, SEEK_SET);
    }

    char *buf = (char *)scratch_alloc(4096);
    if (!buf) {
        close(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }
    off_t remaining = (fsize > 0) ? (end - start + 1) : 0;
    while (remaining > 0) {
        size_t want = (remaining > 4096) ? 4096 : (size_t)remaining;
        ssize_t rd = read(fd, buf, want);
        if (rd < 0) {
            if (errno == EINTR) continue;  /* not a truncation — retry */
            ESP_LOGW(TAG, "/photo %s: read errno=%d with %lld B left",
                     fname, errno, (long long)remaining);
            break;
        }
        if (rd == 0) break;  /* unexpected EOF (file shrank under us) */
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
            ESP_LOGW(TAG, "/photo %s: send_chunk failed", fname);
            break;
        }
        remaining -= rd;
    }
    free(buf);
    close(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── EXIF view helpers (shared by /view, /photo/exif, /last.json) ─────── */

/* Parse ?d=<bucket>&f=<leaf>, validate both (no '/', no ".." — same gate as
 * /photo), and build the on-card path. On bad input sends a 400 and returns
 * false. dparam defaults to the legacy root bucket. */
static bool photo_param_path(httpd_req_t *req, char *dparam, size_t dcap,
                             char *fname, size_t fcap, char *path, size_t pathcap) {
    snprintf(dparam, dcap, "%s", SD_LAYOUT_ROOT);
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK ||
        httpd_query_key_value(qbuf, "f", fname, fcap) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?f=");
        return false;
    }
    char v[16];
    if (httpd_query_key_value(qbuf, "d", v, sizeof(v)) == ESP_OK) {
        if (!sd_layout_valid_day_param(v)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ?d=");
            return false;
        }
        snprintf(dparam, dcap, "%s", v);
    }
    if (!sd_layout_valid_leaf(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename: alphanumerics + . _ - only");
        return false;
    }
    if (strcmp(dparam, SD_LAYOUT_ROOT) == 0)
        snprintf(path, pathcap, "%s/%s", SD_MOUNT_POINT, fname);
    else
        snprintf(path, pathcap, "%s/%s/%s", SD_MOUNT_POINT, dparam, fname);
    return true;
}

/* Read the EXIF metadata out of a stored photo by peeking only the first
 * EXIF_PEEK_BYTES of the file (the APP1 segment lives right after SOI). */
static bool exif_read_file(const char *path, exif_meta_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint8_t *hdr = (uint8_t *)scratch_alloc(EXIF_PEEK_BYTES);
    bool ok = false;
    if (hdr) {
        size_t total = 0;
        while (total < EXIF_PEEK_BYTES) {
            ssize_t r = read(fd, hdr + total, EXIF_PEEK_BYTES - total);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            total += (size_t)r;
        }
        if (total > 0) ok = exif_read(hdr, total, out);
        free(hdr);
    }
    close(fd);
    return ok;
}

/* Emit EXIF as JSON. The UserComment payload is already a JSON object, so it's
 * embedded verbatim under "telemetry" when it looks well-formed (firmware
 * always writes it so), else null. String fields are JSON-escaped. */
static void exif_send_json(httpd_req_t *req, const exif_meta_t *m, bool have) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (!have) { httpd_resp_sendstr(req, "{\"exif\":false}"); return; }
    char buf[256], e[160];
    httpd_resp_sendstr_chunk(req, "{\"exif\":true");
#define J(key, val) do {                                            \
        json_escape_str((val), e, sizeof(e));                       \
        snprintf(buf, sizeof(buf), ",\"" key "\":\"%s\"", e);       \
        httpd_resp_sendstr_chunk(req, buf);                         \
    } while (0)
    J("datetime", m->datetime);
    J("offset",   m->offset);
    J("trigger",  m->trigger);
    J("model",    m->model);
    J("software", m->software);
    J("lens",     m->lens);
#undef J
    snprintf(buf, sizeof(buf), ",\"have_dt\":%s,\"telemetry\":",
             m->have_dt ? "true" : "false");
    httpd_resp_sendstr_chunk(req, buf);
    size_t ul = strlen(m->user_json);
    bool obj_ok = ul >= 2 && m->user_json[0] == '{' && m->user_json[ul - 1] == '}';
    httpd_resp_sendstr_chunk(req, obj_ok ? m->user_json : "null");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, NULL);
}

/* GET /photo/exif?d=&f= — EXIF metadata of one stored photo as JSON. */
static esp_err_t photo_exif_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char dparam[16], fname[80], path[180];
    if (!photo_param_path(req, dparam, sizeof(dparam), fname, sizeof(fname),
                          path, sizeof(path)))
        return ESP_OK;  /* 400 already sent */
    exif_meta_t m;
    bool have = exif_read_file(path, &m);
    exif_send_json(req, &m, have);
    return ESP_OK;
}

/* GET /last.json — EXIF of the last *stored* frame (the one /last.jpg serves),
 * for live debugging without opening the gallery. */
static esp_err_t last_json_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    exif_meta_t m;
    memset(&m, 0, sizeof(m));
    bool have = false;
    uint8_t *hdr = (uint8_t *)scratch_alloc(EXIF_PEEK_BYTES);
    if (hdr) {
        size_t n = camera_last_jpeg_peek_header(hdr, EXIF_PEEK_BYTES);
        if (n) have = exif_read(hdr, n, &m);
        free(hdr);
    }
    exif_send_json(req, &m, have);
    return ESP_OK;
}

/* GET /view?d=&f= — single-photo viewer: the image with a capture-time +
 * telemetry caption overlaid (HTML/CSS, no pixel burn-in), plus a full EXIF
 * table below and a link to download the original. */
static esp_err_t view_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char dparam[16], fname[80], path[180];
    if (!photo_param_path(req, dparam, sizeof(dparam), fname, sizeof(fname),
                          path, sizeof(path)))
        return ESP_OK;  /* 400 already sent */

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[700], esc_f[160], esc_d[24];
    scan_html_escape(fname, esc_f, sizeof(esc_f));
    scan_html_escape(dparam, esc_d, sizeof(esc_d));

    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"dark\">"
        "<link rel=\"stylesheet\" href=\"/style.css\">");
    snprintf(buf, sizeof(buf), "<title>%s — %s</title></head><body>",
             tr(STR_VIEW_TITLE), esc_f);
    httpd_resp_sendstr_chunk(req, buf);
    send_security_banner(req);
    snprintf(buf, sizeof(buf),
        "<h1>%s <small>%s</small></h1>"
        "<p><a href=\"/photos\">%s</a> &middot; <a href=\"/\">%s</a></p>",
        tr(STR_VIEW_TITLE), esc_f, tr(STR_PH_ALL_DAYS), tr(STR_NAV_HOME));
    httpd_resp_sendstr_chunk(req, buf);

    exif_meta_t m;
    bool have = exif_read_file(path, &m);

    snprintf(buf, sizeof(buf),
        "<div class=\"frame\"><img alt=\"\" src=\"/photo?d=%s&amp;f=%s\">",
        esc_d, esc_f);
    httpd_resp_sendstr_chunk(req, buf);
    if (have) {
        char capt[256];
        exif_caption_html(&m, capt, sizeof(capt));
        if (capt[0]) {
            snprintf(buf, sizeof(buf), "<div class=\"cap\">%s</div>", capt);
            httpd_resp_sendstr_chunk(req, buf);
        }
    }
    httpd_resp_sendstr_chunk(req, "</div>");

    snprintf(buf, sizeof(buf),
        "<p><a href=\"/photo?d=%s&amp;f=%s\">&#11015; %s</a></p><h2>%s</h2>",
        esc_d, esc_f, tr(STR_DL_ORIGINAL), tr(STR_EXIF_META));
    httpd_resp_sendstr_chunk(req, buf);

    if (!have) {
        snprintf(buf, sizeof(buf), "<p class=\"empty\">%s</p></body></html>",
                 tr(STR_NO_EXIF));
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    httpd_resp_sendstr_chunk(req, "<table>");
#define ROW(label, valhtml) do {                                              \
        snprintf(buf, sizeof(buf),                                            \
                 "<tr><td class=\"k\">%s</td><td class=\"v\">%s</td></tr>",    \
                 (label), (valhtml));                                         \
        httpd_resp_sendstr_chunk(req, buf);                                   \
    } while (0)
    char e[160];
    {   /* Captured = datetime (+ offset). EXIF tag names stay English. */
        char dt[24], ev[64];
        if (m.have_dt) exif_reformat_dt(m.datetime, dt, sizeof(dt));
        else           snprintf(dt, sizeof(dt), "%s", tr(STR_NO_CLOCK));
        if (m.offset[0]) snprintf(ev, sizeof(ev), "%s %.7s", dt, m.offset);
        else             snprintf(ev, sizeof(ev), "%s", dt);
        ROW("Captured", ev);
    }
    if (m.trigger[0])  { scan_html_escape(m.trigger,  e, sizeof(e)); ROW("Trigger",  e); }
    if (m.model[0])    { scan_html_escape(m.model,    e, sizeof(e)); ROW("Device",   e); }
    if (m.software[0]) { scan_html_escape(m.software, e, sizeof(e)); ROW("Firmware", e); }
    if (m.lens[0])     { scan_html_escape(m.lens,     e, sizeof(e)); ROW("Sensor",   e); }

    /* Telemetry from the UserComment JSON. prec<0 = boolean (yes/no). */
    static const struct { const char *key, *label, *unit; int prec; } TEL[] = {
        {"agc", "AGC", "", 0}, {"ir", "IR", "", -1},
        {"rssi", "RSSI", "&nbsp;dBm", 0}, {"framesize", "Framesize", "", 0},
        {"quality", "JPEG&nbsp;quality", "", 0},
        {"mcu_c", "MCU&nbsp;temp", "&nbsp;&deg;C", 1},
        {"vbatt", "Battery", "&nbsp;V", 3}, {"soc", "SOC", "&nbsp;%", 0},
        {"heap", "Free&nbsp;heap", "&nbsp;B", 0}, {"uptime_s", "Uptime", "&nbsp;s", 0},
        {"capture_ms", "Capture", "&nbsp;ms", 0}, {"seq", "Sequence", "", 0},
    };
    for (size_t i = 0; i < sizeof(TEL) / sizeof(TEL[0]); i++) {
        double v;
        if (!exif_json_num(m.user_json, TEL[i].key, &v)) continue;
        char vv[48];
        if (TEL[i].prec < 0)
            snprintf(vv, sizeof(vv), "%s", (v != 0) ? "yes" : "no");
        else
            snprintf(vv, sizeof(vv), "%.*f%s", TEL[i].prec, v, TEL[i].unit);
        ROW(TEL[i].label, vv);
    }
#undef ROW
    httpd_resp_sendstr_chunk(req, "</table></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* GET /i2c — sweep the shared I²C bus and report which addresses answer,
 * annotated against the chips we ship drivers for. Read-only — safe to
 * call from the field. Useful when wiring a new sensor: re-hit the
 * endpoint after each wire move instead of rebooting to see the boot
 * scan again. */
static esp_err_t i2c_scan_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* parity with the other sensitive endpoints */
    char report[1024];
    i2c_bus_scan_report(report, sizeof(report));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, report, strlen(report));
}

/* GET /i2c/bus1_diag — deep diagnostic for the bit-banged bus1 (D6/D7).
 *
 * Reports — in this order:
 *   pins         which GPIOs the bit-bang has cached (-1 = uninitialised)
 *   idle_pre     SDA/SCL pad levels right now, before we touch anything
 *   reinit_rc    return code of forcing a fresh i2c_bb init (recovery
 *                clock burst included). Even on a "working" bus we run
 *                this because we want the idle reading *after* the
 *                recovery sequence too — that's what catches a slave
 *                that wedged a few seconds after boot.
 *   idle_post    SDA/SCL pad levels after reinit
 *   probe_0x44   number of ACKs out of 8 probes at SHT41 address
 *   probe_0x36   number of ACKs out of 8 probes at MAX17048 address
 *
 * Reading: idle_post must be (1,1) for the bus to be electrically usable.
 * If either is 0 → line stuck low (slave holding it, wrong pin, missing
 * pull-up, or a peripheral grabbed the IO_MUX pad after our init). With
 * idle (1,1) and probe_0x44=0 the slave isn't responding at the expected
 * address — usually means it's wired to a different addr-strap or its
 * VDD isn't connected. */
static esp_err_t i2c_bus1_diag_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* parity with the other sensitive endpoints */
    char body[768];
    char *p = body;
    size_t left = sizeof(body);

    int sda_pin = -1, scl_pin = -1;
    i2c_bb_pins(&sda_pin, &scl_pin);

    int sda_pre = -1, scl_pre = -1;
    i2c_bb_idle_read(&sda_pre, &scl_pre);
    /* Reflect the live pin-map view of which GPIOs bus1 SHOULD be on
     * (vs sda_pin/scl_pin = what i2c_bb_init actually cached). After
     * the migration to the pin function map these can disagree only
     * if NVS was modified between the bus1_ensure call at boot and
     * now — e.g. operator reassigned slots via MQTT without rebooting
     * yet. */
    int sda_cfg = app_config_pin_for_first("i2c1_sda");
    int scl_cfg = app_config_pin_for_first("i2c1_scl");
    int n = snprintf(p, left,
                     "pins: sda=GPIO%d scl=GPIO%d (pin_map %d/%d)\n"
                     "idle_pre:  SDA=%d SCL=%d\n",
                     sda_pin, scl_pin, sda_cfg, scl_cfg,
                     sda_pre, scl_pre);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    esp_err_t e = (sda_cfg >= 0 && scl_cfg >= 0)
        ? i2c_bb_reinit(sda_cfg, scl_cfg)
        : ESP_ERR_INVALID_STATE;
    int sda_post = -1, scl_post = -1;
    i2c_bb_idle_read(&sda_post, &scl_post);
    n = snprintf(p, left,
                 "reinit_rc: %s\nidle_post: SDA=%d SCL=%d\n",
                 esp_err_to_name(e), sda_post, scl_post);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    /* Drive-low readback. Healthy IO buffer reads 0 when we sink the
     * line. A pad whose output stage died (ESD, latch-up) stays at 1
     * because only the internal pull-up is active. This separates a
     * "no slave wired" symptom from a "XIAO can't drive bus1 anymore"
     * symptom — both produce the same probe=0/8 reading. */
    int sda_dl = -1, scl_dl = -1;
    i2c_bb_drive_low_check(&sda_dl, &scl_dl);
    n = snprintf(p, left,
                 "drive_low_readback: SDA=%d SCL=%d  (expect 0/0 on a healthy pad)\n",
                 sda_dl, scl_dl);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    int sda_to_scl = -1, scl_to_sda = -1;
    i2c_bb_short_check(&sda_to_scl, &scl_to_sda);
    n = snprintf(p, left,
                 "short_check: sda→scl_bleed=%d scl→sda_bleed=%d  "
                 "(both 0 = clean; either 1 = SDA/SCL shorted)\n",
                 sda_to_scl, scl_to_sda);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    /* Drive-low leaves the bit-bang state pristine (both lines released
     * back to HIGH at the end), but a slave that saw the spurious SDA
     * falling edge during the check may be sitting in addressing limbo.
     * One more reinit (recovery clocks + STOP) clears that. */
    if (sda_cfg >= 0 && scl_cfg >= 0) {
        i2c_bb_reinit(sda_cfg, scl_cfg);
    }

    /* Wider scan: list every 7-bit address that ACKs on bus1. Same
     * single-shot we do in /i2c, but here we always print so the
     * operator can spot a strap-variant slave at 0x45/0x46/0x47 (SHT4x)
     * or 0x6C (MAX17041/44) without re-hitting /i2c afterward. */
    int n_found = 0;
    n = snprintf(p, left, "scan: ");
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_bb_probe(a)) {
            n = snprintf(p, left, "%s0x%02x", n_found ? " " : "", a);
            if (n > 0 && (size_t)n < left) { p += n; left -= n; }
            n_found++;
        }
    }
    if (!n_found) {
        n = snprintf(p, left, "(none)");
        if (n > 0 && (size_t)n < left) { p += n; left -= n; }
    }
    n = snprintf(p, left, "\n");
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    int ack44 = 0, ack36 = 0;
    for (int i = 0; i < 8; i++) {
        if (i2c_bb_probe(0x44))
            ack44++;
        if (i2c_bb_probe(0x36))
            ack36++;
    }
    n = snprintf(p, left,
                 "probe_0x44 (SHT41): %d/8\n"
                 "probe_0x36 (MAX17048): %d/8\n",
                 ack44, ack36);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    /* Active recovery hook: if probes ACKed but the bus1 SHT41 (registry id
     * "sht1") is still not ready (sensor was just plugged in / wires fixed
     * and the periodic re-probe hasn't fired yet), force a probe directly
     * from this handler. Means the operator hits /i2c/bus1_diag once, sees
     * ACKs, AND the homepage's "outside" row goes live without a reboot. */
    if (ack44 >= 4 && !cb_sensor_ready("sht1")) {
        esp_err_t ie = cb_sensor_probe("sht1");
        n = snprintf(p, left,
                     "sht1 probe triggered (probes ACKed): %s — now ready=%d\n",
                     esp_err_to_name(ie), (int)cb_sensor_ready("sht1"));
        if (n > 0 && (size_t)n < left) { p += n; left -= n; }
    } else {
        n = snprintf(p, left, "sht1 ready: %d  (cached state)\n",
                     (int)cb_sensor_ready("sht1"));
        if (n > 0 && (size_t)n < left) { p += n; left -= n; }
    }

    /* SDA/SCL swap test — flip the firmware pin assignment and re-probe.
     * If the breakout / harness has SDA-SCL crossed (common when both
     * lines are red/yellow and the silkscreen got misread), this pass
     * will ACK while the default pass won't. Restore the canonical pin
     * order at the end so subsequent /i2c calls and the periodic
     * sht41_ext re-probe see the bus as expected. */
    int swap_ack44 = 0;
    if (sda_cfg >= 0 && scl_cfg >= 0 &&
        i2c_bb_reinit(scl_cfg, sda_cfg) == ESP_OK) {
        for (int i = 0; i < 8; i++) {
            if (i2c_bb_probe(0x44))
                swap_ack44++;
        }
    }
    if (sda_cfg >= 0 && scl_cfg >= 0) {
        i2c_bb_reinit(sda_cfg, scl_cfg);
    }
    n = snprintf(p, left,
                 "swap_test (sda=GPIO%d scl=GPIO%d): probe_0x44 %d/8\n",
                 scl_cfg, sda_cfg, swap_ack44);
    if (n > 0 && (size_t)n < left) { p += n; left -= n; }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, strlen(body));
}

/* GET /sht41/bus1 — single read from the diagnostic SHT41 on the
 * bit-banged bus1 (D6/D7). Returns "T=…°C RH=…%" plaintext, or an
 * error message with the underlying esp_err_t name on failure. No
 * caching here — this is a manual diagnostic endpoint. */
static esp_err_t sht41_bus1_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* parity with the other sensitive endpoints */
    esp_err_t e = cb_sensor_probe("sht1");   /* (re)detect a just-wired sensor */
    cb_sensor_refresh_one("sht1");           /* force a fresh read into the cache */
    float t_c = 0, rh = 0;
    char body[96];
    int n;
    if (cb_sensor_chan("sht1", "temp_ext", &t_c) &&
        cb_sensor_chan("sht1", "humidity_ext", &rh)) {
        n = snprintf(body, sizeof(body), "T=%.2f°C  RH=%.2f%%\n", t_c, rh);
    } else {
        /* No reading: nothing on bus1 (D6/D7) or a wired sensor faulted.
         * esp_err_to_name(e) carries the probe result — typically
         * ESP_ERR_NOT_FOUND when the diagnostic SHT41 simply isn't fitted. */
        n = snprintf(body, sizeof(body), "sht41 bus1: not detected (%s)\n",
                     esp_err_to_name(e));
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, n);
}

/* GET /sensors — on-demand FRESH read of every registered I²C sensor
 * (bus0 + bus1), straight from the sensor registry. Unlike the homepage
 * (which shows the telemetry-refreshed caches), this forces a live read,
 * so it's the diagnostic to trust for "is the sensor actually reading
 * right now?" — across all sensors, not just the two bus1 ones. New
 * sensors appear here automatically. */
static esp_err_t sensors_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* parity with the other sensitive endpoints */
    cb_sensors_refresh();      /* live reads (on-demand/diagnostic, rare) */
    char body[768];
    int o = 0;
    for (size_t si = 0; si < CB_SENSORS_N && o < (int)sizeof(body) - 1; si++) {
        const cb_sensor_t *s = &CB_SENSORS[si];
        bool present = s->read_ok ? s->read_ok() : (!s->present || s->present());
        o += snprintf(body + o, sizeof(body) - o, "%-4s %-16s bus%d 0x%02x  %s\n",
                      s->id, s->name, (int)s->bus, s->addr, present ? "present" : "ABSENT");
        if (!present)
            continue;
        for (size_t ci = 0; ci < s->n_chans && o < (int)sizeof(body) - 1; ci++) {
            const cb_chan_t *c = &s->chans[ci];
            float v;
            if (c->read(&v) && isfinite(v))
                o += snprintf(body + o, sizeof(body) - o, "  %-13s = %.*f %s\n",
                              c->obj, c->decimals, (double)v, c->unit);
            else
                o += snprintf(body + o, sizeof(body) - o, "  %-13s = n/a\n", c->obj);
        }
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, o);
}

/* Minimal in-place percent-decode (%XX + '+'→space) for a query value. */
static void url_decode(char *s) {
    char *o = s;
    for (char *i = s; *i; i++) {
        if (*i == '+') {
            *o++ = ' ';
        } else if (*i == '%' && isxdigit((unsigned char)i[1]) && isxdigit((unsigned char)i[2])) {
            char h[3] = {i[1], i[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            *o++ = *i;
        }
    }
    *o = 0;
}

/* GET /oled/qr[?text=...] — render a QR on the bench OLED for ~90 s. Default
 * text is now the board's OWN web-UI URL (https://<id>.<domain>/), so scanning
 * the panel opens the live page on a phone; pass ?text=... to override. The
 * screen returns to the status page afterwards. */
static esp_err_t oled_qr_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char text[160];
    device_url(text, sizeof(text), wifi_mgr_get_domain());
    char q[256], val[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "text", val, sizeof(val)) == ESP_OK) {
        url_decode(val);
        strlcpy(text, val, sizeof(text));
    }
    bool ok = oled_show_qr(text);
    char body[240];
    int n = snprintf(body, sizeof(body), "%s\n%s\n",
                     ok ? "QR shown on OLED for ~90 s — scan it." : "no OLED present", text);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, n);
}

/* GET /oled/logo.bin — the EFFECTIVE boot logo (NVS custom, else baked
 * default) as raw 1024 bytes, so the upload form can show what's current. */
static esp_err_t oled_logo_bin_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    uint8_t *buf = malloc(OLED_LOGO_BYTES);
    if (!buf)
        return httpd_resp_send_500(req);
    oled_get_boot_logo(buf, OLED_LOGO_BYTES);
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t e = httpd_resp_send(req, (const char *)buf, OLED_LOGO_BYTES);
    free(buf);
    return e;
}

/* GET /oled/logo — a small in-browser uploader: pick any image, it's drawn
 * to a 128x64 canvas, thresholded to 1-bit (with a live preview) and the raw
 * 1024-byte SSD1306 bitmap is POSTed to this same URL. No server-side image
 * decoding — all the conversion is client-side JS. */
static esp_err_t oled_logo_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    static const char PAGE[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>OLED boot logo</title>"
        "<body style='font-family:sans-serif;max-width:30em;margin:1em'>"
        "<h2>OLED boot logo</h2>"
        "<p>Pick an image &mdash; it&apos;s converted to 128&times;64 1-bit in your browser and uploaded. "
        "White (lit) pixels = bright areas of the image.</p>"
        "<input type=file id=f accept='image/*'><br><br>"
        "threshold <input type=range id=th min=1 max=254 value=128> <span id=tv>128</span><br>"
        "invert <input type=checkbox id=inv><br><br>"
        "<canvas id=c width=128 height=64 style='border:1px solid #888;image-rendering:pixelated;width:256px;height:128px;background:#000'></canvas><br><br>"
        "<button onclick=up()>Upload &amp; store</button> "
        "<button onclick=test()>Preview boot screen on OLED</button> "
        "<button onclick=clr()>Clear (text splash)</button>"
        "<pre id=msg></pre>"
        "<script>"
        "let img=null;"
        "const c=document.getElementById('c'),x=c.getContext('2d'),"
        "th=document.getElementById('th'),inv=document.getElementById('inv'),msg=document.getElementById('msg');"
        "document.getElementById('f').onchange=async e=>{img=new Image();img.src=URL.createObjectURL(e.target.files[0]);await img.decode();render();};"
        "th.oninput=()=>{document.getElementById('tv').textContent=th.value;render();};"
        "inv.onchange=render;"
        /* pack() always redraws the ORIGINAL image before sampling (never the
         * 1-bit preview render() leaves on the canvas) — else invert/threshold
         * get applied twice (the "invert does nothing" bug). */
        "function pack(){x.fillStyle='#000';x.fillRect(0,0,128,64);if(img)x.drawImage(img,0,0,128,64);"
        "const d=x.getImageData(0,0,128,64).data,b=new Uint8Array(1024),t=+th.value,iv=inv.checked;"
        "for(let y=0;y<64;y++)for(let xx=0;xx<128;xx++){const i=(y*128+xx)*4;let on=(.299*d[i]+.587*d[i+1]+.114*d[i+2])>=t;if(iv)on=!on;if(on)b[(y>>3)*128+xx]|=1<<(y&7);}return b;}"
        "function render(){draw(pack());}"
        "async function up(){if(!img){msg.textContent='pick an image first';return;}"
        "const r=await fetch('/oled/logo',{method:'POST',body:pack()});msg.textContent=await r.text()+' (reboot to see it)';}"
        "async function clr(){const r=await fetch('/oled/logo?clear=1',{method:'POST'});msg.textContent=await r.text();}"
        "async function test(){const r=await fetch('/oled/logo?test=1',{method:'POST'});msg.textContent=await r.text();}"
        "function draw(b){x.fillStyle='#000';x.fillRect(0,0,128,64);x.fillStyle='#0ff';"
        "for(let y=0;y<64;y++)for(let xx=0;xx<128;xx++)if(b[(y>>3)*128+xx]&(1<<(y&7)))x.fillRect(xx,y,1,1);}"
        /* on load, show the current effective boot logo so you see what's set */
        "(async()=>{try{const r=await fetch('/oled/logo.bin');if(r.ok){const b=new Uint8Array(await r.arrayBuffer());if(b.length==1024)draw(b);}}catch(e){}})();"
        "</script></body>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

/* POST /oled/logo — upload a custom boot logo: the raw 1024-byte 128x64
 * SSD1306 bitmap (page-major, LSB=top — e.g. image2cpp "vertical, 1 bit",
 * or via the GET /oled/logo form). /oled/logo?clear=1 removes it. */
static esp_err_t oled_logo_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    char q[32], v[8];
    bool has_q = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK;
    if (has_q && httpd_query_key_value(q, "clear", v, sizeof(v)) == ESP_OK) {
        oled_clear_logo();
        oled_show_boot();   /* preview the (now baked-default) boot screen */
        return httpd_resp_sendstr(req, "custom logo cleared (baked-in default restored); previewing\n");
    }
    if (has_q && httpd_query_key_value(q, "test", v, sizeof(v)) == ESP_OK) {
        bool ok = oled_show_boot();   /* preview current boot screen, no upload */
        return httpd_resp_sendstr(req, ok ? "previewing boot screen on OLED (~10 s)\n"
                                          : "no OLED present\n");
    }
    if (req->content_len != OLED_LOGO_BYTES) {
        char m[140];
        int n = snprintf(m, sizeof(m),
                         "POST exactly %d bytes (128x64 SSD1306 page-major bitmap); "
                         "got %d. Clear with /oled/logo?clear=1.\n",
                         OLED_LOGO_BYTES, (int)req->content_len);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, m, n);
    }
    uint8_t *buf = malloc(OLED_LOGO_BYTES);
    if (!buf)
        return httpd_resp_send_500(req);
    int got = 0;
    while (got < OLED_LOGO_BYTES) {
        int r = httpd_req_recv(req, (char *)buf + got, OLED_LOGO_BYTES - got);
        if (r <= 0) { free(buf); return httpd_resp_send_500(req); }
        got += r;
    }
    bool ok = oled_set_logo(buf, OLED_LOGO_BYTES);
    free(buf);
    if (ok)
        oled_show_boot();   /* preview the just-stored logo immediately */
    return httpd_resp_sendstr(req, ok ? "boot logo stored — previewing on OLED (~10 s)\n"
                                      : "store failed\n");
}

/* GET /max17048/bus1 — diagnostic readout from a MAX17048 fuel gauge
 * wired to the bit-banged bus1. Tries the standard 16-bit register
 * read (addr+W, reg, repeated START, addr+R, 2 bytes). We hit:
 *   0x08 VERSION — should report 0x001x for a real MAX17048
 *   0x02 VCELL   — 78.125 µV / LSB, 16-bit big-endian
 *   0x04 SOC     — 1/256 % per LSB
 * Each register is retried up to 8 times since the AliExpress clones
 * are intermittent. The point of this endpoint is to compare bit-bang
 * results against the HW i2c_master path on bus0, which never gets
 * past register reads despite seeing the chip ACK at the address. */
static esp_err_t max17048_bus1_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* parity with the other sensitive endpoints */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (!i2c_bus1_ensure()) {
        return httpd_resp_send(req, "bus1 init failed\n", HTTPD_RESP_USE_STRLEN);
    }

    static const struct { uint8_t reg; const char *name; } REGS[] = {
        {0x08, "VERSION"},
        {0x02, "VCELL"},
        {0x04, "SOC"},
    };

    char body[512];
    char *p = body;
    size_t left = sizeof(body);

    for (size_t i = 0; i < sizeof(REGS) / sizeof(REGS[0]); i++) {
        uint8_t rx[2] = {0};
        esp_err_t e = ESP_FAIL;
        int attempts = 0;
        for (; attempts < 8; attempts++) {
            e = i2c_bb_transmit_receive(0x36, &REGS[i].reg, 1, rx, 2);
            if (e == ESP_OK)
                break;
        }
        int n;
        if (e == ESP_OK) {
            uint16_t v = ((uint16_t)rx[0] << 8) | rx[1];
            if (REGS[i].reg == 0x02) {
                float volts = (float)v * 78.125e-6f;
                n = snprintf(p, left, "0x%02x %-8s 0x%04x  %.3f V  (try %d/8)\n",
                             REGS[i].reg, REGS[i].name, v, volts, attempts + 1);
            } else if (REGS[i].reg == 0x04) {
                float soc = (float)v / 256.0f;
                n = snprintf(p, left, "0x%02x %-8s 0x%04x  %.2f %%  (try %d/8)\n",
                             REGS[i].reg, REGS[i].name, v, soc, attempts + 1);
            } else {
                n = snprintf(p, left, "0x%02x %-8s 0x%04x  (try %d/8)\n",
                             REGS[i].reg, REGS[i].name, v, attempts + 1);
            }
        } else {
            n = snprintf(p, left, "0x%02x %-8s ERR %s after %d tries\n",
                         REGS[i].reg, REGS[i].name, esp_err_to_name(e), attempts);
        }
        if (n > 0 && (size_t)n < left) {
            p += n;
            left -= n;
        }
    }
    return httpd_resp_send(req, body, p - body);
}

#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
/* GET /debug/sd_format?confirm=yes — reformat the SD card in place.
 * Wipes every photo. Use when readdir loses sync with reality
 * (orphan cluster chains from earlier write failures) and a host
 * mkfs.fat is not on offer because the budka is already in the field. */
static esp_err_t debug_sd_format_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);  /* debug routes are destructive — gate behind web-admin auth */
    char qbuf[32], val[8];
    bool confirmed = false;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
        httpd_query_key_value(qbuf, "confirm", val, sizeof(val)) == ESP_OK &&
        strcmp(val, "yes") == 0) {
        confirmed = true;
    }
    if (!confirmed) {
        const char *m = "Reformat SD (wipes all data). Add ?confirm=yes.\n";
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, m, strlen(m));
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    /* Two different ESP-IDF paths: format in place when already mounted
     * (recovers from FATFS chain corruption), or mount-with-format
     * when init failed (recovers a fresh / wiped card on which the
     * boot-time mount returned ESP_FAIL = FR_NO_FILESYSTEM). */
    esp_err_t e = sd_storage_ready() ? sd_storage_format()
                                     : sd_storage_format_unmounted();
    char body[140];
    int n = snprintf(body, sizeof(body), "%s — %s\n", (e == ESP_OK) ? "OK" : "FAIL",
                     (e == ESP_OK) ? "SD card formatted + mounted, /sdcard is empty"
                                   : esp_err_to_name(e));
    return httpd_resp_send(req, body, n);
}

/* GET /debug/sd_remount — non-destructive retry of sd_storage_init().
 * Mount path runs once at boot; after a physical reseat (or hot-plug)
 * the runtime stays in "not mounted" until reboot. This endpoint
 * re-runs sd_storage_init() so the operator can reseat the card and
 * recover without an MQTT cmd/reboot round-trip. Does NOT format —
 * use /debug/sd_format for that. Returns the esp_err_to_name() of
 * the mount attempt so a remaining ESP_ERR_TIMEOUT pinpoints the
 * card-not-detected case vs ESP_FAIL = FR_NO_FILESYSTEM. */
/* GET /debug/sd_migrate — push the legacy flat-root → day-bucket migration
 * along by ONE bounded batch and report progress. Deliberately NOT a
 * run-to-completion loop: that would block the single HTTP server task for
 * minutes on a card with thousands of legacy files (starving every other
 * request). Call it repeatedly to drain a big root, or just let the per-capture
 * lazy path finish it. `moved>0` means more may remain; `moved==0` means done.
 * ?n=<count> overrides the batch size (default 256). */
static esp_err_t debug_sd_migrate_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    int batch = 256;
    char q[24], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int b = atoi(v);
        if (b > 0 && b <= 2000) batch = b;
    }
    int moved = sd_storage_migrate_step(batch);
    char body[96];
    int len = snprintf(body, sizeof(body),
                       "moved %d this batch (%s)\n",
                       moved, moved > 0 ? "call again to continue" : "done — root clean");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, len);
}

static esp_err_t debug_sd_remount_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    esp_err_t e = sd_storage_init();
    char body[160];
    int n = snprintf(body, sizeof(body),
                     "%s — %s%s\n",
                     (e == ESP_OK) ? "OK" : "FAIL",
                     esp_err_to_name(e),
                     (e == ESP_ERR_TIMEOUT)
                         ? " (no card detected — check seating + power-cycle)"
                         : "");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, n);
}

/* GET /debug/pir — live GPIO level + counters for the PIR pin.
 * Sampling-side diagnostic: distinguishes "polling task wedged" from
 * "sensor not driving HIGH" when the operator reports motion not
 * triggering. Reads the raw GPIO level (post-pull-down) plus the
 * polling task's running motion_count + last_motion_ms. */
static esp_err_t debug_pir_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[256];
    int level = pir_raw_level_nth(0);  /* RTC-mux read when RTC-IO — valid in light-sleep */
    uint32_t poll_iters = 0, high_reads = 0, ext1_wakes = 0;
    pir_debug_poll_stats(&poll_iters, &high_reads, &ext1_wakes);
    int n = snprintf(body, sizeof(body),
                     "pin=GPIO%d level=%d ready=%s motion_count=%" PRIu32
                     " last_motion_ms=%" PRIu32
                     " poll_iters=%" PRIu32 " high_reads=%" PRIu32
                     " ext1_wakes=%" PRIu32 "\n",
                     PIR_PIN, level, pir_ready() ? "true" : "false",
                     pir_motion_count(), pir_last_motion_ms(),
                     poll_iters, high_reads, ext1_wakes);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, n > 0 ? n : 0);
}

/* GET /debug/wifi_disconnect?confirm=yes — kick the board off WiFi.
 * Equivalent to UniFi "Reconnect" but works regardless of which AP the
 * board is currently associated to. Exercises the backoff pipeline:
 * the firmware should log a STA_DISCONNECTED, schedule a 500 ms retry,
 * reassociate, and log GOT_IP again with "after N disconnects" tag. */
static esp_err_t debug_wifi_disconnect_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char qbuf[32], val[8];
    bool confirmed = false;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
        httpd_query_key_value(qbuf, "confirm", val, sizeof(val)) == ESP_OK &&
        strcmp(val, "yes") == 0) {
        confirmed = true;
    }
    if (!confirmed) {
        const char *m = "Force WiFi disconnect. Add ?confirm=yes to do it.\n";
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, m, strlen(m));
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const char *ok =
        "OK — disconnecting now. Watch the backoff pipeline "
        "in the serial log.\n";
    esp_err_t e = httpd_resp_send(req, ok, strlen(ok));
    /* httpd_resp_send returns once bytes are queued in the lwIP send
     * ring, not once the TCP ACK lands. Without this delay the
     * STA_DISCONNECTED event tears down lwIP before the client receives
     * the FIN+ACK, and curl/httpx see ECONNRESET instead of the OK
     * body. 200 ms is well below any HIL timeout and barely visible
     * to a developer hitting the URL by hand. */
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_mgr_force_disconnect();
    return e;
}

/* GET /debug/hang?ms=N — instruct the main loop to spin for N ms
 * without feeding TWDT. Used to validate the panic + coredump + reboot
 * pipeline end-to-end. Defaults to 35000 ms (5 s past the 30 s TWDT
 * window). Caller must explicitly opt in with ?confirm=yes so a
 * scraper or stray monitoring probe can't take a board down. */
static esp_err_t debug_hang_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char qbuf[64];
    int ms = 35000;
    bool confirmed = false;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qbuf, "ms", val, sizeof(val)) == ESP_OK) {
            ms = atoi(val);
            if (ms < 1000)
                ms = 1000;
            if (ms > 60000)
                ms = 60000;
        }
        if (httpd_query_key_value(qbuf, "confirm", val, sizeof(val)) == ESP_OK &&
            strcmp(val, "yes") == 0) {
            confirmed = true;
        }
    }
    if (!confirmed) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        const char *msg =
            "Hang the main loop to force a TWDT panic.\n"
            "Add ?confirm=yes to actually do it.\n"
            "Optional: ?ms=N (1000..60000, default 35000).\n";
        return httpd_resp_send(req, msg, strlen(msg));
    }
    /* Respond BEFORE the hang takes effect so the caller doesn't time out
     * before getting confirmation. The actual hang fires on the next main
     * loop iteration which is at most a few ms away. */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "OK — hanging main loop for %d ms. TWDT panics after "
                     "%d s, then coredump + reboot.\n",
                     ms, CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    esp_err_t e = httpd_resp_send(req, body, n);
    /* See debug_wifi_disconnect_get for rationale: give lwIP a moment
     * to flush the TCP response before the main loop starts spinning
     * (the TWDT-stuck CPU starves the IP task too). */
    vTaskDelay(pdMS_TO_TICKS(200));
    debug_hang_main_for_ms(ms);
    return e;
}

/* GET /debug/cores — enumerate every FreeRTOS task with its current
 * core (or -1 for NO_AFFINITY), priority, and stack high-water mark.
 * Used by tests/hil/test_dual_core.py to catch accidental repinning in
 * future refactors.
 *
 * Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (cheap, default y after
 * the dual-core refactor). The optional runtime_pct field is only
 * populated when CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y, which adds
 * ~5% context-switch overhead and is therefore opt-in via menuconfig.
 * `stats_enabled` lets the caller branch on whether to assert on
 * percentages. */
static esp_err_t debug_cores_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    UBaseType_t n = uxTaskGetNumberOfTasks();
    /* Snapshot a few extra slots so a task created between the count
     * call and the enumeration doesn't overflow the buffer. */
    UBaseType_t cap = n + 4;
    TaskStatus_t *arr =
        (TaskStatus_t *)heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_DEFAULT);
    if (!arr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_OK;
    }
    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, cap, &total_runtime);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[64];
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    const bool stats_enabled = true;
#else
    const bool stats_enabled = false;
#endif
    int hn = snprintf(head, sizeof(head), "{\"stats_enabled\":%s,\"tasks\":[",
                      stats_enabled ? "true" : "false");
    httpd_resp_send_chunk(req, head, hn);

    char buf[224];
    for (UBaseType_t i = 0; i < got; i++) {
        TaskStatus_t *t = &arr[i];
        /* ESP-IDF non-SMP FreeRTOS (configNUMBER_OF_CORES == 2,
         * CONFIG_FREERTOS_SMP=n) populates TaskStatus_t::xCoreID with
         * the task's pinned core (0, 1, or tskNO_AFFINITY = -1). No
         * separate affinity-mask getter needed; the field is the
         * answer. */
        int core = (int)t->xCoreID;
        if (core < 0 || core > 1) core = -1;

        char rt_field[24];
        if (stats_enabled && total_runtime > 0) {
            uint64_t pct = ((uint64_t)t->ulRunTimeCounter * 100ULL) / total_runtime;
            if (pct > 100) pct = 100;
            snprintf(rt_field, sizeof(rt_field), "%u", (unsigned)pct);
        } else {
            snprintf(rt_field, sizeof(rt_field), "null");
        }
        int bn = snprintf(buf, sizeof(buf),
                          "%s{\"name\":\"%s\",\"core\":%d,\"priority\":%u,"
                          "\"stack_hwm\":%u,\"runtime_pct\":%s}",
                          (i == 0) ? "" : ",",
                          t->pcTaskName ? t->pcTaskName : "?",
                          core,
                          (unsigned)t->uxCurrentPriority,
                          (unsigned)t->usStackHighWaterMark,
                          rt_field);
        if (bn > 0 && (size_t)bn < sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, bn);
        }
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    free(arr);
    return ESP_OK;
}

/* POST /debug/uart_servo
 *
 * Raw hex-encoded byte exchange with the UART servo bus — useful for
 * reverse-engineering an unknown servo board protocol or for "does
 * this thing answer when I poke it" sanity checks before any C-side
 * protocol code exists.
 *
 * Request body: ASCII hex of bytes to send (e.g. "55aa0301" — no
 * spaces, no 0x prefix). Whitespace tolerated.
 * Response body: hex of bytes received within `timeout_ms` query
 * argument (default 200 ms), or empty if nothing arrived. */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t debug_uart_servo_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    if (!uart_servo_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "uart_servo not ready — set pin_d?_fn=uart_tx and pin_d?_fn=uart_rx "
            "in the pin map then reboot\n");
    }

    /* Parse ?timeout_ms=N. Default 200 ms — typical Feetech/Dynamixel
     * round-trip is 5-50 ms; 200 gives headroom for the slowest servo
     * but doesn't stall the HTTP worker too long. */
    uint32_t timeout_ms = 200;
    char qs[64];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char buf[16];
        if (httpd_query_key_value(qs, "timeout_ms", buf, sizeof(buf)) == ESP_OK) {
            uint32_t v = (uint32_t)strtoul(buf, NULL, 10);
            if (v > 0 && v <= 2000) timeout_ms = v;
        }
    }

    /* Read body — hex string, max 512 chars = 256 bytes. Plenty for
     * any single servo packet on a half-duplex bus. */
    char hex_in[512];
    int body_len = httpd_req_recv(req, hex_in, sizeof(hex_in) - 1);
    if (body_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "empty body — expect hex bytes to send\n");
    }
    hex_in[body_len] = 0;

    /* Hex decode. Skip whitespace, tolerate odd nibble counts by
     * rejecting (operator typo'd a byte). */
    uint8_t tx_buf[256];
    size_t tx_len = 0;
    int hi = -1;
    for (int i = 0; i < body_len && tx_len < sizeof(tx_buf); i++) {
        char c = hex_in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        int d = hex_digit(c);
        if (d < 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "non-hex character in body\n");
        }
        if (hi < 0) {
            hi = d;
        } else {
            tx_buf[tx_len++] = (uint8_t)((hi << 4) | d);
            hi = -1;
        }
    }
    if (hi >= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "odd hex digit count — bytes need pairs\n");
    }
    if (tx_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "no bytes decoded from body\n");
    }

    /* Send. uart_servo_write drains the TX FIFO before returning so
     * the read loop below sees the actual response, not echo. */
    int sent = uart_servo_write(tx_buf, tx_len);
    if (sent < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "uart_write_bytes failed\n");
    }

    /* Read whatever the servo sends back within timeout. 256 bytes
     * matches the TX cap — typical packets are under 32 bytes but
     * leave headroom for a chatty servo. */
    uint8_t rx_buf[256];
    int recv = uart_servo_read(rx_buf, sizeof(rx_buf), timeout_ms);
    if (recv < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "uart_read_bytes failed\n");
    }

    /* Emit as ASCII hex. recv*2 ASCII chars + a trailing newline. */
    char hex_out[513];
    int op = 0;
    for (int i = 0; i < recv && op + 2 < (int)sizeof(hex_out); i++) {
        op += snprintf(hex_out + op, sizeof(hex_out) - op, "%02x", rx_buf[i]);
    }
    if (op < (int)sizeof(hex_out) - 1) {
        hex_out[op++] = '\n';
    }
    hex_out[op] = 0;
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, hex_out, op);
}

/* POST /debug/capture
 *
 * Enqueue a capture with an operator-supplied trigger string. Exists so
 * the HIL EXIF test can exercise the inline-ASCII branch of emit_ascii()
 * — production triggers ("mqtt", "reed_open", "timelapse", "http") are
 * all >4 bytes and always take the offset path. Sending a ≤3-char body
 * (e.g. "pir") forces strlen+NUL ≤ 4 and routes through the inline
 * branch where the original `\xb0\0\0\0` bug lived.
 *
 * Body: bare ASCII trigger string, no quoting. Whitelist [a-z0-9_]+ to
 * keep the EXIF payload printable and the SD filename safe (capture
 * path uses the trigger in `<ts>_<mactail>_<trigger>.jpg`). Length
 * capped at 15 to leave headroom under the camera_worker queue item's
 * 16-char `trigger` field. */
static esp_err_t debug_capture_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[32];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "empty body — expect a trigger string like \"pir\"\n");
    }
    body[n] = 0;
    /* Strip trailing whitespace (curl --data-binary often leaves a
     * newline behind; we don't want it inside the EXIF tag). */
    while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r' ||
                     body[n - 1] == ' ' || body[n - 1] == '\t')) {
        body[--n] = 0;
    }
    if (n == 0 || n > 15) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "trigger must be 1..15 chars after whitespace strip\n");
    }
    for (int i = 0; i < n; i++) {
        char c = body[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req,
                "trigger must match [a-z0-9_]+ (keeps EXIF + SD path safe)\n");
        }
    }
    esp_err_t e = camera_request_event(body);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "camera_request_event failed (queue full or camera not ready)\n");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "queued\n");
}

/* GET /debug/cam_standby?on=1|0
 *
 * BENCH-ONLY measurement spike: put the OV3660 into software standby
 * (?on=1, the default) or wake it (?on=0) to measure the sensor's residual
 * analog draw on the bench power meter. Wraps camera_debug_sensor_standby(),
 * which holds the capture mutex + a NO_LIGHT_SLEEP lock and drains a few
 * frames on wake. NOT wired into the mode FSM — keep the camera idle (no
 * live stream) across the toggle, then capture a photo after ?on=0 to
 * confirm clean recovery. Compiled out of field/production. */
static esp_err_t debug_cam_standby_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    bool on = true;
    char qbuf[32], val[8];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
        httpd_query_key_value(qbuf, "on", val, sizeof(val)) == ESP_OK) {
        on = (val[0] != '0');
    }
    esp_err_t e = camera_debug_sensor_standby(on);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char body[96];
    int n = snprintf(body, sizeof(body), "OV3660 software standby %s: %s\n",
                     on ? "ON" : "OFF", esp_err_to_name(e));
    return httpd_resp_send(req, body, n);
}

/* GET /debug/tls_csr
 *
 * Generate a fresh ECDSA P-256 keypair on-device and emit the
 * matching X.509 CSR (PEM) for the current device identity. Lets a
 * dev curl-test the keygen + CSR builder path without needing the
 * full MQTT signer flow:
 *
 *   curl http://192.0.2.x/debug/tls_csr | openssl req -text -noout
 *
 * Verifies subject CN = cb-<id>.<domain>, SAN list, EC P-256
 * pubkey, SHA-256 signature. The keypair is generated fresh per call
 * and DISCARDED — this endpoint is purely for verification, not a
 * shortcut for enrollment. Production builds (CONFIG_..._DEBUG=n)
 * don't include this handler.
 *
 * Cost: ~3-5 s CPU per call for the ECC keygen (entropy + curve math).
 * Don't loop this. */
static esp_err_t debug_tls_csr_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    tls_enroll_keypair_t kp = {0};
    if (tls_enroll_generate_keypair(&kp) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "keygen failed (see serial log)\n");
    }

    char fqdn[64];
    snprintf(fqdn, sizeof(fqdn), "%s.%s",
             mqtt_topic_base(), wifi_mgr_get_domain());

    char mdns[64];
    snprintf(mdns, sizeof(mdns), "%s.local", mqtt_topic_base());

    char ip[20];
    bool have_ip = wifi_mgr_get_ip_str(ip, sizeof(ip));

    tls_enroll_csr_subject_t subj = {
        .cn            = fqdn,
        .san_dns_fqdn  = fqdn,
        .san_dns_short = mqtt_topic_base(),
        .san_dns_mdns  = mdns,
        .san_ip_str    = have_ip ? ip : NULL,
    };

    char pem[2048];
    size_t pem_len = 0;
    esp_err_t e = tls_enroll_build_csr(&kp, &subj, pem, sizeof(pem), &pem_len);
    tls_enroll_keypair_free(&kp);

    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "csr build failed (see serial log)\n");
    }

    httpd_resp_set_type(req, "application/x-pem-file");
    return httpd_resp_send(req, pem, pem_len);
}

/* POST /debug/tls_erase
 *
 * Wipes the "tls" NVS namespace (cert, key, expiry, SAN fingerprint).
 * Next boot will hit the no-cert branch of tls_boot_enroll_if_needed
 * and run a fresh enrollment. Useful for HIL tests that exercise the
 * "factory reset" path without re-flashing.
 *
 * Does NOT reboot; the running HTTPS server keeps serving the cert
 * loaded from the now-erased NVS until the next reboot. Operator
 * follows up with `mqtt.sh pub <id>/cmd/reboot 1`.
 *
 *   curl -X POST http://192.0.2.x/debug/tls_erase
 */
static esp_err_t debug_tls_erase_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    esp_err_t e = tls_store_erase();
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char buf[64];
        snprintf(buf, sizeof(buf), "erase failed: %s\n", esp_err_to_name(e));
        return httpd_resp_sendstr(req, buf);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req,
        "tls namespace erased — reboot to re-enroll on next boot\n");
}

/* POST /debug/tls_enroll
 *
 * Run the full enrollment pipeline: keygen → CSR → publish over MQTT
 * to <base>/cmd/enroll → await signed leaf on <base>/state/cert →
 * validate chain + pubkey match → persist into NVS. On success the
 * board prints "ok\n" and the operator can reboot to start the HTTPS
 * server with the new cert.
 *
 * Blocks the httpd worker for up to 30 s. The signer (cbd) usually
 * responds within ~200 ms — the timeout headroom covers a cold-start
 * sub-CA key read + a few retries on transient MQTT hiccup.
 *
 *   curl -X POST http://192.0.2.x/debug/tls_enroll
 *
 * Production builds (CONFIG_..._DEBUG=n) don't include this — the
 * production trigger is the boot-path "no cert / stale cert" gate that
 * runs tls_enroll_run() automatically after WiFi+MQTT are up. */
static esp_err_t debug_tls_enroll_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    esp_err_t e = tls_enroll_run(/*timeout_ms*/ 30000);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char buf[64];
        snprintf(buf, sizeof(buf), "enroll failed: %s\n", esp_err_to_name(e));
        return httpd_resp_sendstr(req, buf);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok\n");
}

/* POST /debug/wifi_softap — force the SoftAP recovery fallback up NOW
 * (without waiting out the ~10-min STA-fail trigger) so the AP + /wifi
 * portal can be exercised on the bench. Bench-only (gated). */
static esp_err_t debug_wifi_softap_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    /* Hold STA down so the good home AP doesn't immediately reconnect and
     * trip the main-loop teardown — gives a window to join the AP + test
     * the portal over RF. Auto-recovers after the hold. */
    wifi_mgr_test_hold_sta_down(300);
    esp_err_t e = wifi_mgr_start_softap();
    if (e == ESP_OK) e = http_softap_portal_start();
    char buf[80];
    snprintf(buf, sizeof(buf), "softap start: %s (ssid " AP_SSID_FMT ")\n",
             esp_err_to_name(e), device_id_suffix());
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
}
#endif /* CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS */

/* /-catch-all handler for the :80 redirect server. Returns 301 to the
 * HTTPS equivalent of the incoming request. Host header is taken from
 * the request; if missing we fall back to the WiFi IP. Path + query
 * are preserved verbatim.
 *
 * TODO(host-allowlist): the Host header is currently trusted as-is,
 * which means a malicious `Host: attacker.com` request gets a 301
 * pointing browsers at the attacker. Acceptable on a private LAN with
 * no external exposure but should narrow to {<device_id>.lan,
 * <device_id>.lan, <device_id>.local, <local IP>} before any wider
 * deployment. */
static esp_err_t redirect_to_https_get(httpd_req_t *req) {
    char host[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        host[0] == 0) {
        if (!wifi_mgr_get_ip_str(host, sizeof(host))) {
            snprintf(host, sizeof(host), "%s", "chytra-budka");
        }
    }
    /* Strip any ":80" trailing port from Host so the resulting URL
     * defaults to 443 (no port). */
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;

    /* Host (~80) + "https://" (8) + uri (CONFIG_HTTPD_MAX_URI_LEN=512) +
     * NUL — 768 keeps a comfortable margin and the buffer is on stack
     * for the short lifetime of one request. */
    char loc[768];
    snprintf(loc, sizeof(loc), "https://%s%s", host, req->uri);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", loc);
    /* Empty body is fine; browsers follow Location without rendering it. */
    return httpd_resp_send(req, NULL, 0);
}

/* Start a minimal :80 server whose single wildcard handler emits a
 * 301 to the HTTPS counterpart. Only runs when the primary server is
 * HTTPS (so we don't loopback on plain-HTTP fallback). */
static esp_err_t start_redirect_server(void) {
    if (s_redirect_srv) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* ctrl_port must differ from the primary server's so the two
     * httpd ctrl sockets don't collide. */
    cfg.ctrl_port = 32769;
    cfg.max_uri_handlers = 1;
    cfg.max_open_sockets = 3;  /* redirects are short-lived */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    /* 8 KB, not 4 KB: redirect_to_https_get puts loc[768] + host[80] on the
     * stack, and the esp_http_server framework's per-request parsing
     * (HTTPD_MAX_REQ_HDR_LEN=1024 + HTTPD_MAX_URI_LEN=512 scratch + req
     * structs) sits under it. A browser/OS captive-portal connectivity-check
     * request overran the old 4 KB stack — the canary trips on some heap
     * layouts ("stack overflow in task httpd"), but on others the overrun
     * silently smashes the adjacent heap-allocated TCB and the next context
     * switch faults (StoreProhibited in vPortYieldFromInt). That was the real
     * cause of the SoftAP-join "onboarding loop" crash. 8 KB matches the
     * plain-HTTP main server below, which serves every handler without issue. */
    cfg.stack_size = 8192;
    /* uri_match_fn must be the wildcard matcher so "*" catches every
     * incoming path. Without this the handler only catches the literal
     * "*" URI. */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t e = httpd_start(&s_redirect_srv, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "redirect_srv start: %s", esp_err_to_name(e));
        s_redirect_srv = NULL;
        return e;
    }
    httpd_uri_t r_all = {
        .uri = "/*", .method = HTTP_GET, .handler = redirect_to_https_get,
    };
    httpd_register_uri_handler(s_redirect_srv, &r_all);
    ESP_LOGI(TAG, "redirect server up on :80 (→ https://)");
    return ESP_OK;
}

/* ── SoftAP recovery portal (GET/POST /wifi) ──────────────────────────
 * The handlers below are shared: they're registered on the MAIN server
 * (so a plain-HTTP board exposes the form at http://172.31.4.1/wifi
 * directly), and ALSO on a dedicated plain :80 server when the main
 * server is HTTPS (so the operator gets a plain form instead of a cert
 * warning). Both are gated to "SoftAP active" — in normal operation
 * /wifi returns 404 and is not an attack surface. No auth gate: WPA2 on
 * the AP is the access control, and a board being recovered may have no
 * usable basic-auth creds. Submitted creds go through
 * wifi_store_set_candidate + reboot — the same verify-before-commit
 * ladder as cmd/wifi, so a typo can't brick the board. */
static httpd_handle_t s_softap_srv = NULL;

/* Gate for the /wifi + /config forms. These expose cred changes, AP-only,
 * and factory reset, so they must never be served unauthenticated in normal
 * operation. Two ways in:
 *   1. SoftAP/AP recovery active — open, because WPA2 on the AP IS the access
 *      control and a board being recovered may have no usable basic-auth creds.
 *   2. Normal operation — requires basic auth to be BOTH configured and passed.
 * When HTTP_BASIC_* are still placeholders there is no way to authenticate, so
 * we fail CLOSED (404) rather than serve factory-reset open on the LAN. An
 * operator who wants web config without the SoftAP just sets real HTTP_BASIC_*
 * creds in secrets.h. Returns true to proceed; on a 401/404 it has already
 * sent the response and the caller must just return ESP_OK. */
static bool wifi_form_allowed(httpd_req_t *req, bool *sent_response) {
    *sent_response = false;
    if (wifi_mgr_softap_active()) return true;
    if (!basic_auth_enabled()) {
        /* No credentials configured → no authenticated path → don't expose it. */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not available");
        *sent_response = true;
        return false;
    }
    if (basic_auth_gate(req) != ESP_OK) {
        *sent_response = true; /* 401 already sent by the gate */
        return false;
    }
    return true;
}

static esp_err_t wifi_form_get(httpd_req_t *req) {
    bool sent;
    if (!wifi_form_allowed(req, &sent)) return ESP_OK;
    (void)sent;
    /* Optional ?ssid=<name> (from the /wifiscan?to=wifi picker) pre-fills the
     * SSID box so the operator stays in the lightweight captive flow instead
     * of bouncing to /config. httpd_query_key_value doesn't URL-decode, so we
     * decode + HTML-escape it ourselves. */
    char prefill_ssid[200] = {0};   /* HTML-escaped SSID can expand ~6x */
    {
        char q[160], raw[96];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "ssid", raw, sizeof(raw)) == ESP_OK) {
            char dec[64];
            scan_url_decode(raw, dec, sizeof(dec));
            scan_html_escape(dec, prefill_ssid, sizeof(prefill_ssid));
        }
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<meta name=color-scheme content=dark><link rel=stylesheet href=/style.css>"
        "<title>Chytr\xc3\xa1 Budka WiFi</title></head><body>");
    send_security_banner(req);
    /* Focus the empty field: SSID on a fresh open, the password when a scan
     * already pre-filled the SSID. */
    const char *ssid_af = prefill_ssid[0] ? "" : " autofocus";
    const char *pass_af = prefill_ssid[0] ? " autofocus" : "";
    char page[1400];
    snprintf(page, sizeof(page),
        "<h2>%s</h2>"
        "<p><a href=\"/wifiscan?to=wifi\">&#128246; %s</a></p>"
        "<form method=POST action=/wifi>"
        "<p>%s:<br><input name=ssid maxlength=32 value=\"%s\"%s required></p>"
        "<p>%s:<br><input name=password type=password maxlength=63%s></p>"
        "<p><button>%s</button></p></form>"
        "<p><small>%s</small></p>"
        "<p><a href=/config>%s</a></p>"
        "</body></html>",
        tr(STR_WIFI_TITLE), tr(STR_SCAN_TITLE),
        tr(STR_WIFI_SSID_LABEL), prefill_ssid, ssid_af, tr(STR_WIFI_PASS_LABEL), pass_af,
        tr(STR_WIFI_SAVE_BTN), tr(STR_WIFI_HELP), tr(STR_WIFI_ADVANCED_LINK));
    httpd_resp_sendstr_chunk(req, page);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* x-www-form-urlencoded single-field extractor with %XX + '+' decoding. */
static bool form_field(const char *body, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = body;
    out[0] = 0;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = amp ? amp : v + strlen(v);
            size_t o = 0;
            for (const char *c = v; c < end && o + 1 < cap; c++) {
                if (*c == '+') {
                    out[o++] = ' ';
                } else if (*c == '%' && c + 2 < end &&
                           isxdigit((unsigned char)c[1]) &&
                           isxdigit((unsigned char)c[2])) {
                    char hex[3] = {c[1], c[2], 0};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    c += 2;
                } else {
                    out[o++] = *c;
                }
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static esp_err_t wifi_form_post(httpd_req_t *req) {
    bool sent;
    if (!wifi_form_allowed(req, &sent)) return ESP_OK;
    (void)sent;
    char body[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body length");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[WIFI_STORE_SSID_CAP] = {0};
    char pass[WIFI_STORE_PASS_CAP] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "password", pass, sizeof(pass));

    esp_err_t e = wifi_store_set_candidate(ssid, pass);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    static const char REPLY_HEAD[] =
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<link rel=stylesheet href=/style.css><body><p>";
    char p[420];
    if (e != ESP_OK) {
        snprintf(p, sizeof(p), "%s%s <a href=/wifi>%s</a></p>",
                 REPLY_HEAD, tr(STR_WIFI_BAD_CREDS), tr(STR_BACK));
        httpd_resp_sendstr(req, p);
        return ESP_OK;
    }
    /* SSID logged, password never logged. */
    ESP_LOGW(TAG, "softap portal: candidate staged via /wifi (ssid='%s') — rebooting", ssid);
    snprintf(p, sizeof(p), "%s%s</p>", REPLY_HEAD, tr(STR_WIFI_SAVED_REBOOT));
    httpd_resp_sendstr(req, p);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

/* Captive-portal catch-all on the dedicated AP server: any GET → the form. */
static esp_err_t wifi_portal_catchall(httpd_req_t *req) {
    return wifi_form_get(req);
}

/* Captive-portal 404 (AP mode): every unresolved path — the OS connectivity-
 * check probes (/generate_204, /hotspot-detect.html, /ncsi.txt, …), now aimed
 * at us by the DNS hijack, plus any stray asset request — returns the /wifi
 * onboarding form. Serving real content with a 200 (not the 204/"Success" the
 * OS expects) is what trips both Android's "Sign in to network" and the iOS
 * captive sheet, so the portal opens without the operator typing the AP IP.
 * Registered on the main :80 server only while the SoftAP portal is up;
 * STA-mode 404s are untouched (this handler isn't installed there). */
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return wifi_form_get(req);
}

static esp_err_t wifiscan_get(httpd_req_t *req);  /* defined below; used by the AP-mode portal */

esp_err_t http_softap_portal_start(void) {
    /* Plain-HTTP main server already hosts /wifi on :80 (reachable at
     * http://172.31.4.1/wifi). No extra bind needed — but install the captive
     * 404 handler so OS connectivity checks (pointed at us by the DNS hijack)
     * auto-open the portal instead of needing the AP IP typed. Only the HTTPS
     * case needs a dedicated plain :80 portal (below) to avoid a cert warning. */
    if (!http_server_is_https()) {
        if (s_server)
            httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404);
        ESP_LOGW(TAG, "softap portal: /wifi on main :80 + captive 404 → portal");
        return ESP_OK;
    }
    if (s_softap_srv) return ESP_OK;
    if (s_redirect_srv) {
        httpd_stop(s_redirect_srv);
        s_redirect_srv = NULL;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32770; /* distinct from primary (32768) + redirect (32769) */
    cfg.max_uri_handlers = 4;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    /* 8 KB, not 4 KB — same reason as the redirect server (see
     * start_redirect_server): the captive-portal catch-all + /wifi handlers
     * plus esp_http_server's per-request parsing overran a 4 KB stack when a
     * phone's captive-portal detection probed the portal right after joining
     * the onboarding AP. This is the AP-only-mode half of the SoftAP-join
     * crash that produced the "reconnect with a new password 3×" loop. */
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t e = httpd_start(&s_softap_srv, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "softap portal start: %s", esp_err_to_name(e));
        s_softap_srv = NULL;
        return e;
    }
    httpd_uri_t r_get = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_form_get};
    httpd_uri_t r_post = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_form_post};
    /* The /wifi form links to /wifiscan?to=wifi for the tap-list of nearby APs.
     * It MUST be registered here (and BEFORE the wildcard catch-all, which is
     * matched in registration order) — otherwise the scan link falls through to
     * wifi_portal_catchall and the network list never appears in AP-only mode.
     * wifi_mgr_scan() borrows the STA interface (AP->APSTA->scan->AP) so it works
     * with no station up. */
    httpd_uri_t r_scan = {.uri = "/wifiscan", .method = HTTP_GET, .handler = wifiscan_get};
    httpd_uri_t r_all = {.uri = "/*", .method = HTTP_GET, .handler = wifi_portal_catchall};
    httpd_register_uri_handler(s_softap_srv, &r_get);
    httpd_register_uri_handler(s_softap_srv, &r_post);
    httpd_register_uri_handler(s_softap_srv, &r_scan);
    httpd_register_uri_handler(s_softap_srv, &r_all);
    ESP_LOGW(TAG, "softap portal up on plain :80 (GET/POST /wifi, /wifiscan)");
    return ESP_OK;
}

void http_softap_portal_stop(void) {
    if (s_softap_srv) {
        httpd_stop(s_softap_srv);
        s_softap_srv = NULL;
    }
}

/* ── Local config page (/config) ──────────────────────────────────────
 * A full local UI mirroring the MQTT/HA control surface: every app_config
 * setting (rendered from the schema), action buttons, and WiFi/AP config —
 * so the box is fully manageable from a browser without HA/MQTT (essential
 * in AP-only mode). Same gate as /wifi: open while the SoftAP/AP is up
 * (WPA2 is the access control), else authenticated HTTPS only. */
static void cfg_reply(httpd_req_t *req, const char *msg, bool back) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<link rel=stylesheet href=/style.css><body><p>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</p>");
    if (back) {
        char b[96];
        snprintf(b, sizeof(b), "<p><a href=/config>%s</a></p>", tr(STR_BACK));
        httpd_resp_sendstr_chunk(req, b);
    }
    httpd_resp_sendstr_chunk(req, "</body>");
    httpd_resp_sendstr_chunk(req, NULL);
}

/* Collapsible groups for the settings form, so WiFi/AP aren't buried under ~20
 * camera knobs. Each config key maps to one group (catch-all last); the page
 * renders one <details> per group. Grouping by key prefix keeps it in one place
 * without bloating the schema with a category column. */
static const i18n_str_t GRP_STR[] = {
    STR_GRP_CAMERA, STR_GRP_AUDIO, STR_GRP_SENSORS, STR_GRP_MODE,
    STR_GRP_PINS, STR_GRP_LED, STR_GRP_OTHER,
};
#define CFG_NGROUPS ((int)(sizeof(GRP_STR) / sizeof(GRP_STR[0])))
static int cfg_group_of(const char *k) {
    if (!strncmp(k, "cam_", 4) || !strncmp(k, "mjpg_", 5) || !strncmp(k, "tlapse", 6) ||
        !strncmp(k, "ir_", 3) || !strncmp(k, "cap_led", 7))
        return 0;
    if (!strncmp(k, "vad_", 4) || !strncmp(k, "flac", 4)) return 1;
    if (!strncmp(k, "pir_", 4) || !strncmp(k, "reed", 4) ||
        !strncmp(k, "sonar_", 6) || !strncmp(k, "soil_", 5))
        return 2;
    if (!strcmp(k, "power_profile") || !strncmp(k, "tlm_", 4) || !strncmp(k, "ota_", 4) ||
        !strncmp(k, "soc_", 4) || !strncmp(k, "ds_", 3)) return 3;
    if (!strncmp(k, "pin_d", 5) || !strncmp(k, "uart_", 5)) return 4;
    if (!strncmp(k, "status_led", 10)) return 5;
    return CFG_NGROUPS - 1;
}

/* Percent-encode for a URL query value (RFC 3986 unreserved kept verbatim,
 * everything else %XX). Used to put a scanned SSID into a /config?ssid= link. */
static void scan_url_encode(const char *s, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xf];
        }
    }
    out[o] = 0;
}

/* Decode a URL query value (%XX + '+'→space). httpd_query_key_value does NOT
 * decode, so the ?ssid= prefill needs this. */
static void scan_url_decode(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '+') {
            out[o++] = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
                   isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

/* Minimal HTML-escape for attribute/text contexts (SSIDs can contain & < > "). */
static void scan_html_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        switch (*s) {
            case '&': memcpy(out + o, "&amp;",  5); o += 5; break;
            case '<': memcpy(out + o, "&lt;",   4); o += 4; break;
            case '>': memcpy(out + o, "&gt;",   4); o += 4; break;
            case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
            default:  out[o++] = *s;
        }
    }
    out[o] = 0;
}

/* RSSI → 4-cell signal bar (filled █ + light ░ via HTML entities). */
static const char *rssi_bars(int rssi) {
    int b = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : rssi >= -88 ? 1 : 0;
    static const char *const BARS[5] = {
        "&#9617;&#9617;&#9617;&#9617;",
        "&#9608;&#9617;&#9617;&#9617;",
        "&#9608;&#9608;&#9617;&#9617;",
        "&#9608;&#9608;&#9608;&#9617;",
        "&#9608;&#9608;&#9608;&#9608;",
    };
    return BARS[b];
}

/* GET /wifiscan — active scan of nearby APs, rendered as a tap-list. Each row
 * pre-fills the SSID box on the destination form: default → /config?ssid=…#wifi
 * (advanced page); with ?to=wifi → the lightweight captive /wifi form, so the
 * onboarding flow stays in the portal. Works without JS (just links). Gated
 * like /config. */
static esp_err_t wifiscan_get(httpd_req_t *req) {
    bool sent;
    if (!wifi_form_allowed(req, &sent)) return ESP_OK;
    apply_hsts(req);
    /* ?to=wifi → return to the captive /wifi form; else the /config page. */
    bool to_wifi = false;
    {
        char q[64], v[16];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "to", v, sizeof(v)) == ESP_OK)
            to_wifi = (strcmp(v, "wifi") == 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<meta name=color-scheme content=dark>"
        "<link rel=stylesheet href=/style.css>"
        "<title>Chytr\xc3\xa1 Budka</title></head><body>");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "<h2>&#128246; %s</h2><p><a href=\"%s\">%s</a></p>",
             tr(STR_SCAN_TITLE), to_wifi ? "/wifi" : "/config#wifi", tr(STR_BACK));
    httpd_resp_sendstr_chunk(req, buf);

    wifi_scan_ap_t aps[24];
    int n = wifi_mgr_scan(aps, (int)(sizeof(aps) / sizeof(aps[0])));
    if (n < 0) {
        snprintf(buf, sizeof(buf), "<div class=\"warn-bar\">%s</div>", tr(STR_SCAN_UNAVAIL));
        httpd_resp_sendstr_chunk(req, buf);
    } else if (n == 0) {
        snprintf(buf, sizeof(buf), "<p>%s</p>", tr(STR_SCAN_NONE));
        httpd_resp_sendstr_chunk(req, buf);
    } else {
        httpd_resp_sendstr_chunk(req, "<ul class=scan>");
        for (int i = 0; i < n; i++) {
            char enc[120], esc[100], href[160], row[400];
            scan_url_encode(aps[i].ssid, enc, sizeof(enc));
            scan_html_escape(aps[i].ssid, esc, sizeof(esc));
            const char *lock = (aps[i].authmode == WIFI_AUTH_OPEN) ? "" : " &#128274;";
            snprintf(href, sizeof(href),
                     to_wifi ? "/wifi?ssid=%s" : "/config?ssid=%s#wifi", enc);
            snprintf(row, sizeof(row),
                     "<li><a href=\"%s\">"
                     "<span class=bars>%s</span>"
                     "<span class=ssid>%s%s</span>"
                     "<span class=dbm>%d dBm</span></a></li>",
                     href, rssi_bars(aps[i].rssi), esc, lock, aps[i].rssi);
            httpd_resp_sendstr_chunk(req, row);
        }
        httpd_resp_sendstr_chunk(req, "</ul>");
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t *req) {
    bool sent;
    if (!wifi_form_allowed(req, &sent)) return ESP_OK;
    /* Optional ?ssid=<name> (from the /wifiscan picker) pre-fills the STA SSID
     * box. httpd_query_key_value doesn't URL-decode, so decode it ourselves. */
    char prefill_ssid[200] = {0};  /* escaped SSID can expand ~6x (32 → ~192) */
    {
        char q[160], raw[96];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "ssid", raw, sizeof(raw)) == ESP_OK) {
            char dec[64];
            scan_url_decode(raw, dec, sizeof(dec));
            scan_html_escape(dec, prefill_ssid, sizeof(prefill_ssid));
        }
    }
    /* No ?ssid= override → pre-fill the STA box with the currently CONFIGURED
     * client SSID (known-good / candidate from NVS) so the operator sees what
     * it's set to. Skip the compile-time default (a placeholder/blank on an
     * unprovisioned board) and never the AP SSID. */
    if (prefill_ssid[0] == '\0') {
        char cur[WIFI_STORE_SSID_CAP] = {0}, cpass[WIFI_STORE_PASS_CAP] = {0};
        wifi_creds_src_t src = WIFI_CREDS_DEFAULT;
        wifi_store_get_effective(cur, sizeof(cur), cpass, sizeof(cpass), &src);
        if (src != WIFI_CREDS_DEFAULT && cur[0])
            scan_html_escape(cur, prefill_ssid, sizeof(prefill_ssid));
    }
    send_page_head(req, NULL, "config");
    /* All user text via tr() (current ui_lang knob). HTML scaffold stays inline;
     * composites (button label + confirm()) are snprintf'd into buf. */
    char buf[900];
    snprintf(buf, sizeof(buf),
        "<h3>%s</h3>"
        "<form method=POST action=/config style=\"display:flex;flex-wrap:wrap;gap:.4em\">"
        "<button name=cmd value=ota>%s</button>"
        "<button name=cmd value=reboot onclick=\"return confirm('%s')\">%s</button>"
        "<button name=cmd value=cfg_reset onclick=\"return confirm('%s')\">%s</button>"
        "<button name=cmd value=factory_reset onclick=\"return confirm('%s')\">%s</button>"
        "</form><h3>%s</h3><form method=POST action=/config>",
        tr(STR_ACTIONS), tr(STR_BTN_OTA),
        tr(STR_CONFIRM_REBOOT), tr(STR_BTN_REBOOT),
        tr(STR_CONFIRM_RESET_CFG), tr(STR_BTN_RESET_CFG),
        tr(STR_CONFIRM_FACTORY), tr(STR_BTN_FACTORY), tr(STR_SETTINGS));
    httpd_resp_sendstr_chunk(req, buf);
    /* Settings, grouped into collapsed <details> (one pass per group; a key
     * lands in exactly one group, so no row is emitted twice). */
    char row[512];
    for (int g = 0; g < CFG_NGROUPS; g++) {
        bool open = false;
        for (size_t i = 0; i < app_config_count(); i++) {
            const char *k = app_config_key_at(i);
            if (!k || cfg_group_of(k) != g) continue;
            /* ble_enabled is toggled on the dedicated /ble page now — don't also
             * expose it here (avoid two controls for the same NVS knob). */
            if (strcmp(k, "ble_enabled") == 0) continue;
            if (!open) {
                char hdr[80];
                snprintf(hdr, sizeof(hdr), "<details><summary>%s</summary>", tr(GRP_STR[g]));
                httpd_resp_sendstr_chunk(req, hdr);
                open = true;
            }
            if (app_config_form_row(i, row, sizeof(row))) httpd_resp_sendstr_chunk(req, row);
        }
        if (open) httpd_resp_sendstr_chunk(req, "</details>");
    }
    snprintf(buf, sizeof(buf), "<p><button>%s</button></p></form>", tr(STR_BTN_SAVE_SETTINGS));
    httpd_resp_sendstr_chunk(req, buf);

    /* WiFi (STA): one button does save creds + leave AP-only + reboot to STA.
     * A "Scan WiFi" link opens the /wifiscan picker, whose rows link back here
     * with ?ssid= pre-filling the box. SSID value is the (escaped) ?ssid=. */
    snprintf(buf, sizeof(buf),
        "<h3 id=wifi>%s</h3>"
        "<p><a href=\"/wifiscan\">%s</a></p>"
        "<form method=POST action=/config>"
        "<p>%s<br><input name=ssid maxlength=32 value=\"%s\" required></p>"
        "<p>%s<br><input name=password type=password maxlength=63></p>"
        "<p><button onclick=\"return confirm('%s')\">%s</button></p></form>",
        tr(STR_WIFI_STA), tr(STR_BTN_WIFI_SCAN), tr(STR_LBL_SSID), prefill_ssid,
        tr(STR_LBL_PASS), tr(STR_CONFIRM_STA_SAVE), tr(STR_BTN_STA_SAVE));
    httpd_resp_sendstr_chunk(req, buf);

    /* AP: SSID box shows the derived default as placeholder (empty = default),
     * or the current custom SSID as the value. One button saves + enables
     * AP-only + reboots. Password never pre-filled. */
    char defssid[40];
    snprintf(defssid, sizeof(defssid), AP_SSID_FMT, device_id_suffix());
    char cur_ap[WIFI_STORE_SSID_CAP] = {0}, cur_pass[WIFI_STORE_PASS_CAP] = {0};
    bool ap_custom = false;
    wifi_store_get_ap(cur_ap, sizeof(cur_ap), cur_pass, sizeof(cur_pass), &ap_custom);
    snprintf(buf, sizeof(buf),
        "<h3 id=ap>%s</h3><form method=POST action=/config>"
        "<p>%s<br><input name=ap_ssid maxlength=32 value=\"%s\" placeholder=\"%s\"></p>"
        "<p>%s<br><input name=ap_pass type=password maxlength=63></p>"
        "<p><button onclick=\"return confirm('%s')\">%s</button></p></form>",
        tr(STR_AP_SECTION), tr(STR_LBL_AP_SSID), ap_custom ? cur_ap : "", defssid,
        tr(STR_LBL_AP_PASS), tr(STR_CONFIRM_AP_SAVE), tr(STR_BTN_AP_SAVE));
    httpd_resp_sendstr_chunk(req, buf);

    /* Web admin login (HTTP basic-auth) — runtime-settable like the WiFi creds
     * (auth_store NVS override; never published to MQTT/HA). Applied LIVE: the
     * gate re-reads on the next request, no reboot. Username pre-filled to the
     * current effective value (not secret); password never echoed. Leaving BOTH
     * blank clears the override back to the secrets.h default. */
    char cur_user[AUTH_STORE_USER_CAP] = {0}, cur_pw[AUTH_STORE_PASS_CAP] = {0};
    auth_store_get_effective(cur_user, sizeof(cur_user), cur_pw, sizeof(cur_pw));
    snprintf(buf, sizeof(buf),
        "<h3 id=auth>%s</h3><form method=POST action=/config>"
        "<p>%s<br><input name=auth_user maxlength=31 value=\"%s\"></p>"
        "<p>%s<br><input name=auth_pass type=password maxlength=63></p>"
        "<p>%s<br><input name=auth_pass2 type=password maxlength=63></p>"
        "<p><button onclick=\"return confirm('%s')\">%s</button></p></form>",
        tr(STR_AUTH_SECTION), tr(STR_LBL_AUTH_USER),
        secret_is_placeholder(cur_user) ? "" : cur_user,
        tr(STR_LBL_AUTH_PASS), tr(STR_LBL_AUTH_PASS2),
        tr(STR_CONFIRM_AUTH_SAVE), tr(STR_BTN_AUTH_SAVE));
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t config_post(httpd_req_t *req) {
    bool sent;
    if (!wifi_form_allowed(req, &sent)) return ESP_OK;
    int total = req->content_len;
    if (total <= 0 || total >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body length");
        return ESP_OK;
    }
    char *body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_OK; }
        got += r;
    }
    body[got] = 0;

    char v[80];
    /* Action buttons. */
    if (form_field(body, "cmd", v, sizeof(v)) && v[0]) {
        if (strcmp(v, "ota") == 0) {
            ota_trigger_now();
            cfg_reply(req, tr(STR_R_OTA_STARTED), true);
        } else if (strcmp(v, "reboot") == 0) {
            cfg_reply(req, tr(STR_R_REBOOTING), false);
            free(body); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart();
        } else if (strcmp(v, "cfg_reset") == 0) {
            app_config_reset_defaults();
            cfg_reply(req, tr(STR_R_CFG_RESET), false);
            free(body); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart();
        } else if (strcmp(v, "factory_reset") == 0) {
            app_config_reset_defaults();
            tls_store_erase();
            wifi_store_erase();
            auth_store_erase();
            cfg_reply(req, tr(STR_R_FACTORY), false);
            free(body); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart();
        } else {
            cfg_reply(req, tr(STR_R_UNKNOWN_CMD), true);
        }
        free(body);
        return ESP_OK;
    }
    /* WiFi (STA): save creds as the candidate, LEAVE AP-only, reboot to STA.
     * One action — an operator typing home creds wants to land on the station,
     * not flip a separate AP-only switch afterwards. */
    char ssid[WIFI_STORE_SSID_CAP] = {0}, pass[WIFI_STORE_PASS_CAP] = {0};
    if (form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0]) {
        form_field(body, "password", pass, sizeof(pass));
        esp_err_t e = wifi_store_set_candidate(ssid, pass);
        if (e == ESP_OK) {
            wifi_store_set_ap_only(false);
            cfg_reply(req, tr(STR_R_WIFI_SAVED), false);
            free(body); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart();
        }
        cfg_reply(req, tr(STR_R_WIFI_BAD), true);
        free(body);
        return ESP_OK;
    }
    /* AP: save AP creds + ENABLE AP-only + reboot — the symmetric counterpart,
     * one button to drop the box into the recovery AP on these creds. */
    char ap_ssid[WIFI_STORE_SSID_CAP] = {0}, ap_pass[WIFI_STORE_PASS_CAP] = {0};
    if (form_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid))) {
        form_field(body, "ap_pass", ap_pass, sizeof(ap_pass));
        esp_err_t e = wifi_store_set_ap(ap_ssid, ap_pass);
        if (e == ESP_OK) {
            wifi_store_set_ap_only(true);
            cfg_reply(req, tr(STR_R_AP_SAVED), false);
            free(body); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart();
        }
        cfg_reply(req, tr(STR_R_AP_BAD), true);
        free(body);
        return ESP_OK;
    }
    /* Web admin login: runtime basic-auth creds. Detected by the auth_user
     * field's presence (the settings form never carries it). Applied LIVE — no
     * reboot; the gate re-reads on the next request. Both blank clears the
     * override back to the secrets.h default. */
    char au[AUTH_STORE_USER_CAP] = {0}, ap_pw[AUTH_STORE_PASS_CAP] = {0};
    char ap_pw2[AUTH_STORE_PASS_CAP] = {0};
    if (form_field(body, "auth_user", au, sizeof(au))) {
        form_field(body, "auth_pass", ap_pw, sizeof(ap_pw));
        form_field(body, "auth_pass2", ap_pw2, sizeof(ap_pw2));
        const bool clearing = au[0] == 0 && ap_pw[0] == 0 && ap_pw2[0] == 0;
        if (!clearing) {
            /* Confirm-match + weakness gate before committing a real change.
             * (Clearing — all blank → revert to compile default — skips these.) */
            if (strcmp(ap_pw, ap_pw2) != 0) {
                free(body);
                cfg_reply(req, tr(STR_R_AUTH_MISMATCH), true);
                return ESP_OK;
            }
            if (strlen(ap_pw) < 6) {  /* min length; also rejects the "cb" default */
                free(body);
                cfg_reply(req, tr(STR_R_AUTH_WEAK), true);
                return ESP_OK;
            }
        }
        esp_err_t e = auth_store_set(au, ap_pw);
        free(body);
        cfg_reply(req, e == ESP_OK ? tr(STR_R_AUTH_SAVED) : tr(STR_R_AUTH_BAD), true);
        return ESP_OK;
    }
    /* Otherwise: the settings form — apply every present config key. */
    int applied = 0;
    for (size_t i = 0; i < app_config_count(); i++) {
        const char *k = app_config_key_at(i);
        if (k && form_field(body, k, v, sizeof(v)))
            if (app_config_set_from_string(k, v) == ESP_OK) applied++;
    }
    free(body);
    cfg_reply(req, applied ? tr(STR_R_CFG_SAVED) : tr(STR_R_CFG_NOCHANGE), true);
    return ESP_OK;
}

/* ── BLE device management (/ble) ──────────────────────────────────────────
 * Local web UI for the BLE allowlist: scan in-range devices, name/save them
 * (a saved device is what the firmware will connect to / ingest — allowlist),
 * see live state + last measured values. UC96 meters (GATT V/I/P) + passive
 * BTHome sensors. Labels are English (technical debug surface). */

static const char *ble_kind_str(uint8_t kind) {
    switch (kind) {
    case BLE_DEV_UC96:   return "UC96";
    case BLE_DEV_BTHOME: return "BTHome";
    default:             return "BLE";
    }
}

static const char *ble_state_str(uint8_t cs) {
    switch (cs) {
    case 3: return "streaming";
    case 2: return "connected";
    case 1: return "connecting";
    default: return "&mdash;";
    }
}

/* "aabbcc000003" → "AA:BB:CC:00:00:03" for display. */
static void ble_id_to_mac(const char *id, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < 12 && id[i] && o + 3 < cap; i += 2) {
        if (i) out[o++] = ':';
        out[o++] = id[i];
        out[o++] = id[i + 1];
    }
    out[o] = 0;
}

/* Append the per-device "last values" cell content for the HTML table. */
static void ble_values_html(const ble_dev_view_t *v, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (v->has_uc96) {
        /* &nbsp; between each value and its unit so a number never wraps away
         * from its unit; a plain (breakable) space as the gap between fields so
         * the row can wrap instead of stretching the whole table wide. */
        snprintf(out, cap,
                 "%.2f&nbsp;V %.3f&nbsp;A %.2f&nbsp;W "
                 "%.0f&nbsp;Wh %d&nbsp;&deg;C",
                 v->uc96.voltage_v, v->uc96.current_a, v->uc96.power_w,
                 v->uc96.energy_wh, v->uc96.temperature_c);
        return;
    }
    if (!v->has_bthome) { snprintf(out, cap, "&mdash;"); return; }
    /* BTHome: append the present fields, clamping the offset so `cap - o` can
     * never underflow (keeps snprintf's truncation analysis bounded too). */
    size_t o = 0;
    #define BV_APP(...) do {                                          \
        if (o < cap) { int _k = snprintf(out + o, cap - o, __VA_ARGS__); \
                       if (_k > 0) o += (size_t)_k;                   \
                       if (o > cap) o = cap; }                        \
    } while (0)
    if (v->bthome.temp_present)     BV_APP("%.1f&deg;C ", v->bthome.temp_c);
    if (v->bthome.humidity_present) BV_APP("%.1f%%RH ", v->bthome.humidity_pct);
    if (v->bthome.battery_present)  BV_APP("%u%%bat ", (unsigned)v->bthome.battery_pct);
    if (v->bthome.voltage_present)  BV_APP("%.3fV", v->bthome.voltage_v);
    #undef BV_APP
}

static const ble_dev_view_t *ble_view_by_id(const ble_dev_view_t *views, int n,
                                            const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(views[i].id, id) == 0) return &views[i];
    return NULL;
}

static esp_err_t ble_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* Heap, not a ~3 KB stack array, on the 8 KB plain-HTTP task. nv=0 on alloc
     * failure → the render loops below no-op; single return at the end frees. */
    ble_dev_view_t *views = malloc(sizeof(*views) * BLE_VIEW_MAX);
    int nv = views ? ble_snapshot(views, BLE_VIEW_MAX) : 0;
    bool running = ble_running();
    bool enabled = app_config_get_bool("ble_enabled");

    char buf[1024], esc[80], mac[20], vals[160];
    /* Keep the 6 s auto-refresh (live BLE scan view) via the shared head's
     * extra-markup slot; the rest of the chrome is now identical to / and
     * /config. */
    send_page_head(req, "<meta http-equiv=\"refresh\" content=\"6\">", "ble");
    httpd_resp_sendstr_chunk(req, "<h2>&#128268; BLE devices</h2>");

    /* Power control — toggle ble_enabled here (moved off Settings). BLE starts
     * cleanly only at boot, so offer Reboot too; Enable also tries a runtime
     * start but that's unreliable on a fragmented heap → the warn-bar nudges. */
    snprintf(buf, sizeof(buf),
        "<p>BLE: <b>%s</b> (running: %s)</p>"
        "<form method=post action=/ble/enable style=display:inline>"
        "<input type=hidden name=on value=%d><button>%s BLE</button></form> "
        "<form method=post action=/ble/reboot style=display:inline>"
        "<button>&#128260; Reboot</button></form>",
        enabled ? "ON" : "OFF", running ? "yes" : "no",
        enabled ? 0 : 1, enabled ? "Disable" : "Enable");
    httpd_resp_sendstr_chunk(req, buf);
    if (enabled && !running)
        httpd_resp_sendstr_chunk(req,
            "<div class=\"warn-bar\">BLE enabled but not running — Reboot to start it "
            "(NimBLE needs a clean boot; runtime start is unreliable).</div>");

    /* Saved (allowlisted) devices — these are what the firmware connects to /
     * ingests. Cross-reference the live snapshot for state + last values. */
    httpd_resp_sendstr_chunk(req,
        "<h3>Saved (allowlist)</h3>"
        "<table><tr><th>Name</th><th>MAC</th><th>Type</th><th>State</th>"
        "<th>Last values</th><th></th></tr>");
    int saved_n = ble_store_count();
    if (saved_n == 0)
        httpd_resp_sendstr_chunk(req, "<tr><td colspan=6 class=k>none yet — save one below</td></tr>");
    char sid[BLE_STORE_ID_CAP], sname[BLE_STORE_NAME_CAP];
    for (int i = 0; i < saved_n; i++) {
        if (!ble_store_list(i, sid, sizeof(sid), sname, sizeof(sname))) break;
        const ble_dev_view_t *v = ble_view_by_id(views, nv, sid);
        ble_id_to_mac(sid, mac, sizeof(mac));
        scan_html_escape(sname[0] ? sname : sid, esc, sizeof(esc));
        /* Copy the kind/state pointers into bounded arrays so snprintf's
         * truncation analysis can size the %s (a const char* return can't be). */
        char type[10], st[12];
        snprintf(type, sizeof(type), "%s", v ? ble_kind_str(v->kind) : "?");
        snprintf(st, sizeof(st), "%s", v ? ble_state_str(v->conn_state) : "offline");
        if (v) ble_values_html(v, vals, sizeof(vals)); else snprintf(vals, sizeof(vals), "&mdash;");
        /* row + rename form (prefilled) + forget button — emitted in pieces. */
        httpd_resp_sendstr_chunk(req, "<tr><td>");
        httpd_resp_sendstr_chunk(req, esc);
        httpd_resp_sendstr_chunk(req, "</td><td class=k>");
        httpd_resp_sendstr_chunk(req, mac);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, type);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, st);
        httpd_resp_sendstr_chunk(req, "</td><td class=v>");
        httpd_resp_sendstr_chunk(req, vals);
        httpd_resp_sendstr_chunk(req,
            "</td><td><form method=POST action=/ble/name style=display:inline>"
            "<input type=hidden name=id value=");
        httpd_resp_sendstr_chunk(req, sid);
        httpd_resp_sendstr_chunk(req,
            "><input name=name maxlength=32 placeholder=name size=8>"
            "<button>rename</button></form> "
            "<form method=POST action=/ble/forget style=display:inline>"
            "<input type=hidden name=id value=");
        httpd_resp_sendstr_chunk(req, sid);
        httpd_resp_sendstr_chunk(req, "><button>forget</button></form></td></tr>");
    }
    httpd_resp_sendstr_chunk(req, "</table>");

    /* In-range (scan) — devices seen but NOT saved. */
    httpd_resp_sendstr_chunk(req,
        "<h3>In range (scan)</h3>"
        "<table><tr><th>Name</th><th>MAC</th><th>Type</th><th>RSSI</th>"
        "<th>Values</th><th></th></tr>");
    int shown = 0;
    for (int i = 0; i < nv; i++) {
        if (ble_store_is_saved(views[i].id)) continue;     /* already in saved table */
        shown++;
        ble_id_to_mac(views[i].id, mac, sizeof(mac));
        scan_html_escape(views[i].adv_name[0] ? views[i].adv_name : "(no name)", esc, sizeof(esc));
        ble_values_html(&views[i], vals, sizeof(vals));
        /* Emit in pieces (no single big snprintf) — each %s is a small bounded
         * buffer, sidestepping gcc's -Wformat-truncation worst-case sum. */
        char tiny[40];
        httpd_resp_sendstr_chunk(req, "<tr><td>");
        httpd_resp_sendstr_chunk(req, esc);
        httpd_resp_sendstr_chunk(req, "</td><td class=k>");
        httpd_resp_sendstr_chunk(req, mac);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, ble_kind_str(views[i].kind));
        snprintf(tiny, sizeof(tiny), "</td><td class=v>%d dBm</td><td class=v>", views[i].rssi);
        httpd_resp_sendstr_chunk(req, tiny);
        httpd_resp_sendstr_chunk(req, vals);
        httpd_resp_sendstr_chunk(req,
            "</td><td><form method=POST action=/ble/name style=display:inline>"
            "<input type=hidden name=id value=");
        httpd_resp_sendstr_chunk(req, views[i].id);
        httpd_resp_sendstr_chunk(req,
            "><input name=name maxlength=32 placeholder=name size=8>"
            "<button>save</button></form></td></tr>");
    }
    if (shown == 0)
        httpd_resp_sendstr_chunk(req,
            running ? "<tr><td colspan=6 class=k>nothing new in range</td></tr>"
                    : "<tr><td colspan=6 class=k>BLE not running</td></tr>");
    httpd_resp_sendstr_chunk(req, "</table>");

    httpd_resp_sendstr_chunk(req,
        "<p><form method=POST action=/ble/scan style=display:inline>"
        "<button>&#128260; Rescan</button></form> "
        "<a href=/ble.json>ble.json</a></p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(views);
    return ESP_OK;
}

static esp_err_t ble_json_get(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    /* Heap, not a ~3 KB stack array. nv=0 on alloc failure → empty list; freed
     * at the single return below. */
    ble_dev_view_t *views = malloc(sizeof(*views) * BLE_VIEW_MAX);
    int nv = views ? ble_snapshot(views, BLE_VIEW_MAX) : 0;

    char buf[512];
    snprintf(buf, sizeof(buf), "{\"running\":%s,\"enabled\":%s,\"devices\":[",
             ble_running() ? "true" : "false",
             app_config_get_bool("ble_enabled") ? "true" : "false");
    httpd_resp_sendstr_chunk(req, buf);
    char name[BLE_STORE_NAME_CAP], jname[2 * BLE_STORE_NAME_CAP];
    for (int i = 0; i < nv; i++) {
        const ble_dev_view_t *v = &views[i];
        bool saved = ble_store_get_name(v->id, name, sizeof(name));
        json_escape_str(saved && name[0] ? name : v->adv_name, jname, sizeof(jname));
        char kind[10], st[12];
        snprintf(kind, sizeof(kind), "%s", ble_kind_str(v->kind));
        snprintf(st, sizeof(st), "%s", ble_state_str(v->conn_state));
        int o = snprintf(buf, sizeof(buf),
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"saved\":%s,\"kind\":\"%s\","
            "\"rssi\":%d,\"age_s\":%d,\"state\":\"%s\"",
            i ? "," : "", v->id, jname, saved ? "true" : "false",
            kind, v->rssi, v->age_s, st);
        /* Each append guards `o < sizeof(buf)` first: snprintf returns the
         * would-be length, so o can run past the buffer on a pathological row —
         * and then `sizeof(buf) - o` (size_t) underflows to a huge value, turning
         * the next snprintf into an unbounded write. Guarding keeps `o` from
         * being used once full (truncates cleanly instead of corrupting). */
        if (v->has_uc96 && o < (int)sizeof(buf))
            o += snprintf(buf + o, sizeof(buf) - o,
                ",\"v\":%.2f,\"i\":%.3f,\"p\":%.2f,\"wh\":%.2f,\"temp\":%d",
                v->uc96.voltage_v, v->uc96.current_a, v->uc96.power_w,
                v->uc96.energy_wh, v->uc96.temperature_c);
        if (v->has_bthome) {
            if (v->bthome.temp_present     && o < (int)sizeof(buf)) o += snprintf(buf+o, sizeof(buf)-o, ",\"temp\":%.2f", v->bthome.temp_c);
            if (v->bthome.humidity_present && o < (int)sizeof(buf)) o += snprintf(buf+o, sizeof(buf)-o, ",\"hum\":%.2f", v->bthome.humidity_pct);
            if (v->bthome.battery_present  && o < (int)sizeof(buf)) o += snprintf(buf+o, sizeof(buf)-o, ",\"bat\":%u", (unsigned)v->bthome.battery_pct);
            if (v->bthome.voltage_present  && o < (int)sizeof(buf)) o += snprintf(buf+o, sizeof(buf)-o, ",\"volt\":%.3f", v->bthome.voltage_v);
        }
        if (o < (int)sizeof(buf)) snprintf(buf + o, sizeof(buf) - o, "}");
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    free(views);
    return ESP_OK;
}

/* POST-redirect-GET: on success bounce back to the live /ble page; on error
 * show a short message with a link back. */
static esp_err_t ble_prg(httpd_req_t *req, esp_err_t e, const char *err_msg) {
    if (e == ESP_OK) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/ble");
        return httpd_resp_sendstr(req, "");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char b[200];
    snprintf(b, sizeof(b),
        "<!doctype html><meta charset=utf-8><link rel=stylesheet href=/style.css>"
        "<body><p>%s</p><p><a href=/ble>&larr; BLE</a></p>", err_msg);
    return httpd_resp_sendstr(req, b);
}

/* Read an x-www-form-urlencoded body into a fixed buffer. Returns len or -1. */
static int ble_read_body(httpd_req_t *req, char *buf, size_t cap) {
    int total = req->content_len;
    if (total <= 0 || total >= (int)cap) return -1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

static esp_err_t ble_name_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[256];
    if (ble_read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char id[BLE_STORE_ID_CAP] = {0}, name[BLE_STORE_NAME_CAP] = {0};
    form_field(body, "id", id, sizeof(id));
    form_field(body, "name", name, sizeof(name));
    esp_err_t e = ble_store_save(id, name);  /* validates id; "" name allowed */
    return ble_prg(req, e, "bad device id");
}

static esp_err_t ble_forget_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[128];
    if (ble_read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char id[BLE_STORE_ID_CAP] = {0};
    form_field(body, "id", id, sizeof(id));
    esp_err_t e = ble_store_forget(id);
    return ble_prg(req, e, "bad device id");
}

static esp_err_t ble_scan_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    /* Drain any (empty) body so the socket stays clean. */
    char body[32];
    (void)ble_read_body(req, body, sizeof(body));
    ble_request_scan();
    return ble_prg(req, ESP_OK, NULL);
}

/* POST /ble/enable on=1|0 — flip ble_enabled (the toggle moved here off Settings).
 * Setting the key fires app_config's ble_apply_config side-effect (tries a runtime
 * start on ON; defers stop to reboot on OFF), so a Reboot is offered alongside. */
static esp_err_t ble_enable_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[48];
    if (ble_read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char on[8] = {0};
    form_field(body, "on", on, sizeof(on));
    esp_err_t e = app_config_set_from_string("ble_enabled", on[0] == '1' ? "ON" : "OFF");
    return ble_prg(req, e, "toggle failed");
}

/* POST /ble/reboot — clean restart (BLE starts/stops cleanly only at boot). */
static esp_err_t ble_reboot_post(httpd_req_t *req) {
    HTTP_AUTH_OR_RETURN(req);
    char body[16];
    (void)ble_read_body(req, body, sizeof(body));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8><link rel=stylesheet href=/style.css>"
        "<body><p>Rebooting&hellip;</p><p><a href=/ble>&larr; BLE</a></p>");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;  /* not reached */
}

esp_err_t http_server_start(void) {
    if (s_server)
        return ESP_OK;

    if (!s_listing_mtx)
        s_listing_mtx = xSemaphoreCreateMutex();

    /* Decide HTTP vs HTTPS based on whether the TLS NVS store has a
     * complete cert/key record. The cert+key blob lives on the heap
     * for the server's lifetime (esp_https_server copies the bytes
     * internally on start, but we keep our own copy so a future
     * reload-on-rotation path doesn't need to re-read NVS). */
    /* Full AP-only mode: a client joining our SoftAP cannot validate the
     * per-device private-CA cert by IP — Android sends a fatal TLS alert
     * (mbedtls -0x7780) and won't even offer a "proceed anyway", so the
     * /config + /wifi portal is dead over HTTPS. Serve plain HTTP instead;
     * WPA2 on the AP is the access boundary. (Normal STA boot still uses
     * HTTPS — this only flips while the box is sticky-AP-only.) */
    const bool ap_only_http = wifi_store_is_ap_only();
    bool want_https = false;
    if (!ap_only_http && tls_store_has_cert()) {
        s_tls_blob = calloc(1, sizeof(*s_tls_blob));
        if (s_tls_blob && tls_store_load(s_tls_blob) == ESP_OK) {
            want_https = true;
        } else {
            ESP_LOGW(TAG, "tls_store_load failed — falling back to plain HTTP");
            free(s_tls_blob);
            s_tls_blob = NULL;
        }
    }
    if (ap_only_http)
        ESP_LOGW(TAG, "AP-only: serving plain HTTP on :80 (AP client can't validate our private-CA cert)");

    esp_err_t e;
    if (want_https) {
        httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
        /* PEM bundle (leaf + sub-CA, NUL-terminated). mbedtls_x509_crt_
         * parse loops over the two -----BEGIN CERTIFICATE----- blocks
         * and builds a 2-element crt linked list; the TLS handshake
         * code serialises both into the Certificate message, so the
         * relying party only needs the root in its trust store. */
        cfg.servercert     = s_tls_blob->cert_pem;
        cfg.servercert_len = s_tls_blob->cert_len;
        cfg.prvtkey_pem    = s_tls_blob->key_der;
        cfg.prvtkey_len    = s_tls_blob->key_len;
        cfg.port_secure    = 443;
        cfg.port_insecure  = 0;  /* don't double-bind :80 — redirect_srv does */
        cfg.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
        cfg.httpd.ctrl_port      = 32768;
        cfg.httpd.max_uri_handlers = 50;  /* +wifi +config +wifiscan +ble forms +view/exif/last.json +ble enable/reboot +debug/cam_standby */
        /* max_open_sockets bounds CONCURRENT TLS sessions. mbedtls session
         * working sets (~40 KB) route to PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC),
         * but each in-flight handshake's HW-AES (esp-aes) DMA buffer MUST be
         * internal DRAM — and with BLE on, the BT controller eats ~52 KB internal,
         * leaving dma_largest at only ~2-3 KB (bench-measured). At 8 concurrent
         * handshakes the internal DMA pool starves → "esp-aes: Failed to allocate
         * memory" → handshakes reset (bench: BLE-on, 8x burst = 6/8 ConnectError).
         * Capped to 4 (the historically OOM-safe number) so peak concurrent AES
         * demand stays within the BLE-on internal headroom; a bird box never needs
         * 8 parallel HTTPS. Extra clients queue on the 24-socket LWIP backlog +
         * lru_purge; 10 s recv/send timeouts free a stuck slot fast. */
        cfg.httpd.max_open_sockets = 4;
        cfg.httpd.lru_purge_enable = true;
        cfg.httpd.recv_wait_timeout = 10;
        cfg.httpd.send_wait_timeout = 10;
        cfg.httpd.stack_size = 12288;  /* +4 KB over plain — mbedtls SSL state on stack */
        e = httpd_ssl_start(&s_server, &cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ssl_start: %s", esp_err_to_name(e));
            free(s_tls_blob); s_tls_blob = NULL;
            return e;
        }
        s_https_active = true;
        ESP_LOGI(TAG, "HTTPS server up on :443 (cert %zu B, key %zu B)",
                 s_tls_blob->cert_len, s_tls_blob->key_len);
        /* Loud warning if the sensitive endpoints (/mic.wav, /stream.mjpg,
         * /last.jpg, /photos, /i2c*, /capture, /selftest) are served WITHOUT
         * basic auth because HTTP_BASIC_USER/PASS are still placeholders. We
         * keep serving (the live stream is the field camera-positioning tool,
         * so availability wins) but make the exposure unmissable: a boot
         * ESP_LOGW plus a direct GlitchTip event so it shows up off-box.
         * Set real HTTP_BASIC_* creds in secrets.h to lock it down. */
        if (secret_is_placeholder(HTTP_BASIC_USER) ||
            secret_is_placeholder(HTTP_BASIC_PASS)) {
            ESP_LOGW(TAG,
                     "SECURITY: HTTP basic-auth DISABLED (placeholder "
                     "HTTP_BASIC_* creds) — /mic.wav, /stream.mjpg, /last.jpg, "
                     "/photos, /i2c* served UNAUTHENTICATED over HTTPS. Set "
                     "real creds in secrets.h to lock down.");
            if (glitchtip_ready())
                glitchtip_report(
                    "warning",
                    "HTTP basic-auth disabled (placeholder creds) — sensitive "
                    "endpoints served unauthenticated over HTTPS",
                    NULL);
        }
        start_redirect_server();
    } else {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.ctrl_port = 32768;
        cfg.max_uri_handlers = 50;  // prod + wifi + config + wifiscan + ble + view/exif/last.json + ble enable/reboot + debug + slack + debug/cam_standby
        cfg.lru_purge_enable = true;
        /* Three streams (/stream.mjpg, /mic.wav, /photo) + homepage + last.jpg
         * + ad-hoc clients can legitimately push past the default ~4-7 socket
         * cap. 7 sockets fits comfortably under CONFIG_LWIP_MAX_SOCKETS=16
         * (raised from 10) alongside MQTT + the audio relay + upload bursts. */
        cfg.max_open_sockets = 7;
        /* Recv/send timeouts so a stalled streaming client (laptop closes
         * tab, phone goes to sleep with the MJPEG stream open) eventually
         * frees the handler task slot instead of holding it indefinitely. */
        cfg.recv_wait_timeout = 30;
        cfg.send_wait_timeout = 30;
        /* Default is 4096 B which the homepage handler outgrew: it allocates
         * a 1 KB chunk[] on stack and calls into sht41_read / battery_*
         * / esp_wifi_sta_get_ap_info / printf — observed wedging board .104
         * with no panic dump (USB-CDC TX task starves before output flushes).
         * 8 KB gives generous headroom. */
        cfg.stack_size = 8192;
        e = httpd_start(&s_server, &cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
            return e;
        }
        s_https_active = false;
        /* basic-auth is HTTPS-only (creds over plaintext are worse than none),
         * so on the plain-HTTP fallback the sensitive endpoints are served
         * UNAUTHENTICATED. Expected briefly pre-enrollment, but if it persists
         * (signer unreachable, or a cert that expired with no renewal) the box
         * is an open camera/mic on the LAN — make that loud, not silent. */
        ESP_LOGW(TAG,
                 "HTTP (plaintext) up on :80 — no cert in NVS. SECURITY: "
                 "basic-auth is HTTPS-only, so /mic.wav, /stream.mjpg, /last.jpg, "
                 "/photos, /capture, /i2c* are UNAUTHENTICATED until TLS enrolls. "
                 "Expected pre-enrollment; if it sticks, the board isn't reaching "
                 "the enroll signer (or the cert expired).");
        if (glitchtip_ready())
            glitchtip_report("warning",
                             "Serving plain HTTP (no cert) — sensitive endpoints "
                             "unauthenticated until TLS enrollment completes",
                             NULL);
    }

    httpd_uri_t r_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t r_fav = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    httpd_uri_t r_css = {.uri = "/style.css", .method = HTTP_GET, .handler = style_css_get};
    httpd_uri_t r_logo = {.uri = "/logo.svg", .method = HTTP_GET, .handler = logo_svg_get};
    httpd_uri_t r_jpg = {.uri = "/last.jpg", .method = HTTP_GET, .handler = last_jpg_get};
    httpd_uri_t r_cap = {.uri = "/capture", .method = HTTP_GET, .handler = capture_get};
    httpd_uri_t r_mic = {.uri = "/mic.wav", .method = HTTP_GET, .handler = mic_wav_get};
    httpd_uri_t r_mjpg = {.uri = "/stream.mjpg", .method = HTTP_GET, .handler = stream_mjpg_get};
    httpd_uri_t r_st = {.uri = "/selftest", .method = HTTP_GET, .handler = selftest_get};
    httpd_uri_t r_i2c = {.uri = "/i2c", .method = HTTP_GET, .handler = i2c_scan_get};
    httpd_uri_t r_sht_b1 = {.uri = "/sht41/bus1", .method = HTTP_GET, .handler = sht41_bus1_get};
    httpd_uri_t r_max_b1 = {.uri = "/max17048/bus1", .method = HTTP_GET,
                            .handler = max17048_bus1_get};
    httpd_uri_t r_b1_diag = {.uri = "/i2c/bus1_diag", .method = HTTP_GET,
                             .handler = i2c_bus1_diag_get};
    httpd_uri_t r_sensors = {.uri = "/sensors", .method = HTTP_GET, .handler = sensors_get};
    httpd_uri_t r_oled_qr = {.uri = "/oled/qr", .method = HTTP_GET, .handler = oled_qr_get};
    httpd_uri_t r_oled_logo_get = {.uri = "/oled/logo", .method = HTTP_GET, .handler = oled_logo_get};
    httpd_uri_t r_oled_logo_bin = {.uri = "/oled/logo.bin", .method = HTTP_GET, .handler = oled_logo_bin_get};
    httpd_uri_t r_oled_logo = {.uri = "/oled/logo", .method = HTTP_POST, .handler = oled_logo_post};
    httpd_uri_t r_phs = {.uri = "/photos", .method = HTTP_GET, .handler = photos_get};
    httpd_uri_t r_phsj = {.uri = "/photos.json", .method = HTTP_GET, .handler = photos_json_get};
    httpd_uri_t r_ph = {.uri = "/photo", .method = HTTP_GET, .handler = photo_get};
    httpd_uri_t r_view = {.uri = "/view", .method = HTTP_GET, .handler = view_get};
    httpd_uri_t r_phex = {.uri = "/photo/exif", .method = HTTP_GET, .handler = photo_exif_get};
    httpd_uri_t r_lastj = {.uri = "/last.json", .method = HTTP_GET, .handler = last_json_get};
    /* SoftAP recovery form — gated to AP-active inside the handlers, so in
     * normal operation these 404. Registered on the main server so a
     * plain-HTTP board serves the form directly on :80 in AP mode. */
    httpd_uri_t r_wifi_get = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_form_get};
    httpd_uri_t r_wifi_post = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_form_post};
    httpd_uri_t r_cfg_get = {.uri = "/config", .method = HTTP_GET, .handler = config_get};
    httpd_uri_t r_cfg_post = {.uri = "/config", .method = HTTP_POST, .handler = config_post};
    httpd_uri_t r_wscan = {.uri = "/wifiscan", .method = HTTP_GET, .handler = wifiscan_get};
    httpd_uri_t r_ble      = {.uri = "/ble",        .method = HTTP_GET,  .handler = ble_get};
    httpd_uri_t r_ble_json = {.uri = "/ble.json",   .method = HTTP_GET,  .handler = ble_json_get};
    httpd_uri_t r_ble_name = {.uri = "/ble/name",   .method = HTTP_POST, .handler = ble_name_post};
    httpd_uri_t r_ble_forg = {.uri = "/ble/forget", .method = HTTP_POST, .handler = ble_forget_post};
    httpd_uri_t r_ble_scan = {.uri = "/ble/scan",   .method = HTTP_POST, .handler = ble_scan_post};
    httpd_uri_t r_ble_en   = {.uri = "/ble/enable", .method = HTTP_POST, .handler = ble_enable_post};
    httpd_uri_t r_ble_rb   = {.uri = "/ble/reboot", .method = HTTP_POST, .handler = ble_reboot_post};
    /* Loud-fail any registration miss — wrap each call so a future
     * over-cap addition trips the boot log instead of silently 404'ing
     * in the field. cfg.max_uri_handlers=20 leaves room; trip-wire is
     * here because a silent registration drop is exactly how the
     * dual-core rewrite's /debug/cores first 404'd. */
#define REG(URI) do {                                                       \
        esp_err_t _r = httpd_register_uri_handler(s_server, &(URI));        \
        if (_r != ESP_OK)                                                   \
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s — "    \
                     "raise cfg.max_uri_handlers", (URI).uri,               \
                     esp_err_to_name(_r));                                  \
    } while (0)
    REG(r_root);
    REG(r_fav);
    REG(r_css);
    REG(r_logo);
    REG(r_jpg);
    REG(r_cap);
    REG(r_mic);
    REG(r_mjpg);
    REG(r_st);
    REG(r_i2c);
    REG(r_sht_b1);
    REG(r_max_b1);
    REG(r_b1_diag);
    REG(r_sensors);
    REG(r_oled_qr);
    REG(r_oled_logo_get);
    REG(r_oled_logo_bin);
    REG(r_oled_logo);
    REG(r_phs);
    REG(r_phsj);
    REG(r_ph);
    REG(r_view);
    REG(r_phex);
    REG(r_lastj);
    REG(r_wifi_get);
    REG(r_wifi_post);
    REG(r_cfg_get);
    REG(r_cfg_post);
    REG(r_wscan);
    REG(r_ble);
    REG(r_ble_json);
    REG(r_ble_name);
    REG(r_ble_forg);
    REG(r_ble_scan);
    REG(r_ble_en);
    REG(r_ble_rb);
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
    httpd_uri_t r_hang = {.uri = "/debug/hang", .method = HTTP_GET, .handler = debug_hang_get};
    httpd_uri_t r_dc = {
        .uri = "/debug/wifi_disconnect", .method = HTTP_GET, .handler = debug_wifi_disconnect_get};
    httpd_uri_t r_sdf = {
        .uri = "/debug/sd_format", .method = HTTP_GET, .handler = debug_sd_format_get};
    httpd_uri_t r_pir = {.uri = "/debug/pir", .method = HTTP_GET, .handler = debug_pir_get};
    httpd_uri_t r_sdr = {
        .uri = "/debug/sd_remount", .method = HTTP_GET, .handler = debug_sd_remount_get};
    httpd_uri_t r_smig = {
        .uri = "/debug/sd_migrate", .method = HTTP_GET, .handler = debug_sd_migrate_get};
    httpd_uri_t r_cores = {
        .uri = "/debug/cores", .method = HTTP_GET, .handler = debug_cores_get};
    httpd_uri_t r_usrv = {
        .uri = "/debug/uart_servo", .method = HTTP_POST, .handler = debug_uart_servo_post};
    httpd_uri_t r_dcap = {
        .uri = "/debug/capture", .method = HTTP_POST, .handler = debug_capture_post};
    httpd_uri_t r_tcsr = {
        .uri = "/debug/tls_csr", .method = HTTP_GET, .handler = debug_tls_csr_get};
    httpd_uri_t r_tenr = {
        .uri = "/debug/tls_enroll", .method = HTTP_POST, .handler = debug_tls_enroll_post};
    httpd_uri_t r_ters = {
        .uri = "/debug/tls_erase", .method = HTTP_POST, .handler = debug_tls_erase_post};
    httpd_uri_t r_wsap = {
        .uri = "/debug/wifi_softap", .method = HTTP_POST, .handler = debug_wifi_softap_post};
    httpd_uri_t r_cstby = {
        .uri = "/debug/cam_standby", .method = HTTP_GET, .handler = debug_cam_standby_get};
    REG(r_hang);
    REG(r_dc);
    REG(r_sdf);
    REG(r_pir);
    REG(r_sdr);
    REG(r_smig);
    REG(r_cores);
    REG(r_usrv);
    REG(r_dcap);
    REG(r_tcsr);
    REG(r_tenr);
    REG(r_ters);
    REG(r_wsap);
    REG(r_cstby);
    ESP_LOGW(TAG,
             "DEBUG endpoints enabled (/debug/hang, "
             "/debug/wifi_disconnect, /debug/sd_format, /debug/pir, "
             "/debug/sd_remount, /debug/cores, /debug/uart_servo, "
             "/debug/capture, /debug/tls_csr, /debug/tls_enroll, "
             "/debug/tls_erase, /debug/cam_standby) — disable before field "
             "deployment via CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n");
#endif

    ESP_LOGI(TAG, "HTTP server up on :80");
    return ESP_OK;
}

bool http_server_is_https(void) {
    return s_https_active;
}

void http_server_stop(void) {
    if (s_redirect_srv) {
        httpd_stop(s_redirect_srv);
        s_redirect_srv = NULL;
    }
    if (s_server) {
        if (s_https_active) {
            httpd_ssl_stop(s_server);
        } else {
            httpd_stop(s_server);
        }
        s_server = NULL;
    }
    s_https_active = false;
    if (s_tls_blob) {
        free(s_tls_blob);
        s_tls_blob = NULL;
    }
}
