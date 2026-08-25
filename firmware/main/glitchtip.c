/* glitchtip.c — Sentry envelope reporter, see glitchtip.h. */

#include "glitchtip.h"
#include "secrets.h"
#include "device_id.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "glitchtip";

/* Parsed DSN. */
static char s_envelope_url[160];
static char s_auth_header[160];
static bool s_ready = false;

/* Queued event. The body is pre-formatted by the producer and POSTed
 * verbatim by the consumer. Keeping it small (1 KB) keeps the queue
 * cheap to allocate in SRAM. Anything longer gets truncated at
 * source. */
typedef struct {
    char body[1024];
    size_t len;
} gt_event_t;

#define GT_QUEUE_LEN 16
static QueueHandle_t s_queue = NULL;

/* gt_task handle — published in gt_task body so log_vprintf can skip
 * recursion when it's our own task that emitted a log line. */
static TaskHandle_t s_gt_task_handle = NULL;

/* Recursion guard for the log-hook send path. The task-handle check
 * (line ~320) only catches logs emitted directly *from* gt_task. But
 * gt_task's esp_http_client_perform() fans out work onto lwIP, WiFi
 * tx, mbedtls, and esp-tls tasks, each of which can emit ESP_LOGE
 * mid-handshake under degraded networking — those run under different
 * task handles, slip through the per-task guard, get enqueued as
 * envelopes, send fails again, log fires again. The queue is 16
 * slots; under sustained TLS failure the chain saturates the queue
 * and floods the broker (and runs up the GlitchTip ingestion bill).
 *
 * Setting this atomic flag for the duration of a send_envelope_now
 * call lets the log hook short-circuit *any* error logged while we're
 * inside the HTTP/TLS stack, regardless of which task emitted it.
 * Cleared at the end of send_envelope_now; the 200 ms inter-send
 * delay in gt_task lives outside the gate, but the mbedtls/lwIP/
 * WiFi-tx tasks don't routinely log ESP_LOGE outside an active
 * connection so that gap is benign in practice. */
static atomic_int s_send_in_flight = 0;

static bool parse_dsn(const char *dsn) {
    if (!dsn || !*dsn) return false;
    const char *p = strstr(dsn, "://");
    if (!p) return false;
    p += 3;
    const char *at = strchr(p, '@');
    if (!at) return false;
    size_t keylen = (size_t)(at - p);
    if (keylen == 0 || keylen >= 64) return false;
    char key[64];
    memcpy(key, p, keylen);
    key[keylen] = 0;
    const char *host = at + 1;
    const char *slash = strchr(host, '/');
    if (!slash) return false;
    size_t hostlen = (size_t)(slash - host);
    if (hostlen == 0 || hostlen >= 80) return false;
    char host_str[80];
    memcpy(host_str, host, hostlen);
    host_str[hostlen] = 0;
    const char *project = slash + 1;
    if (!*project) return false;
    int n = snprintf(s_envelope_url, sizeof(s_envelope_url),
                     "https://%s/api/%s/envelope/", host_str, project);
    if (n <= 0 || (size_t)n >= sizeof(s_envelope_url)) return false;
    n = snprintf(s_auth_header, sizeof(s_auth_header),
                 "Sentry sentry_version=7, sentry_client=chytra-budka/1.0, "
                 "sentry_key=%s", key);
    if (n <= 0 || (size_t)n >= sizeof(s_auth_header)) return false;
    return true;
}

static void event_id_hex(char out[33]) {
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(esp_random() & 0xff);
    /* Sentry expects a 32-char lowercase hex string, no dashes. */
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
    out[32] = 0;
}

static void iso_now(char out[32]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* JSON-escape src into dst (up to dst_cap-1 chars, NUL-terminated).
 * Just enough: backslash, double-quote, control chars, newline.
 * Drops other non-printable bytes for safety. */
static void json_escape(char *dst, size_t dst_cap, const char *src) {
    size_t o = 0;
    if (dst_cap == 0) return;
    for (size_t i = 0; src[i] && o + 8 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    /* drop other control chars */
                } else {
                    dst[o++] = (char)c;
                }
        }
    }
    dst[o] = 0;
}

