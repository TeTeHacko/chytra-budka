/* oled.c — bench-only SSD1306 128×64 status display. See oled.h. */

#include "oled.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <time.h>

#include "app_config.h"
#include "app_main_exports.h"
#include "cb_ds.h"
#include "audio.h"
#include "camera.h"
#include "config.h"
#include "default_logo.h"
#include "sleep_cat.h"
#include "device_id.h"
#include "diag.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_xport.h"
#include "mqtt.h"
#include "nvs.h"
#include "ota.h"
#include "photo_queue.h"
#include "pir.h"
#include "qrcode.h"
#include "audiofx.h"
#include "reed.h"
#include "sd_storage.h"
#include "sensors.h"
#include "soil.h"
#include "sonar.h"
#include "speaker.h"
#include "wifi_mgr.h"

static const char *TAG = "oled";

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)          /* 8 byte-rows */
#define OLED_COLS_TXT (OLED_W / 6)       /* 21 chars per row (5px glyph + 1) */
/* Per-transaction timeouts. These must OUTWAIT a busy shared bus, not just
 * cover the transfer itself: the IDF i2c_master timeout includes the wait
 * for the bus, so a short value makes the OLED's long bursts spuriously
 * fail with "I2C software timeout / bus still busy" when they queue behind
 * a concurrent SHT41 measurement (+ its retries / bus recovery). The OLED
 * is low-priority and non-critical, so waiting is fine; it never starves
 * SHT41 (separate, higher-priority transactions). */
#define I2C_CMD_TMO_MS   250             /* probe + command writes */
#define I2C_FLUSH_TMO_MS 1000            /* the ~1 KB framebuffer transmit */

/* Which bus the panel is wired to. Bench wiring puts it on bus0; routed
 * through i2c_xport so moving it to bus1 is this one line. */
#define OLED_BUS CB_BUS0

static i2c_xport_t s_xport = {.bus = OLED_BUS, .addr = OLED_ADDR};
static bool s_dev_open;                  /* transport device opened */
static bool s_ok;
/* Operator power switch (oled_enabled NVS knob). When set, the refresh task
 * blanks the panel + stops flushing; toggled live, applied by oled_task. */
static volatile bool s_user_off;
/* Optional GPIO that gates the panel's VCC via a MOSFET (PIN_FN_OLED_PWR).
 * -1 = unmapped → software-only off (0xAE). Resolved once at task start. */
static int s_pwr_pin = -1;
static uint8_t s_fb[OLED_PAGES * OLED_W]; /* page-major: page*128 + x */

/* ── low-level I²C (control byte 0x00 = command, 0x40 = data) ──────────── */

static esp_err_t oled_cmd(uint8_t c) {
    uint8_t buf[2] = {0x00, c};
    return i2c_xport_tx(&s_xport, buf, sizeof(buf), I2C_CMD_TMO_MS);
}

static esp_err_t oled_cmds(const uint8_t *cmds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = oled_cmd(cmds[i]);
        if (e != ESP_OK)
            return e;
    }
    return ESP_OK;
}

/* SSD1306 128×64 init (charge-pump on, horizontal addressing). */
static const uint8_t SSD1306_INIT[] = {
    0xAE,             /* display off */
    0x20, 0x00,       /* memory addressing mode = horizontal */
    0x40,             /* display start line 0 */
    0xA1,             /* segment remap (column 127 → SEG0) */
    0xC8,             /* COM scan direction remapped */
    0xA8, 0x3F,       /* multiplex ratio = 63 (64 rows) */
    0xD3, 0x00,       /* display offset 0 */
    0xDA, 0x12,       /* COM pins: alternating, no remap (128×64) */
    0xD5, 0x80,       /* clock divide / osc freq */
    0xD9, 0xF1,       /* pre-charge period */
    0xDB, 0x40,       /* VCOMH deselect level */
    0x81, 0xCF,       /* contrast */
    0x8D, 0x14,       /* charge pump on */
    0xA4,             /* resume to RAM content */
    0xA6,             /* normal (non-inverted) */
    0xAF,             /* display on */
};

/* Push the whole framebuffer. One data transmit at 400 kHz ≈ 23 ms; the
 * IDF driver serialises this against SHT41 transfers, so worst case an
 * ambient read queues ~23 ms behind a refresh — acceptable, and refresh
 * is only ~0.3 Hz. */
static esp_err_t oled_flush(void) {
    const uint8_t window[] = {
        0x21, 0x00, OLED_W - 1,        /* column address 0..127 */
        0x22, 0x00, OLED_PAGES - 1,    /* page address 0..7 */
    };
    esp_err_t e = oled_cmds(window, sizeof(window));
    if (e != ESP_OK)
        return e;

    static uint8_t tx[1 + sizeof(s_fb)];
    tx[0] = 0x40;                       /* data stream control byte */
    memcpy(&tx[1], s_fb, sizeof(s_fb));
    return i2c_xport_tx(&s_xport, tx, sizeof(tx), I2C_FLUSH_TMO_MS);
}

/* ── framebuffer text ─────────────────────────────────────────────────── */

static void fb_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

/* Set/clear a single pixel (on = lit). Page-major: byte = page*W + x. */
static void fb_set(int x, int y, int on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H)
        return;
    uint8_t *b = &s_fb[(y / 8) * OLED_W + x];
    if (on)
        *b |= (uint8_t)(1u << (y % 8));
    else
        *b &= (uint8_t)~(1u << (y % 8));
}

/* Draw one glyph at pixel column x, page row (0..7). 5 cols + 1 spacer. */
static void fb_glyph(int x, int page, char ch) {
    if (page < 0 || page >= OLED_PAGES)
        return;
    const uint8_t *g = FONT5X7[(uint8_t)ch];
    for (int col = 0; col < 6; col++) {
        int px = x + col;
        if (px < 0 || px >= OLED_W)
            continue;
        s_fb[page * OLED_W + px] = (col < 5) ? g[col] : 0x00;
    }
}

/* Draw a string at char-cell (col, row). Clips at the right edge. */
static void fb_text(int col, int row, const char *s) {
    int x = col * 6;
    for (; *s && x <= OLED_W - 6; s++, x += 6)
        fb_glyph(x, row, *s);
}

/* Draw `label` left-aligned at char-col `lcol` and `val` RIGHT-aligned so its
 * last char sits at char-col `vend` — lines sensor values up into tidy columns
 * (labels on the left, values stacked). Nudges the value right if it would
 * collide with the label. */
static void fb_kv(int lcol, int vend, int row, const char *label, const char *val) {
    fb_text(lcol, row, label);
    int vcol = vend - (int)strlen(val) + 1;
    int min_vcol = lcol + (int)strlen(label) + 1;   /* ≥1 space after the label */
    if (vcol < min_vcol) vcol = min_vcol;
    fb_text(vcol, row, val);
}

/* ── bigger / graphic helpers (paged screens + overlays) ───────────────── */

/* One 5×7 glyph scaled 2× (→10×14 px) at pixel (x,y). For the big-font
 * environment page — readable from across the room. */
static void fb_glyph_2x(int x, int y, char ch) {
    const uint8_t *g = FONT5X7[(uint8_t)ch];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int r = 0; r < 7; r++) {
            if (!(bits & (1u << r)))
                continue;
            int px = x + col * 2, py = y + r * 2;
            fb_set(px, py, 1);     fb_set(px + 1, py, 1);
            fb_set(px, py + 1, 1); fb_set(px + 1, py + 1, 1);
        }
    }
}

/* Double-height string anchored at pixel (x,y). 12 px advance per glyph. */
static void fb_text_2x(int x, int y, const char *s) {
    for (; *s && x <= OLED_W - 10; s++, x += 12)
        fb_glyph_2x(x, y, *s);
}

/* A bordered horizontal bar filled to `pct` (0..100) in the box
 * [x,x+w) × [y,y+h). Used by the reset-hold / OTA / VU / SOC bars. */
static void fb_hbar(int x, int y, int w, int h, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill = (w - 2) * pct / 100;
    for (int yy = y; yy < y + h && yy < OLED_H; yy++)
        for (int xx = x; xx < x + w && xx < OLED_W; xx++) {
            int border = (xx == x || xx == x + w - 1 || yy == y || yy == y + h - 1);
            fb_set(xx, yy, border || ((xx - x - 1) < fill && (xx - x - 1) >= 0));
        }
}

/* A 2 px threshold tick poking up just above a horizontal bar, at the x where
 * fb_hbar's fill would reach `pct`. Marks the trigger level the live bar is
 * compared against (VAD dBFS on the mic bar, IR gain threshold on the light
 * bar) — when the live fill crosses the tick, the corresponding event fires. */
static void fb_bar_marker(int x, int y, int w, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int mx = x + 1 + (w - 2) * pct / 100;   /* same interior mapping as fb_hbar */
    if (mx > x + w - 2) mx = x + w - 2;
    fb_set(mx, y - 2, 1);
    fb_set(mx, y - 1, 1);
}

/* XOR-invert a pixel rectangle — used to flag the SLEEP code on the status bar
 * (normal = light-sleep armed, inverted = actively light-sleeping). */
