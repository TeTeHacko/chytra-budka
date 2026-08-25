#include "diag.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mqtt.h"

static const char *TAG = "diag";

/* Counter in RTC slow memory survives soft resets but is cleared by
 * power-on / brownout. That's exactly the semantics we want: a boot loop
 * caused by a software fault counts up, while pulling power resets it. */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_boot_fail_count;
/* Set by diag_pre_boot_fail_set() right before app_main calls
 * esp_restart() on a fatal init-time failure (WiFi init, NVS, etc).
 * The following boot's diag_capture_boot() treats this as a crash
 * even though esp_reset_reason() reports ESP_RST_SW — without it the
 * crash counter wouldn't increment, safe-mode trip + GlitchTip
 * dedup would silently miss a field unit stuck in a soft-reset
 * loop. Cleared after consumption in diag_capture_boot(). */
static RTC_NOINIT_ATTR uint32_t s_rtc_pre_boot_fail_flag;
#define PRE_BOOT_FAIL_MAGIC 0x50544641U /* "PFTA": Pre-boot Fail Token Active */

/* Per-boot attempt counter for a pending WiFi credential CANDIDATE.
 * Incremented before wifi_mgr_init when wifi_store has a candidate;
 * cleared on promotion. Survives esp_restart()/panic (so a candidate that
 * crash-loops the radio still counts attempts) but resets on power loss.
 * Guarded by the RTC_MAGIC block below — a newly-added RTC_NOINIT var is
 * NOT auto-zeroed on a boot where the magic was already set by older
 * firmware, so the magic is bumped to v02 to force one clean init across
 * the OTA that introduces this. */
static RTC_NOINIT_ATTR uint32_t s_rtc_wifi_try_count;

/* Consecutive net-watchdog self-reboots with NO successful MQTT session in
 * between. Drives an escalating reboot threshold so a sustained broker outage
 * doesn't turn into a fleet-wide reboot storm that burns solar battery — the
 * FIRST wedged-stack reboot still fires fast, then the interval backs off.
 * Reset on a successful MQTT connect; survives esp_restart, cleared on power
 * loss. Guarded by the magic block (bumped to v03 for this addition). */
static RTC_NOINIT_ATTR uint32_t s_rtc_netwdt_count;

#define RTC_MAGIC 0xCB000B03U /* "ChytraBudka boot v03" (bumped: + netwdt) */

static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static bool s_have_coredump = false;
static size_t s_coredump_size = 0;
static uint32_t s_boot_count = 0;

static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "esp_restart";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "other_wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep_wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "unknown";
    }
}

/* "Crash class" resets are the ones we should count toward the safe-mode
 * trip threshold. A clean esp_restart() or power-on shouldn't bump it. */
static bool is_crash_reset(esp_reset_reason_t r) {
    /* BROWNOUT / PWR_GLITCH are power events, not software faults — but they are
     * COUNTED here ON PURPOSE. Repeated brownouts mean a starving battery, and
     * the consequences of the counter (safe-mode at >=5 → camera+audio off;
     * ramped boot delay) both SHED LOAD, which is exactly the right response to
     * power starvation. The only downside is they show up in the crash dedup as
     * if they were bugs; that's an accepted trade for the load-shed behaviour.
     * (Review F6 suggested excluding them; we keep them for the power benefit.) */
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH ||
           r == ESP_RST_CPU_LOCKUP;
}

