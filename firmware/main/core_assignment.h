/* core_assignment.h — single-source-of-truth task → core pinning map.
 *
 * This header is docs-only (no runtime code). Update this table AND
 * firmware/tests/hil/test_dual_core.py whenever you add or repin a task,
 * so the bench regression test catches accidental drift.
 *
 *   Task name      Core     Prio    Stack   Created by
 *   ─────────────────────────────────────────────────────────────────────
 *   main           0        1       5120    IDF (CONFIG_ESP_MAIN_TASK_*),
 *                                            priority dropped to 1 in
 *                                            app_main() so audio (10) and
 *                                            WiFi (23) cleanly preempt.
 *   audio          1        10      8192    audio.cpp::audio_task_start
 *   cam_wrk        1        5       6144    camera.c::camera_worker_start
 *
 *   tiT (lwIP)     0        18      —       IDF
 *   wifi           0        23      —       IDF
 *   mqtt_task      NO_AFF   5       —       esp-mqtt (sched picks; today
 *                                            lands on CPU0 because
 *                                            CONFIG_MQTT_TASK_CORE_SELECTION
 *                                            _ENABLED is unset)
 *   cam_task       0        23      —       esp32-camera driver DMA
 *                                            feeder (CPU0 by driver default)
 *   esp_timer      0        22      —       IDF
 *   sys_evt        0        20      —       IDF
 *   ipc0           0        24      —       IDF
 *   ipc1           1        24      —       IDF
 *
 *   reed           NO_AFF   1       2048    reed.c
 *   pir            NO_AFF   1       2560    pir.c
 *   ota            NO_AFF   1       12288   ota.c
 *   glitchtip      NO_AFF   1       6144    glitchtip.c
 *   status_led     NO_AFF   1       2048    status_led.c
 *   photo_queue    NO_AFF   3       4096    photo_queue.c
 *
 * Why this layout:
 *   - WiFi/LWIP/MQTT/HTTPD stay on CPU0 per IDF defaults — moving them
 *     fights the framework and breaks Espressif's tested topology.
 *   - audio_task on CPU1 prio 10 means the 32 ms PDM cadence is never
 *     interrupted by a long camera capture, MQTT publish, or HTTP
 *     handler running on CPU0.
 *   - cam_wrk on CPU1 prio 5 sits below audio so a 480 ms IR-shot
 *     warmup never starves the audio pump. WiFi at prio 23 on CPU0
 *     still preempts everything; LWIP at 18 preempts the supervisor.
 *   - app_main supervisor on CPU0 at prio 1 is intentionally low: it
 *     only runs a 10 Hz tick (sleep 100 ms tail) and never holds the
 *     CPU under contention.
 *
 * TWDT: main, audio, and cam_wrk all subscribe; each one resets at the
 * top of its loop before any blocking call. Idle tasks on both cores
 * are also TWDT-checked (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU{0,1}=y)
 * so a busy-spin starvation also panics.
 */
#pragma once
