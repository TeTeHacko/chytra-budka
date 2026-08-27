# WIRING — wire-by-wire for the bird box

This page tells you **which wire clips where** — no GPIO numbers to memorise.
For every module in [SHOPPING.md](SHOPPING.md) that's already on your bench,
there's a *"label on the module → label on the XIAO"* table below plus a short
test to confirm it came up after reset.

> **Already handled:** the Sense expansion (camera, mic, microSD) mounts on the
> **underside** of the XIAO via the B2B connector. No wiring — all its signals
> run through the hidden flat-FPC tab. The XIAO's top pads (D0–D10, 3V3, GND,
> 5V) stay free and are what you use for the external modules below.

---

## XIAO ESP32-S3 — where each pad is

View from **above** (USB-C connector at the top, antenna pad at the bottom).
14 pads total, 7 per side. The pads are numbered in gold around the board edge:

```
                     USB-C
                  ┌────────┐
       (GPIO1)  D0│●      ●│5V
       (GPIO2)  D1│●      ●│GND
       (GPIO3)  D2│●      ●│3V3
       (GPIO4)  D3│●      ●│D10  (GPIO8)
       (GPIO5)  D4│●      ●│D9   (GPIO9)
       (GPIO6)  D5│●      ●│D8   (GPIO7)
       (GPIO43) D6│●      ●│D7   (GPIO44)
                  └────────┘
                  U.FL antenna
```

**What each pad is used for** in the current firmware (rev3.2):

| Pad  | GPIO   | Role in firmware                  | Status    |
| ---- | ------ | --------------------------------- | --------- |
| **D0**   | GPIO1  | **Reed switch IN** (door/lid contact, NVS opt-in) | optional |
| **D1**   | GPIO2  | **PIR signal IN** (RTC wake)  | used      |
| **D2**   | GPIO3  | **IR LED PWM output** (AGC-gated, MOSFET gate) | used |
| **D3**   | GPIO4  | **Capture indicator LED** (visible, every photo) | used |
| **D4**   | GPIO5  | **I²C SDA** (shared bus)      | used      |
| **D5**   | GPIO6  | **I²C SCL** (shared bus)      | used      |
| **D6**   | GPIO43 | **I²C1 SDA** (secondary bus, bit-bang) | used |
| **D7**   | GPIO44 | **I²C1 SCL** (secondary bus, bit-bang) | used |
| D8   | GPIO7  | SDMMC CLK (internal, Sense SD)    | **taken by the Sense card** |
| D9   | GPIO9  | SDMMC CMD (internal, Sense SD)    | **taken by the Sense card** |
| D10  | GPIO8  | SDMMC DAT0 (internal, Sense SD)   | **taken by the Sense card** |
| 3V3  | —      | output from the internal LDO (max ~500 mA) | module power |
| 5V   | —      | input from USB-C / ext. 5 V supply | not used for peripherals |
| GND  | —      | common ground                     | —         |

> **Important:** D8–D10 (GPIO 7/8/9) are *internally* wired to the microSD slot
> on the Sense expansion. **Don't connect anything external there**, or you'll
> break the SD card. Use D0–D7 for external modules; D6/D7 are an optional
> second I²C bus and are not used for a UART console (the firmware logs over
> USB-CDC).

> **Runtime pin map** (firmware ≥ 680721f, phases 1–10): the role of each of
> D0–D7 is **runtime-switchable** via the schema knobs `pin_d{0..7}_fn` (the HA
> dashboard "Pin map" section, or MQTT `cmd/cfg/pin_dN_fn <label>`). The table
> above shows the **rev3.2 defaults**, not a fixed assignment. Changes take
> effect **after a reboot**. Valid functions: `none`, `reed`, `pir`, `ir_led`,
> `capture_led`, `i2c0_sda`/`scl`, `i2c1_sda`/`scl`, `uart_tx`, `uart_rx`.
> Cross-validation: singletons (ir_led, capture_led, UART) max 1× per slot; pairs (I²C, UART)
> must be complete (otherwise the module refuses to init at boot). Reed/PIR
> support **multiple instances** up to 4× per device. You can see the current
> map at `http://<box>/` in the "Pin map" section.

---

## Universal convention

