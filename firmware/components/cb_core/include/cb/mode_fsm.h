// mode_fsm.h — battery-SOC driven power-profile ladder.
// Pure C++17, no platform deps.
//
// One ladder replaces the old {Safe,Triggered,Continuous} modes + the
// pm_lightsleep / deep-sleep bolt-ons. Each tier bundles activity (audio /
// capture), sleep depth, telemetry cadence and WiFi power-save. `auto` walks
// this ladder by SOC; an operator can also pin any tier. Ordered ascending by
// power draw so ladder comparisons read naturally (higher enum = more power).
//
//   Hibernate  deep-sleep duty cycle, UNREACHABLE        ~0.1 W
//   Sentinel   sensors only, audio off, light-sleep      ~0.25 W
//   Eco        motion capture, audio off, light-sleep    ~0.3 W
//   Active     motion capture, audio (VAD burst), no sleep ~1.1 W
//   Max        continuous stream + capture, no sleep      ~1.1 W+
//
// The sleeping tiers (Eco/Sentinel/Hibernate) force audio OFF: the mic's
// continuous I2S holds cb_pm's NO_LIGHT_SLEEP lock, so sleep can't engage while
// audio runs. That coupling is intrinsic, baked into the tier here + enforced
// in apply_power_state().
#pragma once

#include <cstdint>

namespace cb {

enum class Profile : uint8_t {
  Boot = 0,
  Hibernate = 1,
  Sentinel = 2,
  Eco = 3,
  Active = 4,
  Max = 5,
};

const char *profile_name(Profile p);

struct ProfileThresholds {
  // Enter going UP / leave going DOWN SOC % (0..100), with hysteresis
  // (enter > leave) on each boundary. Defaults span a 1S Li-ion curve.
  float max_enter = 65.0f;   // Active → Max
  float max_leave = 55.0f;   // Max → Active
  float act_enter = 45.0f;   // Eco → Active
  float act_leave = 38.0f;   // Active → Eco
  float eco_enter = 28.0f;   // Sentinel → Eco
  float eco_leave = 22.0f;   // Eco → Sentinel
  // Opt-in survival floor: when > 0, auto may descend below Sentinel into
  // Hibernate (deep sleep, unreachable). 0 = auto NEVER self-hibernates; the
  // ladder floors at Sentinel and Hibernate is a manual-only choice.
  float hib_enter = 0.0f;    // Sentinel → Hibernate (if > 0)
};

// Decide the next profile given the current one and a SOC reading. Moves at
// most one rung per call (hysteresis on each boundary). From Boot, seeds the
// SOC-appropriate tier directly (no hysteresis). When hib_enter == 0 the auto
// ladder floors at Sentinel.
Profile next_profile(Profile current, float soc, const ProfileThresholds &t);

}  // namespace cb
