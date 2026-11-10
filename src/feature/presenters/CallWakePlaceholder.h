#pragma once

#include "lib/calls/CallScreen.h"
#include "lib/calls/CallTypes.h"

#include <cstdint>
#include <optional>

namespace pbr {

/**
 * The incoming-call placeholder page (iOS): a tap on a call alert opens the app on "Incoming call, connecting..."
 * until the real ring takes over. Decided here (no GUI, no backend, no platform types: the caller passes in what
 * PendingNotificationTap says), so it can run before the vault is unlocked.
 *
 * Starts on a fresh call tap that has not been handled, with no call screen up, whose notification is not stale.
 * Ends, quietly, when the ring shows (Ring), the tap goes stale or the caller cannot still be ringing (Expired), or
 * messaging has been ready for kNoRingAfterReadyMs without a ring (Timeout). The caller rings for at most
 * kDefaultCallInviteTtlMs, so notification age plus time shown past that ends it even if messaging never became
 * ready (a locked phone nobody unlocks). Every end marks the tap handled: the same tap never shows it again, a new tap
 * does. A ring that arrives after the end is the ordinary ring; nothing is reported for it.
 */
class CallWakePlaceholder {
public:
  enum class Outcome { None, Ring, Timeout, Expired };

  /** Messaging ready this long without a ring: the call is not coming. */
  static constexpr int64_t kNoRingAfterReadyMs = 10'000;
  /** A tapped alert older than this is not a call to wait for; same length as the invite TTL. */
  static constexpr int64_t kMaxNotificationAgeMs = kDefaultCallInviteTtlMs;

  struct Input {
    uint64_t tap_seq = 0;
    /** PendingNotificationTap::PeekCallRoute. */
    bool tap_fresh = false;
    /** How long before the tap the tapped notification was delivered; unknown = nullopt. */
    std::optional<int64_t> tap_notification_age_ms;
    CallScreen::Kind screen_kind = CallScreen::Kind::None;
    bool messaging_ready = false;
    int64_t now_ms = 0;
  };

  struct View {
    bool active = false;
    /** Set on the one step where the placeholder ends; the two fields below are valid with it. */
    Outcome outcome = Outcome::None;
    /** How long it was shown. */
    int64_t shown_ms = 0;
    /** Tap to messaging ready, if it became ready while shown. */
    std::optional<int64_t> ready_ms;
  };

  View Step(const Input& in);

private:
  View End(const Input& in, Outcome outcome);

  bool active_ = false;
  uint64_t handled_seq_ = 0;
  uint64_t active_seq_ = 0;
  int64_t start_ms_ = 0;
  std::optional<int64_t> ready_since_ms_;
};

} // namespace pbr
