# Universal power front-end — ground-up architecture

> Design doc only — **no BOM, no schematic**. Everything in the repo so far is
> paper; nothing is built. This is a **clean ground-up design**, decided once and
> correctly — explicitly **not** an incremental "buy a cheap module now, replace
> it later" plan. Authored 2026-06-10, supersedes the earlier V1/V2 framing.
>
> Pairs with [`firmware/POWER.md`](../POWER.md) (measured ~1.1 W / 27 Wh/day load)
> and the firmware duty-model work. The energy budget is still firmware-gated —
> this board is designed to be **populated** to whatever load the firmware lands
> on (1–3 cells, panel to suit), not built to a fixed number.

## Goal

One power design that spans deployments differing in:

- **Battery:** 1, 2, or 3× 18650 — same cell type per budka, **never mixed**.
- **Solar:** any panel, "as available / as needed."

The win: **the only thing tuned per budka is population** — how many cells you
slot in and which panel you bolt on. Nothing electrical changes.

## Locked requirements (the foundation)

These came out of the conversation and are now firm. The charger topology must
satisfy **all** of them — this is why no single cheap off-the-shelf module fits.

1. **Variable solar input**, ~6–24 V, any wattage, with **real MPPT** (not a
   single fixed setpoint) so harvest holds up across different panels.
2. **1–3× 18650 in parallel** (1S1P/2P/3P), same type per budka. One shared 1S
   protection. Count must require **zero electrical retuning**.
3. **Low quiescent current** — hard lesson from the survey: SW6106-based modules
   idle at ~80 mA (~7 Wh/day burned doing nothing → dead in ~8 days under shade).
   Target **single-digit mA** for the whole idle chain.
4. **5 V to the XIAO** (5 V pin → onboard 3V3 LDO). Raw VBAT won't do — the LDO
   drops out near 3.4 V. Needs a boost stage.
5. **Low-temp-correct charging.** When **cold + sunny**: *pause charging the
   cell* (avoid lithium plating < 0 °C) but **keep powering the load from the
   panel** — don't waste daylight, don't needlessly discharge a cold cell. This
   needs **power-path management** (load on a SYS rail, not the bare battery node)
   + a **TS/NTC** charge-inhibit. Discharge stays allowed to −20 °C.
6. **Firmware lever.** Prefer a charger the firmware can read/override (I²C), so
   the duty-model logic can act on charge state / temp — not a black box.
7. **Telemetry:** true SOC % (MAX17048-class ModelGauge) + solar V/I.
8. **Field robust:** reverse-polarity protection on PV + battery, sane fusing.

## Target topology

```
[Solar 6–24V, any W] ──PV──▶ ┌────────────────────────────────┐
                             │ Wide-input MPPT charger + power- │
[NTC on cells] ───TS───────▶ │ path + TS (charge-inhibit <0°C)  │
                             │ — I²C-configurable PMIC preferred │
                             └──────┬──────────────────┬────────┘
                                SYS │              BAT  │ (1S)
                                    │                   ▼
                            ┌───────▼──────┐   [ 1–3× 18650 ∥, one 1S PCM ]
                            │  5 V boost    │          │
                            └──────┬────────┘          │ VBAT tap
                                   ▼ 5 V               ▼
                            [ XIAO ESP32-S3 ]   [ MAX17048 ]  I²C 0x36 (SOC %)
                              5V pin → 3V3 LDO

  Solar V/I: from the PMIC's integrated ADC if it has one,
             else a discrete INA226 (I²C 0x40).
```

Why this spans 1–3 cells + variable panel with no electrical retuning:

- **The whole chain is 1S.** Cells go in **parallel** → rail stays ~3.7 V
  regardless of count; more cells = more capacity + lower ESR, nothing else.
  (Series is wrong — it would move the rail and break the charger, the boost
  setpoint, and the gauge.)
- **Real MPPT over 6–24 V** spans the panel range; a bigger panel just charges
  faster up to the charge-current limit.
- **The gauge reports a percentage, not amp-hours.** MAX17048 derives SOC from
  the 1S voltage curve, identical for 1P/2P/3P → **% is correct for any count,
  no recalibration**. Only runtime-in-hours scales, inferred downstream.

