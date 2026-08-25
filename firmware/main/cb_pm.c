/* cb_pm.c — see cb_pm.h. Light-sleep without DFS (camera-safe). */
#include "cb_pm.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_err.h"
#include "esp_pm.h"
#endif

static const char *TAG = "cb_pm";

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_no_sleep;  /* held during audio/camera DMA */
static bool s_ls_enabled;                /* current configured state (guard) */

static void reconfigure(bool light_sleep) {
    /* min == max == 240: NO frequency scaling. APB/LCD_CAM XCLK never drop, so
     * the camera SCCB-NACK bug cannot trigger; light_sleep just gates clocks
     * in idle gaps and fully restores to 240 MHz on wake. */
    esp_pm_config_t cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 240,
        .light_sleep_enable = light_sleep,
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "esp_pm_configure(ls=%d): %s", (int)light_sleep, esp_err_to_name(e));
}
#endif

void cb_pm_init(void) {
#if CONFIG_PM_ENABLE
    reconfigure(false);  /* inert until the pm_lightsleep knob enables it */
    esp_err_t e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "cb_nols", &s_no_sleep);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_lock_create: %s", esp_err_to_name(e));
        s_no_sleep = NULL;
    }
    s_ls_enabled = false;
    ESP_LOGI(TAG, "PM ready (240/240 fixed, light-sleep off until enabled)");
#else
    ESP_LOGI(TAG, "PM compiled out (CONFIG_PM_ENABLE unset)");
#endif
}

void cb_pm_set_lightsleep(bool enable) {
#if CONFIG_PM_ENABLE
    if (enable == s_ls_enabled)
        return;
    s_ls_enabled = enable;
    reconfigure(enable);
    ESP_LOGI(TAG, "light-sleep %s", enable ? "ENABLED (experimental)" : "disabled");
#else
    (void)enable;
#endif
}

void cb_pm_no_sleep_acquire(void) {
#if CONFIG_PM_ENABLE
    if (s_no_sleep)
        esp_pm_lock_acquire(s_no_sleep);
#endif
}

void cb_pm_no_sleep_release(void) {
#if CONFIG_PM_ENABLE
    if (s_no_sleep)
        esp_pm_lock_release(s_no_sleep);
#endif
}
