#pragma once

#include "lib/calls/CallScreen.h"
#include "lib/calls/CallTypes.h"
#include "common/ShareableLine.h"

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
 * kDefaultCallInviteTtlMs, so the alert's age (at the tap, plus since the tap) past that ends it even if messaging
 * never became ready (a locked phone nobody unlocks). It waits (not handled) while the app is not visible, and ends
 * (Expired, handled) if the app goes hidden while it is shown. Every end marks the tap handled: the same tap never shows it again, a new tap
 * does. A ring that arrives after the end is the ordinary ring; nothing is reported for it.
 *
 * A call "who can call me" dropped (`blocked`, DEV/PLAN-call-stranger-declined.md) will never ring: a page up for it
 * ends at once (Blocked), and a tap for it never puts the page up (Blocked, handled, shown_ms 0). A ring on screen
 * comes first: it is never ended for someone else's dropped call.
 */
class CallWakePlaceholder {
public:
  enum class Outcome { None, Ring, Timeout, Expired, Blocked };

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
    /** How long ago the tap was recorded (PendingNotificationTap::CallTapElapsed): the alert is this much older. */
    int64_t tap_elapsed_ms = 0;
    /** The app's screen is up (AppLifecycle::IsVisible); a ring page nobody can see must not ring. */
    bool app_visible = true;
    CallScreen::Kind screen_kind = CallScreen::Kind::None;
    bool messaging_ready = false;
    /** A call dropped by "who can call me" is the tapped one (its id is the tap's hint, or the tap has none). */
    bool blocked = false;
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
  /** `call.wake_ring`'s outcome word (ring / timeout / blocked / expired). */
  static ShareWord OutcomeWord(Outcome outcome);

private:
  /** The alert was delivered more than the caller rings for before now. */
  static bool AlertTooOld(const Input& in);
  View End(const Input& in, Outcome outcome);

  bool active_ = false;
  uint64_t handled_seq_ = 0;
  uint64_t active_seq_ = 0;
  int64_t start_ms_ = 0;
  std::optional<int64_t> ready_since_ms_;
};

} // namespace pbr
