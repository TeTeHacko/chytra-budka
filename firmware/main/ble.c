/* ble.c — optional BLE meter subsystem. See ble.h / firmware/BLE.md.
 *
 * Active low-duty scan brings up NimBLE (WiFi keeps RF priority via coex),
 * finds Atorch UC96 USB power meters by the 0xffe0 service UUID (in the ADV)
 * or the "UC96_BLE" name (in the scan-response — hence ACTIVE scan), connects
 * to each, negotiates a big-enough MTU (the report frame is 36 B > the 23 B
 * default), subscribes to 0xffe1 notifications, decodes them with
 * ble_parse_uc96() and publishes V/I/P/Wh per meter over MQTT.
 *
 * Multi-meter: the field rig is panel→UC96→powerbank→UC96→Xiao, so we hold up
 * to CONFIG_BT_NIMBLE_MAX_CONNECTIONS connections at once, each keyed by its
 * MAC (HA friendly-names downstream, as elsewhere in this project). NimBLE +
 * the BT controller live ONLY in internal DRAM, so the bench overlay carries a
 * memory diet (sdkconfig.defaults.bench) — without it the controller starves
 * i2s/camera DMA and the box task_wdt crash-loops. */

#include "ble.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ble";

#if CONFIG_CHYTRA_BUDKA_BLE

#include "app_config.h"
#include "ble_parse.h"
#include "ble_store.h"
#include "mqtt.h"
#include "esp_coexist.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

#define UC96_SVC_UUID16    0xffe0   /* Atorch UC96 GATT service (also in its ADV) */
#define UC96_NOTIFY_UUID16 0xffe1   /* report-frame notify characteristic */
#define CCCD_UUID16        0x2902   /* client characteristic config descriptor */
#define BTHOME_SVC_UUID16  0xfcd2   /* BTHome v2 service-data UUID (passive) */
#define MAX_METERS         CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define MAX_DISCOVERED     16        /* in-range devices surfaced to the web UI */

static const ble_uuid16_t UC96_SVC_UUID = BLE_UUID16_INIT(UC96_SVC_UUID16);
static const ble_uuid16_t UC96_CHR_UUID = BLE_UUID16_INIT(UC96_NOTIFY_UUID16);

/* Per-meter connection state. One slot per simultaneous meter; keyed by MAC. */
typedef enum { M_FREE = 0, M_CONNECTING, M_CONNECTED, M_STREAMING } mstate_t;
typedef struct {
    mstate_t   state;
    ble_addr_t addr;
    uint16_t   conn_handle;
    uint16_t   svc_start, svc_end;
    uint16_t   val_handle;     /* 0xffe1 value handle */
    uint16_t   cccd_handle;    /* its 0x2902 descriptor */
    int64_t    last_rx_us;
    uint32_t   frames;
    char       id[13];         /* MAC hex, no colons — MQTT/HA key */
    bool       has_reading;    /* last holds a valid UC96 frame */
    ble_uc96_reading_t last;   /* last decoded V/I/P/Wh/temp (for the web UI) */
} meter_t;

/* In-range devices surfaced by the scan for the web UI (whether or not we
 * connect to them). Updated/deduped by MAC on each advert. */
typedef struct {
    bool       used;
    ble_addr_t addr;
    char       id[13];
    char       name[32];       /* advertised name (scan-response), "" if none */
    int8_t     rssi;
    uint8_t    kind;           /* ble_dev_kind_t */
    int64_t    last_us;
    bool       has_bthome;
    ble_bthome_reading_t bthome;
    bool       bthome_announced;  /* HA discovery sent once */
    int64_t    bthome_pub_us;     /* throttle MQTT publishes */
} disc_t;