void diag_capture_boot(void) {
    s_reset_reason = esp_reset_reason();

    /* Initialise / increment RTC-RAM counter. RTC_NOINIT_ATTR memory holds
     * arbitrary garbage on cold boot, so guard with a magic value. */
    if (s_rtc_magic != RTC_MAGIC) {
        s_rtc_magic = RTC_MAGIC;
        s_rtc_boot_fail_count = 0;
        s_rtc_wifi_try_count = 0;
        s_rtc_netwdt_count = 0;
    }
    if (is_crash_reset(s_reset_reason)) {
        s_rtc_boot_fail_count++;
    } else if (s_rtc_pre_boot_fail_flag == PRE_BOOT_FAIL_MAGIC) {
        /* Soft-reset reached us from a deliberate esp_restart() that
         * the previous boot issued when it couldn't finish init (e.g.
         * wifi_mgr_init() returned ESP_FAIL). Treat as crash-equivalent
         * so the safe-mode trip threshold still fires and the field
         * unit doesn't hide in a 10-min reboot loop. */
        s_rtc_boot_fail_count++;
    }
    s_rtc_pre_boot_fail_flag = 0;  // consume regardless of outcome
    s_boot_count = s_rtc_boot_fail_count;

    /* Check for a persisted core dump from the previous run. Don't erase —
     * leave it for off-device extraction via espcoredump.py until we know
     * we've successfully reported on it. */
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0) {
        s_have_coredump = true;
        s_coredump_size = size;
    }

    ESP_LOGI(TAG, "boot reason=%s coredump=%s consecutive_crashes=%" PRIu32,
             reset_reason_name(s_reset_reason), s_have_coredump ? "present" : "none", s_boot_count);
    if (s_have_coredump) {
        ESP_LOGI(TAG,
                 "coredump partition holds %u bytes — pull with "
                 "`espcoredump.py info_corefile build/chytra-budka.elf`",
                 (unsigned)s_coredump_size);
    }
}

uint32_t diag_consecutive_boot_count(void) {
    return s_boot_count;
}

void diag_publish_boot(void) {
    if (!mqtt_is_connected())
        return;

    const esp_app_desc_t *app = esp_app_get_description();
    char base_topic[80];
    snprintf(base_topic, sizeof(base_topic), "%s/diag/boot", mqtt_topic_base());

    char payload[320];
    int n = snprintf(payload, sizeof(payload),
                     "{\"reset\":\"%s\","
                     "\"coredump\":%s,"
                     "\"coredump_bytes\":%u,"
                     "\"consecutive_crashes\":%" PRIu32
                     ","
                     "\"version\":\"%s\","
                     "\"built\":\"%s %s\","
                     "\"idf\":\"%s\"}",
                     reset_reason_name(s_reset_reason), s_have_coredump ? "true" : "false",
                     (unsigned)s_coredump_size, s_boot_count, app->version, app->date, app->time,
                     app->idf_ver);
    if (n <= 0)
        return;
    /* Publish retained so the broker holds the latest boot context — the
     * HA observer can subscribe at any time and see "last boot was a
     * TWDT reset 4 minutes ago." */
    extern void mqtt_pub_retained(const char *topic, const char *value);
    mqtt_pub_retained(base_topic, payload);
    ESP_LOGI(TAG, "published %s → %s", base_topic, payload);
}

#if CONFIG_CHYTRA_BUDKA_SHIP_COREDUMP
/* Ship the saved coredump partition over MQTT, base64-chunked, so a field unit
 * (no USB) can be symbolized off-device. Runs in its OWN task: spawned at
 * MQTT-connect, it gets the dump out even if the main task is hanging toward a
 * watchdog reboot — the panic we most want to read is exactly that case.
 * Reassemble with tools/coredump_recv.py + decode against the version's
 * archived ELF (reproducible build → bytes match). Gated; OFF once stable. */
