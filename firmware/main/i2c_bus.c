/* i2c_bus.c — shared I²C master bus init + diagnostics. */

#include "i2c_bus.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_config.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bb.h"
#include "sensors.h"

static const char *TAG = "i2c_bus";

/* Main bus uses HW I²C NUM_0 (battery + SHT41). The camera SCCB driver
 * claims NUM_1 internally, so the diagnostic D6/D7 bus has no HW
 * controller available — it goes through the software bit-bang in
 * i2c_bb.[ch]. From the caller's perspective the diagnostic bus is a
 * pin-pair plus a few address probes; no handle to track. */
static i2c_master_bus_handle_t s_bus0 = NULL;

i2c_master_bus_handle_t i2c_bus_get(void) {
    /* NOTE: address scan removed from here — it was running BEFORE
     * any device-specific init, which on the bench appeared to leave
     * MAX17048 in a state where its subsequent address ACK never
     * fired (probing 112 addresses in a row may exercise the chip's
     * "ACKs every address" buggy path enough to wedge it). Each
     * device driver now opens its handle without a global pre-probe;
     * the live /i2c endpoint runs the scan on demand after all
     * drivers have had a clean first shot at their chip. */
    if (s_bus0)
        return s_bus0;

    /* Pin map lookup. Defaults match rev3.2 (SDA=GPIO5, SCL=GPIO6),
     * but the operator can move the bus by reassigning two slots in
     * the pin function map. The setter cross-validation ensures the
     * pair is consistent at the time of write; if NVS load somehow
     * leaves a half-paired map (one side unassigned), we refuse to
     * bring the bus up — sensors fail their probes loudly in
     * selftest rather than initializing on a single line. */
    int sda = app_config_pin_for_first("i2c0_sda");
    int scl = app_config_pin_for_first("i2c0_scl");
    if (sda < 0 || scl < 0) {
        ESP_LOGE(TAG, "bus0 pin map incomplete (sda=%d scl=%d) — bus not initialised",
                 sda, scl);
        return NULL;
    }

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&cfg, &s_bus0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bus0/main: i2c_new_master_bus: %s", esp_err_to_name(e));
        s_bus0 = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "bus0/main ready (sda=%d scl=%d)", sda, scl);

    /* Some AliExpress sensor clones need a few tens of ms after the
     * bus comes up before they ACK the first address probe (POR is
     * datasheet ≤1 ms, but clones we tested were closer to 50 ms). */
    vTaskDelay(pdMS_TO_TICKS(100));
    return s_bus0;
}

esp_err_t i2c_bus0_recover(void) {
    if (!s_bus0)
        return ESP_ERR_INVALID_STATE;
    esp_err_t e = i2c_master_bus_reset(s_bus0);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "bus0 recover (reset): %s", esp_err_to_name(e));
    else
        ESP_LOGW(TAG, "bus0 recovered (reset issued)");
    return e;
}

bool i2c_bus1_ensure(void) {
    /* Same pin-map pattern as bus0. rev3.2 defaults SDA=GPIO43 SCL=GPIO44
     * (D6/D7) — operator can move via the pin function map. Half-paired
     * map refuses bring-up. */
    int sda = app_config_pin_for_first("i2c1_sda");
    int scl = app_config_pin_for_first("i2c1_scl");
    if (sda < 0 || scl < 0) {
        ESP_LOGW(TAG, "bus1 pin map incomplete (sda=%d scl=%d) — bit-bang not initialised",
                 sda, scl);
        return false;
    }
    return i2c_bb_init(sda, scl) == ESP_OK;
}

struct expected_addr {
    uint8_t addr;
    const char *role;
};

/* Expected devices per bus now come from the shared sensor registry
 * (CB_SENSORS in sensors.[ch]) — single source of truth, so /i2c, /sensors,
 * the HTML UI, the OLED and MQTT can't disagree. Only the wrong-part
 * variants stay here (they aren't real registry sensors). */

/* Variant addresses that mean "you bought the wrong part" — flagged
 * loudly if they answer. */
static const struct expected_addr VARIANTS[] = {
    {0x45, "unexpected: SHT4x-AD2B (driver expects 0x44; address-strap variant?)"},
    {0x6C, "unexpected: MAX17041/44 (driver expects MAX17048 at 0x36)"},
};

static void appendf(char **p, size_t *left, const char *fmt, ...) {
    if (!p || !*p || !left || *left == 0)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;

    if ((size_t)n >= *left) {
        *p += *left - 1;
        *left = 1;
        return;
    }
    *p += n;
    *left -= (size_t)n;
}

/* HW-controller probe (bus0). 50 ms single-shot — long enough to find
 * real chips, short enough not to false-positive on bus noise. */