static void fb_invert_rect(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h && yy < OLED_H; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < OLED_W; xx++) {
            if (xx < 0) continue;
            s_fb[(yy / 8) * OLED_W + xx] ^= (uint8_t)(1u << (yy % 8));
        }
    }
}

/* Blinking "alive" dot, top-right corner (cols 21+ are unused by the 21-char
 * text grid, so it never collides). Phase off uptime so it agrees on every
 * page regardless of that page's refresh rate. */
static void draw_heartbeat(void) {
    int on = (esp_timer_get_time() % 2000000) < 1000000;
    fb_set(OLED_W - 2, 0, on); fb_set(OLED_W - 1, 0, on);
    fb_set(OLED_W - 2, 1, on); fb_set(OLED_W - 1, 1, on);
}

/* Power-profile word + 2-letter status-bar code. Read app_mode_current()
 * (extern "C" int = cb::Profile) and map locally so this C TU needn't link the
 * C++ profile_name(). Index = cb::Profile enum: 0=Boot 1=Hibernate 2=Sentinel
 * 3=Eco 4=Active 5=Max. */
static const char *mode_str(void) {
    static const char *N[] = {"boot", "hibern", "sentinel", "eco", "active", "max"};
    int m = app_mode_current();
    return (m >= 0 && m < 6) ? N[m] : "?";
}
/* "P" + tier initial for the row-0 status bar: PB/PH/PS/PE/PA/PM. */
static const char *profile_code(void) {
    static const char *C[] = {"PB", "PH", "PS", "PE", "PA", "PM"};
    int m = app_mode_current();
    return (m >= 0 && m < 6) ? C[m] : "P?";
}

/* Cached value of a sensor channel, found by registry id + channel obj (e.g.
 * "sht0"/"temp", "ina"/"solar_p"). Reads only the cache the telemetry owner
 * fills — never touches I²C, so a status page adds no bus0 traffic. Returns
 * false when the sensor is absent or has no good reading yet. */
static bool chan_val(const char *id, const char *obj, float *out) {
    for (size_t si = 0; si < CB_SENSORS_N; si++) {
        if (strcmp(CB_SENSORS[si].id, id) != 0)
            continue;
        const cb_sensor_t *s = &CB_SENSORS[si];
        for (size_t ci = 0; ci < s->n_chans; ci++)
            if (strcmp(s->chans[ci].obj, obj) == 0) {
                float v;
                if (s->chans[ci].read(&v) && isfinite(v)) { *out = v; return true; }
                return false;
            }
    }
    return false;
}

/* ── QR code ──────────────────────────────────────────────────────────── */
/* When a QR is requested, the refresh task paints it (instead of the status
 * page) until s_qr_until_us. Rendered as DARK modules on a LIT background
 * (the OLED is emissive, so "lit" reads as the white background a scanner
 * expects) with a quiet-zone border, scaled to the largest size that fits
 * the 64 px height and centred. */
static char    s_qr_text[160];
static int64_t s_qr_until_us;
static int64_t s_splash_until_us;   /* on-demand boot-screen preview window */
static bool    s_anim_on;           /* logo flap: loops until the next button press */
static int64_t s_anim_deadline_us;  /* safety auto-stop if no press dismisses it */
static int     s_anim_frame;        /* advances once per rendered flap frame */
/* WiFi-onboarding QR: when non-empty it is painted PERSISTENTLY (no expiry),
 * overriding the status page, until the next reboot — the operator may take a
 * while to scan + provision, and once they submit creds the box reboots into
 * STA so this slot starts empty again. Sized for the worst-case escaped
 * SSID (32→64) + password (63→126) + schema overhead; a string this long
 * won't fit a v6 QR and render_qr fails gracefully, but the common
 * onboarding string (~50 B) encodes fine. */
static char    s_onboard_qr[210];

/* ── paged status display + transient overlays ─────────────────────────── */
/* The refresh task picks ONE thing to paint each tick, by precedence:
 *   reboot (synchronous, off-loop) > reset-hold bar > OTA bar > onboarding QR
 *   > timed QR > splash preview > event flash > the current status page.
 * The status page is one of s_pages[], cycled by the BOOT button (short press
 * → oled_next_page()). Each page carries its own refresh interval so only the
 * animated VU page pays a fast-refresh bus0 cost, and only while it's visible. */
static volatile int s_page;             /* index into s_pages[] (button-cycled) */

/* Reset-hold progress (written by the BOOT-button task, read here): held_ms<0
 * = not holding; else draw a "HOLD = FACTORY RESET" bar filling to hold_ms. */
static volatile int s_reset_held_ms = -1;
static volatile int s_reset_hold_ms;

/* Event flash: a number of SSD1306 invert toggles still to do (blinks the whole
 * panel, whatever's on screen) and the ms each toggle holds — patterned per
 * event kind by oled_flash(). */
static volatile int s_invert_pulses;
static volatile int s_invert_step_ms = 100;
static int64_t      s_flash_cooldown_us;   /* rate-limit so bursts can't monopolize */

/* Ambient-light readout (camera AE: gain + exposure), cached + throttled — see
 * page_levels. Exposure is the responsive proxy; gain shown for reference. */
static int     s_light_gain = -1;
static int     s_light_exp  = -1;
static int64_t s_light_next_us;

/* Reboot screen reason word ("OTA", "FACTORY", …); optional, generic if empty. */
static char    s_reboot_reason[16];

/* Set true the instant a reboot starts (oled_show_rebooting, from the shutdown
 * handler) so the refresh task stops flushing and can't overwrite the
 * "REBOOTING" frame in the window before the chip actually resets. */
static volatile bool s_rebooting;

static void qr_display_cb(esp_qrcode_handle_t qr) {
    int n = esp_qrcode_get_size(qr);
    if (n <= 0)
        return;
    /* Use the WHOLE panel: light every pixel (the OLED is emissive, so a lit
     * pixel reads as the white background a scanner expects), then draw the
     * QR as large as the 64 px height allows, centred. The entire lit
     * surround IS the quiet zone — best contrast + biggest modules. */
    int scale = OLED_H / n;
    if (scale < 1)
        scale = 1;
    int dim = n * scale;
    int ox = (OLED_W - dim) / 2;
    int oy = (OLED_H - dim) / 2;

    memset(s_fb, 0xFF, sizeof(s_fb));        /* whole display lit = white bg */
    for (int my = 0; my < n; my++)           /* dark modules = unlit */
        for (int mx = 0; mx < n; mx++) {
            if (!esp_qrcode_get_module(qr, mx, my))
                continue;
            int bx = ox + mx * scale, by = oy + my * scale;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    fb_set(bx + sx, by + sy, 0);
        }
    ESP_LOGI(TAG, "QR rendered: %d modules, scale %d (%dpx)", n, scale, dim);
}

static void render_qr(const char *text) {
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_display_cb;
    cfg.max_qrcode_version = 6;              /* keep modules small enough to scale on 64 px */
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    if (esp_qrcode_generate(&cfg, text) != ESP_OK) {
        fb_clear();
        fb_text(0, 0, "QR encode failed");
    }
}

bool oled_show_qr(const char *text) {
    if (!text || !text[0])
        return false;
    strlcpy(s_qr_text, text, sizeof(s_qr_text));
    s_qr_until_us = esp_timer_get_time() + 90LL * 1000000;  /* show ~90 s */
    return s_ok;
}

/* Backslash-escape the WIFI: QR-schema meta characters ( \ ; , : " ) in a
 * field. Our generated SSID/password never contain these, but an operator-set
 * custom AP password might, so encode correctly regardless. */
static void wifi_qr_escape(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 2 < cap; i++) {
        char c = in[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"')
            out[o++] = '\\';
        out[o++] = c;
    }
    if (cap)
        out[o < cap ? o : cap - 1] = '\0';
}

bool oled_show_wifi_qr(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0])
        return false;
    char es[65], ep[127];               /* worst-case escaped SSID / password */
    wifi_qr_escape(es, sizeof(es), ssid);
    wifi_qr_escape(ep, sizeof(ep), pass);
    char qr[sizeof(s_onboard_qr)];
    if (pass && pass[0])
        snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", es, ep);
    else
        snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", es);
    strlcpy(s_onboard_qr, qr, sizeof(s_onboard_qr));
    ESP_LOGI(TAG, "WiFi onboarding QR set for SSID '%s' (%s)",
             ssid, (pass && pass[0]) ? "WPA2" : "open");
    return s_ok;
}

bool oled_show_boot(void) {
    /* Preview the boot screen (custom logo or text splash) on demand for
     * ~10 s — so you can see an uploaded logo without rebooting. */
    s_splash_until_us = esp_timer_get_time() + 10LL * 1000000;
    return s_ok;
}

/* ── custom boot logo (NVS) ───────────────────────────────────────────── */
/* A logo is just a full-frame SSD1306 bitmap = exactly sizeof(s_fb) bytes
 * (1024, page-major, LSB=top), stored as one NVS blob. Optional. */
#define LOGO_NS  "oled"
#define LOGO_KEY "logo"

