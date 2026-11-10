#include "feature/presenters/CallWakePlaceholder.h"

namespace pbr {

CallWakePlaceholder::View CallWakePlaceholder::Step(const Input& in) {
  using Outcome = CallWakePlaceholder::Outcome;
  if (!active_) {
    if (!in.tap_fresh || in.tap_seq == handled_seq_) {
      return {};
    }
    const bool stale = in.tap_notification_age_ms && *in.tap_notification_age_ms > kMaxNotificationAgeMs;
    if (in.screen_kind != CallScreen::Kind::None || stale) {
      handled_seq_ = in.tap_seq;
      return {};
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
  if (in.dismissed) {
    return End(in, Outcome::Dismissed);
  }
  if (!in.tap_fresh) {
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
