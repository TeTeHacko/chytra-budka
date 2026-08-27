// ota.c — periodic esp_https_ota poller with Basic Auth and MQTT status.

#include "ota.h"
#include "app_config.h"
#include "audiofx.h"
#include "battery.h"
#include "config.h"
#include "net_store.h"
#include "mqtt.h"
#include "secret_helpers.h"
#include "secrets.h"
#include "status_led.h"
#include "wifi_mgr.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "ota";

/* Wall-clock cap on a single OTA download. esp_http_client_config_t's
 * timeout_ms covers per-recv socket inactivity (30 s default), but a
 * pathological server that dribbles 1 byte every 25 s keeps the recv
 * timer reset and the download alive forever — TWDT doesn't fire
 * because the outer vTaskDelay(50) yields, so the symptom is
 * "downloading" stuck on the dashboard with nothing in the log.
 * Capping the whole transaction gives us a clean abort + retry on the
 * next poll cycle. 15 minutes is generous for the 1.3 MB image even
 * over a -85 dBm link; healthy downloads finish in <2 min. */
#define OTA_WALLCLOCK_TIMEOUT_US (15LL * 60 * 1000 * 1000)

/* Cadence for the in-progress log line. Mirrors the cadence the
 * dashboard would want — once per 5 s gives ~24 lines for a 2-min
 * download, granular enough to spot a stall, not so chatty that the
 * log floods. */
#define OTA_PROGRESS_LOG_INTERVAL_US (5LL * 1000 * 1000)

/* Set by ota_trigger_now() and consumed by the OTA task. The task
 * polls this between sleeps and runs ota_check_once() immediately
 * when set, then clears it. atomic = avoids a mutex for one flag. */
static atomic_bool s_trigger_now = false;

/* Re-entrancy guard: ota_check_once() runs from the OTA task (periodic +
 * trigger) AND synchronously from cb_ds (hibernate on-wake check). They never
 * normally overlap (the task sleeps for minutes), but this makes a concurrent
 * call a clean no-op instead of two HTTPS sessions fighting over s_ota_pct. */
static atomic_bool s_check_running = false;

/* OTA progress for the bench OLED overlay: -1 = no OTA in flight, else 0..100.
 * Single writer (the OTA task), the OLED task only reads, so a plain int is
 * fine — a torn read at worst shows a stale percent for one 250 ms frame.
 * Set/cleared via ota_set_active() so it stays paired with the status LED on
 * every exit path; updated each download tick in ota_check_once(). */
static int s_ota_pct = -1;

int ota_progress_pct(void) { return s_ota_pct; }

/* Bracket the OTA window: drives the status LED AND the OLED progress state
 * together (on → 0 %, off → cleared). Replaces the bare status_led_ota_active
 * calls so no exit path can leave a stale bar on the panel. */
static void ota_set_active(bool on) {
    status_led_ota_active(on);
    s_ota_pct = on ? 0 : -1;
}

/* Forward decl matches the helper exported by mqtt.c (same pattern used
 * by app_config.c — kept local rather than adding to mqtt.h to keep the
 * header surface tight). Retained so HA sees the last OTA outcome on
 * subscribe instead of waiting for the next poll. */
extern void mqtt_pub_retained(const char *topic, const char *value);

// MQTT topic for OTA status: "checking", "up-to-date", "downloading", "done", "error"
static void publish_ota_status(const char *status) {
    ESP_LOGI(TAG, "status: %s", status);
    if (!mqtt_is_connected()) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/state/ota", mqtt_topic_base());
    mqtt_pub_retained(topic, status);
}

/* Parse the build date + time fields of esp_app_desc_t into a sortable
 * time_t. Returns 0 on parse failure — caller treats 0 as "unknown" and
 * fails OPEN (does not enforce downgrade block) so a missing/garbled
 * field can't get a board permanently stuck on a broken build. */
static time_t parse_build_time(const char *date, const char *time_str) {
    if (!date || !time_str || !date[0] || !time_str[0])
        return 0;
    /* IDF format: date "MMM DD YYYY" (compiler's __DATE__), time
     * "HH:MM:SS" (__TIME__). Note the day field is space-padded to 2
     * chars when single digit ("May  5 2026") — strptime's %e handles
     * that; %d would not. */
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "%s %s", date, time_str);
    if (n <= 0 || n >= (int)sizeof(buf))
        return 0;
    struct tm tm = {0};
    if (strptime(buf, "%b %e %Y %H:%M:%S", &tm) == NULL)
        return 0;
    return mktime(&tm);
}

