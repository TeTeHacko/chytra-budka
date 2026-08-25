/* photo_queue.c — see photo_queue.h. */

#include "photo_queue.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt.h"

static const char *TAG = "photoq";

/* Sizing. 4 × 400 KB (+1 drain shadow) = 2.0 MB PSRAM. Header per
 * entry is ~96 B which is negligible.
 *
 * The old 200 KB cap was sized from "UXGA q=12 ~100 KB" — measured
 * wrong on 2026-07-10: a lit indoor UXGA q=12 shot came out at 342 KB
 * and was silently dropped from MQTT ("too big for queue — SD only"),
 * so HA showed stale photos whenever the scene wasn't dark. 400 KB
 * covers the driver's own JPEG framebuffer bound (UXGA w·h/5 ≈ 384 KB
 * in esp32-camera jpeg_mode) — a frame that fits the driver fb fits
 * the slot, so the drop path is now unreachable for real captures.
 * Depth 4 (was 8) keeps the PSRAM budget flat; captures are seconds
 * apart, so the extra depth only ever mattered during long MQTT
 * outages where drop-oldest is the designed behavior anyway.
 * MUST match MQTT_OUT_BUFFER_SIZE in mqtt.c — see the comment there. */
#define RING_N          4
#define ENTRY_CAP_BYTES (400u * 1024u)

#define TRIGGER_MAX     24
#define PATH_MAX_Q      128

typedef struct {
    uint8_t  *jpg;          /* PSRAM-allocated; sized ENTRY_CAP_BYTES */
    size_t    jpg_len;      /* 0 == slot empty */
    char      trigger[TRIGGER_MAX];
    char      path[PATH_MAX_Q];  /* SD path → /photo?f= URL in the event (or "") */
    int64_t   ts_us;
    uint32_t  seq;
    int       agc_gain;
    bool      ir_active;
    int       framesize;
    int       quality;
} photo_entry_t;

static photo_entry_t  s_ring[RING_N];
static size_t         s_head = 0;       /* next slot to write */
static size_t         s_tail = 0;       /* next slot to read */
/* Atomic so photo_queue_depth() readers don't tear with mutex-held
 * increment/decrement on a 32-bit architecture. Writes still occur
 * under s_mtx (the surrounding ring index invariants need it) but
 * the explicit atomic type spells out the cross-thread read contract
 * instead of leaving it implicit. */
static atomic_size_t  s_count = 0;

/* Drain shadow buffer — the JPEG that drain_task is currently publishing
 * is memcpy'd here under the mutex before the publish runs unlocked. The
 * pointer in s_ring[idx].jpg is therefore free to be re-used by enqueue
 * the moment the mutex is released, which closes the race the previous
 * pointer-only-snap design had: esp_mqtt_client_publish for any frame
 * larger than connection->buffer_length re-reads from the source pointer
 * mid-send (mqtt_client.c send-loop's `current_data` walk), so a
 * concurrent enqueue overwrite of the slot would have leaked half-new
 * bytes into the MQTT stream. Today our 160 KB out_size > max JPEG hides
 * this in practice, but the shadow buffer makes correctness explicit
 * regardless of buffer-size config drift. */
static uint8_t       *s_drain_shadow = NULL;
static atomic_uint_fast32_t s_dropped_total = 0;
static atomic_uint_fast32_t s_drained_total = 0;
static atomic_size_t  s_bytes_total = 0;

static SemaphoreHandle_t s_mtx = NULL;
static EventGroupHandle_t s_evt = NULL;
#define EVT_KICK_BIT (1u << 0)

static bool s_inited = false;

/* ── ring helpers (must be called with s_mtx held) ────────────────────── */

static void slot_clear(size_t idx) {
    photo_entry_t *e = &s_ring[idx];
    if (e->jpg_len) {
        atomic_fetch_sub(&s_bytes_total, e->jpg_len);
        e->jpg_len = 0;
    }
}

static photo_entry_t *slot_write_init(size_t idx) {
    photo_entry_t *e = &s_ring[idx];
    /* jpg buffer was allocated once at init; reuse. */
    e->jpg_len = 0;
    e->trigger[0] = 0;
    return e;
}

/* ── drain task ───────────────────────────────────────────────────────── */