static bool     s_running = false;
static uint8_t  s_own_addr_type;
static meter_t  s_meters[MAX_METERS];
static meter_t *s_connecting_meter = NULL;  /* the one connect in flight, if any */
static bool     s_connecting = false;       /* GAP allows one initiator at a time */
static int64_t  s_uc96_last_us = 0;         /* last UC96 advert seen, 0=never */
static disc_t   s_disc[MAX_DISCOVERED];     /* in-range devices for the web UI */
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
static int64_t  s_dbg_log_us   = 0;         /* throttle the bench raw-adv survey log */
static uint32_t s_dbg_adv_count = 0;        /* total adverts the radio has received */
#endif

static void start_scan(void);
static void start_scan_if_room(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

/* Match the UC96 by advertised name (same pattern as power-meter/uc96d.py).
 * The name lives in the SCAN RESPONSE, so this only fires under an active scan. */
static bool name_is_uc96(const char *name) {
    return name && name[0] && strstr(name, "UC96") != NULL;
}

/* Match by the 0xffe0 service UUID — the UC96 carries this in its ADV payload,
 * so it works even when the name (scan-response only) hasn't arrived yet. */
static bool adv_has_uc96_svc(const struct ble_hs_adv_fields *f) {
    for (int i = 0; i < f->num_uuids16; i++)
        if (ble_uuid_u16(&f->uuids16[i].u) == UC96_SVC_UUID16)
            return true;
    return false;
}

/* ── meter table helpers ───────────────────────────────────────────── */

static void meter_id_str(const ble_addr_t *a, char out[13]) {
    /* addr.val is little-endian; print MSB-first to match the displayed MAC. */
    const uint8_t *v = a->val;
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", v[5], v[4], v[3], v[2], v[1], v[0]);
}

static meter_t *meter_by_addr(const ble_addr_t *a) {
    for (int i = 0; i < MAX_METERS; i++)
        if (s_meters[i].state != M_FREE && ble_addr_cmp(&s_meters[i].addr, a) == 0)
            return &s_meters[i];
    return NULL;
}

static meter_t *meter_by_conn(uint16_t h) {
    for (int i = 0; i < MAX_METERS; i++)
        if (s_meters[i].state >= M_CONNECTED && s_meters[i].conn_handle == h)
            return &s_meters[i];
    return NULL;
}

static int connected_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_METERS; i++)
        if (s_meters[i].state == M_STREAMING) n++;
    return n;
}

static void meter_free(meter_t *m) {
    if (m) memset(m, 0, sizeof(*m));  /* state → M_FREE */
}

/* ── discovered-device buffer (web UI scan) ────────────────────────── */

static disc_t *disc_find(const ble_addr_t *a) {
    for (int i = 0; i < MAX_DISCOVERED; i++)
        if (s_disc[i].used && ble_addr_cmp(&s_disc[i].addr, a) == 0) return &s_disc[i];
    return NULL;
}

