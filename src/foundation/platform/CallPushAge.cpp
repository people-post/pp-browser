#include "foundation/platform/CallPushAge.h"

#include <cmath>

namespace pbr {

namespace {

/** 2001-09-09 in Unix ms: a smaller `sent_at` is seconds, or garbage — never a relay's send time in ms. */
constexpr double kMinPlausibleSentAtMs = 1e12;
/** Year 2286: a larger one is not a time either (and would not fit the age arithmetic). */
constexpr double kMaxPlausibleSentAtMs = 1e13;

} // namespace

CallPushAge JudgeCallPushAge(const std::optional<double> sent_at_ms, const int64_t now_ms) {
  CallPushAge result;
  if (!sent_at_ms || !std::isfinite(*sent_at_ms) || *sent_at_ms < kMinPlausibleSentAtMs ||
      *sent_at_ms > kMaxPlausibleSentAtMs) {
    return result;
  }
  result.known = true;
  const int64_t age = now_ms - static_cast<int64_t>(*sent_at_ms);
  result.age_ms = age > 0 ? age : 0;
  result.stale = age > kCallPushStaleAfterMs && age <= kCallPushClockSuspectMs;
  return result;
}

} // namespace pbr
