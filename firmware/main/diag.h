/* diag.h — boot diagnostics: reset reason, persisted core dump, boot fail
 * counter held in RTC slow memory.
 *
 * Field deployment can't be SSH'd into. The host this firmware sees most is
 * the MQTT broker, so anything we want to know after the fact has to be
 * published. This module is the bridge:
 *
 *   - At boot, examine esp_reset_reason() and esp_core_dump_image_check().
 *     The previous run's panic frame, if any, lives in the coredump
 *     partition until a successful next boot replaces it.
 *   - Publish a structured `diag/boot` MQTT event once MQTT is up so the
 *     server-side observer (or HA log) sees: what reset us, whether a
 *     core dump is waiting, current boot streak.
 *   - Maintain a "consecutive crash" counter in RTC slow memory that
 *     survives soft resets. After diag_boot_succeeded() is called (call
 *     it from the main loop once the firmware has been running cleanly
 *     for a few minutes), the counter is cleared. If we crash N times
 *     in a row without ever reaching that point, escalate (caller's
 *     choice — typically deep-sleep and try again later). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture reset reason + coredump state into module-local fields. Must
 * be called before nvs/wifi/mqtt init since both NVS erase and OTA
 * rollback will clear coredump in the background. */
void diag_capture_boot(void);

/* Publish "diag/boot" JSON to MQTT — call once after MQTT connects. */
void diag_publish_boot(void);

/* If a coredump is present in flash, ship it (base64, chunked) to the
 * <id>/diag/coredump/ topics over MQTT in a background task — so a field unit
 * (no USB) can be symbolized off-device with tools/coredump_recv.py + the
 * archived ELF. Call once after MQTT connects. No-op unless
 * CONFIG_CHYTRA_BUDKA_SHIP_COREDUMP is set and a dump is present; ships at most
 * once per boot. */
void diag_ship_coredump_mqtt(void);

/* Read the consecutive boot-fail counter held in RTC slow memory. The
 * counter is incremented inside diag_capture_boot() (NOT here); this is a
 * read-only accessor. Alias of diag_consecutive_crashes() — kept as a
 * second name for the early-boot call sites in app_main. */
uint32_t diag_consecutive_boot_count(void);

/* Clear the consecutive-crash counter once this boot has stayed up long
 * enough (a pure UPTIME signal — call it on a fixed runtime milestone, NOT
 * gated on network/MQTT). Splitting this from the OTA mark-valid below is what
 * stops a broker outage from pinning a healthy board in crash-loop safe mode.
 * Idempotent; subsequent calls are no-ops. */
void diag_clear_crash_count(void);

/* If a freshly-OTA'd image is still PENDING_VERIFY, cancel the bootloader
 * rollback so it becomes permanent. Call ONLY once the control plane is
 * proven (MQTT up), i.e. we've confirmed the new image can still receive a
 * corrective OTA. Idempotent; retries internally until a pending image is
 * actually confirmed. */
void diag_mark_ota_valid(void);

/* Set a sticky RTC flag right before issuing esp_restart() from an
 * init-time failure path (wifi_mgr_init fail, NVS fail, etc). The
 * following boot's diag_capture_boot() treats the SW reset as a
 * crash-equivalent and increments the consecutive-crashes counter,
 * so a field unit stuck in a soft-reset loop still trips safe-mode
 * + ramps reboot delay instead of looping silently forever. Cleared
 * after consumption on next boot regardless of outcome. */
void diag_pre_boot_fail_set(void);

/* WiFi credential-candidate attempt counter (RTC slow memory; survives
 * esp_restart()/panic, resets on power loss). diag_wifi_try_inc() bumps
 * and returns the new count; used by app_main to cap how many boots a
 * pending candidate may consume before it's auto-reverted to known-good.
 * Cleared on promotion via diag_wifi_try_clear(). See wifi_store.h. */
uint32_t diag_wifi_try_inc(void);
uint32_t diag_wifi_try_get(void);
void     diag_wifi_try_clear(void);

/* Net-watchdog reboot escalation: count of consecutive net-watchdog reboots
 * with no successful MQTT session in between. The watchdog uses it to back off
 * its reboot threshold so a sustained broker outage doesn't reboot-storm the
 * fleet. inc() before a net-watchdog esp_restart; reset() on MQTT connect. */
uint32_t diag_netwdt_count(void);
void     diag_netwdt_inc(void);
void     diag_netwdt_reset(void);

/* Accessors for boot context (populated by diag_capture_boot). Used by
 * GlitchTip so we can ship a crash event with the same fields as
 * diag/boot MQTT. */
const char *diag_reset_reason_name(void);
uint32_t diag_consecutive_crashes(void);
size_t diag_coredump_size(void);

/* True when consecutive_crashes has reached the safe-mode threshold —
 * i.e. the firmware has crashed (or pre-boot-failed) several times in a
 * row WITHOUT ever reaching diag_boot_succeeded(). The bootloader already
 * rolls a *pending* OTA image back on the first crash; this covers the
 * worse case of an already-valid image that starts crash-looping (a
 * config/environmental trigger), where there is nothing to roll back to.
 * app_main consults this to boot a minimal control-plane-only mode
 * (skip the heavy camera/audio subsystems, keep WiFi/MQTT/OTA/HTTP) so
 * an operator can still push a fixed OTA to the unreflashable field unit.
 * Cleared (via the counter) by diag_boot_succeeded() after a clean run. */
bool diag_in_crash_loop(void);

/* Log free-stack high-water marks for known long-lived tasks (main,
 * sys_evt, glitchtip, ota, wifi, tiT). Inexpensive — one line in the
 * log per call. Call periodically from the telemetry tick to spot a
 * task trending toward zero before it actually panics. */
void diag_log_task_stacks(void);

/* Internal MCU die temperature in °C. Lazy-installs the ESP32-S3
 * temperature_sensor on first call, returns NAN if the sensor failed to
 * install or read. Cheap to call (~µA when enabled); callers decide
 * cadence. Shared by http_server and mqtt diag publisher. */
float diag_mcu_temp_c(void);

#ifdef __cplusplus
}
#endif