/* Find-or-allocate a slot for addr; evict the stalest entry if full. */
static disc_t *disc_slot(const ble_addr_t *a) {
    disc_t *d = disc_find(a);
    if (d) return d;
    disc_t *victim = &s_disc[0];
    for (int i = 0; i < MAX_DISCOVERED; i++) {
        if (!s_disc[i].used) { victim = &s_disc[i]; break; }
        if (s_disc[i].last_us < victim->last_us) victim = &s_disc[i];
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    victim->addr = *a;
    meter_id_str(a, victim->id);
    return victim;
}

/* ── GATT discovery chain: connect → MTU → 0xffe0 → 0xffe1 → CCCD → subscribe ── */

static int on_subscribe(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg) {
    (void)conn; (void)attr;
    meter_t *m = &s_meters[(intptr_t)arg];
    if (err->status != 0) {
        ESP_LOGW(TAG, "%s CCCD subscribe failed (status=%d)", m->id, err->status);
        return 0;
    }
    m->state = M_STREAMING;
    ESP_LOGI(TAG, "%s subscribed — streaming V/I/P", m->id);
    mqtt_publish_uc96_discovery(m->id);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val_handle;
    meter_t *m = &s_meters[(intptr_t)arg];
    if (err->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) m->cccd_handle = dsc->handle;
        return 0;
    }
    /* BLE_HS_EDONE (or error) ends the walk — subscribe if we found the CCCD. */
    if (m->cccd_handle) {
        uint8_t en[2] = { 1, 0 };  /* enable notifications */
        int rc = ble_gattc_write_flat(conn, m->cccd_handle, en, sizeof(en), on_subscribe, arg);
        if (rc != 0) ESP_LOGW(TAG, "%s CCCD write rc=%d", m->id, rc);
    } else {
        ESP_LOGW(TAG, "%s no CCCD (0x2902) — cannot enable notifications", m->id);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg) {
    meter_t *m = &s_meters[(intptr_t)arg];
    if (err->status == 0 && chr) {
        m->val_handle = chr->val_handle;
        return 0;
    }
    if (m->val_handle) {
        ble_gattc_disc_all_dscs(conn, m->val_handle, m->svc_end, on_dsc, arg);
    } else {
        ESP_LOGW(TAG, "%s 0xffe1 characteristic not found", m->id);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg) {
    meter_t *m = &s_meters[(intptr_t)arg];
    if (err->status == 0 && svc) {
        m->svc_start = svc->start_handle;
        m->svc_end   = svc->end_handle;
        return 0;
    }
    if (m->svc_end) {
        ble_gattc_disc_chrs_by_uuid(conn, m->svc_start, m->svc_end,
                                    &UC96_CHR_UUID.u, on_chr, arg);
    } else {
        ESP_LOGW(TAG, "%s 0xffe0 service not found", m->id);
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg) {
    meter_t *m = &s_meters[(intptr_t)arg];
    /* The 36-byte report needs MTU >= 39; log low MTU but proceed (some peers
     * still chunk fine). Discovery starts regardless of the exchange result. */
    if (err->status == 0) ESP_LOGI(TAG, "%s MTU=%u", m->id, mtu);
    else ESP_LOGW(TAG, "%s MTU exchange status=%d (using default)", m->id, err->status);
    ble_gattc_disc_svc_by_uuid(conn, &UC96_SVC_UUID.u, on_svc, arg);
    return 0;
}

static void try_connect(const ble_addr_t *addr) {
    if (s_connecting) return;            /* one initiator at a time */
    if (meter_by_addr(addr)) return;     /* already connecting / connected */
    meter_t *m = NULL;
    for (int i = 0; i < MAX_METERS; i++)
        if (s_meters[i].state == M_FREE) { m = &s_meters[i]; break; }
    if (!m) return;                      /* all slots in use */

    memset(m, 0, sizeof(*m));
    m->addr  = *addr;
    m->state = M_CONNECTING;
    meter_id_str(addr, m->id);
    s_connecting = true;
    s_connecting_meter = m;

    ble_gap_disc_cancel();               /* must stop scanning to initiate a connect */
    int rc = ble_gap_connect(s_own_addr_type, addr, 8000 /*ms*/, NULL, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s connect initiate rc=%d", m->id, rc);
        meter_free(m);
        s_connecting = false;
        s_connecting_meter = NULL;
        start_scan_if_room();
    } else {
        ESP_LOGI(TAG, "%s connecting…", m->id);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0)
            return 0;
        char name[32] = {0};
        if (f.name != NULL && f.name_len > 0) {
            size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
            memcpy(name, f.name, n);
        }
        bool has_svc = adv_has_uc96_svc(&f);
        int64_t now = esp_timer_get_time();

        /* BTHome v2 passive sensor data — service-data UUID 0xFCD2. NimBLE keeps
         * the UUID at the head of svc_data_uuid16 (LE: d2 fc); the device-info
         * byte + TLV objects follow. ble_parse_bthome handles the rest (unencrypted
         * v2 only). If a future NimBLE strips the UUID this just won't match —
         * graceful (BTHome stays undetected), never a crash. */
        bool has_bthome = false;
        ble_bthome_reading_t bth = {0};
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 3 &&
            f.svc_data_uuid16[0] == 0xd2 && f.svc_data_uuid16[1] == 0xfc) {
            has_bthome = ble_parse_bthome(f.svc_data_uuid16 + 2,
                                          f.svc_data_uuid16_len - 2, &bth);
        }

        uint8_t kind = (name_is_uc96(name) || has_svc) ? BLE_DEV_UC96
                     : has_bthome                       ? BLE_DEV_BTHOME
                                                        : BLE_DEV_OTHER;

        /* Surface interesting devices (named / UC96 / BTHome) to the web UI,
         * deduped by MAC. Anonymous beacons are skipped so the buffer tracks
         * things worth saving. */
        if (name[0] || kind != BLE_DEV_OTHER) {
            disc_t *d = disc_slot(&event->disc.addr);
            d->last_us = now;
            d->rssi    = event->disc.rssi;
            if (kind != BLE_DEV_OTHER) d->kind = kind;  /* don't downgrade */
            if (name[0]) snprintf(d->name, sizeof(d->name), "%s", name);
            if (has_bthome) { d->has_bthome = true; d->bthome = bth; }
        }
#if CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS
        /* Bench-only survey of named / 0xffe0-bearing adverts (throttled ~1 Hz),
         * to see what's in range while bringing meters up. Gated by the bench
         * debug flag — silent in field/production. */
        s_dbg_adv_count++;
        if ((name[0] || has_svc || has_bthome) && now - s_dbg_log_us > 1000000) {
            s_dbg_log_us = now;
            const uint8_t *a = event->disc.addr.val;
            ESP_LOGI(TAG, "adv #%lu %02x:%02x:%02x:%02x:%02x:%02x rssi=%d name='%s' uc96=%d bthome=%d",
                     (unsigned long)s_dbg_adv_count,
                     a[5], a[4], a[3], a[2], a[1], a[0], event->disc.rssi,
                     name, kind == BLE_DEV_UC96, has_bthome);
        }
#endif
        /* UC96: connect ONLY to allowlisted (saved) meters — ignore a
         * neighbour's UC96 even though it shows in the scan list. */
        if (kind == BLE_DEV_UC96) {
            s_uc96_last_us = now;
            char id[13];
            meter_id_str(&event->disc.addr, id);
            if (ble_store_is_saved(id)) try_connect(&event->disc.addr);
        }
        /* BTHome: passive — publish ONLY saved sensors (throttled), HA discovery
         * once. The reading lives in the disc slot for the web UI regardless. */
        if (has_bthome) {
            char id[13];
            meter_id_str(&event->disc.addr, id);
            if (ble_store_is_saved(id)) {
                disc_t *d = disc_find(&event->disc.addr);
                if (d && !d->bthome_announced) {
                    mqtt_publish_bthome_discovery(id, &bth);
                    d->bthome_announced = true;
                }
                if (d && now - d->bthome_pub_us > 30000000) {  /* ≤ once / 30 s */
                    d->bthome_pub_us = now;
                    mqtt_publish_bthome(id, &bth);
                }
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* A forever scan only "completes" if it was cancelled (e.g. to connect);
         * the connect path restarts it. Relaunch only when nothing's in flight. */
        if (!s_connecting) start_scan_if_room();
        return 0;

    case BLE_GAP_EVENT_CONNECT: {
        meter_t *m = s_connecting_meter;
        s_connecting = false;
        s_connecting_meter = NULL;
        if (m == NULL) { start_scan_if_room(); return 0; }
        if (event->connect.status == 0) {
            m->conn_handle = event->connect.conn_handle;
            m->state = M_CONNECTED;
            ESP_LOGI(TAG, "%s connected (handle=%u) — discovering", m->id, m->conn_handle);
            /* Negotiate MTU first (frame is 36 B > 23 B default), then discover. */
            int rc = ble_gattc_exchange_mtu(m->conn_handle, on_mtu,
                                            (void *)(intptr_t)(m - s_meters));
            if (rc != 0) {
                ESP_LOGW(TAG, "%s MTU exchange rc=%d — discovering anyway", m->id, rc);
                ble_gattc_disc_svc_by_uuid(m->conn_handle, &UC96_SVC_UUID.u, on_svc,
                                           (void *)(intptr_t)(m - s_meters));
            }
        } else {
            ESP_LOGW(TAG, "%s connect failed (status=%d)", m->id, event->connect.status);
            meter_free(m);
        }
        start_scan_if_room();  /* find the next meter while this one runs */
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        meter_t *m = meter_by_conn(event->disconnect.conn.conn_handle);
        if (m) {
            ESP_LOGW(TAG, "%s disconnected (reason=%d)", m->id, event->disconnect.reason);
            meter_free(m);
        }
        start_scan_if_room();  /* a slot freed → rediscover */
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        meter_t *m = meter_by_conn(event->notify_rx.conn_handle);
        if (!m) return 0;
        uint8_t buf[64];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) != 0)
            return 0;
        ble_uc96_reading_t r;
        if (!ble_parse_uc96(buf, len, &r)) return 0;  /* not a 36-byte report */
        m->last_rx_us = esp_timer_get_time();
        m->frames++;
        m->last = r;            /* keep for the web UI */
        m->has_reading = true;
        mqtt_publish_uc96(m->id, r.voltage_v, r.current_a, r.power_w,
                          r.energy_wh, r.temperature_c);
        if (m->frames == 1 || (m->frames % 30) == 0)
            ESP_LOGI(TAG, "%s #%lu  %.2f V  %.3f A  %.2f W  %d °C",
                     m->id, (unsigned long)m->frames,
                     r.voltage_v, r.current_a, r.power_w, r.temperature_c);
        return 0;
    }

    default:
        return 0;
    }
}

static void start_scan(void) {
    /* ACTIVE + moderate duty. Active (passive=0) so we solicit the scan-response:
     * the Atorch UC96 advertises a minimal ADV (flags + the 0xffe0 service UUID)
     * and puts its name ("UC96_BLE") only in the scan-response, which a passive
     * scan never requests — bleak's discover() (power-meter/uc96d.py) is active
     * for the same reason. 50 % duty (30 ms window / 60 ms interval): at the old
     * 3 % the bench caught zero adverts in 55 s with WiFi-priority coex eating
     * BLE's slice. Scanning runs only while a meter slot is free; once full it
     * pauses. WiFi still wins RF via the coex preference set in on_sync. */
    struct ble_gap_disc_params dp = {0};
    dp.passive = 0;
    dp.filter_duplicates = 0;
    dp.itvl = 96;    /* 60 ms */
    dp.window = 48;  /* 30 ms */
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "active scan started (50%% duty, WiFi-priority coexistence)");
}