If you keep the coloured wires from a kids' kit, stick to:

- **red** = 3V3 (NEVER 5V into these modules!)
- **black** = GND
- **yellow / white** = data (SDA, OUT, PWM)
- **blue / green** = clock (SCL)

All modules below run on **3V3**. Never connect them to the 5V pad — the SHT41
and MAX17048 would survive 5V, but the bus pull-ups are 3V3 and the XIAO is not
5V-tolerant on its inputs.

---

## 1. SHT41 — temperature and humidity (I²C 0x44)

The breakout has 4 pads on a header (Adafruit ID 5776 and the Chinese clones use
the same labels):

| SHT41 module | XIAO pad | Position on the XIAO     |
| ----------- | -------- | ------------------------ |
| `VIN` / `VDD` / `3V3` | **3V3**  | right side, 3rd from top |
| `GND`       | **GND**  | right side, 2nd from top |
| `SDA`       | **D4**   | left side, 5th from top   |
| `SCL`       | **D5**   | left side, 6th from top   |

**Pull-ups:** the Adafruit version has 10 kΩ pull-ups on board. Chinese clones
sometimes don't — if `i2cdetect` doesn't show `0x44` after connecting, solder
external 4.7 kΩ resistors between SDA→3V3 and SCL→3V3.

**Test after connecting and resetting:**
```
I (xxx) sht41: SHT41 detected at 0x44, serial=0xXXXXXXXX
I (xxx) selftest: {"summary":"degraded (6/9)", … ,"sht41":true, …}
```
After MQTT discovery, `sensor.chytra_budka_cb_<id>_temperature` and
`_humidity` appear in Home Assistant.

---

## 1b. Second I²C bus on D6/D7 — test SHT41 or MAX isolation

Besides the main I²C on D4/D5, the firmware also has a **diagnostic bus1**:

| Bus1 signal | XIAO pad | GPIO |
| ----------- | -------- | ---- |
| `SDA`       | **D6**   | GPIO43 |
| `SCL`       | **D7**   | GPIO44 |
| `VCC`       | **3V3**  | — |
| `GND`       | **GND**  | — |

Uses:

- **a second SHT41** — because the SHT41 has a fixed address `0x44`, a second
  unit must sit on a different physical bus. Handy for inside/outside or
  electronics/ambient.
- **quarantine for MAX17048 clones** — broken clones with a phantom-ACK storm
  can be wired here so they don't disturb the main SHT41 on D4/D5.

`/i2c` reports both buses separately:

```text
bus0 D4/D5 (GPIO5/GPIO6): found: 0x44
  0x36  MAX17048 (battery)    MISSING (0/3)
  0x40  INA226 (solar)        MISSING (0/3)
  0x44  SHT41 (temp/RH)       OK (3/3)

bus1 D6/D7 (GPIO43/GPIO44): found: 0x44
  0x36  MAX17048 quarantine   MISSING (0/3)
  0x44  SHT41 secondary       OK (3/3)
```

Bus1 is diagnostic for now: production telemetry still uses the main SHT41 on
D4/D5. The second SHT41 has a firmware helper for one-off bench reads; we'll add
MQTT/HA entities once it's clear what it physically measures.

---

## 2. MAX17048 — battery fuel gauge (I²C 0x36)

The module has 6 pads — 4 for I²C to the XIAO + 2 for the battery connection:

| MAX17048 module | XIAO pad | Note                                |
| -------------- | -------- | --------------------------------------- |
| `VCC` / `VIN`  | **3V3**  | communication power                     |
| `GND`          | **GND**  | —                                       |
| `SDA`          | **D4**   | **shares the wire with the SHT41** (in parallel) |
| `SCL`          | **D5**   | **shares the wire with the SHT41** (in parallel) |
| `B+` / `BAT+`  | —        | + terminal of the 18650 holder          |
| `B-` / `BAT-`  | —        | − terminal of the 18650 holder (= GND)  |

**Pull-ups:** same as the SHT41 — if the SHT41 already has on-board pull-ups,
the fuel gauge needs no more. Pull-ups belong on the bus once only; they add in
parallel and small values (= many pull-ups in parallel) overload the bus.

