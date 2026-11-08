#pragma once

#include <cstdint>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * How old a call push may be and still ring (P017): the age past which the app drops the invite itself (a ring's
 * 60 s plus 5 s of slack, kDefaultCallInviteTtlMs + kCallInviteRelayAgeSlackMs in CallSessionLogic::
 * ShouldDropStaleInvite; the invite's relay age is at least the push's). A younger push still finds its ring, so its
 * call waits for it: ended earlier, the ring found next would ring as a second call.
 */
inline constexpr int64_t kCallPushStaleAfterMs = 65'000;
/**
 * An age past this says the phone's clock and the relay's disagree, not that the push waited: such a push rings
 * as before (the catch-up ends it if its call is over). A missed call is worse than a ghost one.
 */
inline constexpr int64_t kCallPushClockSuspectMs = 60LL * 60 * 1000;

/** What a call push's `sent_at` says about it. */
struct CallPushAge {
  /** The push carried a usable `sent_at`. */
  bool known = false;
  /** Now minus `sent_at`, at least 0 (a clock behind the relay's reads as fresh); 0 when not known. */
  int64_t age_ms = 0;
  /** Too old to ring: report it to the phone's call screen as required and end it at once. */
  bool stale = false;
};

/**
 * `sent_at_ms`: the push's `sent_at` (the relay's send time, Unix ms, a JSON number; unsigned and optional: an
 * older relay sends none). A missing, non-finite or implausible value (not milliseconds after 2001) says nothing,
 * and the push rings as before. `now_ms`: this device's clock, Unix ms.
 */
CallPushAge JudgeCallPushAge(std::optional<double> sent_at_ms, int64_t now_ms);

} // namespace pbr
