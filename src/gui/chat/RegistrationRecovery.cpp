#include "gui/chat/RegistrationRecovery.h"

#include "common/PbrCompat.h"

namespace pbr {

RecoveryActions RegistrationRecovery::NeedUser(const bool call_live) {
  RecoveryActions out;
  if (state_ != State::NeedsUser) {
    state_ = State::NeedsUser;
    out.banner_changed = true;
    out.needs_user_now = true;
  }
  if (!alert_shown_) {
    // Never over a call: asked again at the next signal.
    if (!call_live) {
      out.show_alert = true;
      alert_shown_ = true;
    }
  }
  return out;
}

RecoveryActions RegistrationRecovery::OnNotRegistered(const bool locked, const bool auto_renew, const bool call_live,
                                                      const int64_t now_ms) {
  if (locked) {
    return {};
  }
  if (!auto_renew) {
    return NeedUser(call_live);
  }
  RecoveryActions out;
  if (state_ == State::Ok) {
    state_ = State::AutoPending;
  } else if (state_ == State::NeedsUser) {
    out = NeedUser(call_live); // the row stays; a held-back alert may show now
  }
  if (!last_request_ms_ || now_ms - *last_request_ms_ >= kRetryIntervalMs) {
    last_request_ms_ = now_ms;
    out.request_renewal = true;
  }
  return out;
}

RecoveryActions RegistrationRecovery::OnRenewal(const RenewalOutcome outcome, const bool auto_renew,
                                                const bool call_live) {
  if (state_ == State::Ok) {
    return {};
  }
  switch (outcome) {
    case RenewalOutcome::Refused:
      return NeedUser(call_live);
    case RenewalOutcome::NothingDue:
      if (!auto_renew) {
        return NeedUser(call_live); // turned off meanwhile
      }
      return {};
    case RenewalOutcome::Renewed:
    case RenewalOutcome::Transient:
      return {}; // a success shows when a poll does; a passing failure waits for the next try
  }
  return {};
}

RecoveryActions RegistrationRecovery::OnPollOk() {
  if (state_ == State::Ok) {
    return {};
  }
  RecoveryActions out;
  out.banner_changed = state_ == State::NeedsUser;
  state_ = State::Ok;
  if (manual_in_progress_) {
    out.toast_reconnected = true;
  } else if (!auto_toast_shown_) {
    out.toast_reconnected = true;
    auto_toast_shown_ = true;
  }
  manual_in_progress_ = false;
  return out;
}

void RegistrationRecovery::OnRoutineRenewed(const int64_t now_ms) {
  if (state_ == State::Ok) {
    last_request_ms_ = now_ms;
  }
}

bool RegistrationRecovery::OnUserTap() {
  if (state_ == State::Ok) {
    return false;
  }
  manual_in_progress_ = true;
  return true;
}

void RegistrationRecovery::ResetSession() {
  state_ = State::Ok;
  last_request_ms_.reset();
  manual_in_progress_ = false;
}

void ApplyRecoveryActions(const RecoveryActions& actions, const RegistrationRecovery& recovery,
                          const RecoveryHooks& hooks) {
  if (actions.needs_user_now && hooks.needs_user) {
    hooks.needs_user();
  }
  if (hooks.sync_banner) {
    hooks.sync_banner(recovery.BannerVisible());
  }
  if (actions.request_renewal) {
    // The renewal finds the registration due only when the expired mark is stored first.
    if (hooks.mark_expired && hooks.mark_expired() && hooks.run_renewal) {
      hooks.run_renewal();
    }
  }
  if (actions.show_alert && hooks.show_alert) {
    hooks.show_alert();
  }
  if (actions.toast_reconnected && hooks.toast_reconnected) {
    hooks.toast_reconnected();
  }
}

void HandleNotRegisteredSignal(RegistrationRecovery& recovery, const bool locked, const bool auto_renew,
                               const bool call_live, const int64_t now_ms, const RecoveryHooks& hooks) {
  ApplyRecoveryActions(recovery.OnNotRegistered(locked, auto_renew, call_live, now_ms), recovery, hooks);
}

} // namespace pbr
