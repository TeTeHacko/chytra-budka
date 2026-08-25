// test_mode_fsm.cpp — ladder + hysteresis tests for cb::next_profile.

#include <cassert>
#include <cstdio>

#include "cb/mode_fsm.h"

int main() {
  using cb::Profile;
  cb::ProfileThresholds t;  // defaults: max 65/55, act 45/38, eco 28/22, hib 0

  std::printf("test_mode_fsm:\n");

  // --- Boot seed: SOC-appropriate tier directly, no hysteresis ---
  assert(cb::next_profile(Profile::Boot, 80.0f, t) == Profile::Max);
  assert(cb::next_profile(Profile::Boot, 50.0f, t) == Profile::Active);
  assert(cb::next_profile(Profile::Boot, 30.0f, t) == Profile::Eco);
  assert(cb::next_profile(Profile::Boot, 20.0f, t) == Profile::Sentinel);
  // hib_enter == 0 → Boot floors at Sentinel even at empty SOC.
  assert(cb::next_profile(Profile::Boot, 2.0f, t) == Profile::Sentinel);

  // --- Step UP one rung at each enter threshold ---
  assert(cb::next_profile(Profile::Sentinel, 27.9f, t) == Profile::Sentinel);
  assert(cb::next_profile(Profile::Sentinel, 28.0f, t) == Profile::Eco);
  assert(cb::next_profile(Profile::Eco, 44.9f, t) == Profile::Eco);
  assert(cb::next_profile(Profile::Eco, 45.0f, t) == Profile::Active);
  assert(cb::next_profile(Profile::Active, 64.9f, t) == Profile::Active);
  assert(cb::next_profile(Profile::Active, 65.0f, t) == Profile::Max);

  // --- Step DOWN one rung at each leave threshold (hysteresis gap) ---
  assert(cb::next_profile(Profile::Max, 60.0f, t) == Profile::Max);      // in gap
  assert(cb::next_profile(Profile::Max, 55.0f, t) == Profile::Max);      // boundary holds
  assert(cb::next_profile(Profile::Max, 54.9f, t) == Profile::Active);
  assert(cb::next_profile(Profile::Active, 40.0f, t) == Profile::Active);  // in gap
  assert(cb::next_profile(Profile::Active, 38.0f, t) == Profile::Active);  // boundary holds
  assert(cb::next_profile(Profile::Active, 37.9f, t) == Profile::Eco);
  assert(cb::next_profile(Profile::Eco, 25.0f, t) == Profile::Eco);        // in gap
  assert(cb::next_profile(Profile::Eco, 22.0f, t) == Profile::Eco);        // boundary holds
  assert(cb::next_profile(Profile::Eco, 21.9f, t) == Profile::Sentinel);

  // --- Moves at most one rung per call ---
  assert(cb::next_profile(Profile::Max, 10.0f, t) == Profile::Active);     // not Sentinel
  assert(cb::next_profile(Profile::Sentinel, 90.0f, t) == Profile::Eco);   // not Max

  // --- Hibernate is opt-in only (hib_enter default 0 = never) ---
  assert(cb::next_profile(Profile::Sentinel, 1.0f, t) == Profile::Sentinel);

  cb::ProfileThresholds h = t;
  h.hib_enter = 15.0f;
  assert(cb::next_profile(Profile::Sentinel, 10.0f, h) == Profile::Hibernate);
  assert(cb::next_profile(Profile::Sentinel, 20.0f, h) == Profile::Sentinel);  // >hib, <eco
  assert(cb::next_profile(Profile::Boot, 10.0f, h) == Profile::Hibernate);
  // Recovery out of survival once meaningfully recharged.
  assert(cb::next_profile(Profile::Hibernate, 30.0f, h) == Profile::Sentinel);
  assert(cb::next_profile(Profile::Hibernate, 25.0f, h) == Profile::Hibernate);

  std::printf("  ok: ladder seed, step-up/down hysteresis, one-rung, hibernate opt-in\n");
  std::printf("test_mode_fsm: PASS\n");
  return 0;
}
