// config.h — compile-time configuration for Chytrá Budka firmware
// SECRETS LIVE IN secrets.h (gitignored). DO NOT commit secrets.

#pragma once

#include "secrets.h"  // defines WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD, MQTT_USER, MQTT_PASSWORD, RELAY_AUTH

// ─── Network ──────────────────────────────────────────────────────────────
// WIFI_SSID, WIFI_PASSWORD come from secrets.h
// Per-device identity namespace: device_id() = HOSTNAME-<mactail>, e.g.
// "cb-ex01". Kept short + hyphen-only so the id is a valid DNS hostname
// ("<id>.<domain>" resolves). This is the DEVICE id, distinct from the
// project / OTA-artifact name "chytra-budka" (ota.example.com/chytra-budka/, ota.c).
#define HOSTNAME            "cb"

// Audio relay endpoint on server-host (HTTP chunked POST or WebSocket)
// IP used directly — IoT WiFi DHCP may not provide .lan DNS resolver
#define RELAY_HOST          "198.51.100.23"
#define RELAY_PORT          8765
#define RELAY_PATH          "/audio/chytra-budka"
// Authorization token; defined in secrets.h as RELAY_AUTH

// MQTT broker — the standalone stack (the fleet cut over 2026-07-27; the old
// HA add-on broker at 192.0.2.5 still exists but serves zigbee2mqtt etc.,
// not the budky). These are only the compile-time FLOOR: any board that has
// been through net_store keeps its stored endpoint. They matter after a
// factory reset or on a freshly flashed board.
//
// mtls means a fresh board cannot reach the broker until it holds an
// enrollment leaf — :8883 has require_certificate. That is deliberate: it
// enrolls over ENROLL_URL first (WiFi + SNTP only, no MQTT), which for an
// unknown CN parks in the TOFU queue until an operator approves it.
#define MQTT_HOST           "cb.example.com"
#define MQTT_PORT           8883
#define MQTT_SCHEME         "mqtts"
#define MQTT_AUTH_DEFAULT   "mtls"
#define MQTT_BASE_TOPIC     "chytra-budka"

// Enrollment endpoint (HTTPS, manager). Empty would fall back to the legacy
// MQTT signer, which was retired with the cutover.
#define ENROLL_URL          "https://cb.example.com/api/v1/enroll"

// ─── SoftAP / AP-only recovery network ────────────────────────────────────
// The recovery SoftAP and the full AP-only mode run on THIS subnet. Kept
// OUTSIDE 192.168.0.0/16 on purpose: the operator's LAN/VLAN mesh routes all of
// 192.168/16 over VPN, so the ESP-default 192.0.2.1 would collide with it.
// Relocate by editing AP_IP (+ netmask) and rebuilding; the DHCP lease pool is
// derived from AP_IP by the dhcps server. AP_IP doubles as gateway + the
// captive-portal host.
#define AP_IP               "172.31.4.1"
#define AP_NETMASK          "255.255.255.0"
#define AP_CHANNEL          1                          // 2.4 GHz channel
#define AP_MAX_CONN         4                          // simultaneous AP clients
#define AP_SSID_FMT         "cb-%s"                    // %s = device-id suffix → AP SSID == device_id
// Default WPA2 pass when the operator hasn't set a custom one. INSECURE on
// purpose-of-being-simple: it's a fixed, public default. The old cb-<mac>
// scheme only LOOKED secret — the AP broadcasts its MAC, so cb-<mac> was
// trivially derivable. We don't pretend: the UI shows a "change me" warning
// whenever this default is in use. Operators set a real pass via /config.
// (≥8 chars for WPA2.)
#define AP_PASS_DEFAULT     "chytrabudka"

// Unix epoch (seconds) past which the wall clock is taken to be SNTP-synced
// (≈ 2023-11-14). The firmware uses "time(NULL) > this" as the single proxy for
// "is the clock real?" — for EXIF/MQTT timestamps, cert-validity checks, the
// audio active-hours window, the hibernate sleep clock, etc. One source of
// truth so the threshold can't drift between call sites.
#define CB_CLOCK_SYNCED_EPOCH 1700000000

