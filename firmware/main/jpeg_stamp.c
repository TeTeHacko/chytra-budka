/* jpeg_stamp.c — see jpeg_stamp.h.
 *
 * EXIF-metadata only: builds an APP1 (Exif) segment from jpeg_stamp_info_t
 * and splices it right after the JPEG SOI. There is intentionally NO
 * burned-in visual overlay — see jpeg_stamp.h for the why (the
 * OV3660 has no on-chip OSD, the ESP32-S3 has no hardware JPEG codec, so
 * drawing any pixel needs a full software decode→re-encode of the whole
 * frame: ~3.6 s at UXGA, measured — not worth it on a solar field unit). */

#include "jpeg_stamp.h"
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "diag.h"
#include "sensors.h"
#include "esp_app_desc.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt.h"
#include "wifi_mgr.h"

static const char *TAG = "stamp";

/* ── EXIF builder ─────────────────────────────────────────────────────
 * Emits a single APP1 segment (FF E1 …) containing a minimal but
 * standards-compliant TIFF/Exif IFD0 + ExifSubIFD with everything the
 * jpeg_stamp_info_t carries plus firmware version, framesize, quality
 * read live from esp_camera. Little-endian TIFF (matches CPU); tags
 * within each IFD sorted ascending by tag id (Exif spec requirement).
 *
 * Layout (offsets from start of TIFF header at out+10):
 *   0    II 2A 00  08 00 00 00     (8-byte TIFF header, IFD0 @ 8)
 *   8    IFD0: 7 entries + next=0 (90 bytes, ends @ 98)
 *  98    ExifSubIFD: 7 entries + next=0 (90 bytes, ends @ 188)
 * 188    External data block (strings + UserComment)
 */

static inline void w16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void w32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline void w16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}
static inline void emit_entry(uint8_t *e, uint16_t tag, uint16_t type,
                              uint32_t count, uint32_t value) {
    w16le(e,     tag);
    w16le(e + 2, type);
    w32le(e + 4, count);
    w32le(e + 8, value);
}

/* Emit an ASCII tag with auto inline/offset selection. The Exif spec
 * mandates that when the payload (including NUL) fits in the 4-byte
 * value field it MUST be stored inline — storing it as offset makes
 * conformant readers (exiftool) read the offset bytes as the string,
 * yielding `\xb0\0\0\0` instead of "vad\0". Bug was visible on the
 * pir/vad triggers (≤3 chars + NUL = 4 bytes). */
static uint8_t *emit_ascii(uint8_t *entry, uint16_t tag, const char *s,
                           uint8_t *tiff, uint32_t *ext) {
    size_t n = strlen(s) + 1;
    if (n <= 4) {
        w16le(entry,     tag);
        w16le(entry + 2, 2);                 /* type ASCII */
        w32le(entry + 4, (uint32_t)n);
        entry[8] = entry[9] = entry[10] = entry[11] = 0;
        memcpy(entry + 8, s, n);             /* inline value */
    } else {
        memcpy(tiff + *ext, s, n);
        emit_entry(entry, tag, 2, (uint32_t)n, *ext);
        *ext += (uint32_t)n;
    }
    return entry + 12;
}

/* Format the local-time zone offset as "+HH:MM" / "-HH:MM" (7 bytes
 * incl. NUL). %z gives "+HHMM" so we splice the colon manually. */
static void fmt_tz_offset(time_t t, char out[8]) {
    struct tm tm_l;
    localtime_r(&t, &tm_l);
    char zbuf[8] = {0};
    strftime(zbuf, sizeof(zbuf), "%z", &tm_l);  /* e.g. "+0200" */
    if (strlen(zbuf) == 5) {
        out[0] = zbuf[0]; out[1] = zbuf[1]; out[2] = zbuf[2];
        out[3] = ':';
        out[4] = zbuf[3]; out[5] = zbuf[4];
        out[6] = '\0';
    } else {
        memcpy(out, "+00:00", 7);
    }
}

#define EXIF_BUF_SIZE  1024  /* worst-case ~650 B; headroom for future tags */

