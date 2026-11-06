#include "feature/presenters/CallConnectStory.h"

namespace pbr {

namespace {

using Status = CallScreen::Status;

/** Before media comes up these lines all mean "still getting through"; the failure lines have their own screen. */
bool StillConnecting(const Status status) {
  switch (status) {
  case Status::Calling:
  case Status::Connecting:
  case Status::SettingUpGroup:
  case Status::WaitingForMediaPath:
  case Status::Activity:
    return true;
  default:
    return false;
  }
}

CallSignalTone ToneFor(const CallPathQuality quality) {
  switch (quality) {
  case CallPathQuality::Excellent:
  case CallPathQuality::Good:
    return CallSignalTone::Good;
  case CallPathQuality::Fair:
    return CallSignalTone::Fair;
  default:
    return CallSignalTone::Poor;
  }
}

} // namespace

CallRoute CallRouteFromPathKind(const std::string& path_kind) {
  if (path_kind == "direct") {
    return CallRoute::Direct;
  }
  if (path_kind == "punched") {
    return CallRoute::AroundLimits;
  }
  if (path_kind == "circuit" || path_kind == "media_relay" || path_kind == "relay") {
    return CallRoute::Relayed;
  }
  return CallRoute::Unknown;
}

void CallConnectStory::Reset() {
  *this = CallConnectStory{};
}

void CallConnectStory::Raise(const CallStoryNotice notice, const int64_t now_ms) {
  view_.notice = notice;
  view_.notice_fading = false;
  notice_ms_ = now_ms;
}

const CallStoryView& CallConnectStory::Step(const CallStoryInput& in) {
  const int64_t now = in.now_ms;
  if (in.call_id != call_id_) {
    Reset();
    call_id_ = in.call_id;
    began_ms_ = now;
    last_status_ = in.status;
    // The direction is the call's own (who started it), not a guess from the first frame: a joiner from the group's
    // join bar also first reads "Calling" while its media starts.
    view_.outgoing = in.placed_here && in.kind == CallScreen::Kind::InCall;
    if (!view_.outgoing) {
      answered_ = true;
      answered_ms_ = now;
    }
    view_.events.push_back({.kind = view_.outgoing ? CallStoryEvent::Kind::Placed : CallStoryEvent::Kind::Answered,
                            .wall_ms = now});
    if (in.media_connected) {
      // Already live at first sight (the app came back to a running call): how long it took is not known, so no
      // result line and no times relative to a start this device did not see.
      connected_ = true;
      connected_ms_ = now;
      answered_ = true;
      answered_ms_ = now;
      view_.route = CallRouteFromPathKind(in.path_kind);
      view_.events.push_back({.kind = CallStoryEvent::Kind::Connected, .route = view_.route, .wall_ms = now});
      view_.events.push_back({.kind = CallStoryEvent::Kind::Encrypted, .wall_ms = now});
    }
  }

  // The other side answered: our own call left "Calling" for a connecting line (its media started). A call that
  // fails while still ringing was not answered.
  const bool left_calling = in.status != Status::Calling && StillConnecting(in.status);
  if (view_.outgoing && !answered_ && (left_calling || in.media_connected)) {
    answered_ = true;
    answered_ms_ = now;
    if (!in.group) {
      view_.events.push_back(
          {.kind = CallStoryEvent::Kind::PeerAnswered, .at_ms = now - began_ms_, .wall_ms = now});
    }
  }

  if (!connected_ && in.media_connected) {
    connected_ = true;
    connected_ms_ = now;
    // The path is only worth reading now (before media it is empty or a default "direct").
    view_.route = CallRouteFromPathKind(in.path_kind);
    view_.events.push_back(
        {.kind = CallStoryEvent::Kind::Connected, .at_ms = now - began_ms_, .route = view_.route, .wall_ms = now});
    view_.events.push_back({.kind = CallStoryEvent::Kind::Encrypted, .at_ms = now - began_ms_, .wall_ms = now});
    if (!in.group) {
      view_.result_shown = true;
      view_.result_route = view_.route;
      const int64_t from = answered_ ? answered_ms_ : began_ms_;
      view_.connect_tenths = (now - from + 50) / 100;
    }
  }

  view_.connecting = !connected_ && StillConnecting(in.status);
  // The page shows one stage at a time, moved on by the real change only (#11). "Calling" answered within
  // kStageQuickMs is still held to kStageMinShowMs so it is seen to pass; connecting or failing ends the hold.
  const int stage = view_.outgoing && !answered_ ? 0 : 1;
  const bool hold = view_.outgoing && view_.connecting && stage > view_.stage &&
                    answered_ms_ - began_ms_ < kStageQuickMs && now - began_ms_ < kStageMinShowMs;
  if (!hold) {
    view_.stage = stage;
  }

  if (view_.result_shown) {
    const int64_t shown_for = now - connected_ms_;
    view_.result_fading = shown_for >= kResultShowMs;
    if (shown_for >= kResultShowMs + kResultFadeMs) {
      view_.result_shown = false;
      view_.result_fading = false;
    }
  }

  if (connected_) {
    const bool reconnecting = in.status == Status::Reconnecting;
    if (reconnecting && last_status_ != Status::Reconnecting) {
      Raise(CallStoryNotice::Reconnecting, now);
    }
    if (in.media_connected && !reconnecting) {
      const CallRoute route = CallRouteFromPathKind(in.path_kind);
      if (view_.route == CallRoute::Relayed && (route == CallRoute::Direct || route == CallRoute::AroundLimits)) {
        // A relayed call found a nearer way (the direct retries at 3 / 20 / 60 s) and moved onto it. The timeline
        // keeps only the latest move, however often a call flips between paths.
        std::erase_if(view_.events, [](const CallStoryEvent& e) { return e.kind == CallStoryEvent::Kind::Closer; });
        view_.events.push_back({.kind = CallStoryEvent::Kind::Closer,
                                .at_ms = now - began_ms_,
                                .call_ms = now - connected_ms_,
                                .route = route,
                                .wall_ms = now});
        Raise(CallStoryNotice::Closer, now);
      }
      if (route != CallRoute::Unknown) {
        view_.route = route;
      }
    }
    // Audio warms up for a moment after media connects (no audio yet, still settling): the bar waits for the
    // first good sample or kSettleMs, and that sample is the baseline "slower" is measured against.
    const CallSignalTone tone = ToneFor(in.quality);
    if (!settled_ && (tone == CallSignalTone::Good || now - connected_ms_ >= kSettleMs)) {
      settled_ = true;
      view_.signal_bars = in.quality_bars;
      view_.signal_quality = in.quality;
      view_.signal_tone = tone;
      last_tone_ = tone;
      signal_changed_ms_ = now;
    }
    // The bar follows the measured quality, held at least kSignalHoldMs between changes.
    const bool differs = in.quality_bars != view_.signal_bars || in.quality != view_.signal_quality;
    if (settled_ && differs && now - signal_changed_ms_ >= kSignalHoldMs) {
      view_.signal_bars = in.quality_bars;
      view_.signal_quality = in.quality;
      view_.signal_tone = tone;
      signal_changed_ms_ = now;
      // "The line got slower": good turning fair or poor while the call runs. A reconnect says so itself, and no
      // audio is not a slower line (it has its own hint: "you can't hear them").
      const bool slower = in.quality == CallPathQuality::Fair || in.quality == CallPathQuality::Poor;
      if (last_tone_ == CallSignalTone::Good && slower && !reconnecting &&
          (!slow_noticed_ || now - slow_notice_ms_ >= kSlowNoticeEveryMs)) {
        slow_noticed_ = true;
        slow_notice_ms_ = now;
        Raise(CallStoryNotice::Slow, now);
      }
      last_tone_ = tone;
    }
  }
  view_.signal_shown = connected_ && settled_ && !view_.result_shown && in.status != Status::CouldntConnect &&
                       in.status != Status::Disconnected;

  if (view_.notice != CallStoryNotice::None) {
    const int64_t shown_for = now - notice_ms_;
    view_.notice_fading = shown_for >= kNoticeShowMs;
    if (shown_for >= kNoticeShowMs + kNoticeFadeMs) {
      view_.notice = CallStoryNotice::None;
      view_.notice_fading = false;
    }
  }
  last_status_ = in.status;
  return view_;
}

const char* CallRouteResultKey(const CallRoute route) {
  switch (route) {
  case CallRoute::Direct:
    return "call.story.result.direct";
  case CallRoute::AroundLimits:
    return "call.story.result.around";
  case CallRoute::Relayed:
    return "call.story.result.relayed";
  case CallRoute::Unknown:
    break;
  }
  return "call.story.result.unknown";
}

const char* CallRouteEventKey(const CallRoute route) {
  switch (route) {
  case CallRoute::Direct:
    return "call.story.event.direct";
  case CallRoute::AroundLimits:
    return "call.story.event.around";
  case CallRoute::Relayed:
    return "call.story.event.relayed";
  case CallRoute::Unknown:
    break;
  }
  return "call.story.event.unknown";
}

const char* CallRouteHowKey(const CallRoute route) {
  switch (route) {
  case CallRoute::Direct:
    return "call.story.how.direct";
  case CallRoute::AroundLimits:
    return "call.story.how.around";
  case CallRoute::Relayed:
    return "call.story.how.relayed";
  case CallRoute::Unknown:
    break;
  }
  return "call.story.how.unknown";
}

const char* CallStoryNoticeKey(const CallStoryNotice notice) {
  switch (notice) {
  case CallStoryNotice::Slow:
    return "call.story.notice.slow";
  case CallStoryNotice::Reconnecting:
    return "call.story.notice.reconnecting";
  case CallStoryNotice::Closer:
    return "call.story.notice.closer";
  case CallStoryNotice::None:
    break;
  }
  return "";
}

const char* CallSignalWordKey(const CallPathQuality quality) {
  switch (quality) {
  case CallPathQuality::Fair:
    return "call.quality.fair";
  case CallPathQuality::Poor:
  case CallPathQuality::Reconnecting:
    return "call.quality.poor";
  case CallPathQuality::NoAudio:
    return "call.quality.no_audio";
  case CallPathQuality::Excellent:
  case CallPathQuality::Good:
    break;
  }
  return "";
}

} // namespace pbr
