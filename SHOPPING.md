# Parts list (rev3.2)

Plain BOM — source the parts wherever you like. Prices are **approximate EUR**
from budget module sellers (AliExpress-class, single-unit); shipping from Asia
commonly takes **3–6 weeks**, EU-warehouse listings cost a bit more and halve
the wait. The *search term* column identifies the exact part — some modules
have near-identical siblings that do NOT work (see notes).

## Bench minimum — runs the whole firmware on USB (~€28)

Camera, mic, PIR photo-trap, ambient sensing, MQTT/HA, IR night fill — powered
from USB-C, no solar chain, no enclosure.

| Part | Qty | Search term | ~€ | Notes |
| --- | --- | --- | --- | --- |
| Seeed XIAO ESP32-S3 **Sense** | 1 | "XIAO ESP32S3 Sense antenna connector" | 14 | The **Sense** variant (camera + PDM mic + microSD). Current listings ship the OV3660 camera even where the page says OV2640 — both fine, firmware expects OV3660. Get the U.FL-antenna variant. |
| SHT41 I²C breakout | 1 | "SHT41 breakout" | 6 | Harder to find than its siblings. SHT40 works with a ~1-line driver change, SHT31 needs ~10 lines — staying with SHT41 (accuracy, 0.4 µA idle) is recommended. |
| AM312 PIR mini | 1 | "AM312 PIR mini sensor" | 1.50 | 3.3 V, ~12 µA idle — the photo-trap trigger. |
| MAX17048 fuel gauge breakout | 1 | "MAX17048 lipo fuel gauge module" | 4 | True SOC % over I²C (0x36) — the hybrid power modes key off it. MAX17043 is the more common sibling; get the 17048. |
| 940 nm IR LED 3 mm | 1 (pack) | "940nm IR LED 3mm" | 1.50 | Night illumination invisible to birds. |
| AO3400 N-MOSFET SOT-23 | 1 (pack) | "AO3400 SOT-23" | 1 | IR LED driver (draws more than a GPIO can source). |
| 220 Ω resistor | 1 (pack) | — | 0.50 | IR LED series resistor. |
| microSD ≤32 GB | 1 | "32GB microSD class10" | 5 | FAT32, mounted via SDIO 1-bit. |

Assumed on hand: breadboard, Dupont F-F wires, USB-C cable.

## Deployment additions — the outdoor/solar stage (~+€35)

> The solar chain below is the **designed** power path — no node runs it yet
> (see [firmware/POWER.md](firmware/POWER.md) for the as-built reality). Treat
> these rows as the plan, not a field-proven recipe.

| Part | Qty | Search term | ~€ | Notes |
| --- | --- | --- | --- | --- |
| WaveShare Solar Power Manager (**basic**) | 1 | "Waveshare Solar Power Manager 14500 CN3791" | 13 | **Must be the CN3791-based basic variant** — see the rationale below; the B/C/D variants kill the battery at idle. Onboard 14500 holder stays unused; battery connects via the PH2.0 lead. 6–24 V solar in, 5 V/1 A out. |
| 2× 18650 holder, 1S2P, with protection | 1 | "2x 18650 battery holder PCB protection JST-PH" | 3 | DW01+8205A protection, JST-PH 2.0 lead matches the WaveShare BAT input. |
| 18650 cells (NCR18650B or VTC6) | 2 | "NCR18650B 3400mAh genuine" | 12 | Fakes are common — buy from a reputable seller. **Cells must be matched**: same brand, capacity, ideally the same lot. |
| Solar panel 18 V / 10 W mono | 1 | "10W 18V monocrystalline solar panel" | 10 | 18 V panel + the 6–24 V input gives margin in shade. Pre-soldered cable + DC plug. |
| IP65 ABS enclosure 100×68×50 mm | 1 | "ip65 plastic enclosure 100x68x50" | 4 | Screw lid + gasket — the weather shell. |
| 2.4 GHz SMA dipole + U.FL→SMA pigtail | 1 | "2.4ghz antenna SMA dipole U.FL pigtail" | 4 | External bulkhead through the side wall — a U.FL grommet pass-through leaks and kinks. |
| M16 cable gland | 1 | "M16 IP68 cable gland nylon" | 0.60 | Solar cable entry. |
| Brass M3 heat-set inserts + stainless M3 screws | 10 | "brass M3 thread insert heat set" | 2 | Mounting box ↔ nest box + inner bracket. |
| Heatsink 4×4×2 mm + thermal pad | 1 | "ESP32 small heatsink 4mm" | 1.20 | The XIAO Sense SoC reaches ~64 °C bare while streaming. |

