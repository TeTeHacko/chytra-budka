# Chytrá Budka — alerting & SLOs (config-as-code)

Symptom-based alerting rules and SLO definitions for the `cb_*` metrics that
[`cbprom`](../README.md) exports — version-controlled here so they can be
reviewed, tested, and restored, instead of living click-configured inside a
Grafana instance.

## Files

| File | What |
| --- | --- |
| `chytra-budka.alerts.yml` | Symptom-based alerting rules (device + backing services). |
| `chytra-budka.slo.rules.yml` | SLI recording rules + SLO-burn alerts. |

## Loading

**Mimir ruler** (the documented backend):

```sh
mimirtool rules load --address=https://<mimir> --id=<tenant> \
  metrics-bridge/alerts/chytra-budka.alerts.yml \
  metrics-bridge/alerts/chytra-budka.slo.rules.yml
```

Or import into **Grafana-managed alerting** (Alerting → import Prometheus
rules). Either way, point the notification policy at your own contact point
(the author uses XMPP/Jabber), and wire the load step into whatever
config-management deploys your Mimir/Grafana so the rules are deployed, not
hand-loaded.

`severity` label: **`page`** = notify a human now (user-visible outage);
**`ticket`** = look when convenient (degraded, not down).

## SLOs (per device, rolling 30 days)

| SLI | Definition (recording rule) | SLO target | Error budget |
| --- | --- | --- | --- |
| Availability | `cb:device_availability:ratio30d` = `avg_over_time(cb_online[30d])` | ≥ 99 % | ~7.2 h/mo down |
| Image delivery | `cb:photo_capture:rate1h` > 0 while awake | ≥ 95 % of awake hours | — |
| Health | `cb:selftest_healthy:ratio` = ok/total components | ≥ 99 % | — |

The image-delivery SLI is the product-level one — "is the box alive **and**
actually capturing." It's backed by `cb_cam_stalled` (wedged sensor),
`cb_photo_queue_dropped_total` (delivery loss), and the capture rate. Refine the
"awake hours" denominator with a daylight/PIR-activity signal when one exists;
today it's gated on mode != safe.

## Scrape targets

`cbprom` exposes Prometheus metrics on `:9878` — point any Prometheus/Alloy
scrape job at it (in the compose stack the port is published per `.env`,
`CBPROM_PORT`).

**Relay** (`chytra_relay_*` metrics on `:8765/metrics`) may run on a different
host than the bridge (wherever BirdNET-Go lives) — its scrape target belongs
in *that* host's scrape config:

```yaml
- targets: ["127.0.0.1:8765"]
  labels: { job: "chytra_budka_relay" }
```

The `RelayDown` / `RelayFfmpegFlapping` rules no-op until that target exists.
