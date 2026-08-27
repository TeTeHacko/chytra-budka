/* cb_time.h — small time helpers with FreeRTOS-tick-safe semantics.
 *
 * WHY THIS EXISTS
 * ---------------
 * The tick rate on this project is CONFIG_FREERTOS_HZ = 100, i.e. one tick
 * is 10 ms. `pdMS_TO_TICKS(ms)` is integer `(ms * HZ) / 1000`, so ANY value
 * below 10 ms rounds DOWN to 0 ticks. `vTaskDelay(0)` does NOT block and does
 * NOT yield to lower-priority tasks (with preemption it only switches between
 * equal/higher priority) — so a poll loop that "sleeps" for <10 ms actually
 * busy-spins at its own priority. If that task sits above the main loop and
 * the idle task, it starves them → the Task Watchdog fires (observed:
 * v0.4.4 `task_wdt` reboots, main + IDLE0 on CPU0, root-caused from a coredump
 * to mic_wav_get's `vTaskDelay(pdMS_TO_TICKS(8))`).
 *
 * ESP-IDF guidance (Watchdogs → "Common Error Logs"): in wait loops use a
 * blocking call with a NON-ZERO timeout, or vTaskDelay() that actually yields.
 *
 * cb_delay_ms() guarantees at least one tick, so it always yields the CPU to
 * the idle task and any lower-priority work — turning a silent busy-spin into
 * a real sleep. Use it instead of bare `vTaskDelay(pdMS_TO_TICKS(<small>))`
 * anywhere a delay shorter than the tick period might be requested.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sleep for at least `ms`, but never less than one tick (so it always
 * yields). At HZ=100 the floor is 10 ms; sub-tick requests are rounded UP to
 * one tick rather than silently truncated to zero. */
static inline void cb_delay_ms(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

#ifdef __cplusplus
}
#endif
