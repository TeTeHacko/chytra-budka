/* status_led.c — status/debug blink patterns on onboard GPIO21 LED. */

#include "status_led.h"

#include <stdatomic.h>

#include "app_config.h"
#include "app_main_exports.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 21
#endif

/* Seeed XIAO ESP32-S3 onboard user LED is active-low. */
#define LED_ON_LEVEL  0
#define LED_OFF_LEVEL 1

static atomic_bool s_started = false;
static atomic_bool s_wifi_connected = false;
static atomic_bool s_mqtt_connected = false;
static atomic_bool s_capture_active = false;
static atomic_bool s_ota_active = false;
static _Atomic int64_t s_boot_until_ms = 0;
static _Atomic int64_t s_pir_until_ms = 0;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void led_write(bool on) {
    gpio_set_level((gpio_num_t)STATUS_LED_PIN, on ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

static bool in_window(uint32_t phase, uint32_t start, uint32_t end) {
    return phase >= start && phase < end;
}

static bool render_debug_pattern(int64_t now) {
    int64_t boot_until = atomic_load(&s_boot_until_ms);
    if (now < boot_until) {
        /* Boot: three quick flashes in the first second. */
        uint32_t p = (uint32_t)(now % 1200);
        return in_window(p, 0, 90) || in_window(p, 250, 340) || in_window(p, 500, 590);
    }

    if (atomic_load(&s_capture_active)) {
        /* Camera capture: solid LED while the long path runs. */
        return true;
    }

    if (atomic_load(&s_ota_active)) {
        /* OTA check/download: fast regular blink. */
        return (now % 250) < 125;
    }

    if (now < atomic_load(&s_pir_until_ms)) {
        /* PIR: short double-flash, independent of connectivity. */
        uint32_t p = (uint32_t)(now % 700);
        return in_window(p, 0, 90) || in_window(p, 200, 290);
    }

    if (!atomic_load(&s_wifi_connected)) {
        /* WiFi connecting/reconnecting: 1 Hz blink. */
        return (now % 1000) < 500;
    }

    if (!atomic_load(&s_mqtt_connected)) {
        /* MQTT connecting: double-blink every 2 s. */
        uint32_t p = (uint32_t)(now % 2000);
        return in_window(p, 0, 120) || in_window(p, 300, 420);
    }

    /* Idle/healthy: tiny heartbeat every 5 s. */
    return (now % 5000) < 80;
}

static void task(void *arg) {
    (void)arg;
    while (true) {
        bool enabled = app_config_get_bool("status_led_en");
        bool debug = app_config_get_bool("status_led_dbg");
        bool on = false;

        if (enabled) {
            int64_t now = now_ms();
            if (debug) {
                on = render_debug_pattern(now);
            } else {
                /* Non-debug mode stays dark except during explicit long-running
                 * actions where a visible "don't unplug me" cue is valuable. */
                on = atomic_load(&s_capture_active) || atomic_load(&s_ota_active);
            }
        }

        led_write(on);
        /* Deep-save: poll slowly so this task doesn't wake CPU0 every 50 ms.
         * The LED is dark in Safe anyway (no capture/OTA), so a sluggish cue is
         * acceptable; full/normal modes keep the 50 ms blink cadence. */
        vTaskDelay(pdMS_TO_TICKS(app_profile_sleeps() ? 500 : 50));
    }
}

void status_led_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_started, &expected, true))
        return;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d init failed: %s", STATUS_LED_PIN, esp_err_to_name(e));
        return;
    }
    led_write(false);
    atomic_store(&s_boot_until_ms, now_ms() + 1800);

    BaseType_t ok = xTaskCreate(task, "status_led", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        led_write(false);
        return;
    }
    ESP_LOGI(TAG, "onboard LED ready on GPIO%d (active-low)", STATUS_LED_PIN);
}

void status_led_wifi_connected(bool connected) {
    atomic_store(&s_wifi_connected, connected);
}

void status_led_mqtt_connected(bool connected) {
    atomic_store(&s_mqtt_connected, connected);
}

void status_led_pir_pulse(void) {
    atomic_store(&s_pir_until_ms, now_ms() + 700);
}

void status_led_capture_begin(void) {
    atomic_store(&s_capture_active, true);
}

void status_led_capture_end(void) {
    atomic_store(&s_capture_active, false);
}

void status_led_ota_active(bool active) {
    atomic_store(&s_ota_active, active);
}
