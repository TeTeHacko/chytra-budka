/* sensors.h — generic I²C sensor registry.
 *
 * One table of sensors → channels drives ALL outputs uniformly: MQTT
 * telemetry, Home Assistant discovery, the HTML UI, and the OLED. Adding a
 * sensor = write a driver + add one row here; every output picks it up
 * automatically. A sensor instance is just (present, refresh, channels),
 * so any sensor on any I²C bus fits — the channel read fns hide the bus.
 *
 * Scope: physical I²C measurement sensors only (SHT41×N, BMP388, …).
 * System/diagnostic metrics (RSSI, heap, MCU temp, SOC, solar) keep their
 * existing dedicated paths — they aren't "a sensor on a bus".
 *
 * HA entity ids (the `obj` field) are STABLE — they double as the MQTT
 * state-topic suffix and the discovery object_id, so existing dashboards
 * keep working. Never rename an `obj` in use; add new ones instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_xport.h"   /* cb_bus_t lives here now (shared with the drivers) */

#ifdef __cplusplus
extern "C" {
#endif

/* Read a channel's latest cached value. Returns false if unavailable
 * (sensor absent or no good reading yet). */
typedef bool (*cb_read_fn)(float *out);

typedef struct {
    const char *obj;      /* HA object_id + state-topic suffix (STABLE) */
    const char *name;     /* HA friendly name */
    const char *dev_cla;  /* HA device_class, or NULL */
    const char *unit;     /* unit string: "°C", "%", "hPa" */
    int decimals;         /* publish/display precision */
    cb_read_fn read;      /* cached read */
} cb_chan_t;

typedef struct {
    const char *id;          /* short stable id: "sht0","sht1","bmp","bat","max1" */
    const char *label;       /* short OLED label: "in","out","BMP" */
    const char *name;        /* friendly UI name: "SHT41 inside" */
    cb_bus_t    bus;         /* which I²C bus the instance lives on */
    uint8_t     addr;        /* 7-bit I²C address */
    bool        mqtt;        /* publish to MQTT/HA?  false = diagnostic-only
                              * (battery + the bus1 quarantine MAX have their own
                              * presentation / no telemetry), but still listed in
                              * /sensors and /i2c and shown on HTML. */
    bool        oled;        /* show on the bench OLED?  Independent of mqtt:
                              * false hides a sensor from the panel while keeping
                              * its MQTT/HTML presence (e.g. the bus1 "ext" probe
                              * and the bus1 quarantine MAX, which are panel noise). */
    bool (*present)(void);   /* cached "init succeeded" state */
    void (*refresh)(void);   /* populate caches for the channels (NULL = none) */
    bool (*read_ok)(void);   /* HONEST fresh real read — used by /i2c + /sensors
                              * so a bit-bang false-ACK can't read as present */
    const cb_chan_t *chans;
    size_t n_chans;
} cb_sensor_t;

extern const cb_sensor_t CB_SENSORS[];
extern const size_t CB_SENSORS_N;

/* Create + probe every instance in the registry. Call once at boot, after
 * the I²C buses can come up. Replaces the per-driver *_init() calls; the
 * registry owns the instances and their bus binding from here on. */
void cb_sensors_init(void);

/* Re-probe any instance that isn't ready yet (hot-plug / transient-flaky
 * recovery), throttled per instance to once a minute. Call from the
 * telemetry tick. Replaces the per-sensor re-probe blocks. */
void cb_sensors_retry_absent(void);

/* Refresh every present sensor's cache (one I²C read per sensor). Call this
 * from the single telemetry owner; the HTML UI and OLED only read caches so
 * they never add concurrent bus traffic. */
void cb_sensors_refresh(void);

/* Refresh just one sensor's cache by id (diagnostic endpoints that want a
 * fresh read of a single sensor without sweeping the whole registry). */
void cb_sensor_refresh_one(const char *id);

/* Readiness of one sensor by id ("sht0","sht1","bmp","ina",…). False for an
 * unknown id or a sensor with no present() probe. */
bool cb_sensor_ready(const char *id);

/* Cached value of one channel, addressed by sensor id + channel obj
 * ("sht0"+"temp", "ina"+"solar_v", …). False if the sensor/channel is
 * unknown or has no fresh reading. Never touches the bus. */
bool cb_sensor_chan(const char *id, const char *obj, float *out);

/* Force a (re)probe of one instance by id, ignoring the retry throttle.
 * Used by the bus1 diagnostic endpoint to recover a just-wired sensor.
 * ESP_ERR_NOT_FOUND for an unknown id. */
esp_err_t cb_sensor_probe(const char *id);

#ifdef __cplusplus
}
#endif
