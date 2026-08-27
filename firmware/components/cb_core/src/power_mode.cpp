#include "cb/power_mode.h"

namespace cb {

static int clamp_hour(int h) {
  if (h < 0) return 0;
  if (h > 23) return 23;
  return h;
}

bool audio_window_open(int hour, int on_h, int off_h) {
  hour = clamp_hour(hour);
  on_h = clamp_hour(on_h);
  off_h = clamp_hour(off_h);

  if (on_h == off_h) return true;          // disabled => always open
  if (on_h < off_h) return hour >= on_h && hour < off_h;  // same-day
  return hour >= on_h || hour < off_h;     // wraps midnight
}

}  // namespace cb