static void coredump_ship_task(void *arg) {
    (void)arg;
    extern void mqtt_pub(const char *topic, const char *value);
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    size_t total = s_coredump_size;
    if (!part || total == 0) {
        ESP_LOGW(TAG, "coredump ship: nothing to ship");
        vTaskDelete(NULL);
        return;
    }
    if (total > part->size)
        total = part->size; /* defensive — never read past the partition */

    const char *base = mqtt_topic_base();
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);

    const size_t RAW = 1536; /* → 2048 base64 chars per chunk */
    const size_t chunks = (total + RAW - 1) / RAW;

    char topic[96], meta[288];
    snprintf(meta, sizeof(meta),
             "{\"bytes\":%u,\"chunks\":%u,\"raw_chunk\":%u,\"enc\":\"base64\","
             "\"reset\":\"%s\",\"version\":\"%s\",\"app_sha\":\"%s\"}",
             (unsigned)total, (unsigned)chunks, (unsigned)RAW,
             reset_reason_name(s_reset_reason), app->version, sha);
    snprintf(topic, sizeof(topic), "%s/diag/coredump/meta", base);
    mqtt_pub(topic, meta);
    ESP_LOGW(TAG, "coredump ship: %u bytes, %u chunks → %s/diag/coredump/*",
             (unsigned)total, (unsigned)chunks, base);

    uint8_t *raw = malloc(RAW);
    const size_t b64cap = ((RAW + 2) / 3) * 4 + 4;
    char *b64 = malloc(b64cap);
    if (!raw || !b64) {
        ESP_LOGE(TAG, "coredump ship: OOM (raw=%p b64=%p)", raw, b64);
        free(raw);
        free(b64);
        vTaskDelete(NULL);
        return;
    }
    for (size_t seq = 0, off = 0; off < total; seq++, off += RAW) {
        size_t n = (total - off < RAW) ? (total - off) : RAW;
        if (esp_partition_read(part, off, raw, n) != ESP_OK) {
            ESP_LOGE(TAG, "coredump ship: read fail @%u", (unsigned)off);
            break;
        }
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, b64cap, &olen, raw, n) != 0)
            break;
        b64[olen] = 0;
        snprintf(topic, sizeof(topic), "%s/diag/coredump/%u", base, (unsigned)seq);
        mqtt_pub(topic, b64);
        vTaskDelay(pdMS_TO_TICKS(80)); /* pace the broker + yield the CPU */
    }
    free(raw);
    free(b64);
    ESP_LOGW(TAG, "coredump ship: done");
    vTaskDelete(NULL);
}
#endif /* CONFIG_CHYTRA_BUDKA_SHIP_COREDUMP */

void diag_ship_coredump_mqtt(void) {
#if CONFIG_CHYTRA_BUDKA_SHIP_COREDUMP
    if (!s_have_coredump || !mqtt_is_connected())
        return;
    static bool s_shipped = false;
    if (s_shipped)
        return; /* once per boot — the host only needs it once */
    s_shipped = true;
    xTaskCreate(coredump_ship_task, "cdump_ship", 4096, NULL, 4, NULL);
#endif
}

const char *diag_reset_reason_name(void) {
    return reset_reason_name(s_reset_reason);
}

uint32_t diag_consecutive_crashes(void) {
    return s_boot_count;
}

/* Safe-mode trip threshold: this many consecutive crash-class resets (or
 * pre-boot-fail-flagged esp_restart()s) without an intervening clean run.
 * Set above the "one bad OTA → bootloader rollback on first reset" case
 * (which never reaches here) so safe mode only engages for a genuinely
 * self-sustaining loop on an already-valid image. */
#define DIAG_CRASH_LOOP_THRESHOLD 5

bool diag_in_crash_loop(void) {
    return s_boot_count >= DIAG_CRASH_LOOP_THRESHOLD;
}

size_t diag_coredump_size(void) {
    return s_coredump_size;
}

void diag_log_task_stacks(void) {
    /* Periodic snapshot of stack headroom for the long-lived tasks.
     * uxTaskGetStackHighWaterMark returns the smallest amount of stack
     * the task has ever had free, in words (StackType_t = 4 B on the
     * ESP32-S3). A trend toward zero is a future panic. */
    static const char *const NAMES[] = {
        "main",      /* app_main / our hub loop */
        "audio",     /* audio_task on CPU1 (PDM pump + relay) */
        "cam_wrk",   /* camera_worker on CPU1 (capture drain) */
        "sys_evt",   /* esp_event default loop — wifi + IP handlers */
        "glitchtip", /* gt_task POST loop */
        "ota",       /* ota_task poll loop */
        "wifi",      /* wifi driver task */
        "tiT",       /* lwIP tcpip thread */
        NULL,
    };
    char buf[256];
    int o = 0;
    for (int i = 0; NAMES[i] && o < (int)sizeof(buf) - 32; i++) {
        TaskHandle_t h = xTaskGetHandle(NAMES[i]);
        if (!h)
            continue;
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(h);
        o += snprintf(buf + o, sizeof(buf) - o, "%s=%u%s", NAMES[i],
                      (unsigned)(hwm * sizeof(StackType_t)), NAMES[i + 1] ? " " : "");
    }
    if (o > 0)
        ESP_LOGI(TAG, "stacks free B: %s", buf);
}