static void start_scan_if_room(void) {
    /* Scan continuously while BLE is up: with the allowlist model we connect
     * only to saved UC96s (gated in the DISC handler), but scanning must keep
     * running to surface NEW candidates for the web UI and to observe passive
     * BTHome sensors — not just until the meter slots fill. The only thing that
     * blocks a scan is an in-flight connect (GAP allows one initiator). */
    if (s_connecting) return;            /* a connect is initiating — can't scan too */
    if (ble_gap_disc_active()) return;   /* already scanning */
    start_scan();
}

static void on_sync(void) {
    /* WiFi wins RF contention — the whole point. Set it before scanning. */
    esp_err_t ce = esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if (ce != ESP_OK) ESP_LOGW(TAG, "coex prefer WiFi: %s", esp_err_to_name(ce));

    /* Ask for a bigger ATT MTU so a whole 36-byte report fits one notification
     * (default 23 → only 20 B payload). The per-connection exchange uses this. */
    ble_att_set_preferred_mtu(128);

    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address — not scanning");
        return;
    }
    start_scan();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();              /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_start(void) {
    if (s_running) return;
    if (!app_config_get_bool("ble_enabled")) {
        ESP_LOGI(TAG, "compiled in but disabled (ble_enabled=OFF)");
        return;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(ble_host_task);
    s_running = true;
    ESP_LOGI(TAG, "started — NimBLE central, UC96 detect (V/I/P read is phase 4)");
}

void ble_apply_config(void) {
    bool want = app_config_get_bool("ble_enabled");
    if (want && !s_running) {
        /* Do NOT runtime-start here. nimble_port_init wants a chunk of contiguous
         * internal DRAM a fragmented running heap often can't give, and it runs
         * synchronously in the caller (e.g. the httpd task for the /ble or /config
         * toggle) — blocking the web server for seconds and risking OOM. Persist
         * the intent; the boot path (main → ble_start, gated on ble_enabled)
         * brings BLE up cleanly on the next reboot. */
        ESP_LOGW(TAG, "ble_enabled=ON — reboot to start BLE (runtime start disabled)");
    } else if (!want && s_running) {
        /* Cleanly tearing NimBLE down mid-flight is fiddly; the scan is a
         * harmless low-duty observer, so defer the actual stop to the next
         * reboot. Make the intent visible. */
        ESP_LOGW(TAG, "ble_enabled=OFF — BLE keeps scanning until next reboot");
    }
}

bool ble_running(void) { return s_running; }

void ble_status(char *out, size_t n) {
    if (!out || n == 0) return;
    if (!s_running) { snprintf(out, n, "off"); return; }
    int streaming = connected_count();
    if (streaming > 0) { snprintf(out, n, "uc96 %d streaming", streaming); return; }
    if (s_uc96_last_us == 0) { snprintf(out, n, "scanning,no-uc96"); return; }
    int ago = (int)((esp_timer_get_time() - s_uc96_last_us) / 1000000);
    snprintf(out, n, "uc96 seen %ds ago", ago);
}

int ble_snapshot(ble_dev_view_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    int64_t now = esp_timer_get_time();
    /* Connected meters first — while connected they aren't advertising, so they
     * won't be in the discovered buffer. conn_state maps from mstate_t (1..3). */
    for (int i = 0; i < MAX_METERS && n < max; i++) {
        if (s_meters[i].state == M_FREE) continue;
        ble_dev_view_t *v = &out[n++];
        memset(v, 0, sizeof(*v));
        snprintf(v->id, sizeof(v->id), "%s", s_meters[i].id);
        v->kind = BLE_DEV_UC96;
        v->conn_state = (uint8_t)s_meters[i].state;
        v->age_s = s_meters[i].last_rx_us
                 ? (int)((now - s_meters[i].last_rx_us) / 1000000) : -1;
        if (s_meters[i].has_reading) { v->has_uc96 = true; v->uc96 = s_meters[i].last; }
    }
    /* Then in-range advertisers not already listed (dedup by id). */
    for (int i = 0; i < MAX_DISCOVERED && n < max; i++) {
        if (!s_disc[i].used) continue;
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(out[j].id, s_disc[i].id) == 0) { dup = true; break; }
        if (dup) continue;
        ble_dev_view_t *v = &out[n++];
        memset(v, 0, sizeof(*v));
        snprintf(v->id, sizeof(v->id), "%s", s_disc[i].id);
        snprintf(v->adv_name, sizeof(v->adv_name), "%s", s_disc[i].name);
        v->rssi = s_disc[i].rssi;
        v->kind = s_disc[i].kind;
        v->age_s = (int)((now - s_disc[i].last_us) / 1000000);
        if (s_disc[i].has_bthome) { v->has_bthome = true; v->bthome = s_disc[i].bthome; }
    }
    return n;
}

void ble_request_scan(void) {
    if (s_running) start_scan_if_room();
}

#else /* !CONFIG_CHYTRA_BUDKA_BLE — no NimBLE built, all no-ops */

void ble_start(void) {}
void ble_apply_config(void) {}
bool ble_running(void) { return false; }
void ble_status(char *out, size_t n) { if (out && n) snprintf(out, n, "off"); }
int  ble_snapshot(ble_dev_view_t *out, int max) { (void)out; (void)max; return 0; }
void ble_request_scan(void) {}

#endif /* CONFIG_CHYTRA_BUDKA_BLE */