static size_t build_exif_app1(const jpeg_stamp_info_t *info,
                              uint8_t *out, size_t cap) {
    if (cap < EXIF_BUF_SIZE) return 0;

    /* APP1 marker + length placeholder + "Exif\0\0" identifier. */
    out[0] = 0xFF; out[1] = 0xE1;
    out[2] = 0;    out[3] = 0;     /* length filled in at the end */
    memcpy(out + 4, "Exif\0\0", 6);

    uint8_t *tiff = out + 10;

    /* TIFF header: II, magic 0x002A, IFD0 offset = 8. */
    tiff[0] = 'I'; tiff[1] = 'I';
    w16le(tiff + 2, 0x002A);
    w32le(tiff + 4, 8);

    /* DateTime string shared by IFD0/DateTime + ExifSubIFD/
     * DateTimeOriginal + DateTimeDigitized. Sentinel "1970:01:01 …"
     * when SNTP hasn't synced — viewers will still display *something*
     * instead of falling back to file mtime. */
    char dt[20];
    time_t t_capture;
    if (info->epoch_ms > (CB_CLOCK_SYNCED_EPOCH * 1000LL)) {
        t_capture = (time_t)(info->epoch_ms / 1000);
    } else {
        t_capture = 0;
    }
    {
        struct tm tm_l;
        localtime_r(&t_capture, &tm_l);
        strftime(dt, sizeof(dt), "%Y:%m:%d %H:%M:%S", &tm_l);
    }
    char tz[8];
    fmt_tz_offset(t_capture ? t_capture : time(NULL), tz);

    /* Strings for external data block. */
    const char *trigger = info->trigger ? info->trigger : "unknown";
    const char *make = "Chytra Budka";
    const char *model = mqtt_topic_base();  /* "cb-ex01" */
    const esp_app_desc_t *app = esp_app_get_description();
    const char *software = app ? app->version : "?";

    /* Detected sensor name for LensModel tag — esp_camera_sensor_get_info()
     * looks up the live PID against the driver's internal table, so we
     * report what the chip actually reported during probe. Fallback to
     * "PID_<hex>" when the PID isn't in the table (rather than skipping
     * the tag), because skipping would force a runtime-conditional IFD
     * entry count and break the EXIF rule that tags within an IFD are
     * stored in ascending order. */
    sensor_t *s = esp_camera_sensor_get();
    int fs = -1, qual = -1;
    char sensor_name_buf[16];
    const char *sensor_name = "unknown";
    if (s) {
        fs = s->status.framesize;
        qual = s->status.quality;
        sensor_id_t id = s->id;
        camera_sensor_info_t *info_ = esp_camera_sensor_get_info(&id);
        if (info_ && info_->name) {
            sensor_name = info_->name;
        } else {
            snprintf(sensor_name_buf, sizeof(sensor_name_buf),
                     "PID_0x%04x", (unsigned)s->id.PID);
            sensor_name = sensor_name_buf;
        }
    }

    /* Residual fields → UserComment as compact JSON. Pulled live so values
     * reflect this exact frame's environment, not stale cfg. Battery /
     * mcu_temp are skipped when not available rather than emitting -1
     * sentinels that pollute downstream analysis. */
    int rssi = wifi_mgr_rssi();
    uint32_t heap = esp_get_free_heap_size();
    float mcu_c = diag_mcu_temp_c();
    bool batt_ok = battery_ready();
    float vbatt = batt_ok ? battery_vbat() : NAN;
    float soc   = batt_ok ? battery_soc()  : NAN;

    char uc_json[320];
    int n = snprintf(uc_json, sizeof(uc_json),
        "{\"seq\":%u,\"uptime_s\":%u,\"capture_ms\":%u,"
        "\"agc\":%d,\"ir\":%d,\"framesize\":%d,\"quality\":%d,"
        "\"rssi\":%d,\"heap\":%u",
        (unsigned)info->sequence, (unsigned)info->uptime_s,
        (unsigned)info->capture_ms,
        info->agc_gain, info->ir_active ? 1 : 0, fs, qual,
        rssi, (unsigned)heap);
    if (n > 0 && isfinite(mcu_c)) {
        n += snprintf(uc_json + n, sizeof(uc_json) - n,
                      ",\"mcu_c\":%.1f", mcu_c);
    }
    /* Every MQTT-published environmental channel (SHT41 temp/humidity, BMP388
     * temp/pressure, any bus1 ext probe) by its stable obj key, so the EXIF
     * carries the full capture-time sensor picture — the photo caption picks
     * temp/humidity/pressure out of it; mcu_c above is the CPU die, vbatt/soc
     * below the battery. Battery/INA (mqtt=false) are emitted via their own
     * fields. Per-channel skipped when not reading; bounded against uc_json. */
    for (size_t si = 0; si < CB_SENSORS_N; si++) {
        const cb_sensor_t *sen = &CB_SENSORS[si];
        if (!sen->mqtt) continue;
        for (size_t ci = 0; ci < sen->n_chans; ci++) {
            const cb_chan_t *c = &sen->chans[ci];
            float v;
            if (n > 0 && n < (int)sizeof(uc_json) - 24 && c->read(&v) && isfinite(v))
                n += snprintf(uc_json + n, sizeof(uc_json) - n,
                              ",\"%s\":%.*f", c->obj, c->decimals, (double)v);
        }
    }
    if (n > 0 && batt_ok && isfinite(vbatt)) {
        n += snprintf(uc_json + n, sizeof(uc_json) - n,
                      ",\"vbatt\":%.3f,\"soc\":%.1f", vbatt, soc);
    }
    if (n > 0 && n < (int)sizeof(uc_json) - 1) {
        uc_json[n++] = '}';
        uc_json[n] = '\0';
    }
    int uc_json_len = (n > 0) ? n : 0;

    /* IFD0 — 7 entries, in ascending tag order. Each entry is 12 bytes:
     * tag(2), type(2), count(4), value_or_offset(4). External data
     * lives at offset >= 176 from start of TIFF. */
    uint8_t *ifd0 = tiff + 8;
    w16le(ifd0, 7);
    uint8_t *entry = ifd0 + 2;
    uint32_t ext = 188;  /* running offset into external data */

    /* 0x010E ImageDescription — trigger reason (ASCII). */
    entry = emit_ascii(entry, 0x010E, trigger, tiff, &ext);

    /* 0x010F Make — vendor brand. */
    entry = emit_ascii(entry, 0x010F, make, tiff, &ext);

    /* 0x0110 Model — device id (matches MQTT topic base). */
    entry = emit_ascii(entry, 0x0110, model, tiff, &ext);

    /* 0x0112 Orientation — 1 (Normal); cam_rotate_180 is applied on the
     * sensor so the JPEG is already upright by the time we see it. */
    emit_entry(entry, 0x0112, 3, 1, 1);
    entry += 12;

    /* 0x0131 Software — firmware build version. */
    entry = emit_ascii(entry, 0x0131, software, tiff, &ext);

    /* 0x0132 DateTime (20 bytes incl NUL). */
    memcpy(tiff + ext, dt, 20);
    emit_entry(entry, 0x0132, 2, 20, ext);
    entry += 12; ext += 20;

    /* 0x8769 ExifIFDPointer → ExifSubIFD at offset 98. */
    emit_entry(entry, 0x8769, 4, 1, 98);
    entry += 12;

    /* IFD0 next-pointer = 0 (no IFD1, no thumbnail). */
    w32le(entry, 0);

    /* ExifSubIFD — 7 entries (was 6; added LensModel for sensor PID). */
    uint8_t *subifd = tiff + 98;
    w16le(subifd, 7);
    entry = subifd + 2;

    /* 0x9000 ExifVersion — UNDEFINED 4 bytes "0230" (Exif 2.30).
     * Fits in the inline value field. */
    emit_entry(entry, 0x9000, 7, 4, 0);
    memcpy(entry + 8, "0230", 4);
    entry += 12;

    /* 0x9003 DateTimeOriginal. */
    memcpy(tiff + ext, dt, 20);
    emit_entry(entry, 0x9003, 2, 20, ext);
    entry += 12; ext += 20;

    /* 0x9004 DateTimeDigitized. */
    memcpy(tiff + ext, dt, 20);
    emit_entry(entry, 0x9004, 2, 20, ext);
    entry += 12; ext += 20;

    /* 0x9010 OffsetTime (7 bytes "+HH:MM\0"). */
    memcpy(tiff + ext, tz, 7);
    emit_entry(entry, 0x9010, 2, 7, ext);
    entry += 12; ext += 7;

    /* 0x9011 OffsetTimeOriginal. */
    memcpy(tiff + ext, tz, 7);
    emit_entry(entry, 0x9011, 2, 7, ext);
    entry += 12; ext += 7;

    /* 0x9286 UserComment — UNDEFINED, 8-byte charset prefix + payload.
     * "ASCII\0\0\0" tells consumers the payload is plain ASCII. */
    uint32_t uc_total = 8 + (uint32_t)uc_json_len;
    if (ext + uc_total > EXIF_BUF_SIZE - 10) {
        /* Defensive truncation — sw version + JSON together shouldn't
         * blow 1024 B but cap rather than corrupt. Floor at 8 so the
         * `uc_total - 8` memcpy below can't underflow if we somehow end
         * up with less than the charset-prefix worth of room. */
        uc_total = (uint32_t)(EXIF_BUF_SIZE - 10 - ext);
        if (uc_total < 8) uc_total = 8;
    }
    memcpy(tiff + ext, "ASCII\0\0\0", 8);
    memcpy(tiff + ext + 8, uc_json, uc_total - 8);
    emit_entry(entry, 0x9286, 7, uc_total, ext);
    entry += 12; ext += uc_total;

    /* 0xA434 LensModel — sensor chip name as detected by
     * esp_camera_sensor_get_info() against the live PID, with
     * "PID_0x<hex>" / "unknown" fallback for an unrecognized chip.
     * Always emitted so the IFD entry count stays static and tags
     * remain in ascending order (strict EXIF parsers reject IFDs
     * with non-monotonic tag IDs and drop everything past the
     * violation). */
    entry = emit_ascii(entry, 0xA434, sensor_name, tiff, &ext);

    /* ExifSubIFD next-pointer = 0. */
    w32le(entry, 0);

    /* Total APP1 segment = marker(2) + length(2) + ident(6) + tiff(ext).
     * Length field is BIG-endian and includes itself + identifier +
     * tiff, but excludes the FF E1 marker. */
    uint32_t tiff_len = ext;
    uint32_t app1_len_field = 2 + 6 + tiff_len;
    w16be(out + 2, (uint16_t)app1_len_field);
    return 2 + app1_len_field;  /* incl. FF E1 marker */
}

