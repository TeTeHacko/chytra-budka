# metrics-bridge — MQTT → Prometheus translator for Chytrá Budka

One Python daemon (`cbprom.py`) subscribes to the broker, translates every
retained `cb-+/state/*` topic into a Prometheus metric, and exposes them on
`:9878/metrics` for any Prometheus-compatible scraper (Prometheus, Grafana
Alloy, VictoriaMetrics agent, …). Alerting/SLO rules for these metrics live
in [`alerts/`](alerts/README.md).


> In the compose stack this daemon runs as the `cbprom` container — see
> [`server/`](../server/README.md). The sections below document the metric
> schema; the author's legacy bare-metal deployment notes are not published.

## Metric catalog

All metrics are prefixed `cb_` and labeled with `device` (stable MAC
tail like `ex02`) + `device_name` (friendly from inventory map,
falls back to the tail).

Gauges (instantaneous):

- `cb_online` (1/0 from LWT availability)
- `cb_battery_soc_percent`, `cb_battery_voltage_volts`, `cb_battery_charge_rate_pct_per_hour`
- `cb_solar_voltage_volts`, `cb_solar_current_amps`, `cb_solar_power_watts`
- `cb_temperature_celsius_inside`, `cb_humidity_percent_inside`
- `cb_temperature_celsius_outside`, `cb_humidity_percent_outside`
- `cb_mcu_temperature_celsius`
- `cb_wifi_rssi_dbm`, `cb_uptime_seconds`, `cb_heap_free_bytes`
- `cb_audio_rms_dbfs`, `cb_audio_streaming`
- `cb_camera_agc_gain`
- `cb_motion_active`, `cb_reed_open`

Counters (firmware-resetting — use `rate()` / `increase()`, NOT raw):

- `cb_audio_burst_total`, `cb_audio_chunks_sent_total`
- `cb_photo_capture_total`, `cb_motion_total`, `cb_reed_events_total`

Info (string label as metric label, value always 1.0):

- `cb_mode_info{mode="continuous|triggered|safe|boot"}`
- `cb_reset_reason_info{reason="task_wdt|brownout|sw|..."}`
- `cb_fw_version_info{version="<JSON blob>"}` — raw firmware payload.
  TODO: split the JSON into `version_sha`, `version_date`, `idf_ver`
  labels (small follow-up in cbprom.py if anyone wants `cb_fw_version_info`
  to be useful for Grafana variable templating).

Config knobs auto-discovered from `state/cfg/*` — one `cb_cfg_<knob>`
gauge per knob (numeric raw, bools as 1/0). Deprecated/cleared knobs
(empty payload) are NOT materialized — see firmware `app_config.c`
`DEPRECATED_DISC[]` for the rename history.

## Adding a new firmware topic

1. In `firmware/main/mqtt.c::register_topics()`, the new `BT(field,
   "/state/<sub>")` line appears.
2. In `metrics-bridge/cbprom.py`, add a `MetricDef` row to `METRICS`
   for the subtopic suffix. Pick the convert helper based on payload
   format (`to_float`, `to_int`, `to_bool_onoff`, `to_bool_door`,
   `to_bool_avail`) or write a new one.
3. Redeploy with `--tags code`. No firmware change, no config change.

For new `state/cfg/<knob>` knobs no code change is needed — the
dynamic `_handle_cfg` path discovers them on first message and
materializes a `cb_cfg_<knob>` gauge.

## Wire it into your metrics stack

Point any Prometheus-compatible scrape job at `:9878` (in the compose stack
the port is published per `.env`, `CBPROM_PORT`) and the `cb_*` series appear
under whatever job label you give it. Ready-made alerting + SLO rules are in
[`alerts/`](alerts/README.md).
