// power_mode.h — wall-clock "active-hours" window predicate for audio.
// Pure C++17, no platform deps (testable on host like mode_fsm).
#pragma once

namespace cb {

// Is the current local hour inside the audio active-hours window?
//   hour   : local wall-clock hour, 0..23
//   on_h   : window start hour, 0..23
//   off_h  : window end hour (exclusive), 0..23
//
// Semantics:
//   - on_h == off_h        => window disabled  => always open (returns true).
//                             This is the default (on=off=0) so a unit with no
//                             window configured behaves exactly as before.
//   - on_h <  off_h        => same-day window  => on_h <= hour < off_h.
//   - on_h >  off_h        => wraps midnight   => hour >= on_h || hour < off_h.
//
// Out-of-range hour/on/off are clamped to 0..23 defensively so a stale NVS
// value can never make this throw or read as "always closed".
bool audio_window_open(int hour, int on_h, int off_h);

}  // namespace cb