/* Splice a pre-built segment right after the SOI marker. Returns a
 * freshly-allocated buffer (PSRAM preferred); *out_len receives the new
 * length. */
static uint8_t *inject_app1(const uint8_t *jpg, size_t jpg_len,
                            const uint8_t *seg, size_t seg_len,
                            size_t *out_len) {
    if (jpg_len < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        return NULL;
    }
    size_t new_len = jpg_len + seg_len;
    uint8_t *out = (uint8_t *)heap_caps_malloc(new_len, MALLOC_CAP_SPIRAM);
    if (!out) {
        size_t psram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG,
                 "PSRAM alloc %zu B failed (largest block %zu B, "
                 "total free %zu B) — falling back to DRAM",
                 new_len, psram_largest, psram_free);
        out = (uint8_t *)malloc(new_len);
    }
    if (!out) {
        size_t dram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ESP_LOGE(TAG,
                 "DRAM fallback also failed for %zu B (largest %zu B)",
                 new_len, dram_largest);
        return NULL;
    }

    out[0] = 0xFF; out[1] = 0xD8;          /* SOI */
    memcpy(out + 2, seg, seg_len);          /* APP1 EXIF */
    memcpy(out + 2 + seg_len, jpg + 2, jpg_len - 2);  /* rest of JPEG */
    *out_len = new_len;
    return out;
}