static bool probe_once_hw(i2c_master_bus_handle_t bus, uint8_t addr) {
    return i2c_master_probe(bus, addr, 50) == ESP_OK;
}

static int probe_consensus_hw(i2c_master_bus_handle_t bus, uint8_t addr) {
    int hits = 0;
    for (int i = 0; i < 3; i++) {
        if (probe_once_hw(bus, addr))
            hits++;
    }
    return hits;
}

/* Bit-bang probe (bus1). Same shape — return ACK count out of 3. */
static int probe_consensus_bb(uint8_t addr) {
    int hits = 0;
    for (int i = 0; i < 3; i++) {
        if (i2c_bb_probe(addr))
            hits++;
    }
    return hits;
}

/* Polymorphic across HW (bus) and bit-bang (handle == NULL → uses bb). */
static bool probe_once(i2c_master_bus_handle_t bus, uint8_t addr) {
    if (bus)
        return probe_once_hw(bus, addr);
    return i2c_bb_probe(addr);
}
static int probe_consensus(i2c_master_bus_handle_t bus, uint8_t addr) {
    if (bus)
        return probe_consensus_hw(bus, addr);
    return probe_consensus_bb(addr);
}

/* 3-shot consensus over a sensor's honest real-read fn — gives the OK /
 * FLAKY / MISSING verdict and exposes intermittent reads. */
static int read_consensus(bool (*read_ok)(void)) {
    int hits = 0;
    for (int i = 0; i < 3; i++)
        if (read_ok())
            hits++;
    return hits;
}

/* Render one bus from the registry: the HW bus gets a raw address sweep
 * (catches unexpected chips + noise); both buses then list every registry
 * sensor on that bus with an HONEST real-read verdict (so /i2c can't
 * contradict /sensors or reality — the bit-bang address probe false-ACKs,
 * so we never trust it for a verdict). */
static int scan_bus(char **p, size_t *left, const char *label, cb_bus_t busid,
                    i2c_master_bus_handle_t bus, bool active) {
    appendf(p, left, "%s: ", label);
    if (!active) {
        appendf(p, left, "not initialised\n");
        return 0;
    }

    int count = 0;
    const bool bitbang = (bus == NULL);

    if (!bitbang) {
        bool any = false;
        appendf(p, left, "found: ");
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (probe_once(bus, a)) {
                appendf(p, left, "%s0x%02x", any ? " " : "", a);
                any = true;
            }
        }
        appendf(p, left, "%s\n", any ? "" : "(nothing)");
    } else {
        appendf(p, left, "(bit-bang: address probes false-ACK; per-device reads:)\n");
    }

    for (size_t i = 0; i < CB_SENSORS_N; i++) {
        const cb_sensor_t *s = &CB_SENSORS[i];
        if (s->bus != busid || !s->read_ok)
            continue;
        int hits = read_consensus(s->read_ok);
        const char *verdict = (hits == 3) ? "OK" : (hits == 0) ? "MISSING" : "FLAKY";
        appendf(p, left, "  0x%02x  %-18s %s (%d/3)\n", s->addr, s->name, verdict, hits);
        if (hits == 3)
            count++;
    }

    /* Wrong-part variants — HW bus only (the bit-bang false-ACK would fire
     * them spuriously). */
    if (!bitbang) {
        for (size_t i = 0; i < sizeof(VARIANTS) / sizeof(VARIANTS[0]); i++) {
            if (probe_consensus(bus, VARIANTS[i].addr) == 3)
                appendf(p, left, "  0x%02x  %s\n", VARIANTS[i].addr, VARIANTS[i].role);
        }
    }
    return count;
}

int i2c_bus_scan_report(char *out, size_t out_sz) {
    if (!out || out_sz == 0)
        return 0;
    out[0] = 0;

    /* Main bus is the required bus and must already be initialized by
     * production drivers; touch i2c_bus_get just to be defensive in
     * case the report is requested before any other driver opened it.
     * Bus1 is bit-banged on demand. */
    i2c_master_bus_handle_t bus0 = s_bus0 ? s_bus0 : i2c_bus_get();
    bool bus1_ok = i2c_bus1_ensure();

    char *p = out;
    size_t left = out_sz;
    int count = 0;

    count += scan_bus(&p, &left, "bus0 D4/D5 (GPIO5/GPIO6)", CB_BUS0, bus0, bus0 != NULL);
    appendf(&p, &left, "\n");
    count += scan_bus(&p, &left, "bus1 D6/D7 (GPIO43/GPIO44, bit-bang)", CB_BUS1, NULL, bus1_ok);
    return count;
}
