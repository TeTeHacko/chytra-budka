# Running it — first boot, what to expect, gotchas

A general guide to getting a freshly-built box alive on the bench. For the
exact build/flash commands see [firmware/README.md](firmware/README.md); for
wiring see [WIRING.md](WIRING.md).

## Prerequisites

- A XIAO ESP32-S3 **Sense** (the Sense expansion carries the camera, PDM mic,
  and microSD).
- ESP-IDF **v6.0.1** installed and sourced (`. $IDF_PATH/export.sh`).
- `firmware/main/secrets.h` filled in (copy from `secrets.h.example`) and
  `firmware/main/config.h` pointing `RELAY_HOST` / `MQTT_HOST` / `OTA_URL` at
  your own network. (WiFi creds may stay as placeholders — an unprovisioned
  board boots into an AP provisioning portal, SSID `cb-<suffix>`, password
  **`chytrabudka`**: a fixed, public default. Join it and submit your network at
  `http://172.31.4.1/wifi`, then set a real AP password on `/config` — that page
  warns for as long as the default is in use. **If an OLED is fitted**, the board
  instead generates a random per-boot password and shows it as an on-screen QR;
  that value is deliberately never printed to the serial console, so the QR is
  the only way to read it.)
- An MQTT broker reachable from the board (for Home Assistant discovery and
  control). The camera/audio relay and HA are optional for first boot.
- A microSD card formatted **FAT32** (≤32 GB), label-side up in the slot.

## Bring-up order

You don't need everything wired to boot. A useful order (each step is
independently verifiable in the selftest):

1. **Bare board on USB-C** — confirm it boots, joins WiFi, connects to MQTT.
2. **microSD** — confirm `sd:true`.
3. **SHT41** (temp/RH) — simplest I²C module, confirm `sht41:true`.
4. **MAX17048** fuel gauge — confirm `battery:true` (0 % until a cell is attached).
5. **Battery** to the MAX17048 — real SOC % starts showing.
6. **AM312 PIR**, then **IR LED + MOSFET**. (The PIR is the one step whose
   selftest flag needs a caveat: an idle AM312 is confirmed by the boot probe,
   but a sensor that is *asserting* at boot reads the same as an unwired pin, so
   `pir` stays `false` until the firmware has seen 3 real motion edges. Wave a
   hand in front of the dome, then re-check. See [WIRING.md](WIRING.md#3-am312-pir--motion-detection-3-pins-rtc-wake).)

See [WIRING.md](WIRING.md) for the per-module pinout and a short test for each.

## What to expect on first boot

Watch the serial log (`idf.py monitor`):

- A boot banner with the ESP-IDF version, chip info, and free heap.
- Each peripheral probing in turn — present ones log `ready`/`detected`, absent
  ones log a clear `not detected` and are **gracefully skipped** (the box runs
  fine with missing optional sensors; it does not hang or crash-loop).
- A **`selftest` JSON** line summarising which subsystems came up, e.g.
  `{"summary":"degraded (11/13)","sht41":true,"battery":false,...}`. "degraded"
  just means some optional parts are absent — it's expected on a partial bench.
- WiFi join + an IP, then MQTT connect. With a broker up, Home Assistant
  **auto-discovers** the sensors/controls within a few seconds.
- **Per-device HTTPS enrollment**: on first boot with no stored cert the box
  generates a key, requests a cert, and (once issued) serves HTTPS on `:443`
  with `:80` redirecting. Until a cert exists it serves plain HTTP on `:80`.
- The **OTA** task polls the configured `OTA_URL` periodically and applies a
  newer image automatically.

The box reaches the local web UI at `http://<box-ip>/` (or `https://` once
enrolled): live MJPEG, last photo, `/selftest`, and a "Pin map" view.

## Gotchas (the usual first-time traps)

- **Swapped SDA/SCL** — by far the most common I²C failure. If a module shows
  `MISSING` but the bus scan works for another address, flip the two wires.
- **Powered on 5V instead of 3V3** — all the external modules are 3V3. The XIAO
  inputs are **not** 5V-tolerant. Power off, fix the pad, power on.
- **Missing I²C pull-ups** — if no I²C device is detected at all, add 4.7 kΩ from
  SDA→3V3 and SCL→3V3 (once per bus; many breakouts already have them).
- **AM312 PIR warm-up** — the PIR has a 5–60 s blind period after power-on.
  Random or no pulses at first is normal; wave your hand after it settles.
- **Camera SCCB NACK / `Mismatch PID`** — if the camera fails to init, make sure
  dynamic frequency scaling stays **off** (`CONFIG_PM_ENABLE` /
  `CONFIG_PM_DFS_INIT_AUTO` off; the defaults file pins them off — a manual
  menuconfig can undo that).
- **SD won't mount** — must be **FAT32**; cards >32 GB report `not_mounted`.
- **Cold Dupont joints** — tug each jumper; if it slips out, that's your fault.
- **Debug endpoints** — `CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS` is off by default
  and **must stay off** on anything exposed to a routable network (the `/debug/*`
  routes can crash or wipe the device and have no auth). Enable only on a bench.

## Next

- Wiring detail: [WIRING.md](WIRING.md)
- Build/flash/test + production hardening: [firmware/README.md](firmware/README.md)
