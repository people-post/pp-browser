#include "feature/presenters/CallWakePlaceholder.h"

namespace pbr {

bool CallWakePlaceholder::AlertTooOld(const Input& in) {
  return in.tap_notification_age_ms.value_or(0) + in.tap_elapsed_ms > kMaxNotificationAgeMs;
}

CallWakePlaceholder::View CallWakePlaceholder::Step(const Input& in) {
  using Outcome = CallWakePlaceholder::Outcome;
  if (!active_) {
    if (!in.tap_fresh || in.tap_seq == handled_seq_) {
      return {};
    }
    if (!in.app_visible) {
      return {}; // the tap may arrive before the app is shown again: it waits, not handled
    }
    if (in.screen_kind != CallScreen::Kind::None || AlertTooOld(in)) {
      handled_seq_ = in.tap_seq;
      return {};
    }
    if (in.blocked) {
      handled_seq_ = in.tap_seq; // the call was dropped before the tap: no page for it
      View view;
      view.outcome = Outcome::Blocked;
      return view;
    }
    active_ = true;
    active_seq_ = in.tap_seq;
    start_ms_ = in.now_ms;
    ready_since_ms_.reset();
  } else if (in.tap_seq != active_seq_ && in.tap_fresh) {
    // A newer tap replaced the one being waited on: wait for its call from now.
    active_seq_ = in.tap_seq;
    start_ms_ = in.now_ms;
    ready_since_ms_.reset();
  }

  if (in.screen_kind != CallScreen::Kind::None) {
    return End(in, Outcome::Ring);
  }
  if (in.blocked) {
    return End(in, Outcome::Blocked);
  }
  if (!in.tap_fresh || AlertTooOld(in) || !in.app_visible) {
    return End(in, Outcome::Expired);
  }
  if (in.messaging_ready && !ready_since_ms_) {
    ready_since_ms_ = in.now_ms;
  }
  if (ready_since_ms_ && in.now_ms - *ready_since_ms_ >= kNoRingAfterReadyMs) {
    return End(in, Outcome::Timeout);
  }
  View view;
  view.active = true;
  return view;
}

ShareWord CallWakePlaceholder::OutcomeWord(const Outcome outcome) {
  switch (outcome) {
    case Outcome::Ring:
      return "ring";
    case Outcome::Timeout:
      return "timeout";
    case Outcome::Blocked:
      return "blocked";
    default:
      return "expired";
  }
}

CallWakePlaceholder::View CallWakePlaceholder::End(const Input& in, const Outcome outcome) {
  View view;
  view.outcome = outcome;
  view.shown_ms = in.now_ms - start_ms_;
  if (ready_since_ms_) {
    view.ready_ms = *ready_since_ms_ - start_ms_;
  }
  handled_seq_ = active_seq_;
  active_ = false;
  ready_since_ms_.reset();
  return view;
}

} // namespace pbr
