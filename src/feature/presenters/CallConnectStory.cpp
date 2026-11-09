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

CallPathKind ParseCallPathKind(const std::string& path_kind) {
  if (path_kind == "direct") {
    return CallPathKind::Direct;
  }
  if (path_kind == "punched") {
    return CallPathKind::Punched;
  }
  if (path_kind == "circuit") {
    return CallPathKind::Circuit;
  }
  if (path_kind == "media_relay" || path_kind == "relay") {
    return CallPathKind::MediaRelay;
  }
  return CallPathKind::Unknown;
}

CallRoute CallRouteFromPathKind(const std::string& path_kind) {
  switch (ParseCallPathKind(path_kind)) {
    case CallPathKind::Direct:
      return CallRoute::Direct;
    case CallPathKind::Punched:
      return CallRoute::AroundLimits;
    case CallPathKind::Circuit:
    case CallPathKind::MediaRelay:
      return CallRoute::Relayed;
    case CallPathKind::Unknown:
      break;
  }
  return CallRoute::Unknown;
}

const char* CallPathWordKey(const std::string& path_kind) {
  switch (ParseCallPathKind(path_kind)) {
    case CallPathKind::Direct:
      return "call.details.path.direct";
    case CallPathKind::Punched:
      return "call.details.path.punched";
    case CallPathKind::Circuit:
      return "call.details.path.circuit";
    case CallPathKind::MediaRelay:
      return "call.details.path.media_relay";
    case CallPathKind::Unknown:
      break;
  }
  return "";
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
    // A 1:1 list starts at "calling", or at "you answered" when this device answered.
    view_.stage = in.group ? 0 : (view_.outgoing ? CallConnectStory::kStageCalling : CallConnectStory::kStageAnswered);
    stage_began_ms_ = now;
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
      // No list to show for a call found live: it is already finished and its hold is spent.
      view_.stage = CallConnectStory::kStageDone;
      done_ms_ = now - kListHoldMs;
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

  if (in.group) {
    view_.connecting = !connected_ && StillConnecting(in.status);
    view_.stage = view_.outgoing && !answered_ ? 0 : 1;
  } else {
    // A 1:1 call's five steps (#172). The real stage runs ahead of the screen: the answer, the line being matched
    // (answer to live media) and the finished call are each real moments, but every step stays on screen at least
    // kStepMinShowMs, and the finished list kListHoldMs. Only the words wait; the call itself never does.
    const int target = connected_ ? CallConnectStory::kStageDone
                                  : (view_.outgoing && !answered_ ? CallConnectStory::kStageCalling
                                                                  : CallConnectStory::kStageMatching);
    if (view_.stage < target && now - stage_began_ms_ >= kStepMinShowMs) {
      view_.stage = view_.stage == CallConnectStory::kStageSecuring ? CallConnectStory::kStageDone : view_.stage + 1;
      stage_began_ms_ = now;
      if (view_.stage == CallConnectStory::kStageDone) {
        done_ms_ = now;
      }
    }
    if (connected_) {
      const bool ended = in.status == Status::CouldntConnect || in.status == Status::Disconnected;
      view_.connecting = !ended && (view_.stage < CallConnectStory::kStageDone || now - done_ms_ < kListHoldMs);
    } else {
      view_.connecting = StillConnecting(in.status);
    }
  }

  if (view_.result_shown) {
    // The result line starts when the list is gone (a group has no list).
    const bool list_over = in.group || view_.stage == CallConnectStory::kStageDone;
    const int64_t shown_for = list_over ? now - (in.group ? connected_ms_ : done_ms_ + kListHoldMs) : 0;
    view_.result_fading = shown_for >= kResultShowMs;
    if (shown_for >= kResultShowMs + kResultFadeMs) {
      view_.result_shown = false;
      view_.result_fading = false;
    }
  }

  if (connected_) {
    const bool reconnecting = in.status == Status::Reconnecting;
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

CallStorySteps CallStoryLines(const CallStoryView& view, const bool group) {
  CallStorySteps steps;
  if (group) {
    steps.keys = {view.stage == 0 ? "call.story.step.group_calling" : "call.story.step.group_joined",
                  "call.story.step.group_line", "call.story.step.group_heard"};
    steps.shown = view.stage;
    return steps;
  }
  // The five steps (#172): calling, answered, matching the fastest line, setting up the encrypted channel, connected.
  std::string matching = "call.story.step.matching";
  if (view.stage > CallConnectStory::kStageMatching) {
    matching = view.route == CallRoute::Relayed   ? "call.story.step.matched_relayed"
               : view.route == CallRoute::Unknown ? "call.story.step.matched"
                                                  : "call.story.step.matched_direct";
  }
  if (view.outgoing) {
    steps.keys = {"call.story.step.calling", "call.story.step.peer_answered"};
  } else {
    steps.keys = {"call.story.step.you_answered"};
  }
  steps.keys.push_back(matching);
  steps.keys.push_back("call.story.step.securing");
  steps.keys.push_back("call.story.step.connected");
  // The stage numbers the five steps; a device that answered has no "calling" line, so it counts one less.
  const int first = view.outgoing ? 0 : 1;
  steps.shown = view.stage == CallConnectStory::kStageDone ? static_cast<int>(steps.keys.size()) : view.stage - first;
  return steps;
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