## Optional

| Part | Search term | ~€ | Notes |
| --- | --- | --- | --- |
| INA226 V/I monitor | "INA226 module" | 2 | Solar input telemetry — `ina226.c` gracefully skips if absent. |
| BMP388 / BME280 breakout | "BMP388 breakout" | 2–4 | Barometer on top of the SHT41. |
| DS18B20 waterproof probe | "DS18B20 waterproof" | 1 | External temperature on a long lead. |
| Capacitive soil moisture v2 | "capacitive soil moisture v2" | 2 | For a planter below the box. |
| Custom carrier PCB (JLCPCB + LCSC parts) | — | ~25 | Replaces the flying-lead spider — design + BOM in [firmware/hw/README.md](firmware/hw/README.md). |

**Totals:** bench ~€28 · +deployment ~€35 · +carrier PCB ~€25.

## Design rationale

### Why the CN3791 "basic" WaveShare and not the (C) variant

The (C) variant is the only WaveShare board with onboard 18650 holders, BUT its
SW6106 PMIC idles at **80 mA**: 80 mA × 24 h × 3.7 V = **7 Wh/day of idle
drain**, against ~5 Wh/day of solar input under tree shade — the battery dies
in 8 days doing nothing. The CN3791-based basic variant idles **<2 mA** (~70×
better) and is the right choice; it just needs the external 18650 holder.

### Why this BOM (rev3) over rev2

| Aspect | rev2 (Li-Pol + CN3791 + 3D-printed shell) | rev3 (off-the-shelf + 2× 18650) | Win |
| --- | --- | --- | --- |
| Battery capacity | 7 Wh (Li-Pol pouch 2000 mAh) | 25 Wh (2× 18650) | rev3 |
| Battery longevity | Li-Pol pouches swell after 1–2 hot summers | 18650 cells last 5–10 yrs in cycle | rev3 |
| Battery replaceable | Soldered/glued, hard | Slide-out of holder | rev3 |
| Outer enclosure UV | PETG degrades 1–2 seasons | ABS IP65 box rated decade+ | rev3 |
| Charger circuit | Hand-soldered CN3791 + AO3401 + 100 kΩ | One bolt-in WaveShare module | rev3 |
| SOC monitoring | None — guesstimate from VBAT ADC | I²C MAX17048 fuel gauge, ground truth | rev3 |
| Cost | 22 EUR | 60 EUR | rev2 |
| Cavity volume | 70×50×20 mm | 100×68×50 mm | rev2 (smaller) |
| Mic SNR | Onboard PDM (~61 dB) | Onboard PDM (rev3.2, ~61 dB) | tie |
| Antenna | U.FL through grommet (water+kink) | SMA bulkhead | rev3 |

(rev3.2 additionally dropped the external INMP441 mic — pin conflict with the
SDIO microSD slot — in favour of the XIAO Sense onboard PDM mic; SNR ~61 dB is
fine for BirdNET classification.)

### Power budget

The energy math behind this BOM (solar vs the three firmware modes) lives in
**[DEPLOYMENT.md](DEPLOYMENT.md#power-budget-summer-tree-shade)**. Short
version: ~5 Wh/day of solar under tree shade can't sustain continuous
streaming, so the firmware is power-staged (continuous → sound-triggered →
safe).

## Bench prep checklist (before deployment)

1. **Charge the 18650s separately** to full (4.20 V) on a regular Li-Ion
   charger before first install — the BMS works best with matched cells.
2. **Test camera + PDM mic + WiFi on USB-C alone** (no battery) before
   integrating anything.
3. **Insert the microSD** — verify the FAT32 mount (`sd:true` in `/selftest`).
4. **Flash with OTA enabled** so later updates don't need disassembly.
5. **Verify the WaveShare 5 V output** with battery + a bench PSU at 6–18 V on
   the solar input.
6. **Log MAX17048 SOC over I²C for 24 h** before deploying — confirms the
   hybrid-mode logic against your real cells.
7. **Drill the IP65 box**: camera lens ~10 mm, PIR Fresnel ~8 mm, IR LED
   ~3.5 mm, SMA bulkhead, one cable gland, 2 mm drain hole on the bottom.
8. **Silicone-seal** the lens + gland; drop a **silica-gel sachet** inside
   before the final close.
9. **Check RSSI > −75 dBm** at the mounting spot before fully sealing.
