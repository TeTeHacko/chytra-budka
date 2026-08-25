// cb_ds.h — hibernate (deep-sleep duty cycle) for the bottom power tier.
//
// Active only when the resolved power profile is Hibernate (see cb::Profile /
// app_profile_is_hibernate()). The unit wakes on a periodic RTC timer or a PIR
// motion edge (EXT1, armed by pir_init()), does its work — publish telemetry,
// optionally snap a photo on motion, occasionally check OTA — then arms the wake
// sources and deep-sleeps again. Unreachable while asleep (accepted trade-off).
//
// Lifecycle from main.cpp:
//   app_main():            ds_capture_wake()   once, after diag_capture_boot()
//   supervisor loop:       ds_note_activity()    on each consumed PIR edge
//                          ds_note_telemetry_published()  after each publish
//                          ds_maybe_sleep()    at the loop tail (may not return)
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Classify the deep-sleep wake cause (timer / PIR / cold boot), bump the
// RTC-persisted counters, and open the wake window. On a clean ESP_RST_DEEPSLEEP
// wake also clears the consecutive-crash counter (a completed sleep→wake cycle
// proves a non-crashing run). Call once early in app_main(), after
// diag_capture_boot(). No-op-safe on any board (the magic guard self-initialises
// on first cold boot).
void ds_capture_wake(void);

// Note that a telemetry publish happened in the current wake window (the HA
// heartbeat gate for re-sleep). Harmless outside hibernate.
void ds_note_telemetry_published(void);

// True when hibernating and no telemetry heartbeat has gone out this wake window
// yet. The supervisor uses this to drive one real (MQTT-connected) publish per
// wake — hibernate's telemetry cadence (ds_sleep_s) is far longer than the wake
// window, so the periodic publisher never fires in time on its own.
bool ds_heartbeat_pending(void);

// Seconds remaining in the current wake window before deep sleep (the OLED
// countdown). -1 when not hibernating. A PIR edge resets it to ds_pir_win_s
// (the window switches to "ds_pir_win_s since last motion"); a plain timer wake
// counts down ds_wake_s. This is the window budget only — actual sleep also
// waits for a telemetry heartbeat + MQTT drain, so 0 means "sleeping imminently".
int ds_seconds_to_sleep(void);

// Note fresh trigger activity (PIR motion, reed edge, or button press) — it
// (re)extends the wake window so a burst of activity keeps the unit awake (gives
// time to cycle the OLED, take the reed photo, etc.). Harmless outside hibernate.
void ds_note_activity(void);

// Re-sleep decision, evaluated once per supervisor tick at the loop tail. When
// the profile is Hibernate and the wake window is satisfied (work done, OTA not
// pending-verify, BLE off, budget elapsed), this drains MQTT, paints the OLED,
// arms the wake sources and calls esp_deep_sleep_start() — it does NOT return in
// that case. No-op when the profile isn't Hibernate.
void ds_maybe_sleep(void);

// True when a PIR-triggered photo is allowed: always outside hibernate; inside
// hibernate only when the ds_pir_photo knob is set. Used to gate the motion→
// capture path in the supervisor loop.
bool ds_pir_photo_allowed(void);

#ifdef __cplusplus
}
#endif
