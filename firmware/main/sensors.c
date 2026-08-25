/* sensors.c — the I²C sensor registry. See sensors.h.
 *
 * Single source of truth for every physical I²C device on the board. The
 * registry OWNS the driver instances (sht41/bmp388/ina226), each bound to a
 * bus via i2c_xport at create time — so any sensor works on any bus and
 * "BMP388 on bus1" is just a table row. Each row wraps its instance into:
 * a refresh() that does the I²C read into the instance cache, per-channel
 * cached reads (so the HTML UI / OLED never touch I²C — only the telemetry
 * owner + /sensors call refresh), and a read_ok() honest fresh read for the
 * /i2c + /sensors presence verdict. */

#include "sensors.h"

#include <math.h>
#include <string.h>

#include "battery.h"
#include "bmp388.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_xport.h"
#include "ina226.h"
#include "sht41.h"

static const char *TAG = "sensors";

#ifndef INA226_ADDR
#define INA226_ADDR 0x40
#endif

/* ── instances owned by the registry (created in cb_sensors_init) ─────── */
static sht41_t  *s_sht_in;    /* bus0 0x44 — on-board (outside) ambient */
static sht41_t  *s_sht_ext;   /* bus1 0x44 — external probe */
static bmp388_t *s_bmp_in;    /* bus0 0x77 — inside pressure/temp */
static bmp388_t *s_bmp_ext;   /* bus1 0x77 — bench diagnostic */
static ina226_t *s_ina;       /* bus0 0x40 — solar shunt */
static ina226_t *s_ina_ext;   /* bus1 0x40 — bench diagnostic */
static i2c_xport_t s_max1_x;  /* bus1 0x36 — quarantine MAX17048 (no driver) */
static bool s_max1_open;

/* ── SHT41 inside (bus0) ─────────────────────────────────────────────── */
static bool sht0_present(void)  { return sht41_ready(s_sht_in); }
static void sht0_refresh(void)  { float t, rh; (void)sht41_read(s_sht_in, &t, &rh); }
static bool sht0_read_ok(void)  { float t, rh; return sht41_read(s_sht_in, &t, &rh) == ESP_OK; }
static bool sht0_temp(float *v) { float t, rh; if (!sht41_get_cached(s_sht_in, &t, &rh)) return false; *v = t; return true; }
static bool sht0_rh(float *v)   { float t, rh; if (!sht41_get_cached(s_sht_in, &t, &rh)) return false; *v = rh; return true; }
/* obj ids ("temp"/"humidity") are STABLE topic/HA suffixes. The on-board
 * SHT41 is the "outside" sensor; BMP388 is "inside". See CB_SENSORS. */
static const cb_chan_t SHT0_CH[] = {
    {"temp",     "Temperature (outside)", "temperature", "°C", 1, sht0_temp},
    {"humidity", "Humidity (outside)",    "humidity",    "%",  0, sht0_rh},
};

/* ── SHT41 external (bus1 bit-bang) ──────────────────────────────────── */
static bool sht1_present(void)  { return sht41_ready(s_sht_ext); }
static void sht1_refresh(void)  { float t, rh; (void)sht41_read(s_sht_ext, &t, &rh); }
static bool sht1_read_ok(void)  { float t, rh; return sht41_read(s_sht_ext, &t, &rh) == ESP_OK; }
static bool sht1_temp(float *v) { float t, rh; if (!sht41_get_cached(s_sht_ext, &t, &rh)) return false; *v = t; return true; }
static bool sht1_rh(float *v)   { float t, rh; if (!sht41_get_cached(s_sht_ext, &t, &rh)) return false; *v = rh; return true; }
static const cb_chan_t SHT1_CH[] = {
    {"temp_ext",     "Temperature (ext)", "temperature", "°C", 1, sht1_temp},
    {"humidity_ext", "Humidity (ext)",    "humidity",    "%",  0, sht1_rh},
};

/* ── BMP388 inside (bus0) ────────────────────────────────────────────── */
static bool bmp0_present(void)  { return bmp388_ready(s_bmp_in); }
static void bmp0_refresh(void)  { float t, p; (void)bmp388_read(s_bmp_in, &t, &p); }
static bool bmp0_read_ok(void)  { float t, p; return bmp388_read(s_bmp_in, &t, &p); }
static bool bmp0_temp(float *v) { float t, p; if (!bmp388_get_cached(s_bmp_in, &t, &p)) return false; *v = t; return true; }
static bool bmp0_press(float *v){ float t, p; if (!bmp388_get_cached(s_bmp_in, &t, &p)) return false; *v = p; return true; }
static const cb_chan_t BMP0_CH[] = {
    {"temp_bmp", "Temperature (inside)", "temperature", "°C",  1, bmp0_temp},
    {"pressure", "Pressure",             "pressure",    "hPa", 0, bmp0_press},
};