## The one open foundational decision: charger / PMIC

This is where the real engineering choice lives. Two ways to satisfy all 8
requirements:

### A — Integrated I²C buck-boost PMIC with MPPT + power-path + TS  *(recommended)*

A modern single-chip solar PMIC (TI **BQ2579x** family — e.g. BQ25798 — or
equivalent) that integrates: wide-input (~24 V) buck-boost, **real MPPT**,
**power-path (SYS output)**, **TS pin**, **I²C** control, and often an
**integrated V/I ADC**.

- **Hits every requirement at once:** variable-solar MPPT (#1), low-temp
  run-from-panel via SYS+TS (#5), firmware lever via I²C (#6), and the on-chip
  ADC can fold in solar V/I telemetry — **possibly retiring the INA226** (#7).
- **Cost:** QFN + inductor + careful layout; more design effort than bolting
  modules together. For a one-off JLCPCB field board that's acceptable, and it's
  the *correct* foundation rather than a compromise.
- **Still external:** the 5 V boost to the XIAO (the PMIC's SYS is ~battery
  voltage), the MAX17048 (better SOC than a charger's fuel estimate), and the 1S
  cell protection.
- ⚠ **Verify against datasheets before committing:** exact input abs-max vs a
  21 V panel's Voc-cold, quiescent/ship-mode current, whether MPPT is true sweep
  or VINDPM-style, single-cell support, package hand-/JLC-assembly feasibility.

### B — Discrete: wide-input MPPT charger + explicit power-path + NTC

A standalone MPPT charger (e.g. **LT3652HV**, which has an NTC temp-qualify pin
and wide input) + a separate power-path/ideal-diode load switch + a 5 V boost +
INA226 for solar telemetry.

- **More parts, more flexible, no I²C** (so weaker on requirement #6 — firmware
  can't read/override the charger directly).
- Power-path isn't integrated, so it's extra circuitry to get "run from panel"
  right.
- Reasonable fallback if the integrated PMIC turns out hard to source / assemble.

**Recommendation:** lean **A** (integrated PMIC). It's the only option that nails
variable-solar *and* low-temp-correct charging *and* the firmware lever in one
coherent part, and it can simplify the telemetry. B is the fallback if sourcing
or assembly blocks A. → **Now resolved to the TI BQ25798** — see *Charger
candidates* below.

> Note: this **drops the CN3791 WaveShare module** as the core. CN3791 has no
> power-path and no TS pin, so it structurally can't do requirement #5 — building
> on it would be knowingly buying the thing we'd have to replace.

## Per-budka tuning matrix

| Knob | Range | Changes electrically |
| --- | --- | --- |
| Cell count | 1 / 2 / 3 × 18650, parallel, identical type | **nothing** — capacity & ESR only |
| Cell type | any 1S Li-ion 18650 (same per budka) | charge time (C-rate), runtime |
| Panel | any 6–24 V, any wattage | charge speed (MPPT handles the rest) |

Populate to the firmware's measured load once the duty model lands.

## Electrical notes (for the detailed design)

- **Charge current vs cell count.** Set the charge-current limit safe for a
  **single** cell (≤ ~0.5C, e.g. ~1 A for a 3400 mAh cell). Then 1/2/3 cells are
  all safe — fewer cells just reach full sooner, no overcharge risk. (An I²C PMIC
  can even raise the limit for 3-cell builds in firmware — bonus from option A.)
- **Parallel insertion balancing.** 1S parallel cells self-balance on the common
  rail; no per-cell balancing. The catch is *first* insertion at mismatched SoC →
  big balancing currents. Mitigation: **charge every cell to 4.2 V before first
  install**, same brand/capacity/ideally lot.
- **Protection PCM.** One 1S protection (DW01+8205A class) covers the whole
  parallel pack; load ~0.3 A, charge ~1 A — far under the FET rating.
- **TS / NTC** mounted on/against the cells (their temp, not the PCB's) to gate
  charging. Firmware can additionally use the existing SHT41 box temp as a
  cross-check / override on an I²C PMIC.
- **MAX17048** stays (best SOC). **INA226** stays only if the chosen charger has
  no usable integrated ADC — decide once the PMIC is picked.

## Charger candidates — research findings (2026-06-10)

Deep-research pass (fan-out search + adversarial verification of datasheet
claims, 24/25 claims confirmed 3-0/2-1, 1 refuted). Specs are from primary
TI/ADI datasheets; **availability/price was NOT independently verified** (see
open items).

| IC | Input / MPPT | Iq | Power-path | TS/NTC | I²C | Integ. ADC | 1S | Package / complexity | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **BQ25798** | buck-boost, **true MPPT** (autonomous VOC-scaling, periodic resample). **VBUS 3.6–24 V op / 30 V abs-max** (datasheet §8.1/§8.3, confirmed 2026-06-11 — the research's refutation was a *false negative*) | **17 µA** batt-only · 11 µA ship · 0.5 µA shutdown · ~540 µA w/ ADC | ✅ NVDC, SYS 2.5–19.4 V | ✅ JEITA cold/hot inhibit | ✅ | ✅ 16-bit, 7 ch (IBUS/IBAT/VBUS/VBAT/VSYS/TS/TDIE) → **retires INA226** | ✅ 1–4 cell, ICHG 0.05–5 A @10 mA (POR 1 A) | 4×4 mm 29-pin QFN-29 · LCSC C2876593, ~$2.6, 5.5k stock | **★ Lead** — covers the most in one part |
| **LTC4015** | **buck-only**, **true sweep MPPT** (sweeps VIN_UVCL, picks peak). **4.5–35 V / 40 V abs-max — VERIFIED** (best cold-panel headroom) | low (charger), needs ext. power stage | ✅ but VSYS is battery-referenced | ✅ NTC | ✅ | ✅ 14-bit + coulomb counter → retires INA226 | ✅ 1S selectable | QFN **+ external N-FETs, inductor, sense R** — most layout/assembly | Runner-up; **can't step up a sagging small panel** |
| **LT8490** / **LT8491** | buck-boost, **true MPPT** (P&O + full panel scan /3 min), **6–80 V** | — | controller | ✅ NTC | ❌ (8490) · ✅ (8491) | analog IMON only | — | ext. FETs/inductor | Great input range; 8490 fails I²C (#6); scan slightly weak on diode-less small panels |
| **BQ24650** | "MPPT" = fixed MPPSET voltage threshold (not a sweep); 5–28 V / 33 V | — | ❌ no SYS rail | ✅ | ❌ | ❌ | — | ext. FETs/inductor/sense, 16-pin QFN | Fails #5a / #6 / #7 |
| **BQ2589x** (25895/96) | VINDPM threshold + ICO (not true MPPT); **op 3.9–14 V, abs-max 22 V** → **too low for an 18–21 V panel** | — | ✅ NVDC, SYS 3.5–4.5 V | ✅ | ✅ | partial | ✅ 1 cell | QFN | Fails #1 + input range |
| **BQ25185** | no MPPT (battery-tracking VINDPM); op ≤18 V / abs-max 25 V → needs pre-reg | — | ✅ SYS 4.5 V | ✅ cold/hot suspend | ❌ | ❌ | ✅ | small | Good charger-*stage* for a two-stage build |
| **Two-stage:** wide buck pre-reg → **BQ24074** → 5 V boost | pre-reg crudely sets the operating point; BQ24074 input only ~10 V | — | ✅ DPPM, OUT ~4.4 V, **runs with no battery** | ✅ fixed 0–50 °C window = **HW below-0 °C charge inhibit** | ❌ | ❌ | ✅ | more parts | Fallback; satisfies #5 in hardware but fails #6/#7 |

### Verdict

**TI BQ25798** is the single best-fit: it's the only evaluated IC that combines
true MPPT + 1S + sub-1 A charge + very low Iq + NVDC power-path + TS/JEITA +
I²C + integrated ADC in one part, and its buck-**boost** topology keeps charging
a **small 6 V panel that sags under load** below the cell voltage — which the
buck-only LTC4015 cannot. The on-chip ADC **retires the INA226**.

But the research is honest that **no single IC is perfect** — three things stay:

1. **Input ceiling vs a cold panel — RESOLVED (2026-06-11).** Datasheet §8.1/§8.3:
   **VBUS 3.6–24 V recommended operating, 30 V absolute max** (VAC1/VAC2 valid-input
   detection also 3.6–24 V). Covers 6 V and 12 V panels cleanly. The edge case is an
   **18 V-nominal panel** (Voc ~22 V @ 25 °C): at −20 °C, Voc rises ~15 % (Si tempco
   ≈ −0.33 %/°C) → **~25 V**, which pokes just over the 24 V *recommended* ceiling but
   stays well under the 30 V abs-max. Mitigation: prefer a panel with Voc ≤ ~20 V @
   25 °C (so cold Voc ≤ 24 V), **or** add an input **TVS/clamp** (~26–28 V) since
   abs-max is 30 V. Not a blocker. (LTC4015 35 V / LT8490 80 V remain the higher-headroom
   fallbacks if a true 21 V-class panel is ever required.)
2. **5 V boost still required.** No charger here outputs a regulated 5 V system
   rail; the BQ25798 SYS tracks the 1S cell and its 2.8–22 V OTG output is a
   USB-OTG/Backup reuse, not a validated always-on 5 V supply. Keep a dedicated
   1S→5 V boost.
3. **SOC is coulomb-counting, not ModelGauge.** Both BQ25798 and LTC4015 give
   charge-integration telemetry (drifts, needs periodic full/empty re-cal), not
   MAX17048-class absolute SOC%. They retire the INA226 (solar V/I), but **keep
   the MAX17048** if absolute SOC% matters (and check it doesn't clash with the
   charger on the I²C bus).

### Open items

Gating items — **both CLOSED 2026-06-11:**

- ✅ **VBUS abs-max / operating ceiling** — 3.6–24 V op / 30 V abs-max (see
  Verdict #1; 18 V-panel cold-Voc handled by panel choice or a TVS clamp).
- ✅ **LCSC availability** — BQ25798RQMR, QFN-29 (4×4), **5,493 in stock**,
  **$2.62 @1 / $2.22 @10**. (JLC *basic* vs *extended* classification still worth
  a glance on the JLC part page, but stock/price are non-issues.)

Remaining for the detailed schematic (not blocking the decision):

- **Can the OTG 2.8–22 V output be the always-on 5 V rail** (eliminating the
  boost), or is a separate 1S→5 V boost mandatory? (Assume boost until validated.)
- **Coulomb-counter SOC good enough** after re-cal, or add MAX17048 — and confirm
  it doesn't clash with the charger's own ADC/telemetry on the I²C bus.

Primary sources: [BQ25798](https://www.ti.com/lit/ds/symlink/bq25798.pdf) ·
[LTC4015](https://www.analog.com/media/en/technical-documentation/data-sheets/4015fb.pdf) ·
[LT8490](https://www.analog.com/media/en/technical-documentation/data-sheets/8490fa.pdf) ·
[BQ24650](https://www.ti.com/lit/ds/symlink/bq24650.pdf) ·
[BQ24074](https://www.ti.com/lit/ds/symlink/bq24074.pdf) ·
[BQ25185](https://www.ti.com/lit/ds/symlink/bq25185.pdf) ·
[BQ25896](https://www.ti.com/lit/ds/symlink/bq25896.pdf) ·
[BQ25798 @ LCSC](https://lcsc.com/product-detail/C2876593.html)

## Cross-references

- [`firmware/POWER.md`](../POWER.md) — measured load, sizing math, firmware
  duty-model fork (the energy target this board gets populated to).
- [`SHOPPING.md`](../../SHOPPING.md) — earlier rev3 BOM + the CN3791-vs-SW6106
  quiescent-current rationale (the source of requirement #3). Its CN3791 core is
  **superseded** by this doc's power-path requirement.
- [`firmware/hw/README.md`](README.md) — rev3.2 carrier, pin map, I²C bus map.
