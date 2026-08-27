#pragma once

/*
 * app_main_exports.h — declarations for symbols defined in main.cpp
 * that other translation units (http_server.c, etc.) call. Replaces
 * one-off `extern void ...` forward decls scattered across the
 * callers; with a shared header a signature change in main.cpp
 * causes a clean compile error at every call site instead of a
 * silent link-time mismatch.
 */

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run the peripheral self-test, publish the JSON summary on the
 * mqtt selftest topic, and optionally copy it into `out` (truncated
 * to `out_sz - 1` bytes + NUL). Caller may pass NULL/0 to skip the
 * copy and only get the MQTT side effect. Defined in main.cpp. */
void selftest_run_and_publish(char *out, size_t out_sz);

/* Debug hatch wired in main.cpp — busy-spins the main loop for `ms`
 * milliseconds without feeding TWDT, forcing the panic + coredump
 * pipeline. Called from /debug/hang to exercise the crash-recovery
 * path on a bench board. Compiled out in production builds when
 * CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n. */
void debug_hang_main_for_ms(int ms);

/* Atomic snapshot of the supervisor's power profile (cb::Profile underlying
 * type). Safe to call from any core; the writer is enter_profile() on app_main.
 * Cast the return to (cb::Profile) at the call site. */
int app_mode_current(void);

/* True when the resolved profile light-sleeps (Eco or Sentinel — the tiers
 * where automatic light sleep is engaged; audio is off there). Periodic poll
 * tasks (PIR, status LED, boot-button, audio idle, the supervisor loop)
 * lengthen their delay when this is true so the CPU can stay in light sleep
 * between the ~400 ms WiFi DTIM wakes instead of being woken every 20-100 ms.
 * False in Max/Active (full polling) and in Hibernate (deep sleep, cb_ds owns
 * it). Cross-core safe. */
bool app_profile_sleeps(void);

/* True when the resolved profile is Hibernate (the deep-sleep duty-cycle tier).
 * Read by cb_ds (a C TU that can't see cb::Profile) to gate the re-sleep
 * decision. Cross-core safe. */
bool app_profile_is_hibernate(void);

#ifdef __cplusplus
}
#endif