void diag_pre_boot_fail_set(void) {
    s_rtc_pre_boot_fail_flag = PRE_BOOT_FAIL_MAGIC;
}

uint32_t diag_wifi_try_inc(void) {
    /* Cold-boot garbage is handled by the RTC_MAGIC guard in
     * diag_capture_boot(), which runs first in app_main. */
    return ++s_rtc_wifi_try_count;
}

uint32_t diag_wifi_try_get(void) {
    return s_rtc_wifi_try_count;
}

void diag_wifi_try_clear(void) {
    s_rtc_wifi_try_count = 0;
}

uint32_t diag_netwdt_count(void) { return s_rtc_netwdt_count; }
void     diag_netwdt_inc(void)   { s_rtc_netwdt_count++; }
void     diag_netwdt_reset(void) { s_rtc_netwdt_count = 0; }

/* Crash-loop detection and OTA mark-valid used to be one call gated on MQTT.
 * That conflated two unrelated invariants and stranded the board: a clean boot
 * that simply couldn't reach the broker (router/ISP outage) never cleared the
 * crash counter, so a long-gone transient crash cause kept re-entering safe
 * mode forever — camera/audio disabled purely on broker reachability. They are
 * now split:
 *   - diag_clear_crash_count(): "did we stay UP long enough?" — pure uptime
 *     signal, NO network gate. The caller invokes it on a fixed runtime
 *     milestone so a broker outage can't pin the board in safe mode.
 *   - diag_mark_ota_valid(): "can we still receive a corrective OTA?" — the
 *     control-plane invariant, correctly gated by the caller on MQTT, since
 *     cancelling rollback should only happen once we've proven the new image
 *     is remotely fixable. */
void diag_clear_crash_count(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (s_rtc_boot_fail_count != 0) {
        ESP_LOGI(TAG,
                 "clean run reached, clearing consecutive crash counter "
                 "(was %" PRIu32 ")",
                 s_rtc_boot_fail_count);
        s_rtc_boot_fail_count = 0;
    }
}

void diag_mark_ota_valid(void) {
    static bool done = false;
    if (done) return;
    /* If we're running a new OTA image that's still in PENDING_VERIFY,
     * mark it valid so the bootloader stops being ready to roll back to
     * the old one. Idempotent — no-op if there's no pending image. Only
     * mark `done` once we've actually confirmed (or there's nothing to
     * confirm), so a failed mark is retried on the next call. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image marked valid (rollback cancelled)");
            done = true;
        } else {
            ESP_LOGW(TAG, "mark_app_valid: %s — will retry", esp_err_to_name(err));
        }
    } else {
        done = true;  /* no pending image — nothing to confirm */
    }
}

/* Lazy-init MCU die temperature sensor (ESP32-S3 has one internal
 * temp sensor that doesn't need calibration). Returns NaN if the
 * sensor failed to install or read — caller's responsibility to
 * suppress the row. */
float diag_mcu_temp_c(void) {
    static temperature_sensor_handle_t tsens = NULL;
    if (!tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &tsens) != ESP_OK) {
            tsens = NULL;
            return NAN;
        }
        if (temperature_sensor_enable(tsens) != ESP_OK) {
            temperature_sensor_uninstall(tsens);
            tsens = NULL;
            return NAN;
        }
    }
    float t = NAN;
    if (temperature_sensor_get_celsius(tsens, &t) != ESP_OK)
        return NAN;
    return t;
}