// ─── GPIO assignments (XIAO ESP32-S3) ────────────────────────────────────
// NOTE: GPIO *assignment* for the D-header (D0..D7) is RUNTIME-OWNED by the
// pin-function map (app_config.c pin_d?_fn → app_config_pin_for_first); the
// drivers resolve their pins from there, NOT from the pin #defines below.
// RTC-capability is the scarce resource (only EXT1 wake sources need it):
// D0..D5 = GPIO1..6 are RTC-capable, D6/D7 = GPIO43/44 are NOT. The OPTIMAL
// layout therefore puts the never-waking primary I²C bus0 on D6/D7, freeing
// the RTC pads for pir/reed/button. BUT the compile DEFAULT keeps bus0 on
// D4/D5 (legacy/field wiring) — flipping it would silently relocate bus0 on
// any board that relied on the default (e.g. the OTA-only field), dropping
// its gauge/sensors. So the optimal D6/D7 layout is set EXPLICITLY per board
// once it's physically rewired; the default flips only in Phase 2.
// The pin #defines below are ADVISORY only (runtime-owned); addresses ARE live.
//
// I²C bus0 — MAX17048 fuel gauge + SHT41 ambient T/RH + BMP388 (+ the
// universal board's BQ25798 charger). ESSENTIAL. Default pads: D4/D5.
#define I2C_SDA_PIN         5  // D4 — default bus0 SDA (advisory; runtime-owned)
#define I2C_SCL_PIN         6  // D5 — default bus0 SCL (advisory; runtime-owned)
#define MAX17048_ADDR       0x36
#define SHT41_ADDR          0x44
// Bench-only SSD1306 128×64 OLED (TIB098) — shares bus0 with the SHT41.
// Non-critical: soft-detected at boot, absent = no-op. Never field HW.
#define OLED_ADDR           0x3C
// Bench-only BMP388 pressure/temp sensor on bus0 (the "internal" sensor;
// SHT41 becomes "external"). SDO=high → 0x77 (SDO=low would be 0x76).
#define BMP388_ADDR         0x77

// I²C bus1 — optional isolated/debug bus (clone-quarantine / 2nd SHT41) and,
// per the OPTIMAL layout, the eventual home of bus0 once boards are rewired
// (set pin_d6/d7_fn = i2c0_* explicitly). Pads below are advisory.
#define I2C1_SDA_PIN        43  // D6
#define I2C1_SCL_PIN        44  // D7

// I²S PDM — onboard MSM261D3526H1CPM digital microphone
// (XIAO ESP32-S3 *Sense* only). External I²S codec NOT used to free
// GPIO 7/8/9 for the onboard SDIO microSD slot (see SD_* below).
#define I2S_PDM_CLK_PIN     42  // PDM clock to mic
#define I2S_PDM_DATA_PIN    41  // PDM data from mic

// microSD slot on Sense expansion board (SDIO 1-bit mode)
#define SD_CLK_PIN          7  // D8
#define SD_CMD_PIN          9  // D9
#define SD_DAT0_PIN         8  // D10

// PIR motion sensor (RTC wake source).
// D1 / GPIO2 — RTC-capable (was D2/GPIO3 originally; moved when the
// field harness pinned the wire on D1). Both are ESP32-S3 strapping
// pins so the EMI-tolerant N-edge promotion logic in pir.c still
// applies regardless of which we pick.
#define PIR_PIN             2

// Reed switch (door/lid magnet contact). Optional — runtime-enabled via
// the `reed_enabled` NVS bool, default OFF. Wiring: one terminal to
// REED_PIN, the other to GND. Firmware enables the internal pull-up so
// no external resistor is needed; switch closure pulls the line LOW.
// GPIO1 (D0) is not a strapping pin on ESP32-S3.
#define REED_PIN            1   // D0

// IR illuminator (940 nm LED through AO3400 N-MOSFET).
// Compile-time default only — the runtime pin is read from NVS key
// `ir_led_pin` (see app_config.c SCHEMA). Override there to retarget
// without rebuilding when the LED ends up on a different GPIO.
// GPIO3 (D2) is a strapping pin (JTAG_SEL) but only sampled at reset;
// firmware can drive it freely afterwards. AO3400 gate with the usual
// pull-down resistor keeps it LOW during boot.
#define IR_LED_PIN          3  // D2 — LEDC PWM channel

// Capture indicator LED (visible feedback for every photo capture).
// Active-high, lit for the duration of the capture window regardless
// of IR/AGC state. Wiring: GPIO --[R≈330Ω]--[LED]-- GND.
// NVS-overridable via `capture_led_pin`; runtime enable via
// `cap_led_en` (short form: 15-char NVS limit). Default ON — low-power blink and
// useful in the field to confirm capture without checking MQTT.
#define CAPTURE_LED_PIN     4  // D3 — visible signal LED