/* ── BMP388 on bus1 (bit-bang) — bench diagnostic, distinct obj ids so it
 * never collides with the bus0 BMP if it were ever published ──────────── */
static bool bmp1_present(void)  { return bmp388_ready(s_bmp_ext); }
static void bmp1_refresh(void)  { float t, p; (void)bmp388_read(s_bmp_ext, &t, &p); }
static bool bmp1_read_ok(void)  { float t, p; return bmp388_read(s_bmp_ext, &t, &p); }
static bool bmp1_temp(float *v) { float t, p; if (!bmp388_get_cached(s_bmp_ext, &t, &p)) return false; *v = t; return true; }
static bool bmp1_press(float *v){ float t, p; if (!bmp388_get_cached(s_bmp_ext, &t, &p)) return false; *v = p; return true; }
static const cb_chan_t BMP1_CH[] = {
    {"temp_bmp1", "Temperature (bus1)", "temperature", "°C",  1, bmp1_temp},
    {"pressure1", "Pressure (bus1)",    "pressure",    "hPa", 0, bmp1_press},
};

/* ── MAX17048 on bus0 (the battery gauge, via battery.c) ─────────────── */
/* mqtt=false: soc/v_bat/crate still go to HA via the dedicated battery path
 * (battery.c + publish_full_telemetry); the registry only DISPLAYS it. */
static float s_bat_soc = NAN, s_bat_v = NAN;
static bool  s_bat_ok = false;
static bool bat_present(void) { return battery_ready(); }
static void bat_refresh(void) {
    if (!battery_ready()) return;
    float s = battery_soc(), v = battery_vbat();
    if (s >= 0 && v >= 0) { s_bat_soc = s; s_bat_v = v; s_bat_ok = true; }
}
static bool bat_read_ok(void) { return battery_ready() && battery_soc() >= 0; }
static bool bat_soc(float *v)  { if (!s_bat_ok) return false; *v = s_bat_soc; return true; }
static bool bat_vbat(float *v) { if (!s_bat_ok) return false; *v = s_bat_v; return true; }
static const cb_chan_t BAT_CH[] = {
    {"soc",   "Battery SOC",     "battery", "%", 0, bat_soc},
    {"v_bat", "Battery Voltage", "voltage", "V", 2, bat_vbat},
};

/* ── MAX17048 on bus1 (bit-bang "quarantine"/test gauge) ──────────────── */
/* No driver tracks it — refresh reads the registers over the bit-bang bus
 * (via the shared i2c_xport) and caches. present() = NULL (no init step). */
static float s_m1_soc = NAN, s_m1_v = NAN;
static bool  s_m1_ok = false;
static bool m1_reg(uint8_t reg, uint16_t *out) {
    if (!s_max1_open)
        return false;
    uint8_t rx[2];
    for (int k = 0; k < 8; k++)
        if (i2c_xport_txrx(&s_max1_x, &reg, 1, rx, 2, 50) == ESP_OK) {
            *out = (uint16_t)((rx[0] << 8) | rx[1]);
            return true;
        }
    return false;
}
static bool m1_read_ok(void) {
    uint16_t ver;
    return m1_reg(0x08, &ver) && (ver & 0xFFF0) == 0x0010;  /* genuine MAX17048 */
}
static void m1_refresh(void) {
    uint16_t soc, vc;
    if (m1_read_ok() && m1_reg(0x04, &soc) && m1_reg(0x02, &vc)) {
        s_m1_soc = (float)soc / 256.0f;
        s_m1_v = (float)vc * 78.125e-6f;
        s_m1_ok = true;
    } else {
        s_m1_ok = false;
    }
}
static bool m1_soc(float *v)  { if (!s_m1_ok) return false; *v = s_m1_soc; return true; }
static bool m1_vbat(float *v) { if (!s_m1_ok) return false; *v = s_m1_v; return true; }
static const cb_chan_t MAX1_CH[] = {
    {"soc",   "Battery SOC",     "battery", "%", 0, m1_soc},
    {"v_bat", "Battery Voltage", "voltage", "V", 2, m1_vbat},
};