static bool ota_is_newer_or_equal(const esp_app_desc_t *new_desc,
                                  const esp_app_desc_t *cur) {
    time_t cur_t = parse_build_time(cur->date, cur->time);
    time_t new_t = parse_build_time(new_desc->date, new_desc->time);
    if (cur_t == 0 || new_t == 0) {
        /* Unparseable on either side — fail open (allow update). Better
         * to permit an unverifiable OTA than to wedge the device on a
         * known-bad build when we can't tell which is newer.
         *
         * IMPORTANT: CONFIG_APP_REPRODUCIBLE_BUILD BLANKS __DATE__/__TIME__, so
         * on our reproducible builds BOTH dates are empty → this branch ALWAYS
         * takes → the downgrade guard is effectively OFF. That's load-bearing
         * for tools/ota_rollback.sh (re-serving an older signed bin must be
         * accepted) but it also means an *accidental* stale upload installs
         * fleet-wide. The real anti-rollback would be eFuse SECURE_VERSION
         * (deliberately NOT burned — it is one-way and unrecoverable),
         * so today downgrade-to-any-older-SIGNED image is possible by design.
         * A monotonic build counter in esp_app_desc would let this fail closed
         * without eFuses — revisit if shipping a security fix. */
        return true;
    }
    return new_t >= cur_t;
}

