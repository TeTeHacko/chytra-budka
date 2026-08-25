/* cb_pm.h — power-management (light-sleep) helper.
 *
 * EXPERIMENTAL. Enabled at runtime by the `pm_lightsleep` NVS knob (default
 * OFF). Compiled in only when CONFIG_PM_ENABLE; every entry point is a safe
 * no-op otherwise.
 *
 * Design note — why this is camera-safe: we configure esp_pm with
 * min_freq == max_freq == 240 MHz, i.e. NO dynamic frequency scaling. The
 * APB clock (and therefore LCD_CAM XCLK) never drops, so the documented
 * DFS↔OV3660-SCCB-NACK bug (see POWER.md / sdkconfig.defaults.esp32s3) cannot
 * trigger. The only saving is automatic light-sleep in idle gaps; the chip
 * fully restores to 240 MHz on wake. Frame DMA / SCCB and I2S DMA must not be
 * gated mid-transfer, so those paths bracket their work with the
 * NO_LIGHT_SLEEP lock below.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure esp_pm (240/240, light-sleep OFF) and create the NO_LIGHT_SLEEP
 * lock. Call once at boot. No-op when CONFIG_PM_ENABLE is unset. */
void cb_pm_init(void);

/* Enable/disable automatic light-sleep at runtime (driven by the
 * `pm_lightsleep` knob via apply_power_state). Change-guarded internally. */
void cb_pm_set_lightsleep(bool enable);

/* Bracket any section that must not be interrupted by light-sleep — I2S DMA
 * running (audio on) or a camera frame/SCCB transfer in flight. Reference-
 * counted (esp_pm_lock), safe to nest/pair. No-ops when PM is compiled out. */
void cb_pm_no_sleep_acquire(void);
void cb_pm_no_sleep_release(void);

#ifdef __cplusplus
}
#endif