/* ── INA226 solar shunt (bus0) — mqtt=false keeps its dedicated solar
 * publish path, the registry just lists + caches it ──────────────────── */
static bool ina0_present(void) { return ina226_ready(s_ina); }
static void ina0_refresh(void) { float v, i, p; (void)ina226_read(s_ina, &v, &i, &p); }
static bool ina0_read_ok(void) { return ina226_ready(s_ina); }
static bool ina0_v(float *x) { float v, i, p; if (!ina226_get_cached(s_ina, &v, &i, &p)) return false; *x = v; return true; }
static bool ina0_i(float *x) { float v, i, p; if (!ina226_get_cached(s_ina, &v, &i, &p)) return false; *x = i; return true; }
static bool ina0_p(float *x) { float v, i, p; if (!ina226_get_cached(s_ina, &v, &i, &p)) return false; *x = p; return true; }
static const cb_chan_t INA0_CH[] = {
    {"solar_v", "Solar Voltage", "voltage", "V", 2, ina0_v},
    {"solar_i", "Solar Current", "current", "A", 3, ina0_i},
    {"solar_p", "Solar Power",   "power",   "W", 2, ina0_p},
};

/* ── INA226 on bus1 (bit-bang) — bench diagnostic, distinct obj ids ───── */
static bool ina1_present(void) { return ina226_ready(s_ina_ext); }
static void ina1_refresh(void) { float v, i, p; (void)ina226_read(s_ina_ext, &v, &i, &p); }
static bool ina1_read_ok(void) { return ina226_ready(s_ina_ext); }
static bool ina1_v(float *x) { float v, i, p; if (!ina226_get_cached(s_ina_ext, &v, &i, &p)) return false; *x = v; return true; }
static bool ina1_i(float *x) { float v, i, p; if (!ina226_get_cached(s_ina_ext, &v, &i, &p)) return false; *x = i; return true; }
static bool ina1_p(float *x) { float v, i, p; if (!ina226_get_cached(s_ina_ext, &v, &i, &p)) return false; *x = p; return true; }
static const cb_chan_t INA1_CH[] = {
    {"solar_v1", "Solar Voltage (bus1)", "voltage", "V", 2, ina1_v},
    {"solar_i1", "Solar Current (bus1)", "current", "A", 3, ina1_i},
    {"solar_p1", "Solar Power (bus1)",   "power",   "W", 2, ina1_p},
};

/* ── the registry: every physical I²C device, with its bus + address ─── */
/* Columns: id, OLED label, friendly name, bus, addr, mqtt, oled, present,
 * refresh, read_ok, channels, n_chans. Bus1 instances are mqtt=false +
 * oled=false (bench diagnostic — visible in /i2c, /sensors, HTML, not in HA
 * or the OLED panel). obj ids are STABLE for the published (bus0) rows. */
const cb_sensor_t CB_SENSORS[] = {
    {"sht0", "out",  "SHT41 outside",   CB_BUS0, 0x44, true,  true,  sht0_present, sht0_refresh, sht0_read_ok, SHT0_CH, 2},
    {"sht1", "ext",  "SHT41 (ext)",     CB_BUS1, 0x44, true,  false, sht1_present, sht1_refresh, sht1_read_ok, SHT1_CH, 2},
    {"bmp",  "in",   "BMP388 inside",   CB_BUS0, 0x77, true,  true,  bmp0_present, bmp0_refresh, bmp0_read_ok, BMP0_CH, 2},
    {"bmp1", "in1",  "BMP388 (bus1)",   CB_BUS1, 0x77, false, false, bmp1_present, bmp1_refresh, bmp1_read_ok, BMP1_CH, 2},
    {"bat",  "bat",  "MAX17048 (bus0)", CB_BUS0, 0x36, false, true,  bat_present,  bat_refresh,  bat_read_ok,  BAT_CH,  2},
    {"ina",  "sol",  "INA226 (solar)",  CB_BUS0, 0x40, false, true,  ina0_present, ina0_refresh, ina0_read_ok, INA0_CH, 3},
    {"ina1", "sol1", "INA226 (bus1)",   CB_BUS1, 0x40, false, false, ina1_present, ina1_refresh, ina1_read_ok, INA1_CH, 3},
    {"max1", "bat1", "MAX17048 (bus1)", CB_BUS1, 0x36, false, false, NULL,         m1_refresh,   m1_read_ok,   MAX1_CH, 2},
};
const size_t CB_SENSORS_N = sizeof(CB_SENSORS) / sizeof(CB_SENSORS[0]);