// Onboard XIAO ESP32-S3 user LED (active-low on Seeed boards). Used only
// for debug/status patterns; charge LED is separate and not firmware-driven.
#define STATUS_LED_PIN      21

// ─── Camera (OV2640/OV3660 on XIAO ESP32-S3 Sense expansion connector) ──
// Pin map fixed by Sense PCB; do NOT remap these.
#define CAM_PIN_PWDN        -1
#define CAM_PIN_RESET       -1
#define CAM_PIN_XCLK        10
#define CAM_PIN_SIOD        40  // I²C SDA (separate bus from main I²C)
#define CAM_PIN_SIOC        39
#define CAM_PIN_D7          48
#define CAM_PIN_D6          11
#define CAM_PIN_D5          12
#define CAM_PIN_D4          14
#define CAM_PIN_D3          16
#define CAM_PIN_D2          18
#define CAM_PIN_D1          17
#define CAM_PIN_D0          15
#define CAM_PIN_VSYNC       38
#define CAM_PIN_HREF        47
#define CAM_PIN_PCLK        13

// ─── Audio ────────────────────────────────────────────────────────────────
// Seeed wiki notes the onboard PDM mic on the XIAO Sense is
// "relatively stable at 16 kHz"; 48 kHz gave us partial DMA fills + read
// timeouts on the bench. 16 kHz is also enough for BirdNET-Go (it
// resamples internally) and halves WiFi load on the relay POST path.
#define I2S_SAMPLE_RATE     16000
#define I2S_BITS_PER_SAMPLE 16  // PDM mic delivers int16 directly
// I2S_CHANNEL_COUNT removed — audio.cpp hardcodes the PDM driver to
// STEREO + SLOT_BOTH (the XIAO Sense's PDM mic delivers L,R interleaved
// at 16 kHz, the relay does the L-channel extraction). The previous
// "1 // mono" #define was always a lie and was never read by audio.cpp.

// ─── Mode thresholds (SOC %) ──────────────────────────────────────────────
#define SOC_CONT_ENTER      65  // triggered → continuous when SOC ≥ this
#define SOC_CONT_LEAVE      50  // continuous → triggered when SOC < this
#define SOC_SAFE_ENTER      30  // any → safe when SOC < this
#define SOC_SAFE_LEAVE      35  // safe → triggered when SOC ≥ this

// ─── VAD / telemetry / safe-mode ──────────────────────────────────────────
// All VAD thresholds and telemetry periods are now NVS-backed runtime
// values via app_config (vad_thr_dbfs, vad_burst_ms, vad_rearm_ms,
// tlm_cont_s, tlm_trig_s). See app_config.c for defaults.
// SAFE_SLEEP_DURATION_S is reserved for future deep-sleep implementation.

// ─── OTA ──────────────────────────────────────────────────────────────────
// HTTPS endpoint serving the latest signed firmware image.
// TLS trust: the FULL esp_crt_bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL)
// — NOT pinned to ISRG X1/X2. Image integrity is guaranteed by the RSA-3072
// app signature (verify-on-update), which is the real anti-tamper control here;
// the TLS bundle only authenticates the host. tools/le_isrg_roots.pem exists to
// pin the OTA fetch to Let's Encrypt roots, but is NOT currently wired in (would
// need DEFAULT_NONE + esp_crt_bundle_set on that PEM) — a deliberate trade so a
// host-CA rotation can't break fleet OTA. See ota.c.
#define OTA_URL             "https://cb.example.com/ota/chytra-budka/chytra-budka.bin"

// Fallback domain suffix used by tls_enroll when DHCP option 15
// (Domain Name) isn't supplied by the LAN's DHCP server. The device's
// FQDN is always "<device_id>.<domain>" — what changes between
// deployments is the suffix. Preferred source is DHCP (zero-config
// moves between .lan and .lan); this is the safety net.
#ifndef CB_DOMAIN_FALLBACK
#define CB_DOMAIN_FALLBACK  "doma"
#endif
/* OTA poll period — Kconfig-driven so dev (5 min) vs production (6 h)
 * is one sdkconfig line, not a code-comment promise. See
 * CHYTRA_BUDKA_OTA_PERIOD_S help text. */
#define OTA_CHECK_PERIOD_MS ((uint32_t)CONFIG_CHYTRA_BUDKA_OTA_PERIOD_S * 1000UL)

// ─── Debug ────────────────────────────────────────────────────────────────
#define ENABLE_SERIAL_LOGS  1