/* FreeRTOS task that flushes the queue when MQTT is connected. Blocks
 * on EVT_KICK_BIT (set by photo_queue_kick() — called from mqtt.c on
 * MQTT_EVENT_CONNECTED). On wake, walks tail→head, publishing each
 * entry; stops if MQTT drops mid-drain (next reconnect kicks again).
 *
 * Concurrency: enqueue + drain both touch the ring under s_mtx, but
 * the per-entry JPEG bytes pointer is stable for the lifetime of the
 * module (allocated once at init, never freed), so the actual publish
 * runs WITHOUT holding the mutex — that lets a concurrent capture
 * enqueue while we're publishing an older frame. We only retake the
 * mutex briefly to mark the slot empty.
 *
 * Delivery acknowledgement is best-effort: esp_mqtt_client_publish
 * with QoS 1 retries internally if the broker doesn't ACK, but there
 * is no callback we can hook to tell when an individual publish
 * actually landed. We treat "still connected after both publishes
 * returned" as "delivered" — wrong only if the broker disconnects in
 * the millisecond gap, in which case the next reconnect's kick will
 * re-attempt the same frame (best case duplicate retained image, no
 * data loss). */
static void drain_task(void *arg) {
    (void)arg;
    /* Subscribe to TWDT — a wedged broker socket can leave the inner
     * publish loop stuck inside esp_mqtt_client_publish for the MQTT
     * task's network timeout (multi-second) and there's nobody else
     * petting for us. The outer wait uses a bounded timeout (well under
     * the 30 s TWDT window) so the idle path also keeps the watchdog
     * happy without needing an external kick. */
    esp_task_wdt_add(NULL);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_evt, EVT_KICK_BIT,
            /*clear*/ pdTRUE, /*wait_all*/ pdFALSE,
            pdMS_TO_TICKS(10000));
        (void)esp_task_wdt_reset();
        if (!(bits & EVT_KICK_BIT)) continue;  /* heartbeat tick, nothing to do */
        for (;;) {
            if (!mqtt_is_connected()) break;

            photo_entry_t snap;
            bool have = false;
            if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
                if (atomic_load(&s_count) > 0) {
                    /* Copy metadata struct (cheap), then memcpy the
                     * JPEG bytes into the drain shadow so the source
                     * slot is free to be enqueue-overwritten the moment
                     * we release the mutex. See the s_drain_shadow
                     * declaration for the race rationale. */
                    snap = s_ring[s_tail];
                    memcpy(s_drain_shadow, s_ring[s_tail].jpg, snap.jpg_len);
                    snap.jpg = s_drain_shadow;
                    have = true;
                }
                xSemaphoreGive(s_mtx);
            }
            if (!have) break;

            mqtt_publish_photo_event(snap.jpg, snap.jpg_len,
                                     snap.trigger,
                                     snap.path[0] ? snap.path : NULL,
                                     snap.agc_gain, snap.ir_active,
                                     snap.framesize, snap.quality);
            mqtt_publish_photo_image(snap.jpg, snap.jpg_len);
            if (!mqtt_is_connected()) break;

            if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
                slot_clear(s_tail);
                s_tail = (s_tail + 1) % RING_N;
                if (atomic_load(&s_count) > 0) atomic_fetch_sub(&s_count, 1);
                atomic_fetch_add(&s_drained_total, 1);
                xSemaphoreGive(s_mtx);
            }
            /* Yield so a flood doesn't monopolise the MQTT outbox —
             * each redelivered shot is ~100-200 KB. */
            vTaskDelay(pdMS_TO_TICKS(50));
            (void)esp_task_wdt_reset();
        }
    }
}

/* ── public API ───────────────────────────────────────────────────────── */