static void ota_check_once(void) {
    if (!wifi_mgr_is_connected()) {
        return;
    }

    /* Runtime gate (NVS): bench boards flip ota_enabled=OFF so they
     * stop pulling whatever (possibly stale) build sits on the OTA
     * server — observed downgrading a dirty bench build to a months-old
     * tag and crash-looping it. */
    if (!app_config_get_bool("ota_enabled")) {
        ESP_LOGI(TAG, "skipped (ota_enabled=OFF)");
        /* Publish the live state so a stale retained "error" from a
         * past failed pull doesn't sit on the broker forever. Without
         * this, a board with ota_enabled flipped OFF post-failure
         * shows `state/ota=error` in HA indefinitely (the early-return
         * skips every status update path further down). */
        publish_ota_status("disabled");
        return;
    }

    /* Don't pull a new image while the CURRENT one is still pending-verify
     * (not yet mark-valid'd). Two reasons:
     *   1. Safety: chaining an OTA on top of an unconfirmed image throws away
     *      the rollback safety-net — if the current boot turns out bad we want
     *      the bootloader to roll back to the known-good slot, not have it
     *      overwritten by yet another untested image.
     *   2. It closes a HIL race: a freshly USB-flashed bench image boots
     *      pending-verify and is mark-valid'd only after 180 s + MQTT (see
     *      main.cpp). The first poll used to fire at a fixed 120 s, which on a
     *      slow provision beat the test harness's ota_enabled=OFF pin and let
     *      the bench self-downgrade to a stale server image. Gating on
     *      mark-valid means OFF (set right after MQTT-online) always lands
     *      first. A long-running field image is already VALID, so steady-state
     *      OTA is unaffected. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "skipped (running image pending-verify — waiting for mark-valid)");
        publish_ota_status("pending-verify");
        return;
    }

    /* Battery safety gate: an OTA flash that browns out mid-write can brick or
     * roll back the image. Skip OTA when running on battery below ota_min_soc;
     * BYPASSED on external/USB power (battery_on_external_power(), which also
     * reads true when no fuel gauge is present — the mains/USB field board). 0
     * disables the gate. */
    int min_soc = app_config_get_int("ota_min_soc");
    if (min_soc > 0 && !battery_on_external_power()) {
        float soc = battery_soc();
        if (soc >= 0.0f && soc < (float)min_soc) {
            ESP_LOGW(TAG, "skipped (battery %.0f%% < ota_min_soc %d%%, on battery)",
                     (double)soc, min_soc);
            publish_ota_status("low-battery");
            return;
        }
    }

    // Skip OTA if password is placeholder (not configured yet). Publish
    // the skip reason to state/ota so an operator looking at HA doesn't
    // see "empty" forever and wonder why OTA isn't running.
    if (secret_is_placeholder(OTA_PASSWORD)) {
        ESP_LOGW(TAG, "skipped (placeholder OTA_PASSWORD in secrets.h)");
        publish_ota_status("disabled-no-creds");
        return;
    }

    /* Runtime OTA URL (net_store good record; compile OTA_URL floor). Read
     * through on every check — a bad value here is always correctable over
     * MQTT, which is why ota_url needs no candidate ladder. Static: the
     * http client config keeps the pointer for the transfer's lifetime. */
    static net_cfg_t s_net;
    net_store_get_effective(&s_net, NULL);

    ESP_LOGI(TAG, "checking %s", s_net.ota_url);
    ota_set_active(true);
    publish_ota_status("checking");

    esp_http_client_config_t http_cfg = {
        .url = s_net.ota_url,
        .username = "chytra-budka",
        .password = OTA_PASSWORD,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t begin_err = esp_https_ota_begin(&ota_cfg, &h);
    if (begin_err != ESP_OK) {
        ESP_LOGW(TAG, "begin failed: %s", esp_err_to_name(begin_err));
        publish_ota_status("error");
        ota_set_active(false);
        return;
    }

    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(h, &new_desc) == ESP_OK) {
        const esp_app_desc_t *cur = esp_app_get_description();
        if (cur && strncmp(new_desc.version, cur->version, sizeof(new_desc.version)) == 0 &&
            memcmp(new_desc.app_elf_sha256, cur->app_elf_sha256, sizeof(new_desc.app_elf_sha256)) ==
                0) {
            ESP_LOGI(TAG, "no update (running %s)", cur->version);
            esp_https_ota_abort(h);
            publish_ota_status("up-to-date");
            ota_set_active(false);
            return;
        }
        /* Downgrade guard: the version+sha equality check above only
         * catches "exactly the same image". Without it, an older image
         * with a different version string (e.g. an accidental rsync of
         * yesterday's bin) would silently install. esp_app_desc_t has
         * build date+time fields populated at link time; parse them
         * into time_t and reject an OTA whose build time is strictly
         * earlier than what we're running. Eq-or-newer is allowed (two
         * builds at the literal same second is a non-event; equal sha
         * was already filtered above so equal-time means rebuild). */
        if (cur && !ota_is_newer_or_equal(&new_desc, cur)) {
            ESP_LOGW(TAG,
                     "rejecting downgrade: running %s (%s %s) vs server %s (%s %s)",
                     cur->version, cur->date, cur->time,
                     new_desc.version, new_desc.date, new_desc.time);
            esp_https_ota_abort(h);
            publish_ota_status("downgrade-blocked");
            ota_set_active(false);
            return;
        }
        ESP_LOGW(TAG, "update available: %s -> %s", cur ? cur->version : "?", new_desc.version);
    }

    publish_ota_status("downloading");
    audiofx_ota_start();

    /* Each perform() iteration pulls one HTTPS chunk. If the server
     * goes quiet between chunks, the TLS layer eventually times out
     * (http_cfg.timeout_ms = 30 s above) and we exit the loop. The
     * vTaskDelay yields each tick so we don't busy-loop, and resets
     * the TWDT we subscribed to in ota_task so a making-progress
     * download doesn't trigger a panic even on slow links. The
     * wall-clock cap (see OTA_WALLCLOCK_TIMEOUT_US) catches the
     * 1-byte-every-25-s dribble case that no per-recv timeout would
     * notice. */
    int64_t ota_start_us  = esp_timer_get_time();
    int64_t last_log_us   = ota_start_us;
    int     last_bytes    = 0;
    int     total_bytes   = esp_https_ota_get_image_size(h);
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Update the OLED progress percent every tick (cheap: just reads a
         * counter off the handle). The OLED overlay polls s_ota_pct at 250 ms,
         * so a per-50 ms update keeps the bar smooth without extra bus work. */
        if (total_bytes > 0) {
            int pc = (int)((int64_t)esp_https_ota_get_image_len_read(h) * 100 / total_bytes);
            s_ota_pct = pc > 100 ? 100 : pc;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - ota_start_us > OTA_WALLCLOCK_TIMEOUT_US) {
            ESP_LOGE(TAG, "wall-clock timeout (%lld s) — aborting",
                     (long long)((now_us - ota_start_us) / 1000000));
            esp_https_ota_abort(h);
            publish_ota_status("error");
            audiofx_ota_fail();
            ota_set_active(false);
            return;
        }
        if (now_us - last_log_us > OTA_PROGRESS_LOG_INTERVAL_US) {
            int got = esp_https_ota_get_image_len_read(h);
            int dt_ms = (int)((now_us - last_log_us) / 1000);
            int rate_kbps = dt_ms > 0 ? ((got - last_bytes) * 8 / dt_ms) : 0;
            if (total_bytes > 0) {
                ESP_LOGI(TAG, "downloading %d / %d B (%d%%, %d kbps)",
                         got, total_bytes, (got * 100) / total_bytes, rate_kbps);
            } else {
                ESP_LOGI(TAG, "downloading %d B (%d kbps, total unknown)",
                         got, rate_kbps);
            }
            last_log_us = now_us;
            last_bytes  = got;
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        if (esp_https_ota_finish(h) == ESP_OK) {
            ESP_LOGW(TAG, "update applied, restarting");
            publish_ota_status("done");
            audiofx_ota_done();
            vTaskDelay(pdMS_TO_TICKS(1500));   /* let the success fanfare finish */
            esp_restart();
        } else {
            ESP_LOGE(TAG, "finish failed (image invalid?)");
            publish_ota_status("error");
            audiofx_ota_fail();
            ota_set_active(false);
        }
    } else {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        publish_ota_status("error");
        audiofx_ota_fail();
        ota_set_active(false);
    }
}

