/* log_throttle.h — per-callsite rate-limited ESP_LOG wrappers.
 *
 * Existing fragile read paths (battery.c MAX17048 retry loop, sht41
 * bit-bang external sensor, ina226 read) can emit one ESP_LOGW per
 * failed I/O. If the sensor is genuinely broken or briefly stuck, the
 * monitor floods with repeated identical lines and the actual cause
 * scrolls off — yet swallowing all logs means the operator never
 * sees "your battery gauge stopped answering 6 hours ago".
 *
 * `LOG_THROTTLED(level, tag, window_ms, fmt, ...)` emits the log line
 * at most once per `window_ms`. The static state is per-callsite,
 * so two distinct call sites don't share a throttle window. Macros
 * mirror ESP_LOGI/W/E spelling so it's a drop-in replacement.
 *
 * Implementation note: uses esp_timer (microsecond monotonic since
 * boot, safe across deep-sleep wakeups), NOT xTaskGetTickCount —
 * the latter rolls over at uint32 ms and would re-fire the line
 * after ~49 days when the window straddles the wrap.
 */
#pragma once

#include "esp_log.h"
#include "esp_timer.h"

#define LOG_THROTTLED(level, tag, window_ms, fmt, ...)                   \
    do {                                                                 \
        static int64_t s_log_throttle_last_us = 0;                       \
        int64_t now_us = esp_timer_get_time();                           \
        if (now_us - s_log_throttle_last_us >= (window_ms) * 1000LL) {   \
            s_log_throttle_last_us = now_us;                             \
            ESP_LOG##level(tag, fmt, ##__VA_ARGS__);                     \
        }                                                                \
    } while (0)

#define LOG_THROTTLED_W(tag, window_ms, fmt, ...) \
    LOG_THROTTLED(W, tag, window_ms, fmt, ##__VA_ARGS__)
#define LOG_THROTTLED_E(tag, window_ms, fmt, ...) \
    LOG_THROTTLED(E, tag, window_ms, fmt, ##__VA_ARGS__)
#define LOG_THROTTLED_I(tag, window_ms, fmt, ...) \
    LOG_THROTTLED(I, tag, window_ms, fmt, ##__VA_ARGS__)