static void send_envelope_now(const char *body, size_t len) {
    /* Block log-hook re-entry from any task while the TLS/HTTP stack
     * is in flight. See s_send_in_flight declaration for rationale. */
    atomic_store(&s_send_in_flight, 1);
    esp_http_client_config_t cfg = {
        .url = s_envelope_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        atomic_store(&s_send_in_flight, 0);
        return;
    }
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-sentry-envelope");
    esp_http_client_set_header(client, "X-Sentry-Auth", s_auth_header);
    esp_http_client_set_post_field(client, body, (int)len);
    esp_err_t e = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    if (e != ESP_OK || code < 200 || code >= 300) {
        ESP_LOGW(TAG, "envelope POST failed: err=%s code=%d",
                 esp_err_to_name(e), code);
    }
    esp_http_client_cleanup(client);
    /* Clear the re-entrancy gate now that the HTTP/TLS stack is idle. The
     * 200 ms inter-send pacing delay in gt_task runs OUTSIDE this gate; the
     * mbedtls/lwIP/WiFi-tx tasks don't routinely log ESP_LOGE outside an
     * active connection, so that gap is benign in practice. */
    atomic_store(&s_send_in_flight, 0);
}

static void gt_task(void *arg) {
    (void)arg;
    /* Publish our own task handle so the log-hook recursion guard can
     * recognise us. Doing it inside the task body guarantees the
     * handle is valid by the time we run. */
    s_gt_task_handle = xTaskGetCurrentTaskHandle();
    gt_event_t ev;
    while (1) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) == pdTRUE) {
            send_envelope_now(ev.body, ev.len);
            /* Pace ourselves so a burst of LOGE doesn't saturate WiFi
             * — 200 ms between POSTs is comfortable. */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

bool glitchtip_init(void) {
    if (s_ready) return true;
    if (!parse_dsn(GLITCHTIP_DSN)) {
        ESP_LOGE(TAG, "DSN parse failed");
        return false;
    }
    if (!s_queue) {
        s_queue = xQueueCreate(GT_QUEUE_LEN, sizeof(gt_event_t));
        if (!s_queue) return false;
    }
    BaseType_t r = xTaskCreate(gt_task, "glitchtip", 6144, NULL,
                               tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) return false;
    s_ready = true;
    ESP_LOGI(TAG, "ready (envelope=%s)", s_envelope_url);
    return true;
}

bool glitchtip_ready(void) { return s_ready; }

bool glitchtip_report(const char *level, const char *message,
                      const char *extra_fields_json) {
    if (!s_ready || !level || !message) return false;

    char event_id[33];
    event_id_hex(event_id);
    char iso[32];
    iso_now(iso);

    const esp_app_desc_t *app = esp_app_get_description();
    const char *ver = app ? app->version : "?";

    /* Heap-allocate the event + escape buffer. gt_event_t alone is
     * ~1 KB; with the 512 B escape buffer that's >1.5 KB, which would
     * blow the 3.5 KB main-task stack when this is called from
     * app_main's crash-boot reporter. The queue makes its own copy of
     * the struct on xQueueSend, so we can free immediately after. */
    gt_event_t *ev = (gt_event_t *)malloc(sizeof(*ev));
    if (!ev) return false;
    char *esc_msg = (char *)malloc(512);
    if (!esc_msg) { free(ev); return false; }
    json_escape(esc_msg, 512, message);

    /* Runtime context — origin core and task. Always surface as tags
     * so a TWDT/panic captured under audio_task on CPU1 doesn't dedup
     * with one under main on CPU0; helpful narrowing when triaging
     * multicore issues. xPortGetCoreID + pcTaskGetName are safe from
     * any IDF task context. */
    const char *task_name = pcTaskGetName(NULL);
    int core_id = xPortGetCoreID();

    ev->len = (size_t)snprintf(ev->body, sizeof(ev->body),
        "{\"event_id\":\"%s\",\"sent_at\":\"%s\"}\n"
        "{\"type\":\"event\",\"content_type\":\"application/json\"}\n"
        "{\"event_id\":\"%s\",\"timestamp\":\"%s\",\"level\":\"%s\","
        "\"platform\":\"native\",\"release\":\"%s\","
        "\"server_name\":\"%s\","
        "\"tags\":{\"core_id\":\"%d\",\"task\":\"%s\"%s%s},"
        "\"message\":{\"formatted\":\"%s\"}}\n",
        event_id, iso,
        event_id, iso, level, ver,
        device_id(),
        core_id, task_name ? task_name : "?",
        extra_fields_json ? "," : "",
        extra_fields_json ? extra_fields_json : "",
        esc_msg);
    free(esc_msg);
    if (ev->len == 0 || ev->len >= sizeof(ev->body)) { free(ev); return false; }

    /* Non-blocking: if queue full, drop the oldest then enqueue. */
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        /* gt_event_t is 1 KB — heap-alloc the dropped slot too rather
         * than put another KB on the caller's stack (this can fire
         * from the main task during a LOGE burst). */
        gt_event_t *dropped = (gt_event_t *)malloc(sizeof(*dropped));
        if (dropped) {
            (void)xQueueReceive(s_queue, dropped, 0);
            free(dropped);
        }
        (void)xQueueSend(s_queue, ev, 0);
        /* Surface the loss once a minute so a sustained LOGE storm
         * doesn't go invisible. The "glitchtip" tag is filtered out
         * by our own log hook so this won't recurse. */
        static int64_t s_last_warn_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_warn_us > 60LL * 1000 * 1000) {
            s_last_warn_us = now_us;
            ESP_LOGW(TAG, "queue full, oldest event dropped");
        }
    }
    free(ev);
    return true;
}