bool oled_set_logo(const uint8_t *data, size_t len) {
    if (!data || len != sizeof(s_fb))
        return false;
    nvs_handle_t h;
    if (nvs_open(LOGO_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_set_blob(h, LOGO_KEY, data, len);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

void oled_clear_logo(void) {
    nvs_handle_t h;
    if (nvs_open(LOGO_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_key(h, LOGO_KEY);
    nvs_commit(h);
    nvs_close(h);
}

bool oled_get_logo(unsigned char *out, size_t cap) {
    if (!out || cap < sizeof(s_fb))
        return false;
    nvs_handle_t h;
    if (nvs_open(LOGO_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = sizeof(s_fb);
    esp_err_t e = nvs_get_blob(h, LOGO_KEY, out, &len);
    nvs_close(h);
    return e == ESP_OK && len == sizeof(s_fb);
}

void oled_get_boot_logo(unsigned char *out, size_t cap) {
    /* The bitmap render_splash would show: NVS custom logo, else the baked
     * default. `out` must hold OLED_LOGO_BYTES. */
    if (!out || cap < sizeof(s_fb))
        return;
    if (!oled_get_logo(out, cap))
        memcpy(out, DEFAULT_LOGO, sizeof(s_fb));
}

/* Load a stored logo directly into the framebuffer. Returns false if none. */
static bool load_logo(void) {
    nvs_handle_t h;
    if (nvs_open(LOGO_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = sizeof(s_fb);
    esp_err_t e = nvs_get_blob(h, LOGO_KEY, s_fb, &len);
    nvs_close(h);
    return e == ESP_OK && len == sizeof(s_fb);
}

/* ── content ──────────────────────────────────────────────────────────── */

static void render_splash(void) {
    /* Custom logo from NVS wins; otherwise the baked-in default logo. Both
     * are a full 1024-byte framebuffer. */
    if (!load_logo())
        memcpy(s_fb, DEFAULT_LOGO, sizeof(s_fb));
    /* FW version, row 0 — overlaid on the boot screen (logo or text). The FULL
     * git-describe string (e.g. "v0.7.2-6-ga3a7551-dirty"), NOT trimmed to the
     * bare tag: a trimmed "v0.7.2" reads like the stable release even on a dirty
     * dev build, which is misleading. Right-aligned when it fits; a longer
     * string clamps to col 0 and clips the right edge — the "-<n>-g<hash>"
     * suffix is enough to tell it's not the clean tag. Opaque glyphs stay
     * readable over a logo. */
    const esp_app_desc_t *app = esp_app_get_description();
    char ver[40];
    snprintf(ver, sizeof(ver), "%s", app ? app->version : "?");
    int col = OLED_COLS_TXT - (int)strlen(ver);
    if (col < 0)
        col = 0;
    fb_text(col, 0, ver);
}

/* Compact 1-char unit for the tiny panel ("°C"→C, "%"→%, else as-is). */
static const char *oled_unit(const char *u) {
    if (!u) return "";
    if (strchr(u, 'C')) return "C";     /* "°C" */
    if (u[0] == '%')    return "%";
    return u;                            /* "hPa" */
}

/* One row per sensor from the registry: "<label> <v0><u0> <v1><u1>", from
 * cached reads. Returns false (caller skips the row) when no channel has a
 * value — i.e. the sensor is absent / not reading — so only AVAILABLE
 * sensors show. */
__attribute__((unused))
static bool fmt_sensor_row(char *line, size_t n, const cb_sensor_t *s) {
    int off = snprintf(line, n, "%-4s", s->label);
    bool any = false;
    for (size_t ci = 0; ci < s->n_chans && off > 0 && off < (int)n - 1; ci++) {
        const cb_chan_t *c = &s->chans[ci];
        float v;
        if (c->read(&v) && isfinite(v)) {
            off += snprintf(line + off, n - off, " %.*f%s", c->decimals,
                            (double)v, oled_unit(c->unit));
            any = true;
        }
    }
    return any;
}

/* ── main-screen status-bar pictograms (Nokia-style), all on page row 0 ──── */

/* WiFi signal as 4 ascending bars at pixel x. AP mode → "AP"; down → 'x'. */
static void draw_wifi_icon(int x, int rssi, bool conn, bool ap) {
    if (ap)    { fb_glyph(x, 0, 'A'); fb_glyph(x + 6, 0, 'P'); return; }
    if (!conn) { fb_glyph(x, 0, 'x'); return; }
    int lvl = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -73 ? 2 : rssi >= -82 ? 1 : 0;
    for (int b = 0; b < 4; b++) {
        int bx = x + b * 3, h = 2 + b * 2;          /* heights 2,4,6,8 px */
        for (int yy = 7; yy > 7 - h; yy--) {
            int on = (b < lvl) || (yy == 7);        /* lit bars solid; rest = base dot */
            fb_set(bx, yy, on); fb_set(bx + 1, yy, on);
        }
    }
}

/* Battery icon with body right-edge at pixel xr (row 0): outline + nub + SOC
 * fill, and the SOC% printed just left of it. Absent (USB) → "USB", hollow. */
static void draw_batt_icon(int xr, int soc, bool present) {
    const int w = 13, h = 7, x0 = xr - w, y0 = 0;
    char t[16];
    if (present && soc >= 0) snprintf(t, sizeof(t), "%d%%", soc > 100 ? 100 : soc);
    else                     snprintf(t, sizeof(t), "USB");
    int tx = x0 - 3 - (int)strlen(t) * 6;
    for (size_t k = 0; t[k]; k++) fb_glyph(tx + (int)k * 6, 0, t[k]);
    for (int xx = x0; xx < x0 + w; xx++) { fb_set(xx, y0, 1); fb_set(xx, y0 + h - 1, 1); }
    for (int yy = y0; yy < y0 + h; yy++) { fb_set(x0, yy, 1); fb_set(x0 + w - 1, yy, 1); }
    fb_set(x0 + w, y0 + 2, 1); fb_set(x0 + w, y0 + 3, 1);            /* nub */
    if (present && soc >= 0) {
        int fill = (w - 2) * soc / 100;
        for (int yy = y0 + 1; yy < y0 + h - 1; yy++)
            for (int xx = x0 + 1; xx < x0 + 1 + fill; xx++) fb_set(xx, yy, 1);
    }
}

/* Main screen — a compact "old-Nokia" dashboard. Row 0 = status bar: WiFi bars ·
 * MQTT dot · power-tier code (PB/PH/PS/PE/PA/PM — INVERTED while actively
 * light-sleeping) · battery. Rows 1-3 = environment sensors; row 4 = mic on/off +
 * VU + VAD marker; row 5 = uptime/crashes; row 6 = clock; row 7 = IP. 21×8. */
static void render_page1(void) {
    char line[48];
    float v;
    fb_clear();

    /* ── row 0: status bar ── */
    if (wifi_mgr_softap_active()) {
        snprintf(line, sizeof(line), "AP %s %dcli", device_id_suffix(), wifi_mgr_ap_sta_count());
        fb_text(0, 0, line);
    } else {
        draw_wifi_icon(0, wifi_mgr_rssi(), wifi_mgr_is_connected(), false);
        fb_glyph(14, 0, mqtt_is_connected() ? 0x07 /* • */
                       : wifi_mgr_is_connected() ? 0x09 /* ○ */ : ' ');
        /* PWR: 2-letter power-tier code (PB/PH/PS/PE/PA/PM). The tier now carries
         * the sleep depth too, so there's no separate SLP code — the code is
         * INVERTED while the unit is actively light-sleeping (Eco/Sentinel). */
        const char *pc = profile_code();
        fb_glyph(26, 0, pc[0]); fb_glyph(32, 0, pc[1]);
        if (app_profile_sleeps()) fb_invert_rect(25, 0, 14, 8);
        /* Hibernate: countdown (seconds) to deep sleep, right after "PH". A PIR
         * edge resets it to the ds_pir_win_s window. */
        if (app_profile_is_hibernate()) {
            int secs = ds_seconds_to_sleep();
            if (secs < 0) secs = 0;
            if (secs > 9999) secs = 9999;   /* bound for the 8-char buffer */
            char cd[8];
            snprintf(cd, sizeof(cd), "%ds", secs);
            for (size_t k = 0; cd[k] && k < 6; k++)
                fb_glyph(42 + (int)k * 6, 0, cd[k]);
        }
        /* battery (far right) */
        float soc = -1; bool batp = chan_val("bat", "soc", &soc);
        draw_batt_icon(OLED_W - 1, batp ? (int)soc : -1, batp);
    }

    /* ── rows 1-3: environment sensors — labels left, values RIGHT-aligned into
     * two columns (left value ends at col 9, right value at col 20). ── */
    {
        char a[10];
        /* row 1: indoor / outdoor temperature */
        if (chan_val("bmp", "temp_bmp", &v)) snprintf(a, sizeof(a), "%.1f", (double)v); else strcpy(a, "--");
        fb_kv(0, 9, 1, "in", a);
        if (chan_val("sht0", "temp", &v)) snprintf(a, sizeof(a), "%.1f", (double)v); else strcpy(a, "--");
        fb_kv(11, 20, 1, "out", a);
        /* row 2: humidity / pressure */
        if (chan_val("sht0", "humidity", &v)) snprintf(a, sizeof(a), "%.0f%%", (double)v); else strcpy(a, "--");
        fb_kv(0, 9, 2, "RH", a);
        if (chan_val("bmp", "pressure", &v)) snprintf(a, sizeof(a), "%.0fhPa", (double)v); else strcpy(a, "--");
        fb_kv(11, 20, 2, "P", a);
        /* row 3: external temp / MCU temp */
        if (chan_val("sht1", "temp_ext", &v)) snprintf(a, sizeof(a), "%.1f", (double)v); else strcpy(a, "--");
        fb_kv(0, 9, 3, "ext", a);
        float mcu = diag_mcu_temp_c();
        snprintf(a, sizeof(a), "%.0fC", isfinite(mcu) ? (double)mcu : 0.0);
        fb_kv(11, 20, 3, "MCU", a);
    }

    /* ── row 4: mic — on/off + VU bar + '!' when live RMS crosses VAD thresh ── */
    {
        float db = audio_last_rms_dbfs();
        bool mic_on = app_config_get_bool("vad_enabled") && audio_ready();
        fb_text(0, 4, "mic");
        int pct = (int)((db + 60.0f) * (100.0f / 60.0f));
        fb_hbar(24, 33, 80, 6, mic_on ? pct : 0);
        if (mic_on && db > app_config_get_float("vad_thr_dbfs"))
            fb_glyph(108, 4, '!');
    }

    /* ── row 5: WiFi RSSI + consecutive-crash count (same two columns) ── */
    {
        char a[10];
        snprintf(a, sizeof(a), "%d", wifi_mgr_rssi());
        fb_kv(0, 9, 5, "rssi", a);
        snprintf(a, sizeof(a), "x%lu", (unsigned long)diag_consecutive_crashes());
        fb_kv(11, 20, 5, "rst", a);
    }

    /* ── row 6: uptime (replaces the wall-clock — useless without SNTP / across
     * hibernate wakes). The hibernate sleep-countdown lives in the top bar, so
     * it's not duplicated here. ── */
    {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        snprintf(line, sizeof(line), "up %lud %02lu:%02lu:%02lu",
                 (unsigned long)(up / 86400), (unsigned long)((up % 86400) / 3600),
                 (unsigned long)((up % 3600) / 60), (unsigned long)(up % 60));
        fb_text(0, 6, line);
    }

    /* ── row 7: IP — the SoftAP address when the AP is up (STA has none then,
     * which used to render a misleading "no-ip"), else the STA lease. ── */
    {
        char ip[20];
        if (wifi_mgr_softap_active())
            strlcpy(ip, AP_IP, sizeof(ip));
        else if (!wifi_mgr_get_ip_str(ip, sizeof(ip)))
            strcpy(ip, "no-ip");
        snprintf(line, sizeof(line), "ip %s", ip);
        fb_text(0, 7, line);
    }
}

/* ── extra status pages (cycled by the BOOT button) ────────────────────── */

/* Power: battery SOC/voltage (+ a SOC bar) and the solar shunt, plus mode.
 * Cached reads only (battery_soc/_vbat do live I²C; the registry caches them,
 * so we go through chan_val — and charge_rate, which has no cache, is left to
 * the web UI to keep the OLED off the bus). */
static void page_power(void) {
    char line[40];
    float soc = 0, vb = 0, v = 0, i = 0, p = 0;
    fb_clear();
    fb_text(0, 0, "POWER");
    draw_heartbeat();
    int row = 1;
    if (chan_val("bat", "soc", &soc) && chan_val("bat", "v_bat", &vb)) {
        snprintf(line, sizeof(line), "bat %3.0f%%  %.2fV", (double)soc, (double)vb);
        fb_text(0, row++, line);
        fb_hbar(0, row * 8 + 1, 120, 6, (int)soc);
        row++;
    } else {
        fb_text(0, row++, "bat: absent");
    }
    if (chan_val("ina", "solar_v", &v)) {
        chan_val("ina", "solar_i", &i);
        chan_val("ina", "solar_p", &p);
        snprintf(line, sizeof(line), "sol %.2fV %.0fmA", (double)v, (double)(i * 1000.0f));
        fb_text(0, row++, line);
        snprintf(line, sizeof(line), "sol %.2fW", (double)p);
        fb_text(0, row++, line);
    } else {
        fb_text(0, row++, "solar: absent");
    }
    snprintf(line, sizeof(line), "mode: %s", mode_str());
    fb_text(0, row++, line);
}

/* Camera / capture pipeline: mode, lifetime shots, the photo queue (depth /
 * bytes / sent / dropped) and the PIR + reed motion sensors. */
static void page_camera(void) {
    char line[40];
    fb_clear();
    fb_text(0, 0, "CAMERA");
    draw_heartbeat();
    snprintf(line, sizeof(line), "mode  %s", mode_str());
    fb_text(0, 1, line);
    snprintf(line, sizeof(line), "shots %lu", (unsigned long)camera_capture_count());
    fb_text(0, 2, line);
    snprintf(line, sizeof(line), "q %u  %luKB", (unsigned)photo_queue_depth(),
             (unsigned long)(photo_queue_bytes() / 1024));
    fb_text(0, 3, line);
    snprintf(line, sizeof(line), "sent%lu drop%lu",
             (unsigned long)photo_queue_drained_total(),
             (unsigned long)photo_queue_dropped_total());
    fb_text(0, 4, line);
    if (pir_active_count() > 0) {
        snprintf(line, sizeof(line), "PIR mot=%lu%s",
                 (unsigned long)pir_motion_count_nth(0), pir_wedged() ? " WEDGE" : "");
        fb_text(0, 5, line);
    }
    if (reed_active_count() > 0) {
        snprintf(line, sizeof(line), "reed: %s", reed_is_closed() ? "closed" : "open");
        fb_text(0, 6, line);
    }
}

/* Network detail: SSID, IP, BSSID, RSSI + MQTT, domain, FW version, and a
 * local clock once SNTP has synced (epoch past 2023). */
static void page_net(void) {
    char line[40];
    fb_clear();
    fb_text(0, 0, "NETWORK");
    draw_heartbeat();
    const char *ssid = wifi_mgr_get_ssid();
    snprintf(line, sizeof(line), "ss %.17s", ssid[0] ? ssid : "-");
    fb_text(0, 1, line);
    char ip[20];
    if (!wifi_mgr_get_ip_str(ip, sizeof(ip)))
        strcpy(ip, "no-ip");
    snprintf(line, sizeof(line), "ip %s", ip);
    fb_text(0, 2, line);
    char bssid[18];
    if (wifi_mgr_get_bssid(bssid, sizeof(bssid))) {
        snprintf(line, sizeof(line), "bs %s", bssid);
        fb_text(0, 3, line);
    }
    snprintf(line, sizeof(line), "rssi %ddBm  mqtt%c", wifi_mgr_rssi(),
             mqtt_is_connected() ? '+' : '-');
    fb_text(0, 4, line);
    /* hostname + domain (the box's address) — the FQDN is wider than the
     * 21-char panel, so put the host on its own row and the domain under it.
     * Version lives on the DIAG page now. */
    fb_text(0, 5, device_id());                  /* e.g. "cb-ex01" */
    const char *dom = wifi_mgr_get_domain();
    snprintf(line, sizeof(line), ".%s", dom[0] ? dom : "(no domain)");
    fb_text(0, 6, line);                          /* ".lan" */
    time_t now = time(NULL);
    if (now > CB_CLOCK_SYNCED_EPOCH) {     /* SNTP synced (epoch past Nov 2023) */
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(line, sizeof(line), "%a %d.%m %H:%M", &lt);
        fb_text(0, 7, line);
    }
}

/* Environment: the two temperatures BIG and unlabelled — inside (BMP388) on the
 * LEFT, outside (SHT41 bus0) on the RIGHT (position is the label). Below, smaller:
 * humidity (SHT41 bus0) and battery %. The bus1 SHT41 ("ext") is omitted. */
static void page_env(void) {
    char b[12], line[24];
    float v;
    fb_clear();
    /* big inside temp, left */
    if (chan_val("bmp", "temp_bmp", &v)) snprintf(b, sizeof(b), "%.1f", (double)v);
    else                                 snprintf(b, sizeof(b), "--");
    fb_text_2x(2, 0, b);
    /* big outside temp, right-aligned (2x glyph advance = 12 px) */
    if (chan_val("sht0", "temp", &v))    snprintf(b, sizeof(b), "%.1f", (double)v);
    else                                 snprintf(b, sizeof(b), "--");
    int x = OLED_W - (int)strlen(b) * 12;
    fb_text_2x(x < 0 ? 0 : x, 0, b);
    /* smaller, filling the lower half: humidity, pressure, battery, MCU temp */
    if (chan_val("sht0", "humidity", &v)) snprintf(line, sizeof(line), "H: %.1f %%", (double)v);
    else                                  strcpy(line, "H: -- %");
    fb_text(0, 3, line);
    if (chan_val("bmp", "pressure", &v))  snprintf(line, sizeof(line), "P: %.0f hPa", (double)v);
    else                                  strcpy(line, "P: -- hPa");
    fb_text(0, 4, line);
    if (chan_val("bat", "soc", &v))       snprintf(line, sizeof(line), "B: %.1f %%", (double)v);
    else                                  strcpy(line, "B: -- %");
    fb_text(0, 5, line);
    float mcu = diag_mcu_temp_c();
    snprintf(line, sizeof(line), "MCU: %.0f C", isfinite(mcu) ? (double)mcu : 0.0);
    fb_text(0, 6, line);
    /* Grove add-ons on the spare bottom row, armed-only (a stock board
     * keeps the row empty). Cached values exclusively — distance from
     * the sonar poll task, moisture from the last telemetry-tick read
     * (seeded once at soil arm) — so the page stays off the pin/ADC.
     * "--" = armed but nothing measured yet (sonar: no echo). */
    if (sonar_ready() || soil_ready()) {
        char d[12] = "--", m[20] = "--";
        if (sonar_ready() && sonar_last_cm(&v))
            snprintf(d, sizeof(d), "%.0fcm", (double)v);
        else if (sonar_ready() && sonar_is_clear())
            snprintf(d, sizeof(d), "inf");   /* ≥ sonar_clear_cm: no target */
        float soil_mv, soil_pct;
        if (soil_ready() && soil_last(&soil_mv, &soil_pct)) {
            /* mV always (it's what the dry/wet calibration is copied
             * from — calibrate straight off the panel), pct appended
             * once the knobs are non-degenerate. Worst case fills the
             * whole 21-char row exactly: "D:430cm M:3100mV 100%". */
            if (isfinite(soil_pct))
                snprintf(m, sizeof(m), "%.0fmV %.0f%%",
                         (double)soil_mv, (double)soil_pct);
            else
                snprintf(m, sizeof(m), "%.0fmV", (double)soil_mv);
        }
        if (sonar_ready() && soil_ready())
            snprintf(line, sizeof(line), "D:%s M:%s", d, m);
        else if (sonar_ready())
            snprintf(line, sizeof(line), "D: %s", d);
        else
            snprintf(line, sizeof(line), "M: %s", m);
        fb_text(0, 7, line);
    }
}

/* Diagnostics: a live module ✓/✗ grid (mirrors selftest's checks via the same
 * *_ready() predicates, no stored struct) + reset reason, crash count, heap,
 * uptime. */
static void page_diag(void) {
    char line[40];
    fb_clear();
    fb_text(0, 0, "DIAG");
    draw_heartbeat();
    const struct { const char *n; bool ok; } mod[] = {
        {"sht", cb_sensor_ready("sht0")}, {"ext", cb_sensor_ready("sht1")}, {"bmp", cb_sensor_ready("bmp")},
        {"cam", camera_ready()},  {"sd", sd_storage_ready()}, {"pir", pir_ready()},
        {"mic", audio_ready()},   {"wifi", wifi_mgr_is_connected()},
        {"mqtt", mqtt_is_connected()},
    };
    int row = 1, col = 0, off = 0;
    for (size_t k = 0; k < sizeof(mod) / sizeof(mod[0]); k++) {
        off += snprintf(line + off, sizeof(line) - off, "%-4s%c ", mod[k].n,
                        mod[k].ok ? 0x07 /* • */ : 'x');
        if (++col == 3) {
            fb_text(0, row++, line);
            col = 0; off = 0; line[0] = 0;
        }
    }
    if (col)
        fb_text(0, row++, line);
    snprintf(line, sizeof(line), "rst %.9s x%lu", diag_reset_reason_name(),
             (unsigned long)diag_consecutive_crashes());
    fb_text(0, row++, line);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(line, sizeof(line), "heap%luk up%lud%02ld:%02ld",
             (unsigned long)(esp_get_free_heap_size() / 1024),
             (unsigned long)(up / 86400), (unsigned long)((up % 86400) / 3600),
             (unsigned long)((up % 3600) / 60));
    fb_text(0, row++, line);
    if (row <= 7) {     /* firmware version (moved here off the net page) */
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(line, sizeof(line), "fw %.17s", app ? app->version : "?");
        fb_text(0, row++, line);
    }
}

/* Brightness 0..100 from the camera AE "total exposure" (aec_value × analog
 * gain). The AE loop lengthens exposure first, then raises gain, as the scene
 * darkens, so the product is a monotonic inverse-luminance proxy across the
 * WHOLE range — unlike raw exposure, which pins at its ceiling for anything
 * dimmer than direct sun (the old bar couldn't tell "lit room" from "lens
 * covered" — both sat at the ceiling). Log-mapped between a bright-scene floor
 * and the dark ceiling (E_MAX × G_MAX). Because the IR-fire decision keys off
 * gain ≥ ir_agc_thresh (exposure already maxed there), the SAME map turns that
 * threshold into a bar position (the marker in page_levels). Bench-calibrated;
 * nudge the two bounds if a board reads consistently high or low. */
#define LIGHT_E_MAX        790.0f  /* aec_value ceiling at the active framesize  */
#define LIGHT_G_MAX        32.0f   /* matches the 32x cam_gainceil default       */
#define LIGHT_TOTAL_BRIGHT 25.0f   /* exposure×gain in a bright scene → bar full */
static int light_brightness_pct(float total) {
    float tmax = LIGHT_E_MAX * LIGHT_G_MAX;
    if (total < LIGHT_TOTAL_BRIGHT) total = LIGHT_TOTAL_BRIGHT;
    if (total > tmax) total = tmax;
    float dark = (log10f(total) - log10f(LIGHT_TOTAL_BRIGHT)) /
                 (log10f(tmax) - log10f(LIGHT_TOTAL_BRIGHT));   /* 0 bright .. 1 dark */
    int pct = (int)((1.0f - dark) * 100.0f + 0.5f);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

/* Live levels: a mic VU bar (audio RMS) and a camera-AE brightness bar, each
 * with a threshold tick (fb_bar_marker) at its trigger point. No title row — the
 * "mic …dB" / "g… e… …%" lines self-label the bars, and dropping it leaves a
 * clean blank row above each bar so the tick is visible (was buried under the
 * mic text). Both ticks read the LIVE config every refresh (vad_thr_dbfs /
 * ir_agc_thresh), so changing a threshold moves its tick — nothing hardcoded.
 * Fast (300 ms) refresh for a lively mic bar; the AE read is throttled to ~1 Hz
 * so it doesn't hammer the SCCB bus.
 *
 *   row 0  "mic -52dB"          + heartbeat
 *   y14-15 VAD tick · y16-26    mic VU bar
 *   row 4  "g5 e790 30%"
 *   y46-47 IR tick  · y48-58    light brightness bar              */
static void page_levels(void) {
    char line[24];
    fb_clear();
    draw_heartbeat();
    /* MIC: VU (−60..0 dBFS → 0..100 %) + live VAD-threshold tick */
    float db = audio_last_rms_dbfs();
    snprintf(line, sizeof(line), "mic %.0fdB", (double)db);
    fb_text(0, 0, line);
    fb_hbar(2, 16, 124, 11, (int)((db + 60.0f) * (100.0f / 60.0f)));
    if (app_config_get_bool("vad_enabled")) {
        float thr = app_config_get_float("vad_thr_dbfs");
        fb_bar_marker(2, 16, 124, (int)((thr + 60.0f) * (100.0f / 60.0f)));
    }
    /* LIGHT: brightness from camera AE (exposure × gain, throttled) */
    int64_t now = esp_timer_get_time();
    if (now >= s_light_next_us) {
        s_light_next_us = now + 1000000;          /* ~1 Hz; AE read is on the SCCB bus */
        int g, e;
        if (camera_ready() && camera_get_ae(&g, &e)) { s_light_gain = g; s_light_exp = e; }
        else                                         { s_light_gain = -1; s_light_exp = -1; }
    }
    if (s_light_exp >= 0) {
        int gv = s_light_gain > 0 ? s_light_gain : 1;
        int bri = light_brightness_pct((float)s_light_exp * (float)gv);
        snprintf(line, sizeof(line), "g%d e%d %d%%", s_light_gain, s_light_exp, bri);
        fb_text(0, 4, line);
        fb_hbar(2, 48, 124, 11, bri);
        /* IR fires when gain ≥ ir_agc_thresh (exposure already at its ceiling
         * there), so mark that gain on the brightness bar: when the live bar
         * drops below the tick, the next capture lights the IR illuminator. */
        if (app_config_get_bool("ir_led_enabled")) {
            int thr = (int)app_config_get_int("ir_agc_thresh");
            fb_bar_marker(2, 48, 124, light_brightness_pct(LIGHT_E_MAX * (float)thr));
        }
    } else {
        fb_text(0, 4, "light  n/a");
    }
}

/* Web-URL QR: scan it straight off the panel to open the box's own page.
 * Reuses render_qr (full-frame). */
static void page_weburl_qr(void) {
    char url[96], ip[20];
    /* Prefer the IP URL: phones reliably linkify "https://<ip>/" and the device
     * cert carries the IP as a SAN (no name-mismatch), whereas the FQDN's
     * underscore (cb-ex01) makes most scanners show plain text, not
     * an openable link. Fall back to the FQDN when no IP yet (e.g. AP mode). */
    if (wifi_mgr_get_ip_str(ip, sizeof(ip)))
        snprintf(url, sizeof(url), "https://%s/", ip);
    else
        device_url(url, sizeof(url), wifi_mgr_get_domain());
    render_qr(url);
}

/* The cycle order. refresh_ms is per-page so only the VU animates fast. */
typedef struct { void (*render)(void); int refresh_ms; } oled_page_t;
static const oled_page_t s_pages[] = {
    { render_page1,   3000 },   /* 0: status (default) */
    { page_power,     3000 },
    { page_camera,    3000 },
    { page_net,       3000 },
    { page_env,       1000 },   /* 1 s so the Grove boost (sonar/soil sample
                                 * ~2 Hz while this page shows) reads live */
    { page_diag,      3000 },
    { page_levels,     300 },   /* fast — animated mic VU + light bars */
    { page_weburl_qr, 6000 },
};
#define OLED_N_PAGES (sizeof(s_pages) / sizeof(s_pages[0]))

/* ── transient overlays (higher precedence than the status page) ───────── */

/* The "HOLD = FACTORY RESET" bar while the BOOT button is held. */
static void render_reset_progress(void) {
    char line[24];
    int hold = s_reset_hold_ms > 0 ? s_reset_hold_ms : 10000;
    int held = s_reset_held_ms < 0 ? 0 : s_reset_held_ms;
    fb_clear();
    fb_text(0, 0, "HOLD = FACTORY");
    fb_text(0, 1, "RESET");
    fb_hbar(2, 26, 124, 18, held * 100 / hold);
    snprintf(line, sizeof(line), "%d / %d s", held / 1000, hold / 1000);
    fb_text(0, 7, line);
}

/* OTA download progress bar (s_ota_pct comes from ota.c). */
static void render_ota_progress(void) {
    char line[24];
    int pct = ota_progress_pct();
    if (pct < 0) pct = 0;
    fb_clear();
    fb_text(0, 0, "OTA UPDATE");
    snprintf(line, sizeof(line), "downloading %d%%", pct);
    fb_text(0, 2, line);
    fb_hbar(2, 28, 124, 18, pct);
    fb_text(0, 7, "do not power off");
}


/* The synchronous "REBOOTING" frame, drawn from the shutdown handler. */
static void render_rebooting(void) {
    fb_clear();
    fb_text_2x(8, 12, "REBOOT");
    if (s_reboot_reason[0])
        fb_text(0, 5, s_reboot_reason);
    fb_text(0, 7, "restarting...");
}

/* Static "hibernate" frame painted just before deep sleep. The SSD1306 holds
 * its RAM (we never cut oled_pwr), so this stays on the panel through the whole
 * sleep, telling an operator the unit is intentionally asleep. */
static void render_deepsleep(int next_wake_s) {
    char line[32], when[16];
    fb_clear();
    /* Blit the sleeping-kitty bitmap full-screen (page-major, lit bit = outline). */
    for (int y = 0; y < SLEEP_CAT_H; y++)
        for (int x = 0; x < SLEEP_CAT_W; x++)
            if ((SLEEP_CAT[(y >> 3) * SLEEP_CAT_W + x] >> (y & 7)) & 1)
                fb_set(x, y, 1);
    /* Wake info on the bottom row, over a cleared strip so it stays legible
     * over the cat's lower outline. */
    if (next_wake_s >= 3600)
        snprintf(when, sizeof(when), "%dh%dm", next_wake_s / 3600, (next_wake_s % 3600) / 60);
    else if (next_wake_s >= 60)
        snprintf(when, sizeof(when), "%dm", (next_wake_s + 30) / 60);
    else
        snprintf(when, sizeof(when), "%ds", next_wake_s);
    /* Bottom line: when the clock is real, the time it fell asleep + the wake
     * interval ("HH:MM +15m") — more useful than a static "wake in 15m" frozen
     * on the panel while real time marches on. No SNTP yet → plain interval. */
    time_t now = time(NULL);
    if (now > CB_CLOCK_SYNCED_EPOCH) {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(line, sizeof(line), "%02d:%02d +%s", lt.tm_hour, lt.tm_min, when);
    } else {
        snprintf(line, sizeof(line), "wake in %s", when);
    }
    for (int y = 56; y < 64; y++)
        for (int x = 0; x < OLED_W; x++) fb_set(x, y, 0);
    fb_text(0, 7, line);
}

/* One pixel of the baked-in DEFAULT logo (page-major, LSB=top). Always the
 * default, never the NVS custom logo — the flap is its own thing. */
static inline int logo_px(int x, int y) {
    if ((unsigned)x >= OLED_W || (unsigned)y >= OLED_H)
        return 0;
    return (DEFAULT_LOGO[(y >> 3) * OLED_W + x] >> (y & 7)) & 1;
}

/* Animate the boot screen: draw the DEFAULT logo, but the bird's wing region
 * (the fanned primary feathers, WING_BOX) is ROTATED around the shoulder pivot
 * by a sine of the frame counter — a computed wing-beat (forward-mapped with a
 * 2×2 fill so the rotation leaves no holes), everything else (body, WiFi arcs,
 * birdhouse, caption) static. On top: the sun's rays twinkle (grow/shrink),
 * the thermometer's mercury rises/falls in its stem, and the WiFi antenna's
 * signal arcs radiate outward (none → inner → inner+outer). The version is
 * overlaid top-right exactly like render_splash (default logo + version, even
 * if a custom boot logo is configured). All regions/pivots were picked off the
 * rendered bitmap; tweak the WING_* / SUN_* / THERMO_* / antenna_arc() constants
 * to retune. */
#define WING_X0 28
#define WING_Y0 3
#define WING_X1 47
#define WING_Y1 15
#define WING_PX 45.0f          /* shoulder pivot */
#define WING_PY 15.0f
#define WING_AMP 0.38f         /* flap amplitude, rad (~22°) */
#define WING_SPEED 0.45f       /* phase advance per frame */
#define SUN_CX 72              /* sun-disc centre (for the twinkle rays) */
#define SUN_CY 5
#define THERMO_X0 78           /* thermometer stem interior (mercury column) */
#define THERMO_X1 79
#define THERMO_TOP_Y 11        /* mercury extent: TOP_Y (hot) .. BULB_Y (cold) */
#define THERMO_BULB_Y 18

/* WiFi-antenna signal-arc level at (x,y): 0 = not an arc (or the static
 * emitter/stand), 1 = inner arc, 2 = outer arc. Drives the radiating pulse. */
static int antenna_arc(int x, int y) {
    if (y < 17 || y > 28) return 0;
    if (x == 85 || x == 86 || x == 87 || x == 92 || x == 93) return 1;
    if (x == 83 || x == 84 || x == 94 || x == 95 || x == 96) return 2;
    return 0;
}

static void render_logo_flap(void) {
    fb_clear();
    float th = WING_AMP * sinf((float)s_anim_frame * WING_SPEED);
    float c = cosf(th), s = sinf(th);
    /* antenna signal pulse: arcs radiate outward — none → inner → inner+outer */
    int pulse = (s_anim_frame / 5) % 3;
    /* base: the whole logo except the wing region; antenna arcs gated by pulse */
    for (int y = 0; y < OLED_H; y++)
        for (int x = 0; x < OLED_W; x++) {
            if (!logo_px(x, y)) continue;
            if (x >= WING_X0 && x <= WING_X1 && y >= WING_Y0 && y <= WING_Y1)
                continue;                       /* wing drawn rotated below */
            if (antenna_arc(x, y) > pulse)
                continue;                       /* arc not lit yet this step */
            fb_set(x, y, 1);
        }
    /* thermometer: fill the hollow stem from the bulb up to a level that
     * rises and falls (a temperature reading sweeping up and down). */
    int mtop = THERMO_BULB_Y - (int)lrintf((float)(THERMO_BULB_Y - THERMO_TOP_Y)
                   * (0.5f + 0.5f * sinf((float)s_anim_frame * 0.18f)));
    for (int yy = mtop; yy <= THERMO_BULB_Y; yy++) {
        fb_set(THERMO_X0, yy, 1);
        fb_set(THERMO_X1, yy, 1);
    }
    /* wing: rotate its pixels around the shoulder (2×2 fill closes gaps) */
    for (int y = WING_Y0; y <= WING_Y1; y++)
        for (int x = WING_X0; x <= WING_X1; x++) {
            if (!logo_px(x, y)) continue;
            float dx = (float)x - WING_PX, dy = (float)y - WING_PY;
            int rx = (int)lrintf(WING_PX + dx * c - dy * s);
            int ry = (int)lrintf(WING_PY + dx * s + dy * c);
            fb_set(rx, ry, 1);     fb_set(rx + 1, ry, 1);
            fb_set(rx, ry + 1, 1); fb_set(rx + 1, ry + 1, 1);
        }
    /* sun twinkle: 5 spokes (up + the four diagonals — the horizontal
     * directions are boxed in by the WiFi arcs and the "!" mark) that grow
     * and shrink for a shine, rather than a hard on/off blink. */
    static const int rd[5][2] = {{0,-1},{-1,-1},{1,-1},{-1,1},{1,1}};
    int rmax = 3 + (int)lrintf(2.0f * (0.5f + 0.5f * sinf((float)s_anim_frame * 0.25f)));
    for (int i = 0; i < 5; i++)
        for (int r = 2; r <= rmax; r++)
            fb_set(SUN_CX + rd[i][0] * r, SUN_CY + rd[i][1] * r, 1);
    /* version, top-right — FULL git-describe (not trimmed to the bare tag, so a
     * dirty dev build never masquerades as the stable release; same as
     * render_splash). Clips the right edge if it overflows the row. */
    const esp_app_desc_t *d = esp_app_get_description();
    char v[40];
    snprintf(v, sizeof(v), "%s", d ? d->version : "?");
    int col = OLED_COLS_TXT - (int)strlen(v);
    if (col < 0) col = 0;
    fb_text(col, 0, v);
    s_anim_frame++;
}

static const uint32_t s_flap_kf[] = {
    0x293078, 0x293078, 0x000078, 0x293078, 0x000078, 0x20B078,
    0x293078, 0x000078, 0x310078, 0x000168, 0x188078, 0x000168,
    0x20B078, 0x0000F0, 0x188078, 0x0000F0, 0x14A078, 0x0000F0,
    0x1B8078, 0x000078, 0x1EE078, 0x000078, 0x1D2078, 0x1B8078,
    0x000078, 0x1880A0, 0x2930A0, 0x3100A0, 0x370078, 0x000078,
    0x2BA078, 0x310078, 0x000078, 0x293078, 0x000078, 0x20B078,
    0x24B078, 0x1EE078, 0x0000F0, 0x20B078, 0x20B078, 0x000078,
    0x20B078, 0x000078, 0x20B078, 0x24B078, 0x293078, 0x20B078,
    0x000078, 0x1B8078, 0x188078, 0x000168, 0x00012C,
};

void oled_anim_logo(void) {
    if (!s_ok) return;
    s_anim_frame = 0;
    s_anim_on = true;
    s_anim_deadline_us = esp_timer_get_time() + 90LL * 1000000;  /* safety cap */

    speaker_note_t seq[sizeof(s_flap_kf) / sizeof(*s_flap_kf)];
    for (size_t i = 0; i < sizeof(seq) / sizeof(*seq); i++) {
        seq[i].freq_hz = (uint16_t)(s_flap_kf[i] >> 12);
        seq[i].ms      = (uint16_t)(s_flap_kf[i] & 0xFFF);
    }
    audiofx_loop(seq, sizeof(seq) / sizeof(*seq));
}
bool oled_anim_running(void) { return s_anim_on; }
void oled_anim_stop(void) { s_anim_on = false; audiofx_stop(); }

/* Is an OLED actually on the bus? Require CONSECUTIVE ACKs, not just one:
 * bus0 produces random single-shot false-ACKs across many addresses (SDA
 * rise-time, same artefact documented for bus1), so a stray 0x3C ACK must
 * not be mistaken for a present display. A real SSD1306 answers every
 * probe; a phantom can't sustain a streak. */
static bool oled_detect(void) {
    int consec = 0;
    for (int i = 0; i < 30 && consec < 3; i++) {
        if (i2c_xport_probe(&s_xport, I2C_CMD_TMO_MS))
            consec++;
        else { consec = 0; vTaskDelay(pdMS_TO_TICKS(40)); }
    }
    return consec >= 3;
}

bool oled_probe_present(void) {
    /* Synchronous presence check for the boot path (the WiFi-onboarding
     * decision happens before the refresh task exists). Same consensus probe;
     * adds no device, spawns no task. i2c_xport_probe brings the OLED's bus up
     * as needed (bus0 is already up by the time the boot path reaches the AP
     * decision — sht41_create brought it up). */
    return oled_detect();
}

/* Add the device (once) + init sequence + splash. Returns true once the
 * splash has flushed. On the loaded bench bus the OLED's long bursts (25 B
 * init, 1 KB flush) intermittently NACK, so retry a few times. 100 kHz:
 * 400 kHz corrupted the init writes on this wiring. */
static bool oled_start(void) {
    if (!s_dev_open) {
        /* 100 kHz: 400 kHz corrupted the init writes on this wiring. */
        if (i2c_xport_open(&s_xport, OLED_BUS, OLED_ADDR, 100000, 0) != ESP_OK) {
            ESP_LOGE(TAG, "xport open failed");
            return false;
        }
        s_dev_open = true;
        vTaskDelay(pdMS_TO_TICKS(50));   /* SSD1306 power-on / bus settle */
    }
    for (int i = 0; i < 4; i++) {
        if (oled_cmds(SSD1306_INIT, sizeof(SSD1306_INIT)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        s_ok = true;
        render_splash();
        if (oled_flush() == ESP_OK) {
            ESP_LOGI(TAG, "SSD1306 128x64 up at 0x%02X (bench)", OLED_ADDR);
            return true;
        }
        s_ok = false;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return false;
}

/* Optional hard power switch for the panel: a GPIO driving a MOSFET that gates
 * the OLED's VCC (PIN_FN_OLED_PWR / "oled_pwr" in the pin map). When mapped,
 * oled_enabled=OFF also cuts VCC (true power-down); when unmapped, only the
 * SSD1306 software off (0xAE + charge-pump off) runs — the preferred default.
 * Active-high (1 = panel powered). Resolved once: the pin map only changes
 * across a reboot. Mirrors capture_led_init() in camera.c. */
static void oled_pwr_init(void) {
    s_pwr_pin = app_config_pin_for_first("oled_pwr");
    if (s_pwr_pin < 0)
        return;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_pwr_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "oled_pwr GPIO%d config failed — software off only", s_pwr_pin);
        s_pwr_pin = -1;
        return;
    }
    gpio_set_level((gpio_num_t)s_pwr_pin, 1);   /* power the panel for detect/init */
    ESP_LOGI(TAG, "OLED power switch on GPIO%d (active-high)", s_pwr_pin);
}

static void oled_task(void *arg) {
    (void)arg;
    /* Bring the optional VCC switch up first so the panel is powered for the
     * settle window + detect below (no-op when no oled_pwr pin is mapped). */
    oled_pwr_init();
    /* Settle before the first detect on the shared bus0 — the camera/sensor
     * init must finish first or the detect/flush NACKs (bench-measured: a 1 s
     * settle here, ~1.4 s after camera_init, fails + retries; ~3 s was racy;
     * 4 s is reliable). oled_init() now runs early (right after the audio init,
     * before the network block), so this 4 s lands the boot screen at ~10 s
     * instead of ~30 s — and the boot jingle fires from the splash below so the
     * sound arrives with it. */
    vTaskDelay(pdMS_TO_TICKS(4000));

    if (!oled_detect()) {
        /* No display on the bus (e.g. the field board) — exit, no-op. We do
         * NOT keep re-probing: that would just add bus0 traffic competing
         * with the required SHT41 for a display that isn't there. */
        ESP_LOGI(TAG, "no OLED at 0x%02X — display subsystem off", OLED_ADDR);
        vTaskDelete(NULL);
        return;
    }

    /* A display IS present. Be self-healing: the OLED's long bursts on this
     * loaded bus intermittently fail to come up or drop mid-run, so never
     * permanently disable — keep (re)bringing it up and refreshing. This is
     * what lets it appear/recover whenever the bus has a good moment instead
     * of freezing on a stale frame. */
    while (true) {
        if (!oled_start()) {
            ESP_LOGW(TAG, "OLED init/flush failing — will retry");
            if (s_dev_open) { i2c_xport_close(&s_xport); s_dev_open = false; }
            s_ok = false;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Splash is up — play the boot jingle now so the sound lands together
         * with the boot screen. (A display can't reliably come up before the
         * camera/bus settle ~10 s in, so timing it from app_main would always
         * precede the screen.) audiofx_boot() self-latches → fires once even if
         * the self-heal loop re-runs oled_start(). */
        audiofx_boot();

        vTaskDelay(pdMS_TO_TICKS(2500));   /* hold splash, then loop the page */
        int fails = 0;
        while (s_ok) {
            /* A reboot is committing: the shutdown handler owns the panel now
             * (it painted "REBOOTING"). Stop touching it so we don't re-flush
             * the status/QR frame over the reboot screen before the chip resets. */
            if (s_rebooting) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            /* Operator power switch (oled_enabled, read LIVE). When OFF: blank
             * the panel and stop flushing — no flush means no fails accumulate,
             * so the self-heal re-init (which would re-send 0xAF) never fires
             * and the panel can't flash back on. Also cuts the shared-bus0
             * traffic the required SHT41 competes with. */
            bool want_off = !app_config_get_bool("oled_enabled");
            if (want_off != s_user_off) {
                s_user_off = want_off;
                if (want_off) {
                    oled_cmd(0x8D); oled_cmd(0x10);   /* charge pump off */
                    oled_cmd(0xAE);                   /* display off */
                    if (s_pwr_pin >= 0)
                        gpio_set_level((gpio_num_t)s_pwr_pin, 0);  /* cut VCC */
                    ESP_LOGI(TAG, "display OFF (oled_enabled=OFF)");
                } else {
                    if (s_pwr_pin >= 0) {
                        gpio_set_level((gpio_num_t)s_pwr_pin, 1);  /* restore VCC */
                        vTaskDelay(pdMS_TO_TICKS(50));             /* power-on settle */
                        /* VCC was cut → controller lost its state → full re-init. */
                        oled_cmds(SSD1306_INIT, sizeof(SSD1306_INIT));
                    } else {
                        oled_cmd(0x8D); oled_cmd(0x14);   /* charge pump on */
                        oled_cmd(0xAF);                   /* display on */
                    }
                    ESP_LOGI(TAG, "display ON (oled_enabled=ON)");
                    /* fall through: next iteration re-renders + flushes a frame */
                }
            }
            if (s_user_off) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            /* Motion flash: blink the whole panel by toggling SSD1306 invert a
             * few times — no framebuffer change, so it flashes whatever's on
             * screen. Runs to completion before normal rendering resumes. */
            if (s_invert_pulses > 0) {
                s_invert_pulses--;
                oled_cmd((s_invert_pulses & 1) ? 0xA7 : 0xA6);  /* odd = inverted */
                if (s_invert_pulses == 0)
                    oled_cmd(0xA6);                              /* end on normal */
                vTaskDelay(pdMS_TO_TICKS(s_invert_step_ms > 0 ? s_invert_step_ms : 100));
                continue;
            }

            int64_t now = esp_timer_get_time();
            int delay_ms;
            if (s_anim_on && now >= s_anim_deadline_us)
                oled_anim_stop();    /* safety cap lapsed (no dismiss press) — also cuts the cue */

            /* Content precedence (highest first): operator/system overlays beat
             * the onboarding QR, which beats the test QR / splash, which beat
             * the button-cycled status page. */
            if (s_reset_held_ms >= 0) {              /* operator holding BOOT */
                render_reset_progress();
                delay_ms = 250;
            } else if (ota_progress_pct() >= 0) {    /* OTA download in flight */
                render_ota_progress();
                delay_ms = 250;
            } else if (s_anim_on && now < s_anim_deadline_us) {  /* logo flap */
                render_logo_flap();
                delay_ms = 70;                       /* ~14 fps */
            } else if (s_onboard_qr[0]) {            /* onboarding join-QR + AP swap */
                if (wifi_mgr_ap_sta_count() > 0)
                    render_page1();                  /* a client joined: show status */
                else
                    render_qr(s_onboard_qr);
                delay_ms = 3000;
            } else if (s_qr_text[0] && now < s_qr_until_us) {
                render_qr(s_qr_text);                /* timed test QR (/oled/qr) */
                delay_ms = 3000;
            } else if (now < s_splash_until_us) {
                render_splash();                     /* on-demand boot-screen preview */
                delay_ms = 3000;
            } else {                                 /* the button-cycled status page */
                const oled_page_t *pg = &s_pages[s_page % OLED_N_PAGES];
                pg->render();
                delay_ms = pg->refresh_ms;
            }

            /* Re-check after rendering (which is CPU-only): if a reboot began
             * meanwhile, don't push this frame — leave the reboot screen up. */
            if (s_rebooting) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (oled_flush() == ESP_OK) {
                fails = 0;
            } else if (++fails == 3) {
                oled_cmds(SSD1306_INIT, sizeof(SSD1306_INIT)); /* re-init */
            } else if (fails >= 20) {
                ESP_LOGW(TAG, "flush down ~1 min — re-bringing up");
                s_ok = false;   /* drop to the outer loop, full re-init */
            }

            /* Responsive wait: sleep up to delay_ms, but wake early the moment
             * something must show NOW — a button page change, the reset-hold
             * bar, or an OTA starting. The steady-state full flush stays slow
             * (3 s) so bus0 / the SHT41 aren't loaded; we only re-flush on a
             * real change. This is what makes a button tap feel instant. */
            int shown_page = s_page;
            bool shown_reset = (s_reset_held_ms >= 0);
            bool shown_ota   = (ota_progress_pct() >= 0);
            for (int waited = 0; waited < delay_ms && !s_rebooting; waited += 120) {
                vTaskDelay(pdMS_TO_TICKS(120));
                if (s_page != shown_page) break;
                if ((s_reset_held_ms >= 0) != shown_reset) break;
                if ((ota_progress_pct() >= 0) != shown_ota) break;
                if (s_invert_pulses > 0) break;   /* motion flash pending */
            }
        }
        if (s_dev_open) { i2c_xport_close(&s_xport); s_dev_open = false; }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── public control surface (all no-ops when no panel is present) ──────── */

void oled_next_page(void) {
    if (!s_ok || s_user_off)
        return;                 /* no display / turned off — ignore taps */
    int p = s_page + 1;
    s_page = (p >= (int)OLED_N_PAGES) ? 0 : p;
    ESP_LOGI(TAG, "page -> %d", s_page);
}

bool oled_env_page_visible(void) {
    /* "Someone is looking at the ENV readout" — the Grove sensor tasks
     * use this to boost their sampling to ~2 Hz for a live panel while
     * keeping their MQTT cadence at the (much longer) *_poll_s knob.
     * Matched by render fn so a page reorder can't silently break it.
     * A transient overlay (QR/OTA/flash) may cover the page; treating
     * "selected" as "visible" just keeps the boost on — harmless. */
    return s_ok && !s_user_off &&
           s_pages[s_page % OLED_N_PAGES].render == page_env;
}

/* Manual on/off. The oled_enabled NVS knob is the single source of truth, so
 * this just persists + republishes it (keeping HA in sync); the refresh task
 * applies it on its next tick. No-op-safe on the field board (no panel — the
 * task has already exited). */
void oled_set_enabled(bool en) {
    app_config_set_from_string("oled_enabled", en ? "ON" : "OFF");
}

void oled_set_reset_progress(int held_ms, int hold_ms) {
    s_reset_hold_ms = hold_ms;
    s_reset_held_ms = held_ms;  /* <0 clears the overlay */
}

void oled_flash(oled_flash_t kind) {
    if (!s_ok || s_user_off)
        return;
    /* Rate-limit. Each flash blocks page rendering for its duration, and events
     * can fire fast (Continuous-mode captures, VAD bursts) — without a cooldown
     * the panel flashes back-to-back, never renders its content (screen looks
     * broken), and a button page-change never gets drawn. Drop flashes that
     * arrive within the cooldown; one indicator blink per event-burst is enough. */
    int64_t now = esp_timer_get_time();
    if (s_invert_pulses > 0 || now < s_flash_cooldown_us)
        return;
    s_flash_cooldown_us = now + 2500000;   /* ≥2.5 s between flashes */
    /* pulses = toggles (even → ends on normal); step = ms each toggle holds.
     * Set step before pulses so the task never reads pulses>0 with a stale step. */
    switch (kind) {
        case OLED_FLASH_PHOTO:  s_invert_step_ms = 300; s_invert_pulses = 2; break; /* 1 long  */
        case OLED_FLASH_MOTION: s_invert_step_ms = 120; s_invert_pulses = 4; break; /* 2 short */
        case OLED_FLASH_VAD:    s_invert_step_ms = 60;  s_invert_pulses = 6; break; /* 3 quick */
    }
}

void oled_set_reboot_reason(const char *reason) {
    if (reason)
        strlcpy(s_reboot_reason, reason, sizeof(s_reboot_reason));
}

void oled_show_deepsleep(int next_wake_s) {
    /* Reuse the rebooting latch to stop the refresh task overwriting this frame.
     * The unit deep-sleeps immediately after, so the panel keeps this image. */
    s_rebooting = true;
    if (!s_ok || !s_dev_open)
        return;
    render_deepsleep(next_wake_s);
    oled_flush();
}

void oled_show_rebooting(void) {
    s_rebooting = true;   /* stop the refresh task from overwriting this frame */
    if (!s_ok || !s_dev_open)
        return;
    /* Synchronous final frame. Safe from the shutdown-handler context: the
     * scheduler is still up and the i2c-master bus lock serialises us against
     * any in-flight OLED-task flush; a torn frame is moot — we're resetting. */
    render_rebooting();
    oled_flush();
}

/* IDF shutdown handler: fires at the top of esp_restart() on EVERY soft-reboot
 * path. There's no central reboot wrapper (18 direct esp_restart sites), so
 * this single hook covers all of them. No-op when no panel is present. */
static void oled_on_shutdown(void) { oled_show_rebooting(); }

/* ── init ─────────────────────────────────────────────────────────────── */

bool oled_init(void) {
    /* Register the reboot-screen hook regardless of whether a panel is wired
     * (it no-ops without one) — cheap, and keeps the field board harmless. */
    esp_err_t sh = esp_register_shutdown_handler(oled_on_shutdown);
    if (sh != ESP_OK)
        ESP_LOGW(TAG, "shutdown handler not registered (%s) — no reboot screen",
                 esp_err_to_name(sh));

    /* Detection + bring-up happens in the task (after a settle delay), so
     * this never blocks boot and can't be tripped by the early bus churn.
     * 4 KB stack confirmed ample (measured high-water ≈1.4 KB used through QR
     * generation + camera SCCB reads). */
    if (xTaskCreate(oled_task, "oled", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "oled task create failed — no display");
        return false;
    }
    return true;
}

bool oled_present(void) { return s_ok; }