void cb_sensors_init(void) {
    /* bus0 (HW) primaries */
    if (!s_sht_in)  s_sht_in  = sht41_create(CB_BUS0, SHT41_ADDR);
    if (!s_bmp_in)  s_bmp_in  = bmp388_create(CB_BUS0, BMP388_ADDR);
    if (!s_ina)     s_ina     = ina226_create(CB_BUS0, INA226_ADDR);
    /* bus1 (bit-bang) extras — bench diagnostic; absent ⇒ harmless no-ops */
    if (!s_sht_ext) s_sht_ext = sht41_create(CB_BUS1, SHT41_ADDR);
    if (!s_bmp_ext) s_bmp_ext = bmp388_create(CB_BUS1, BMP388_ADDR);
    if (!s_ina_ext) s_ina_ext = ina226_create(CB_BUS1, INA226_ADDR);
    if (!s_max1_open && i2c_xport_open(&s_max1_x, CB_BUS1, MAX17048_ADDR, 0, 0) == ESP_OK)
        s_max1_open = true;
}

void cb_sensors_retry_absent(void) {
    /* One throttle for the whole sweep; first call (deadline 0) fires
     * immediately so a "just plugged it in" sensor recovers fast, not after
     * 60 s. Only the SHT41s are re-probed periodically (matches the prior
     * behavior + they're the required field sensors); BMP388/INA226 are
     * boot-init-once — re-detect them with cb_sensor_probe()/reboot. */
    static int64_t s_dl = 0;
    int64_t now = esp_timer_get_time();
    if (now < s_dl)
        return;
    s_dl = now + 60LL * 1000000LL;

    if (!s_sht_in)
        s_sht_in = sht41_create(CB_BUS0, SHT41_ADDR);
    if (s_sht_in && !sht41_ready(s_sht_in) && sht41_probe(s_sht_in) == ESP_OK)
        ESP_LOGI(TAG, "internal SHT41 recovered on periodic re-probe");

    if (!s_sht_ext)
        s_sht_ext = sht41_create(CB_BUS1, SHT41_ADDR);
    if (s_sht_ext && !sht41_ready(s_sht_ext) && sht41_probe(s_sht_ext) == ESP_OK)
        ESP_LOGI(TAG, "ext SHT41 recovered on periodic re-probe");
}

void cb_sensors_refresh(void) {
    for (size_t i = 0; i < CB_SENSORS_N; i++) {
        const cb_sensor_t *s = &CB_SENSORS[i];
        /* present==NULL → always try (e.g. the bit-bang MAX, which has no
         * init/present step and self-detects on each read). */
        if (s->refresh && (!s->present || s->present()))
            s->refresh();
    }
}

static const cb_sensor_t *find_sensor(const char *id) {
    for (size_t i = 0; i < CB_SENSORS_N; i++)
        if (strcmp(CB_SENSORS[i].id, id) == 0)
            return &CB_SENSORS[i];
    return NULL;
}

void cb_sensor_refresh_one(const char *id) {
    const cb_sensor_t *s = find_sensor(id);
    if (s && s->refresh)
        s->refresh();
}

bool cb_sensor_ready(const char *id) {
    const cb_sensor_t *s = find_sensor(id);
    if (!s)
        return false;
    if (s->present)
        return s->present();
    return s->read_ok && s->read_ok();
}

bool cb_sensor_chan(const char *id, const char *obj, float *out) {
    const cb_sensor_t *s = find_sensor(id);
    if (!s)
        return false;
    for (size_t c = 0; c < s->n_chans; c++)
        if (strcmp(s->chans[c].obj, obj) == 0)
            return s->chans[c].read(out);
    return false;
}

esp_err_t cb_sensor_probe(const char *id) {
    if (!strcmp(id, "sht0")) return sht41_probe(s_sht_in);
    if (!strcmp(id, "sht1")) return sht41_probe(s_sht_ext);
    if (!strcmp(id, "bmp"))  return bmp388_probe(s_bmp_in);
    if (!strcmp(id, "bmp1")) return bmp388_probe(s_bmp_ext);
    if (!strcmp(id, "ina"))  return ina226_probe(s_ina);
    if (!strcmp(id, "ina1")) return ina226_probe(s_ina_ext);
    return ESP_ERR_NOT_FOUND;
}