bool glitchtip_report_crash_boot(const char *reset_reason,
                                 uint32_t consecutive_crashes,
                                 size_t coredump_bytes) {
    if (!s_ready) return false;
    char tags[256];
    /* Inner tag fields only — glitchtip_report wraps them inside
     * "tags":{core_id, task, ...} so callers don't double-emit the
     * outer key. */
    snprintf(tags, sizeof(tags),
        "\"reset_reason\":\"%s\","
        "\"consecutive_crashes\":\"%" PRIu32 "\","
        "\"coredump_bytes\":\"%zu\"",
        reset_reason, consecutive_crashes, coredump_bytes);
    /* Stable, dedup-friendly message: only the reset_reason varies
     * across crash families. The per-event values (crash counter,
     * coredump size) live in tags, where GlitchTip's UI shows them
     * per occurrence without forking off a new issue per crash.
     * Previously the message embedded those counters and the
     * dashboard had 100 separate "boot after int_wdt" issues — one
     * per (crashes=N, coredump=M B) combination. */
    char msg[64];
    snprintf(msg, sizeof(msg), "boot after %s", reset_reason);
    /* Only ship a GlitchTip event for actual crashes — cold boots
     * and clean esp_restart() chatter would flood the dashboard. */
    bool is_crash = (strcmp(reset_reason, "panic") == 0 ||
                     strcmp(reset_reason, "int_wdt") == 0 ||
                     strcmp(reset_reason, "task_wdt") == 0 ||
                     strcmp(reset_reason, "other_wdt") == 0 ||
                     strcmp(reset_reason, "brownout") == 0 ||
                     strcmp(reset_reason, "lockup") == 0);
    if (!is_crash) return false;
    return glitchtip_report("fatal", msg, tags);
}

/* ─── ESP-IDF log hook ─────────────────────────────────────────────────
 * esp_log_set_vprintf installs a single custom vprintf-like callback
 * that ESP_LOGx routes through. We sniff the format string for the
 * ANSI-coded level marker (color escapes are always present in the
 * default log format) and forward E / W lines. */

static vprintf_like_t s_orig_vprintf = NULL;

/* The log hook also relies on s_gt_task_handle (declared above) as a
 * re-entrancy guard: esp_http_client + mbedtls inside our gt_task
 * emit their own ESP_LOGE/W lines when, e.g., a TLS handshake fails,
 * and without the guard every failure would enqueue another event,
 * which would fail again, ad infinitum. */