**Test without a battery:** the firmware detects the chip even without an 18650,
`soc` is just 0 %. After reset:
```
I (xxx) battery: MAX17048 ready (VERSION=0x001x, VCELL raw=0x0000 ≈ 0.000 V)
I (xxx) selftest: { …, "battery":true, …}
```
You get real SOC % only after inserting batteries.

**Diagnostics:** if `/i2c` shows `0x44 SHT41 OK` but `0x36 MAX17048
MISSING (0/3)`, the bus and XIAO pins are fine and the problem is almost
certainly in the MAX17048 module, its power, or its SDA/SCL connection. The
standalone `firmware-test-max17048` harness must find `0x36` and read `VERSION`;
if it fails even after a manual bus recovery and at 10 kHz, don't look for the
bug in the main firmware.

### Warning: many cheap MAX17048 clones are broken silicon

A known failure mode of AliExpress-class MAX17048 modules (the classic
"MAX17048 ACKs every address" bug, documented on EEVblog): an `/i2c` scan
returns a **rotating set of phantom addresses** across the whole space
(different every scan, ~5–10 % ACK rate on *any* address, 0x36 no more likely
than the ghosts), and register reads never ACK no matter the retries, bus
resets, pull-ups or power source. If you see that, the module is defective —
buy a genuine Adafruit or Pimoroni breakout.

The firmware copes: `battery.c` has a probe-then-read retry strategy, selftest
reports `battery:false`, and the ModeFsm simply skips SOC signalling
(USB-only operation is a first-class scenario).


---

## 3. AM312 PIR — motion detection (3 pins, RTC wake)

A mini module with a white Fresnel dome. **Watch out:** some kits order the pins
`VCC-OUT-GND`, others `VCC-GND-OUT`. **Turn the module dome-side-down** — the
labels are on the PCB at the base of the pins.

| AM312 module | XIAO pad | Position on the XIAO    |
| ----------- | -------- | ------------------------ |
| `VCC` / `+` | **3V3**  | right side, 3rd from top |
| `OUT`       | **D1**   | left side, 2nd from top  |
| `GND` / `-` | **GND**  | right side, 2nd from top |

**Warm-up:** the AM312 has a **5–60 s blind period** after power-on while the
internal amplifier settles. Before that, either silence or random pulses. Ignore it.

**Test after connecting and resetting.** A correctly wired, warmed-up AM312 idles
by *sinking* the line, so the boot probe sees it held LOW:
```
I (xxx) pir: probe GPIO2: pull-up=0 pull-dn=0 → driven LOW (sensor present)
I (xxx) pir: pir[0] armed on GPIO2 (RTC-IO + EXT1 wake, poll 20/500 ms × 2) — line: driven LOW (sensor present), initial: LOW
```

The probe has **three** possible verdicts, and only the first confirms a sensor:

| `pull-up` | `pull-dn` | Verdict | Meaning |
| --------- | --------- | ------- | ------- |
| 0 | 0 | `driven LOW (sensor present)` | Something sinks the line — an idle AM312. This is the pass criterion. |
| 1 | 0 | `floating (no sensor)` | The line just follows the internal pull — `OUT` isn't connected. |
| 1 | 1 | `held HIGH (UNCONFIRMED …)` | Something holds the line high. Could be a PIR asserting motion or still warming up — **or nothing wired at all**, since a bare pad can read this way too. |

> ⚠️ **`held HIGH` is not a pass.** It is electrically indistinguishable from an
> unwired pin, so the firmware refuses to call it a sensor: `/selftest` reports
> `"pir": false` until the polling task has seen **3 stable LOW→HIGH
> transitions**, which a real sensor produces and a bare pad does not. If you see
> it right after wiring, wave your hand in front of the dome a few times and
> re-check `/selftest`. If you see it on a board with nothing connected to D1,
> that is expected and needs no action.
>
> (Firmware **≤ v0.10.1** collapsed `held HIGH` into "sensor present", so a bare
> board reported `"pir": true` and this step could not fail. If your board still
> runs one of those builds, that is why.)

The polling task runs regardless of the probe verdict — once the warmed-up AM312
starts pulsing, events are counted either way.