/* See jpeg_stamp.h. Builds the EXIF APP1 from `info` and splices it right
 * after the source JPEG's SOI — METADATA ONLY. There is intentionally no
 * burned-in visual overlay: the OV3660 emits JPEG on-chip with no OSD, and
 * the ESP32-S3 has no hardware JPEG codec, so drawing any pixel needs a
 * full software decode→re-encode of the whole frame (~3.6 s at UXGA). The
 * same identity/time/trigger data lives here in EXIF + the MQTT event JSON;
 * timelapse + HA do any visual overlay downstream. */
size_t jpeg_stamp_apply(const uint8_t *src, size_t src_len,
                        uint16_t width, uint16_t height,
                        uint8_t jpeg_quality,
                        const jpeg_stamp_info_t *info,
                        uint8_t **out_buf) {
    (void)width; (void)height; (void)jpeg_quality;  /* metadata-only: no draw */
    if (!src || !info || !out_buf) return 0;
    *out_buf = NULL;

    uint8_t exif[EXIF_BUF_SIZE];
    size_t exif_len = build_exif_app1(info, exif, sizeof(exif));
    if (exif_len == 0) {
        ESP_LOGW(TAG, "EXIF build failed");
        return 0;
    }

    size_t final_len = 0;
    uint8_t *final_buf = inject_app1(src, src_len, exif, exif_len, &final_len);
    if (!final_buf) {
        ESP_LOGW(TAG, "EXIF inject failed");
        return 0;
    }
    *out_buf = final_buf;
    return final_len;
}
