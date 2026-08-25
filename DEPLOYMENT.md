# Deployment & environment

Field-deployment context for the box: site, climate, seasonal power strategy,
and the energy budget that drives the [hybrid firmware modes](README.md#how-it-works).
For the parts list see [SHOPPING.md](SHOPPING.md); for assembly see
[WIRING.md](WIRING.md).

## Site

- a temperate mountain range, near an upland forest site (~1000–1244 m elevation)
- Under a tree, within house WiFi range (line-of-sight through a window)
- No mains power available — solar + battery only

Substitute your own site; the climate table and power budget below are the
author's worked example and the reason the firmware is power-staged.

> **Status:** this documents the TARGET off-grid deployment the power design is
> built against — no unit is at this site yet. The pilot unit runs on a balcony
> from a folding panel → powerbank → USB; the measured as-built power story is
> [firmware/POWER.md](firmware/POWER.md).

## Power budget (summer, tree shade)

The single source of truth for the box's energy math. The three firmware modes
exist because the solar input under tree shade (~5 Wh/day) only covers
continuous streaming part of the time.

| Item                                       | Value                                       |
| ------------------------------------------ | ------------------------------------------- |
| ESP32-S3 deep sleep (XIAO board)           | ~1 mA (board LDO + indicator LED draw)      |
| ESP32-S3 active + WiFi streaming           | ~150 mA avg @ 3.7 V ≈ 0.55 W                |
| Onboard PDM mic                            | ~0.5 mA                                     |
| WaveShare PMM quiescent                    | ~2 mA                                       |
| **Continuous** mode, daily                 | ~13 Wh                                      |
| **Sound-triggered** mode, daily            | ~1.0–1.5 Wh                                 |
| Solar input (10 W panel under tree, ~10–15 % effective) | ~5 Wh/day                      |
| Battery capacity (2× 18650 NCR18650B parallel) | ~25 Wh                                   |

In practice the firmware spends ~70–80 % of the time in sound-triggered mode
(energy-positive, ~3.8 Wh/day net) and tops up the battery on clear days. The
dawn chorus (~4–9 AM) is the priority window for continuous mode when the
battery allows.

## Climate (an upland forest site / a nearby summit, 1213 m)

| Month | Avg Min °C | Avg Mean °C | Avg Max °C |
| ----- | ---------- | ----------- | ---------- |
| Jan   | -6.2       | -4.0        | -1.8       |
| Feb   | -6.1       | -3.8        | -1.4       |
| Mar   | -3.8       | -1.4        | 1.2        |
| Apr   | 0.1        | 3.2         | 6.7        |
| May   | 4.1        | 7.5         | 11.4       |
| Jun   | 7.4        | 10.7        | 14.6       |
| Jul   | 9.5        | 12.8        | 16.6       |
| Aug   | 9.7        | 12.8        | 16.6       |
| Sep   | 6.0        | 8.6         | 11.8       |
| Oct   | 2.1        | 4.4         | 7.2        |
| Nov   | -1.8       | 0.4         | 2.7        |
| Dec   | -4.9       | -2.8        | -0.6       |

- ~170 frost days/year
- ~25 days below -10 °C/year
- Extreme minimum: -30.4 °C (1956)
- Typical winter night: -5 to -7 °C, regularly -10 to -15 °C

## Seasonal strategy

### Summer (April–October) — the current build

- No heating needed (nights > 0 °C, so Li-Ion is fine to charge)
- 10 W panel for margin under tree shade
- 2× 18650 parallel = ~25 Wh capacity; the hybrid firmware is modelled to operate indefinitely
- The WaveShare module handles MPPT + BMS + cell balancing

### Winter upgrade (future, deferred)

- LiFePO4 4S + self-heating BMS (LiFePO4 also can't charge below 0 °C, but cycles
  better in the cold)
- 15–20 W panel on a pole above the canopy (avoiding tree shade entirely)
- PTC heater + DS18B20 for battery thermal management
- Or: bring the box indoors November–March (the lazy option)

## Field mounting checklist

See [SHOPPING.md](SHOPPING.md#pre-build-bench-steps) for the full pre-deployment
bench + drilling + sealing checklist (charge cells matched, drill lens/PIR/IR/SMA/
gland holes, silicone-seal the lens, silica-gel sachet, verify RSSI > -75 dBm
before final close).