**Debounce model**: no ISR — `pir.c` polls the pin every 20 ms and accepts a
LOW→HIGH transition only after 2 consecutive samples (40 ms window). This gives
immunity to phantom edges that generated int_wdt crash loops on the bench during
WiFi TX under long-jumper EMI coupling (root-caused to EMI on long unshielded
jumpers; shortening the run + the debounce fixed it for good).

---

## 4. IR LED 940 nm + AO3400 MOSFET — night illumination

Through-hole parts, put it on a breadboard (or a prototype board). Schematic:

```
       3V3 ─────────┬────────────────┐
                    │                │
                    │              [220 Ω]
                    │                │
                    │              [IR LED]   anode at top
                    │                │
                    │           ┌────┴────┐
                    │           │ D drain │
   D2 (PWM) ────────┤────[10 kΩ]┤ G gate  │  AO3400 SOT-23
                    │           │ S source│
                    │           └────┬────┘
       GND ─────────┴────────────────┘
```

AO3400 pinout (SOT-23, marking on the back is 4A or 3A):

```
   (G)  ─┤1   3├─ (D)
         │     │
         └─2───┘
          (S)
```

| What goes where                       | XIAO pad | Note                           |
| ------------------------------------- | -------- | ------------------------------ |
| AO3400 **gate** via 10 kΩ to ground   | **D2**   | PWM output, 5 kHz, 8-bit duty  |
| AO3400 **source**                     | **GND**  | —                              |
| AO3400 **drain**                      | (LED−)   | drain pulls the LED cathode to ground |
| LED anode **via 220 Ω**               | **3V3**  | current limit ~10 mA at 3V3    |

**The 10 kΩ pull-down on the gate matters for two reasons**:

1. Without it the gate floats at `Z` during boot and randomly turns the LED on —
   wasting current in deep-sleep and ruining the AEC of the first photos.
2. **D2 (GPIO3) is a strap pin** (JTAG_SEL on the ESP32-S3) — sampled only at
   reset, so the firmware can drive it freely afterwards. The pull-down keeps
   the line LOW during reset = standard JTAG select, no surprises in the
   bootloader.

**Runtime overrides** (NVS via MQTT `cmd/cfg/<key>`):

- `ir_led_pin` — default `3` (D2). The validator rejects strap/flash/PSRAM/
  camera/SD/I²C/PDM/PIR/reed/status pins. Reboot after a change.
- `ir_led_enabled` — master switch (default ON).
- `ir_agc_thresh` — the AGC threshold at which it turns on (default 8, 0 = always,
  99 = never).

**Bench test:** run `idf.py monitor`, trigger a photo via MQTT
(`cb-<id>/cmd/photo`) or PIR — if `agc ≥ ir_agc_thresh`, the log shows
`ir=on` and the LED lights for ~500 ms. The human eye doesn't see 940 nm; a
**phone camera** sees it as a purple light.

---

## 4b. Capture indicator LED — visible "photo now" echo

Optional, but on by default. A visible LED that lights for the whole duration of
the capture window (from mutex-acquire to release, typically 100–900 ms)
**regardless of AGC/IR gating** — operator's quick feedback that "the box just
fired", without waiting for MQTT/HA.

Simple wiring (no MOSFET, an ordinary 5 mm LED):

```
   D3 ────[330 Ω]────►|──── GND
                    LED
              (anode on the resistor)
```

| What goes where   | XIAO pad | Note                                |
| ----------------- | -------- | ----------------------------------- |
| LED anode + 330 Ω | **D3**   | active-high GPIO, ~10 mA at 3V3     |
| LED cathode       | **GND**  | —                                   |

**Runtime overrides** (NVS via MQTT):

- `capture_led_pin` — default `4` (D3). Same validator as the IR LED; also
  refuses a collision with the current `ir_led_pin` (LEDC would steal the pin).
- `cap_led_en` — turn off for stealth deployment (default ON).

**Stealth field unit:** disable via
`mosquitto_pub -t cb-<id>/cmd/cfg/cap_led_en -m OFF`
and reboot. You can independently disable the onboard XIAO status LED (GPIO21)
via `status_led_en` — two independent visibility zones.

---

## 4c. Reed switch (door/lid magnet) — optional contact

A magnetic switch to detect the lid opening/closing. The pull-up is internal, so
you only need two wires: pin → switch → GND.