esp_err_t photo_queue_init(void) {
    if (s_inited) return ESP_OK;

    s_mtx = xSemaphoreCreateMutex();
    s_evt = xEventGroupCreate();
    if (!s_mtx || !s_evt) return ESP_ERR_NO_MEM;

    /* Pre-allocate ENTRY_CAP_BYTES per slot — keeps the hot path of
     * enqueue/drain free of malloc/free, and lets us fail FAST at boot
     * if PSRAM is too tight, rather than at the moment of an outage. */
    for (int i = 0; i < RING_N; i++) {
        s_ring[i].jpg = heap_caps_malloc(ENTRY_CAP_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_ring[i].jpg) {
            ESP_LOGE(TAG, "PSRAM alloc failed at slot %d (need %u B × %d)",
                     i, (unsigned)ENTRY_CAP_BYTES, RING_N);
            for (int j = 0; j < i; j++) {
                free(s_ring[j].jpg);
                s_ring[j].jpg = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_ring[i].jpg_len = 0;
    }

    /* One extra slot-sized buffer that drain_task copies the live JPEG
     * into under the mutex before the publish runs unlocked. Closes the
     * pointer-aliasing race window. */
    s_drain_shadow = heap_caps_malloc(ENTRY_CAP_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_drain_shadow) {
        ESP_LOGE(TAG, "PSRAM alloc failed for drain shadow (need %u B)",
                 (unsigned)ENTRY_CAP_BYTES);
        for (int i = 0; i < RING_N; i++) {
            free(s_ring[i].jpg);
            s_ring[i].jpg = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    /* Drain task: blocks on EVT_KICK_BIT, drains queue while MQTT is
     * connected. Priority 3 keeps it out of the way of the audio +
     * camera tasks but above idle telemetry chatter. */
    /* 6144 (matches cam_wrk): drain_task runs the same mqtt_publish_photo_event
     * (~1 KB of locals — cap[]/ev[]) + mqtt_publish_photo_image, whose esp-mqtt
     * transport write runs inline on this task. 4 KB left little headroom over
     * the TLS write frame. */
    BaseType_t ok = xTaskCreate(drain_task, "photo_queue", 6144, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "drain task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "queue ready (%d slots + 1 drain shadow × %u KB = %u KB PSRAM)",
             RING_N, (unsigned)(ENTRY_CAP_BYTES / 1024),
             (unsigned)((ENTRY_CAP_BYTES * (RING_N + 1)) / 1024));
    return ESP_OK;
}

esp_err_t photo_queue_enqueue(const uint8_t *jpg, size_t jpg_len,
                              const char *trigger, const char *path,
                              int64_t ts_us, uint32_t seq,
                              int agc_gain, bool ir_active,
                              int framesize, int quality) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!jpg || jpg_len == 0) return ESP_ERR_INVALID_ARG;
    if (jpg_len > ENTRY_CAP_BYTES) return ESP_ERR_INVALID_SIZE;

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* If full, drop OLDEST. Then write into freshly-vacated tail slot's
     * old position by advancing tail (since head will overwrite tail). */
    if (atomic_load(&s_count) >= RING_N) {
        slot_clear(s_tail);
        s_tail = (s_tail + 1) % RING_N;
        atomic_fetch_sub(&s_count, 1);
        atomic_fetch_add(&s_dropped_total, 1);
    }

    photo_entry_t *e = slot_write_init(s_head);
    memcpy(e->jpg, jpg, jpg_len);
    e->jpg_len = jpg_len;
    if (trigger) {
        strncpy(e->trigger, trigger, TRIGGER_MAX - 1);
        e->trigger[TRIGGER_MAX - 1] = 0;
    } else {
        e->trigger[0] = 0;
    }
    if (path) {
        strncpy(e->path, path, PATH_MAX_Q - 1);
        e->path[PATH_MAX_Q - 1] = 0;
    } else {
        e->path[0] = 0;
    }
    e->ts_us = ts_us;
    e->seq = seq;
    e->agc_gain = agc_gain;
    e->ir_active = ir_active;
    e->framesize = framesize;
    e->quality = quality;

    s_head = (s_head + 1) % RING_N;
    atomic_fetch_add(&s_count, 1);
    atomic_fetch_add(&s_bytes_total, jpg_len);

    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "enqueued seq=%" PRIu32 " (%u B), depth=%u",
             seq, (unsigned)jpg_len, (unsigned)photo_queue_depth());
    return ESP_OK;
}

void photo_queue_kick(void) {
    if (!s_inited) return;
    xEventGroupSetBits(s_evt, EVT_KICK_BIT);
}

size_t photo_queue_depth(void) {
    if (!s_inited) return 0;
    return atomic_load(&s_count);
}

size_t photo_queue_bytes(void) {
    return atomic_load(&s_bytes_total);
}

uint32_t photo_queue_dropped_total(void) {
    return atomic_load(&s_dropped_total);
}

uint32_t photo_queue_drained_total(void) {
    return atomic_load(&s_drained_total);
}