static void ota_task(void *arg) {
    (void)arg;
    /* Initial 2 min grace period after boot, then poll every
     * OTA_CHECK_PERIOD_MS. Sleep is sliced into 1-second ticks so
     * ota_trigger_now() can wake us up mid-interval. */
    vTaskDelay(pdMS_TO_TICKS(2 * 60 * 1000));

    /* Subscribe to TWDT *after* the grace period so we don't trip
     * during boot. Once subscribed, any code path in this task that
     * stops yielding for > CONFIG_ESP_TASK_WDT_TIMEOUT_S seconds will
     * panic + coredump + reboot. esp_https_ota_perform's inner loop
     * resets the WDT each tick (see ota_check_once), and the sleep
     * loop below resets it every second. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "task_wdt_add failed — ota task not WDT-protected");
    } else {
        ESP_LOGI(TAG, "ota task subscribed to TWDT (timeout %d s)",
                 CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    }

    while (1) {
        if (!atomic_exchange(&s_check_running, true)) {
            ota_check_once();
            atomic_store(&s_check_running, false);
        }
        for (uint32_t slept = 0; slept < OTA_CHECK_PERIOD_MS; slept += 1000) {
            if (atomic_exchange(&s_trigger_now, false)) break;
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void ota_init(void) {
    xTaskCreate(ota_task, "ota", 12288, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void ota_trigger_now(void) {
    atomic_store(&s_trigger_now, true);
}

bool ota_img_pending_verify(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_check_now_blocking(void) {
    /* Synchronous on-wake check for hibernate (cb_ds), run on the CALLING task
     * (the supervisor / app_main). That task is TWDT-subscribed with a 30 s
     * timeout, but esp_https_ota_begin()'s TLS handshake is a single blocking
     * call that resets no watchdog and can exceed 30 s on a weak link — which
     * tripped the task WDT and rebooted the unit mid-hibernate (the OTA check
     * runs BEFORE esp_deep_sleep_start(), so it looped). The download loop in
     * ota_check_once() does feed the WDT, but the begin/handshake phase can't.
     * So suspend THIS task's TWDT for the duration; the OTA transaction is
     * already bounded by the HTTP recv timeout (30 s) + the wall-clock cap
     * (15 min). Re-arm after (unless an applied update rebooted us). The
     * background ota_task keeps its own TWDT subscription untouched. */
    if (atomic_exchange(&s_check_running, true)) {
        ESP_LOGW(TAG, "blocking check skipped — a check is already running");
        return;
    }
    bool was_subscribed = (esp_task_wdt_status(NULL) == ESP_OK);
    if (was_subscribed) esp_task_wdt_delete(NULL);
    ota_check_once();
    if (was_subscribed) esp_task_wdt_add(NULL);
    atomic_store(&s_check_running, false);
}