| What goes where | XIAO pad | Note                                                          |
| --------------- | -------- | ------------------------------------------------------------- |
| reed pin 1      | **D0**   | internal pull-up, polling 20 ms × N samples (NVS-tunable)     |
| reed pin 2      | **GND**  | —                                                             |

**Debounce model**: no ISR — `reed.c` has its own polling task that samples the
pin every 20 ms and accepts a transition only after `reed_db_ms / 20` consecutive
samples in the new state. The default 100 ms is enough for mechanical bounce +
short EMI bursts; **long harnesses / noisy environments** want 200–500 ms (live
over MQTT, no reboot).

```
# default 100 ms suits a bench install
mosquitto_pub -t cb-<id>/cmd/cfg/reed_db_ms -m 200

# harden for a field run with long cabling
mosquitto_pub -t cb-<id>/cmd/cfg/reed_db_ms -m 500
```

The reed master switch defaults to **OFF** (the firmware ignores the pin). Enable:

```
mosquitto_pub -t cb-<id>/cmd/cfg/reed_enabled -m ON
```

After a reboot HA discovers a `binary_sensor` with `device_class=door`
(`OPEN`/`CLOSED` payload) + an event counter + a slider for `reed_db_ms` in
Advanced config.

---

## 5. microSD card

**No wiring** — the slot is on the Sense expansion. Just:

1. Format the card as **FAT32** (32 GB max; larger cards will report
   `not_mounted`).
2. Slide it into the slot on the edge of the Sense board, label-side up.
3. After reset, watch for:
   ```
   I (xxx) sd: mounted at /sdcard
   I (xxx) selftest: { …, "sd":true, …}
   ```
4. Photos are saved as `/sdcard/<YYYYMMDD-HHMMSS>_<mactail>_<trigger>.jpg`.

---

## 6. INA226 (optional)

For monitoring solar current/voltage. If you get one:

| INA226 module | XIAO pad | Note                               |
| ------------ | -------- | ---------------------------------- |
| `VCC`        | **3V3**  | logic power                        |
| `GND`        | **GND**  | —                                  |
| `SDA`        | **D4**   | shares the I²C bus                 |
| `SCL`        | **D5**   | shares the I²C bus                 |
| `IN+`        | —        | + from the solar panel (panel side of the shunt) |
| `IN-`        | —        | + to the charger's solar input (after the shunt) |

Address `0x40`. The firmware `ina226.c` detects it automatically and reports
`ina226: not detected` if absent (no build flag needed).

---

## Recommended assembly order

1. **microSD** — insert and confirm selftest reports `sd:true`.
2. **SHT41** — 4 wires, the simplest. Confirm `sht41:true` in the selftest.
3. **MAX17048** (no battery) — another 4 wires on the bus. Confirm `battery:true`.
4. **18650 holder + batteries** to the MAX17048 — `soc` in the HA dashboard
   starts showing a real value.
5. **AM312 PIR** — 3 wires. Wave your hand. Confirm `pir:true` after a few motions.
6. **IR LED + MOSFET** — breadboard, confirm with a phone camera.

If something doesn't report what it should after a reset, check causes in this
order:

1. **Swapped SDA/SCL** (most common) — flip those two wires.
2. **Power on 5V instead of 3V3** — power off quickly, check the pad, power on.
3. **Missing pull-ups** — solder 4.7 kΩ to 3V3 on both SDA and SCL (once per bus).
4. **Cold joint on a Dupont connector** — tug the wire; if it pulls out, that's
   the joint.

---

## TODO — solar-stage wiring

Wiring for the solar/battery charging stage isn't documented here because that
stage hasn't been built yet — it'll be added once real parts are on the bench.
The enclosure parts (box, antenna + SMA, gland, mounting, heatsink) are in
[SHOPPING.md](SHOPPING.md).

---

## Reference

- [SHOPPING.md](SHOPPING.md) — what to buy and why
- [SCHEMATIC.md](SCHEMATIC.md) — block diagram
- [firmware/hw/README.md](firmware/hw/README.md) — pin assignment from the firmware's point of view
- [XIAO ESP32-S3 Sense pinout (Seeed wiki)](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/#hardware-overview)
