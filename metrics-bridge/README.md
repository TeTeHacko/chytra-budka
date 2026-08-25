# metrics-bridge — MQTT → Prometheus translator for Chytrá Budka

One Python daemon on `server-host` subscribes to the broker, translates
every retained `chytra-budka-+/state/*` topic into a Prometheus metric,
and exposes them on `:9878/metrics` for the existing Grafana Alloy
instance to scrape + `remote_write` into Mimir.

Mirrors the pattern of the author's UC96 BT power-meter exporter (not
published). Same
host, same Alloy, same Mimir tenant — labels (`device`, `device_name`)
are aligned so Grafana panels join cleanly:

```promql
# Battery SOC vs measured wall power for one bird-box
cb_battery_soc_percent{device_name="budka-example-1"}
  /
uc96_power_watts{device="budka-example-1"}
```

## Files

| File                                 | Role |
| ------------------------------------ | ---- |
| `cbprom.py`                          | Daemon (~720 LOC). Subscribes, parses, exposes metrics. |
| `enroll.py`                          | Daemon (~570 LOC). Signs CSRs from `+/cmd/enroll`, replies on `<base>/state/cert`. See [firmware/HTTPS.md](../firmware/HTTPS.md). |
| `requirements.txt`                   | Python deps (`paho-mqtt>=2.1`, `prometheus_client`, `cryptography>=43`). |
| `cbprom-exporter.service`            | systemd unit for cbprom.py. `__VENV__` substituted by Ansible. |
| `cbd-enroll.service`                 | systemd unit for enroll.py. Reads sub-CA from `/etc/ansible-ca/`. |
| `templates/config.toml.j2`           | Jinja template for `/etc/cb-prom/config.toml` (both daemons share). |
| `templates/app-metrics-cbprom.yml.j2`| Alloy file_sd target dropped at `/etc/alloy/scrape-targets/`. |
| `inventory.yml`                      | Ansible inventory + per-host vars + device name map. |
| `deploy.yml`                         | Ansible playbook. Deploys both daemons + venv. |

## Deploy

```bash
# First-time deploy (creates user, venv, config dir, secret files, unit, scrape target):
ansible-playbook -i metrics-bridge/inventory.yml metrics-bridge/deploy.yml

# Subsequent code-only update:
ansible-playbook -i metrics-bridge/inventory.yml metrics-bridge/deploy.yml --tags code

# Config-only (e.g. added a new budka to the device-name map):
ansible-playbook -i metrics-bridge/inventory.yml metrics-bridge/deploy.yml --tags config
```

### MQTT credentials

The play creates `/etc/cb-prom/mqtt_user` and `/etc/cb-prom/mqtt_pass`
as 0640 files owned by the `cbprom` group. The `force: false` flag
keeps them across reruns so secrets aren't re-prompted. First-time
seeding options, in order of preference:

1. **Env vars** (zero-touch — preferred):
   ```bash
   CB_MQTT_USER='...' CB_MQTT_PASS='...' \
     ansible-playbook -i metrics-bridge/inventory.yml metrics-bridge/deploy.yml
   ```
2. **`--extra-vars`** (visible in `ps`, less preferred):
   ```bash
   ansible-playbook -i metrics-bridge/inventory.yml metrics-bridge/deploy.yml \
     -e cbprom_mqtt_user='...' -e cbprom_mqtt_pass='...'
   ```
3. **Manual** after deploy:
   ```bash
   sudo bash -c 'echo "real_user" > /etc/cb-prom/mqtt_user'
   sudo bash -c 'echo "real_pass" > /etc/cb-prom/mqtt_pass'
   sudo systemctl restart cbprom-exporter
   ```

If unset on first deploy, the files get the literal string `CHANGE_ME`
and the daemon will refuse to authenticate — `journalctl -u
cbprom-exporter` shows the failure clearly.

## Quick sanity check

```bash
# After deploy, on server-host:
systemctl status cbprom-exporter
curl -s http://localhost:9878/metrics | grep -c '^cb_'   # expect 50+
journalctl -u cbprom-exporter -f                          # tail logs

# In Grafana (Explore → mimir datasource):
cb_online{device_name="budka-example-1"}     # 1 = online, 0 = LWT offline
cb_battery_soc_percent                    # all boards
rate(cb_motion_total[5m])                 # motion events/s rolling
```

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

## Wire it into Grafana

The Alloy scrape target is auto-installed at
`/etc/alloy/app-metrics-cbprom.yml` (next to the UC96 one in the
same directory). Alloy's existing `local.file_match` glob picks
`app-metrics-*.yml` directly from `/etc/alloy/`; no river edit.
Confirm with:

```bash
sudo systemctl reload alloy
# Wait one scrape interval, then in Grafana → Explore → mimir:
{job="cbprom_exporter"}
```

For the dashboard side: clone the UC96 dashboard (`uc96-power-meters`)
as a starting point and swap the queries for `cb_*` series. Labels
are aligned so panels can be added to the existing per-budka pages
without restructuring.
