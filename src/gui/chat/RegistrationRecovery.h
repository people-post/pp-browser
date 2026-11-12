#pragma once

#include <cstdint>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/** How a registration renewal ended, as the recovery sees it. */
enum class RenewalOutcome {
  Renewed,    // registered (and stored)
  Refused,    // the server refused it (a 4xx but 408 / 429): trying again alone will not help
  Transient,  // network, timeout, 5xx, 408, 429, or a local failure: may pass
  NothingDue, // the renewal found nothing to do (not expired, or "renew automatically" off)
};

/** What the recovery tells its owner to do after an input. */
struct RecoveryActions {
  /** Mark the registration expired, store it, and run the existing renewal task. */
  bool request_renewal = false;
  /** Ask once: "Reconnect your account" with a "Reconnect now" button. */
  bool show_alert = false;
  /** The persistent "Not connected. Tap here to reconnect." row appeared or went (read BannerVisible). */
  bool banner_changed = false;
  /** Say "Reconnected". */
  bool toast_reconnected = false;
  /** The recovery just started needing the user (for the metric). */
  bool needs_user_now = false;
};

/**
 * The relay answered a poll "Relay user not registered" (B216: a new server, or one that lost the registration): what
 * to do about it. Pure: no I/O, no clock (time comes in), no text. One per run of the app, owned by ChatController,
 * UI thread only.
 *
 * - Ok: nothing wrong. AutoPending: renewing automatically, nothing shown. NeedsUser: the persistent row is shown.
 * - Locked: nothing (and no attempt used).
 * - "Renew automatically" on: AutoPending, a renewal at most every kRetryIntervalMs (a success does not reset that).
 * - Off, or a renewal Refused: NeedsUser; the alert once per run, held back while a call is live and shown at a
 *   later signal once it is not.
 * - Transient: nothing; the next signal after the interval tries again.
 * - Only a poll that succeeds (OnPollOk) brings it back to Ok: the row goes; after a tap "Reconnected" always,
 *   otherwise once per run.
 * - A renewal's result while Ok (the routine one after unlock) is ignored.
 */
class RegistrationRecovery {
public:
  enum class State { Ok, AutoPending, NeedsUser };

  static constexpr int64_t kRetryIntervalMs = 5 * 60 * 1000;

  RecoveryActions OnNotRegistered(bool locked, bool auto_renew, bool call_live, int64_t now_ms);
  RecoveryActions OnRenewal(RenewalOutcome outcome, bool auto_renew, bool call_live);
  RecoveryActions OnPollOk();
  /** The user tapped the row or the alert's button. False: already Ok again, nothing to reconnect. */
  bool OnUserTap();
  /** Messaging was torn down: state and the retry clock go; the once-per-run alert and toast stay used. */
  void ResetSession();

  State CurrentState() const { return state_; }
  bool BannerVisible() const { return state_ == State::NeedsUser; }

private:
  RecoveryActions NeedUser(bool call_live);

  State state_ = State::Ok;
  std::optional<int64_t> last_request_ms_;
  bool manual_in_progress_ = false;
  // Once per run:
  bool alert_shown_ = false; // a live call holds it back until a later signal
  bool auto_toast_shown_ = false;
};

} // namespace pbr
