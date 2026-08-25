// test_power_mode.cpp — active-hours window predicate tests for
// cb::audio_window_open (drives the per-mode audio gate in main.cpp).

#include <cassert>
#include <cstdio>

#include "cb/power_mode.h"

int main() {
  using cb::audio_window_open;

  std::printf("test_power_mode:\n");

  // Disabled window (on == off) => always open, regardless of hour.
  assert(audio_window_open(0, 0, 0));
  assert(audio_window_open(13, 0, 0));
  assert(audio_window_open(23, 7, 7));

  // Same-day window 5..21 (inclusive start, exclusive end).
  assert(!audio_window_open(4, 5, 21));
  assert(audio_window_open(5, 5, 21));   // boundary: start is inside
  assert(audio_window_open(12, 5, 21));
  assert(audio_window_open(20, 5, 21));
  assert(!audio_window_open(21, 5, 21)); // boundary: end is outside
  assert(!audio_window_open(23, 5, 21));

  // Wrap-midnight window 22..6 (e.g. nocturnal listening).
  assert(audio_window_open(22, 22, 6));  // boundary: start inside
  assert(audio_window_open(23, 22, 6));
  assert(audio_window_open(0, 22, 6));
  assert(audio_window_open(5, 22, 6));
  assert(!audio_window_open(6, 22, 6));  // boundary: end outside
  assert(!audio_window_open(12, 22, 6));
  assert(!audio_window_open(21, 22, 6));

  // Out-of-range hour/on/off clamp to 0..23 (defends against stale NVS) —
  // must never throw or read as permanently closed.
  assert(audio_window_open(99, 0, 0));    // disabled stays open
  assert(audio_window_open(-3, 0, 0));
  assert(audio_window_open(10, -1, 99));  // clamps to 0..23 => same-day 0..23

  std::printf("  ok: window open/closed across same-day, wrap, disabled, clamp\n");
  std::printf("test_power_mode: PASS\n");
  return 0;
}
