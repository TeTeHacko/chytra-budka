#include "cb/mode_fsm.h"

namespace cb {

const char *profile_name(Profile p) {
  switch (p) {
    case Profile::Max: return "max";
    case Profile::Active: return "active";
    case Profile::Eco: return "eco";
    case Profile::Sentinel: return "sentinel";
    case Profile::Hibernate: return "hibernate";
    case Profile::Boot:
    default: return "boot";
  }
}

Profile next_profile(Profile current, float soc, const ProfileThresholds &t) {
  // Boot seed: pick the SOC-appropriate tier directly, no hysteresis.
  if (current == Profile::Boot) {
    if (t.hib_enter > 0.0f && soc < t.hib_enter) return Profile::Hibernate;
    if (soc >= t.max_enter) return Profile::Max;
    if (soc >= t.act_enter) return Profile::Active;
    if (soc >= t.eco_enter) return Profile::Eco;
    return Profile::Sentinel;
  }

  // Step UP one rung when the enter-threshold is crossed.
  if (current == Profile::Sentinel && soc >= t.eco_enter) return Profile::Eco;
  if (current == Profile::Eco && soc >= t.act_enter) return Profile::Active;
  if (current == Profile::Active && soc >= t.max_enter) return Profile::Max;

  // Step DOWN one rung when the leave-threshold is crossed.
  if (current == Profile::Max && soc < t.max_leave) return Profile::Active;
  if (current == Profile::Active && soc < t.act_leave) return Profile::Eco;
  if (current == Profile::Eco && soc < t.eco_leave) return Profile::Sentinel;

  // Optional survival floor (auto-hibernate) — only when explicitly enabled.
  if (current == Profile::Sentinel && t.hib_enter > 0.0f && soc < t.hib_enter)
    return Profile::Hibernate;
  // Recover out of survival once meaningfully recharged (one rung up).
  if (current == Profile::Hibernate && soc >= t.eco_enter) return Profile::Sentinel;

  return current;
}

}  // namespace cb
