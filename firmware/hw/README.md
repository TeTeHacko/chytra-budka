# Chytrá Budka rev3.2 — Hardware

KiCad 10 project + automation via [KiKit](https://github.com/yaqwsx/KiKit).

## What changed vs rev3.1

| Change                    | Reason                                                                                                                                                              |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Camera enabled**        | Photo-trap mode — the XIAO Sense already has it built in, just unused in firmware until now. Trigger: PIR / VAD burst / MQTT command → JPEG → microSD + HTTP relay + MQTT notif. |
| **+ SHT41 (T+RH sensor)** | Ambient temperature and humidity, condensation monitoring inside the box. I²C 0x44 shares the bus with the MAX17048.                                                |
| **+ AM312 PIR**           | Wake-on-motion for the photo-trap, GPIO D1 (RTC-capable wake; moved from D2 after a harness redo).                                                                  |
| **+ 940 nm IR LED**       | Night views — invisible to the birds. PWM via GPIO.                                                                                                                 |
| Carrier PCB _(optional)_  | Eliminates the spider's web of flying leads — XIAO + WaveShare modules onto a dedicated board with SMD MAX17048, SHT41, JST connectors. JLCPCB ~€5 for 5.           |

## BOM

The bill of materials (parts, search terms, and prices) lives in one place:
**[SHOPPING.md](../../SHOPPING.md)**. This doc covers the PCB/schematic side
only. The carrier-PCB parts (MAX17048, SHT41, passives, JST) are listed under
SHOPPING.md's *Optional — custom carrier PCB* section.

## Pin assignment rev3.2 — XIAO ESP32-S3 Sense

> For a "wire-by-wire" guide on how to physically connect the modules, see
> [`WIRING.md`](../../WIRING.md) in the repo root. This table is from the
> firmware's point of view.

| XIAO pin | GPIO   | Function                        | Connected to                                    |
| -------- | ------ | ------------------------------- | ----------------------------------------------- |
| D0       | GPIO1  | ADC / free                      | —                                               |
| D1       | GPIO2  | **PIR wake** (RTC)              | AM312 OUT                                       |
| D2       | GPIO3  | **IR LED PWM**                  | AO3400 MOSFET gate → IR LED                     |
| D3       | GPIO4  | **Capture indicator LED**       | visible LED (active-high, 330 Ω series)          |
| D4       | GPIO5  | **I²C SDA** (shared bus)        | MAX17048 SDA, SHT41 SDA, INA226 SDA             |
| D5       | GPIO6  | **I²C SCL** (shared bus)        | MAX17048 SCL, SHT41 SCL, INA226 SCL             |
| D6       | GPIO43 | **I²C1 SDA** (secondary bus)    | optional second SHT41 / MAX17048 quarantine      |
| D7       | GPIO44 | **I²C1 SCL** (secondary bus)    | —                                               |
| D8       | GPIO7  | **SDMMC CLK** (internal Sense)  | microSD slot on the Sense board                 |
| D9       | GPIO9  | **SDMMC CMD** (internal Sense)  | microSD slot on the Sense board                 |
| D10      | GPIO8  | **SDMMC DAT0** (internal Sense) | microSD slot on the Sense board                 |
| 5V       | —      | 5V in                           | WaveShare USB OUT                               |
| 3V3      | —      | 3V3 out (internal LDO)          | MAX17048 VCC, SHT41 VCC, AM312 VCC, INA226 VCC  |
| GND      | —      | GND                             | common ground                                   |

**What changed vs rev3.1:** D8/D9/D10 (GPIO 7/8/9) were planned in rev3.1 for an
external INMP441 over I²S. In rev3.2 that external mic was dropped (pin conflict
with the onboard SDIO microSD) and these pins are **hard-wired to the microSD
slot on the Sense expansion** — don't connect anything external there, or you'll
break the SD card. The mic is now the Sense's built-in PDM on GPIO41/42
(internal, no wire).

**Internal (XIAO Sense onboard):**

- **OV3660** (AliExpress XIAO Sense ships OV3660; Seeed page still lists OV2640) → LCD_CAM bus + SCCB I²C port 1 on GPIO40/39
- PDM microphone MSM261D3526H1CPM → GPIO42 (CLK) / GPIO41 (DATA)
- microSD slot → SDIO 1-bit on GPIO 7/8/9 (= D8/D9/D10 user pads)
- 8 MB octal PSRAM
- U.FL antenna connector

**I²C bus map (port 0, shared external, SDA=GPIO5, SCL=GPIO6):**

| Address | Device                    | Detection                |
| ------- | ------------------------- | ------------------------ |
| 0x36    | MAX17048 (fuel gauge)     | optional, gracefully skipped |
| 0x40    | INA226 (solar V/I/P)      | optional, gracefully skipped |
| 0x44    | SHT41 (T + RH)            | optional, gracefully skipped |

(The camera SCCB runs on a **separate** I²C port 1 via pins GPIO40/39, so
external I²C devices don't collide with it.)

## KiCad library setup

This project uses symbols outside the default KiCad libraries. Clone them before
opening:

```bash
mkdir -p ~/kicad-libs && cd ~/kicad-libs

# Seeed Studio — XIAO ESP32-S3
git clone https://github.com/Seeed-Studio/OPL_Kicad_Library.git seeed
# or: git clone https://github.com/limengdu/Seeeduino-XIAO-ESP32S3-KiCAD-libraries.git seeed-xiao

# WaveShare modules (if you want their symbol; otherwise a generic 4-pin header)
git clone https://github.com/waveshareteam/Waveshare-KiCAD.git waveshare
```

Then in KiCad: **Preferences → Manage Symbol Libraries → Project tab → Add** the
paths to the `.kicad_sym` files.

**Available from the default KiCad libs (no download):**

- `Sensor_Humidity:SHT4x` — SHT41
- `Battery_Management:MAX17261xxTD` — generic fuel gauge (not exactly the
  MAX17048, but the same 8-pin TDFN footprint)
- `MCU_Espressif:ESP32-S3-WROOM-1` — if you wanted the bare module instead of the XIAO

**For an exact MAX17048 + OV3660:** download from
[SnapEDA](https://www.snapeda.com/) or
[Component Search Engine](https://componentsearchengine.com/) (free, registration).

## Build and export

### Workflow A — generate via SKiDL (Python, code-first)

The schematic is defined in `generate_schematic.py` as Python code (component
descriptions + nets + connections). From it we generate:

```bash
cd firmware/hw
make netlist        # → out/chytra-budka.net (KiCad netlist) + out/chytra-budka.dot
make blockdiagram   # → out/blockdiagram.svg (graphviz visualisation of connections)
```

**From the netlist you make the PCB layout in Pcbnew:**

```bash
kicad chytra-budka.kicad_pro
# 1. Open Pcbnew (File → New PCB, or open the .kicad_pcb)
# 2. File → Import → Netlist → pick out/chytra-budka.net
# 3. Click "Update PCB" — all parts appear with a ratsnest
# 4. Place the parts and route
# 5. Save → the .kicad_pcb is ready for fabrication
```

**For a full Eeschema schematic:**
SKiDL's `generate_schematic()` has a known bug in 2.2.x with KiCad 9
(`'NoneType' has no attribute 'tx'`). Workaround: draw the .kicad_sch by hand in
Eeschema from blockdiagram.svg + the pin-assignment table above. For
documentation (PDF), blockdiagram.svg is enough.

### Workflow B — fabrication + documentation (after the PCB is done)

```bash
make erc        # Electrical Rules Check (after drawing the .kicad_sch in the GUI)
make drc        # Design Rules Check on the .kicad_pcb
make gerbers    # → out/gerbers.zip for JLCPCB upload
make schematic  # → out/chytra-budka-schematic.pdf
make bom        # → out/bom.csv
make 3d         # → out/3d/index.html (HTML 3D preview via KiKit)
make all        # netlist + blockdiagram + schematic + bom
```

### Why SKiDL instead of drawing by hand?

- **Reproducible:** schematic as code, diff-friendly, code review in a PR.
- **Refactor-safe:** renaming a net = one edit in Python, not 20 clicks in the GUI.
- **No layout fight:** SKiDL's `generate_schematic()` is flaky, so it doesn't
  produce a pretty schematic — netlist + Pcbnew layout skips the visual schematic
  and goes straight to the PCB. `blockdiagram.svg` serves documentation.
- **Free ERC:** SKiDL runs ERC on every run (warnings in stdout).

## Carrier PCB — design (TODO)

Recommended layout if you decide to go the custom-board route:

```
┌──────────────────── 50 × 60 mm, 2-layer ────────────────────┐
│  ┌──────────────┐                                           │
│  │ XIAO ESP32-S3│   ← pin header 2× 7-pin (female)          │
│  │     Sense    │                                           │
│  └──────────────┘                                           │
│                                                             │
│  WaveShare PMS pin header (2.54 mm, 5-pin)                  │
│   [USB+] [USB-] [GND] [BAT+] [BAT-]                         │
│                                                             │
│   U1: MAX17048G+T (TDFN-8)                                  │
│   U2: SHT41-AD1B (DFN-4)                                    │
│   J1: JST-XH 3-pin → AM312 PIR                              │
│   J2: JST-PH 2-pin → IR LED                                 │
│   J3: 5.5/2.1 mm DC barrel (alternative solar in)          │
│   D1: 1N4148 ESD diode on PIR OUT                           │
│   R1-R3: I²C pull-up 10 kΩ × 2 + IR LED current limit       │
│   C1-C4: 100 nF decoupling × 4                              │
│                                                             │
│   M3 mounting holes × 4 (corners)                          │
└──────────────────────────────────────────────────────────────┘
```

> Note: rev3.1's external INMP441 mic (and its JST connector) was dropped — the
> mic is now the Sense's onboard PDM, so the carrier needs no mic connector.

JLC fab options (preset `presets/jlcpcb.json`): 2-layer, 1.6 mm FR4, lead-free
HASL, green soldermask, white silk, 1 oz copper. ~$5 / 5 pcs + $5 shipping ≈
€10 total.

## Reference

- [XIAO ESP32-S3 Sense pinout](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/#hardware-overview)
- [WaveShare Solar Power Manager (basic) wiki](https://www.waveshare.com/wiki/Solar_Power_Manager)
- [MAX17048 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX17048-MAX17049.pdf)
- [SHT41 datasheet](https://sensirion.com/products/catalog/SHT41)
- [KiKit documentation](https://yaqwsx.github.io/KiKit/latest/)