static int log_vprintf(const char *fmt, va_list args) {
    /* va_copy BEFORE the original vprintf consumes `args` — otherwise
     * the copy would start from the end of the arg list and vsnprintf
     * later would read garbage. */
    va_list args_copy;
    va_copy(args_copy, args);
    int rv = s_orig_vprintf ? s_orig_vprintf(fmt, args) : 0;

    if (!s_ready) {
        va_end(args_copy);
        return rv;
    }

    /* The IDF format string starts with the level char, optionally
     * wrapped in an ANSI color escape. Inspect the raw format string
     * (no vsnprintf yet — keeps stack pressure tiny on event task /
     * wifi-internal callers, which have ~2 KB stacks). */
    const char *p = fmt;
    if (*p == 0x1b) {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    char level = *p;
    if (level != 'E') {  // ship errors only — warnings would flood
        va_end(args_copy);
        return rv;
    }

    /* Don't ship if we're currently *inside* the GlitchTip POST task:
     * mbedtls/esp_http_client errors during our own send mustn't
     * trigger another send. */
    if (s_gt_task_handle && xTaskGetCurrentTaskHandle() == s_gt_task_handle) {
        va_end(args_copy);
        return rv;
    }
    /* Cross-task recursion guard — see s_send_in_flight declaration.
     * gt_task's HTTP/TLS work fans out onto lwIP / WiFi-tx / mbedtls
     * tasks; their error logs during a send slip past the per-task
     * check above. This flag closes that gap. */
    if (atomic_load(&s_send_in_flight)) {
        va_end(args_copy);
        return rv;
    }

    /* Format into a heap buffer so we don't blow the caller's stack
     * (event task / wifi callback / etc are tight). Drop the event
     * if we can't allocate — better than crashing. */
    char *buf = (char *)malloc(192);
    if (!buf) {
        va_end(args_copy);
        return rv;
    }
    vsnprintf(buf, 192, fmt, args_copy);
    va_end(args_copy);

    /* Strip leading + trailing ANSI / newlines. */
    char *q = buf;
    if (*q == 0x1b) {
        while (*q && *q != 'm') q++;
        if (*q) q++;
    }
    size_t qlen = strlen(q);
    while (qlen > 0 && (q[qlen-1] == '\n' || q[qlen-1] == '\r')) {
        q[--qlen] = 0;
    }
    char *esc = strstr(q, "\033[");
    if (esc) *esc = 0;

    /* Strip the IDF "<L> (<ms>) " prefix. With LOG_TIMESTAMP_SOURCE_RTOS
     * every line carries the boot-millisecond counter, e.g.
     *   "E (20382290) esp-tls-mbedtls: write error :-0x0050"
     * That counter is unique on every line, so GlitchTip fingerprints
     * each otherwise-identical error as a SEPARATE issue — hundreds of
     * "issues" for one root cause (observed on the -0x0050 churn).
     * Dropping the prefix makes them group by their stable text. The
     * per-event timestamp still lives in the envelope (iso_now) + tags. */
    char *m = q;
    if ((*m == 'E' || *m == 'W' || *m == 'I' || *m == 'D' || *m == 'V') &&
        m[1] == ' ' && m[2] == '(') {
        char *rp = strstr(m, ") ");
        if (rp) m = rp + 2;
    }

    /* Benign-noise denylist: client-side TLS disconnects logged at ERROR
     * by the IDF HTTPS-server / esp-tls stack are remote-peer behaviour,
     * not a device fault — a browser tab or HA poller closing a TLS
     * connection. Shipping them floods GlitchTip with non-actionable
     * "errors". These substrings are unambiguous client-disconnect noise;
     * genuine TLS faults (cert/setup, other -0x7xxx codes) carry different
     * text and still ship. Add to this list as new benign patterns surface. */
    static const char *const benign[] = {
        "-0x0050",                        /* MBEDTLS_ERR_NET_CONN_RESET: peer RST   */
        "select() timeout",               /* httpd reaped an idle socket            */
        "esp_tls_create_server_session",  /* client aborted the handshake           */
        "create_ssl_handle",              /* same, one layer down in esp-tls        */
        "httpd_accept_conn",              /* accept/session churn, not a fault      */
    };
    bool drop = false;
    for (size_t i = 0; i < sizeof(benign) / sizeof(benign[0]); i++) {
        if (strstr(m, benign[i])) { drop = true; break; }
    }

    /* Belt-and-suspenders: still skip our own log lines by tag, in
     * case the task-handle check missed (e.g., log fired from a
     * deferred context). */
    if (!drop && !strstr(m, "glitchtip")) {
        glitchtip_report("error", m, NULL);
    }
    free(buf);
    return rv;
}

void glitchtip_install_log_hook(void) {
    if (!s_orig_vprintf) {
        s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    }
}
