/* oled.h — bench-only SSD1306 128×64 status display.
 *
 * A throwaway "na hraní" bring-up: a tiny self-contained SSD1306 driver
 * (no external component) on the SHARED bus0, plus a boot splash and a
 * single auto-refreshing identity/link page. Deliberately minimal — the
 * full paged design lives in firmware/OLED_STATUS.md.
 *
 * Robustness contract (bus0 also carries the *required* SHT41):
 *   - soft-detected at boot; absent display ⇒ the whole subsystem no-ops,
 *     no behaviour change anywhere else.
 *   - goes through the shared i2c_bus_get() handle (IDF i2c_master
 *     serialises transfers), short timeouts, NACK ⇒ mark dead + stop.
 *     Never retry-spins, never blocks an SHT41 read.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe 0x3C on bus0; if present, init the panel, paint the boot splash,
 * and spawn the low-priority refresh task. Returns true iff the display
 * was detected and initialised. Safe to call once at boot; idempotent. */
bool oled_init(void);

/* True once a display has been detected + initialised. */
bool oled_present(void);

/* Show `text` as a QR code on the panel (dark modules on a lit background,
 * scaled + centred) for ~90 s, overriding the status page. Returns true if
 * a display is up. Used to test QR readability + (later) WiFi onboarding. */
bool oled_show_qr(const char *text);

/* Synchronous, side-effect-free presence probe of the panel on bus0 — the
 * same consensus probe the refresh task uses, but it adds no device and
 * spawns no task. Lets the boot path decide, BEFORE the AP is configured,
 * whether a display is wired up (gates the WiFi-onboarding flow). Bounded
 * (~1 s if absent). Requires the shared I²C bus to be initialised already. */
bool oled_probe_present(void);

/* WiFi onboarding: show a WiFi-join QR (SSID + password; WPA2, or open when
 * pass is empty) and keep it on screen until reboot, overriding the status
 * page. A phone that scans it joins the AP, and the captive portal opens
 * /wifi. Returns true if a display is already up; either way the QR is
 * painted as soon as the panel comes up. */
bool oled_show_wifi_qr(const char *ssid, const char *pass);

/* Preview the boot screen (custom logo, or the text splash if none) on the
 * panel for ~10 s — lets you check an uploaded logo without rebooting.
 * Returns true if a display is up. */
bool oled_show_boot(void);

/* Advance to the next status page (status → power → camera → net → env →
 * diag → mic-VU → web-QR → …). Called from the BOOT-button short-press; a
 * no-op when no panel is present. */
void oled_next_page(void);

/* True while the ENV page is on-screen (panel up + not blanked). The Grove
 * sensor poll tasks (sonar.c/soil.c) boost their sampling to ~2 Hz while
 * this holds so the panel readout is live; MQTT cadence stays at *_poll_s.
 * Always false without a panel (field boards). */
bool oled_env_page_visible(void);

/* Manual display power switch. Persists the `oled_enabled` config knob (so HA
 * stays in sync) and the refresh task applies it on its next tick: blank the
 * panel (SSD1306 charge-pump + display off) and stop refreshing when off, plus
 * cut VCC if a GPIO is mapped to "oled_pwr" in the pin map. No-op on a board
 * with no panel. The knob is also settable directly over MQTT cmd/cfg. */
void oled_set_enabled(bool en);

/* Start the default-logo flap; it loops until oled_anim_stop() (or a long
 * safety cap). No-op when no panel is present. */
void oled_anim_logo(void);
bool oled_anim_running(void);
void oled_anim_stop(void);

/* Drive the "HOLD = FACTORY RESET" progress bar while the BOOT button is held.
 * `held_ms` = how long it's been held, `hold_ms` = the threshold that triggers
 * the reset; pass held_ms < 0 to clear the overlay (button released). */
void oled_set_reset_progress(int held_ms, int hold_ms);

/* Event blink: invert-flash the whole panel (SSD1306 invert toggles — no
 * framebuffer change, flashes whatever's on screen) to signal an event, with a
 * distinct pattern per kind. No-op when no panel is present. */
typedef enum {
    OLED_FLASH_PHOTO,   /* 1 long flash  — a photo was captured (≈ the trigger LED) */
    OLED_FLASH_MOTION,  /* 2 short       — PIR motion */
    OLED_FLASH_VAD,     /* 3 shortest    — a VAD audio burst */
} oled_flash_t;
void oled_flash(oled_flash_t kind);

/* Set the word shown on the reboot screen ("OTA", "FACTORY", …); optional —
 * a generic "REBOOT" is shown if unset. */
void oled_set_reboot_reason(const char *reason);

/* Synchronously paint the "REBOOTING" frame and flush it. Invoked from the
 * IDF shutdown handler so any soft reboot shows it; safe to call directly.
 * No-op when no panel is present. */
void oled_show_rebooting(void);

/* Synchronously paint the "SLEEP / wake in Ns" hibernate frame and flush it.
 * Called by cb_ds just before esp_deep_sleep_start(); the SSD1306 holds the
 * image through the sleep (oled_pwr is not cut). No-op when no panel present. */
void oled_show_deepsleep(int next_wake_s);

/* Custom boot logo: a full-frame 128×64 SSD1306 bitmap (exactly 1024 bytes,
 * page-major, LSB=top) stored in NVS and shown as the boot splash. set
 * returns false on a wrong length / NVS error; clear removes it (back to the
 * text splash). Takes effect on the next boot. */
#define OLED_LOGO_BYTES 1024
bool oled_set_logo(const unsigned char *data, size_t len);
void oled_clear_logo(void);
/* Copy the stored logo (OLED_LOGO_BYTES) into out; false if none stored. */
bool oled_get_logo(unsigned char *out, size_t cap);
/* Copy the EFFECTIVE boot logo (NVS custom, else baked default) into out. */
void oled_get_boot_logo(unsigned char *out, size_t cap);

#ifdef __cplusplus
}
#endif
