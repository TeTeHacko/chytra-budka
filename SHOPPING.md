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

## Outdoor additions — the enclosure stage (~+€12)

The weather shell for a deployed unit (still powered over USB — from a mains
adapter or a powerbank routed through the gland):

| Part | Qty | Search term | ~€ | Notes |
| --- | --- | --- | --- | --- |
| IP65 ABS enclosure 100×68×50 mm | 1 | "ip65 plastic enclosure 100x68x50" | 4 | Screw lid + gasket — the weather shell. |
| 2.4 GHz SMA dipole + U.FL→SMA pigtail | 1 | "2.4ghz antenna SMA dipole U.FL pigtail" | 4 | External bulkhead through the side wall — a U.FL grommet pass-through leaks and kinks. |
| M16 cable gland | 1 | "M16 IP68 cable gland nylon" | 0.60 | Power-cable entry. |
| Brass M3 heat-set inserts + stainless M3 screws | 10 | "brass M3 thread insert heat set" | 2 | Mounting box ↔ nest box + inner bracket. |
| Heatsink 4×4×2 mm + thermal pad | 1 | "ESP32 small heatsink 4mm" | 1.20 | The XIAO Sense SoC reaches ~64 °C bare while streaming. |

**Solar + battery:** a charging stage (MPPT charger + 18650 cells + panel) is
designed but **not yet built or validated** — this list deliberately doesn't
sell you parts for it. The measured power budget it has to satisfy is in
[firmware/POWER.md](firmware/POWER.md).


## Optional

| Part | Search term | ~€ | Notes |
| --- | --- | --- | --- |
| INA226 V/I monitor | "INA226 module" | 2 | Solar input telemetry — `ina226.c` gracefully skips if absent. |
| BMP388 / BME280 breakout | "BMP388 breakout" | 2–4 | Barometer on top of the SHT41. |
| DS18B20 waterproof probe | "DS18B20 waterproof" | 1 | External temperature on a long lead. |
| Capacitive soil moisture v2 | "capacitive soil moisture v2" | 2 | For a planter below the box. |
| Custom carrier PCB (JLCPCB + LCSC parts) | — | ~25 | Replaces the flying-lead spider — design + BOM in [firmware/hw/README.md](firmware/hw/README.md). |

**Totals:** bench ~€28 · +outdoor enclosure ~€12 · +carrier PCB ~€25.

## Design notes

rev3.2 dropped the external INMP441 mic — pin conflict with the SDIO microSD
slot — in favour of the XIAO Sense onboard PDM mic; SNR ~61 dB is fine for
BirdNET classification. The enclosure is a plain ABS IP65 box (UV-stable for
a decade+, unlike a printed PETG shell) with the antenna on an SMA bulkhead —
a U.FL pass-through grommet leaks and kinks.


## Bench prep checklist (before deployment)

1. **Test camera + PDM mic + WiFi on USB-C alone** before integrating anything.
1. **Insert the microSD** — verify the FAT32 mount (`sd:true` in `/selftest`).
1. **Flash with OTA enabled** so later updates don't need disassembly.
1. **Drill the IP65 box**: camera lens ~10 mm, PIR Fresnel ~8 mm, IR LED
   ~3.5 mm, SMA bulkhead, one cable gland, 2 mm drain hole on the bottom.
1. **Silicone-seal** the lens + gland; drop a **silica-gel sachet** inside
   before the final close.
1. **Check RSSI > −75 dBm** at the mounting spot before fully sealing.
