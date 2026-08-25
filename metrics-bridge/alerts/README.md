# Chytrá Budka — alerting & SLOs (config-as-code)

Until this directory existed, the project's alerting was "described" in the
(private) observability notes as click-configured Grafana alerts
over Mimir + an XMPP contact point — **none of it version-controlled**, so it
couldn't be reviewed, tested, or restored. These files are now the source of
truth.

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
rules). Either way, point the notification policy at the existing XMPP/Jabber
contact point. Wire this into the Ansible/CD that manages Mimir so it's
deployed, not hand-loaded.

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

`cbprom` (`:9878`) and `cbd-enroll` (`:9879`) are scraped on the bridge host via
the Alloy file_sd targets in `../templates/app-metrics-{cbprom,enroll}.yml.j2`
(deployed by `../deploy.yml`).

**Relay** (`chytra_relay_*` metrics on `:8765/metrics`) runs on the **birdnet
host**, not the bridge host, so its scrape target belongs in that host's Alloy
config — add an `app-metrics-relay.yml` there:

```yaml
- targets: ["127.0.0.1:8765"]
  labels: { job: "chytra_budka_relay" }
```

The `RelayDown` / `RelayFfmpegFlapping` rules no-op until that target exists.

## Still out of repo (follow-up)

Grafana **dashboards** are still click-configured. Export them to JSON
(`Dashboard → Share → Export → Save to file`) and commit under a
`grafana/dashboards/` dir, provisioned via Ansible like these rules — so the
whole observability surface is config-as-code, not trapped in a Grafana DB.
