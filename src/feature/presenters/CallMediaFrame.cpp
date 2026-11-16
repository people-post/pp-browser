#include "feature/presenters/CallMediaFrame.h"

#include <algorithm>

namespace pbr {

namespace {

using Status = CallScreen::Status;
using LineKind = CallStatusLine::Kind;

CallStatusLine Words(const char* key) {
  CallStatusLine line;
  line.key = key;
  return line;
}

/** The call timer from `since_ms`; `idle_key` while there is none. */
CallStatusLine Timer(const int64_t since_ms, const char* idle_key) {
  CallStatusLine line;
  line.kind = LineKind::Timer;
  line.key = idle_key;
  line.since_ms = since_ms;
  return line;
}

/** The line the screen's status gives (CallPresenter decided it: DecideInCallStatus). */
CallStatusLine ScreenStatusLine(const CallScreen& screen) {
  switch (screen.status) {
    case Status::CouldntConnect:
      return Words("call.status.couldnt_connect");
    case Status::Activity: {
      CallStatusLine line;
      line.kind = LineKind::Activity; // setting up / finding a path; a live call's switch is Reconnecting
      return line;
    }
    case Status::Reconnecting:
      // k4: the call lost its path and waits for a new one; the timer keeps running (greyed; the capsule says so).
      return Timer(screen.connected_at_ms, "");
    case Status::WaitingForOthers: {
      // V053: everyone else left; counts down to leaving by itself.
      CallStatusLine line = Words("call.status.waiting_for_others");
      line.kind = LineKind::Countdown;
      line.until_ms = screen.alone_until_ms;
      return line;
    }
    case Status::Disconnected:
      return Words("call.status.disconnected");
    case Status::SendingOnly:
      // The quality hint line says "Connecting voice…" / "Your mic isn't sending"; the status line keeps the timer.
    case Status::Connected:
      return Timer(screen.connected_at_ms, "call.status.connected");
    case Status::Calling:
      return Words("call.status.calling");
    case Status::Connecting:
      return Words("call.status.connecting");
    case Status::SettingUpGroup:
      return Words("call.status.setting_up_group");
    case Status::WaitingForMediaPath:
      return Words("call.status.waiting_for_media_path");
  }
  return {};
}

/** The media is broken in a way the line says (TX-only, one-way audio, no audio) — the quality hint aside. */
bool MediaBroken(const CallMediaFacts& facts, const CallMediaHealthView& health) {
  return health.quality == CallPathQuality::NoAudio || health.asymmetry == CallAudioAsymmetry::SendingOnly ||
         health.asymmetry == CallAudioAsymmetry::ReceivingOnly || facts.media_status == CallMediaStatus::DegradedTxOnly;
}

/**
 * The status line, in one place (#181). The screen's status first; what the media says now wins over it (couldn't
 * connect, a lost or stalled line, live media's timer); a group's live line gives way to its head count; a broken
 * line says so when no hint line does; the desktop's path word goes after it; on hold, the line says why.
 */
CallStatusLine DecideStatusLine(const CallScreen& screen, const CallMediaFacts& facts, const CallMediaDecision& d,
                                const CallMediaHealthView& health, const std::string& hint_key,
                                int64_t& timer_since_ms) {
  CallStatusLine line = ScreenStatusLine(screen);
  timer_since_ms = screen.connected_at_ms;
  // The call timer counts the call (first live), not the current path: it runs on through a reconnect.
  bool media_line = false;
  if (d.couldnt_connect) {
    line = Words("call.status.couldnt_connect");
    media_line = true;
  } else if (d.path_lost || (d.reconnect && d.media_dropped) || d.remote_stalling) {
    // The path is gone, the media stopped while the call runs, or the remote video stalled: the timer, greyed.
    timer_since_ms = facts.live_since_ms;
    line = Timer(timer_since_ms, "");
    media_line = true;
  } else if (facts.media_chrome_live) {
    timer_since_ms = facts.live_since_ms;
    line = Timer(timer_since_ms, "call.status.connected");
  }
  if (!media_line && screen.group && ShowsParticipantCount(screen)) {
    line = Words("call.participants_elapsed");
    line.kind = LineKind::Participants;
    line.count = static_cast<int>(screen.members.size());
    line.since_ms = timer_since_ms;
  }
  // Seat Live / Status Live can still be TX-only (NAT one-way): no Connected claim. Reconnecting says it all. The
  // quality hint line carries the sentence when there is one (#181, not the same text twice); else the label.
  if (MediaBroken(facts, health) && facts.media_status != CallMediaStatus::Reconnecting && hint_key.empty()) {
    if (const char* label = CallPathQualityLabelKey(health.quality); label && label[0]) {
      line = Words(label);
    }
  }
  if (facts.media_status != CallMediaStatus::Reconnecting &&
      (facts.media_chrome_live || facts.media_status == CallMediaStatus::DegradedTxOnly)) {
    line.path_key = CallPathWordKey(health.path_kind);
  }
  if (d.on_hold && !d.couldnt_connect && !d.path_lost) {
    // Silence by design, not a broken path: the line says why (V060).
    if (d.local_hold) {
      // A hold that does not end: the one sentence says what helps (CallHoldStuck decides it, once).
      line = Words(!facts.hold_stuck ? "call.status.on_hold_local"
                   : screen.group    ? "call.status.hold_stuck_group"
                                     : "call.status.hold_stuck_peer");
      line.warning = facts.hold_stuck;
    } else {
      line = Words("call.status.on_hold_peer_named");
      line.kind = LineKind::PeerOnHold;
      line.fallback_key = "call.status.on_hold_peer";
    }
  }
  // Their audio arrives but nothing plays: the line says so in place of the timer, for as long as it lasts (a voice call
  // at the ear has the screen black, the pill is gone by then). Holds and failures keep their own line.
  if (facts.playout_stuck && (line.kind == LineKind::Timer || line.kind == LineKind::Participants) && !d.on_hold &&
      !d.couldnt_connect && !d.path_lost && !d.reconnect) {
    line = Words(screen.group ? "call.status.cant_hear_group" : "call.status.cant_hear_peer");
    line.warning = true;
  }
  line.hint = screen.status == Status::CouldntConnect ? screen.hint : CallScreen::Hint::None;
  return line;
}

CallStagePlaceholder DecidePlaceholder(const CallMediaFacts& facts, const CallMediaDecision& d) {
  if (d.remote_video) {
    return CallStagePlaceholder::None;
  }
  if (d.starting_video) {
    return CallStagePlaceholder::StartingVideo;
  }
  if (facts.have_peer_row && facts.peer_on_hold) {
    // The "stepped away" pill and the status line say it; a third copy in the stage centre is dropped (#181).
    return CallStagePlaceholder::None;
  }
  if (facts.have_peer_row && !d.peer_camera_on) {
    return facts.peer_video_paused ? CallStagePlaceholder::CameraPaused : CallStagePlaceholder::CameraOff;
  }
  return CallStagePlaceholder::None;
}

} // namespace

CallHintPrompt DecideCallHintPrompt(const CallHintPromptInput& in, const std::map<std::string, int64_t>& last_pill_ms) {
  CallHintPrompt prompt;
  if (in.group) {
    prompt.center_key = in.hint_shown ? in.hint_key : ""; // a group call's page as before
    // Their audio arrives but nothing plays: the only pill a group call raises (not over a lost mic, steps, a hold or a
    // reconnect), the same at most once in 30 s.
    if (in.playout_stuck && !in.capture_lost && !in.steps_shown && !in.on_hold && !in.quiet) {
      const std::string key = "call.quality.hint.cant_hear_group";
      if (const auto last = last_pill_ms.find(key);
          last == last_pill_ms.end() || in.now_ms - last->second >= kHintPillEveryMs) {
        prompt.pill_key = key;
        prompt.pill_alarm = true;
      }
    }
    return prompt;
  }
  // A 1:1 call: the steps say how the connecting goes (what "Connecting voice…" repeated); a lost or silent mic means
  // something only once connected. On hold the silence is by design.
  if (in.steps_shown || in.on_hold || in.quiet) {
    return prompt;
  }
  std::string key;
  bool alarm = false;
  if (in.capture_lost) {
    key = "call.quality.hint.mic_lost"; // the engine could not get the mic back: they hear nothing until a restart
    alarm = true;
  } else if (in.playout_stuck) {
    key = "call.quality.hint.cant_hear_peer"; // their packets arrive, nothing plays: only a new call helps (#605)
    alarm = true;
  } else if (in.asymmetry == CallAudioAsymmetry::ReceivingOnly && in.not_sending_ms >= kOneWayPillAfterMs) {
    // Why this device sends nothing, when it is known; else the same advice as a lost mic.
    key = in.mic_blocked ? "hints.mic_blocked" : "call.quality.hint.not_sending_restart";
    alarm = true;
  } else if (in.asymmetry == CallAudioAsymmetry::SendingOnly && in.one_way_ms >= kOneWayPillAfterMs) {
    const bool retrying = in.one_way_ms > CallMediaHintClock::kRetryAfterMs;
    key = retrying ? (in.video ? "call.quality.hint.retrying_video" : "call.quality.hint.retrying_voice")
                   : (in.video ? "call.quality.hint.connecting_video" : "call.quality.hint.connecting_voice");
  }
  if (key.empty()) {
    return prompt;
  }
  if (const auto last = last_pill_ms.find(key);
      last != last_pill_ms.end() && in.now_ms - last->second < kHintPillEveryMs) {
    return prompt;
  }
  prompt.pill_key = key;
  prompt.pill_alarm = alarm;
  return prompt;
}

void CallMediaFrame::Reset() {
  *this = CallMediaFrame{};
}

bool CallMediaFrame::StartingVideo(const CallMediaFacts& facts) {
  // Kenneth 2026-09-29: after Accept the phone showed the audio layout for 2–4 s while both cameras opened, so the
  // tap looked like it had not registered. A video call shows the video stage from Accept (callee) or connect (caller
  // — not while still ringing out), with "Starting video…" until the peer's first frame (not "Camera off": its roster
  // says off until its camera opens), for at most kVideoStartMs after connect.
  if (!facts.video_intent || facts.ever_had_remote_video || facts.ringing_out || !facts.video_allowed.value_or(false)) {
    return false;
  }
  if (facts.media_chrome_live && video_start_since_ms_ == 0) {
    video_start_since_ms_ = facts.now_ms;
  }
  return !facts.media_chrome_live || facts.now_ms - video_start_since_ms_ < kVideoStartMs;
}

CallMediaDecision CallMediaFrame::Decide(const CallScreen& screen, const CallMediaFacts& facts) {
  if (facts.call_id != call_id_) {
    call_id_ = facts.call_id;
    video_start_since_ms_ = 0;
    camera_narrowed_ = false;
    link_counters_ = {};
    link_health_ = {};
    link_sampled_ms_ = 0;
    steps_gone_ms_ = 0;
    one_way_since_ms_ = 0;
    not_sending_since_ms_ = 0;
    last_pill_ms_.clear();
  }
  CallMediaDecision d;
  // Whatever VideoAllowedForCall said (false, unknown), a sending camera can always be turned off.
  const bool video_allowed = facts.video_allowed.value_or(false);
  d.show_camera = ShowsCameraButton(facts.encoder_available, video_allowed && facts.path_allows_video, facts.camera_on);
  if (video_allowed) {
    camera_narrowed_ = false;
  } else if (facts.video_allowed.has_value() && facts.camera_on && !camera_narrowed_) {
    // M4: only an explicit false narrows (a read error must not touch the camera). V035: a voice-only Accept (ours
    // or the peer's) after the local camera was on — e.g. the caller turned it on before the callee answered voice.
    camera_narrowed_ = true;
    d.narrow_camera = true;
  }

  // Frames beat the roster (B58, device test 2026-09-29: the phone decoded the Mac's video for a whole call and
  // showed none of it, its roster row still saying "camera off"). A camera that really turns off stops sending.
  d.peer_camera_on = facts.have_peer_row && (facts.peer_camera_on || facts.peer_frames_live);
  d.clear_remote_video = facts.have_peer_row && !d.peer_camera_on;
  const bool has_remote = facts.has_remote_video && !d.clear_remote_video;
  d.remote_stalling = facts.remote_video_stalling && !d.clear_remote_video;
  const bool expect_remote = !facts.have_peer_row || d.peer_camera_on;
  d.video_missing = facts.ever_had_remote_video && expect_remote && !has_remote;
  // Live or soft-stall: the last frame keeps painting. A hard stall / camera off clears it.
  d.remote_video = has_remote && expect_remote;
  d.starting_video = StartingVideo(facts);

  // A call that was live and then failed reads "disconnected" (CallScreen decides); "couldn't connect" is for calls
  // that never connected.
  d.couldnt_connect = facts.direct_connect_failed && !facts.awaiting_sfu_recovery && !facts.on_hop &&
                      screen.status != Status::Disconnected;
  d.path_lost = facts.media_status == CallMediaStatus::Reconnecting;
  d.media_dropped = facts.media_active && !facts.media_chrome_live && facts.live_since_ms > 0;
  d.local_hold = facts.local_on_hold;
  d.on_hold = facts.local_on_hold || (facts.have_peer_row && facts.peer_on_hold);
  // On hold (here or there) the audio is silence by design: only a lost path is a reconnect.
  d.reconnect =
      !d.couldnt_connect && (d.path_lost || (!d.on_hold && (d.remote_stalling || d.video_missing || d.media_dropped)));

  // A reconnect keeps the stage only when video was part of the call; a voice call's reconnect must not open an empty
  // "camera off" stage (Kenneth 2026-10-23).
  const bool video_in_play = facts.camera_on || facts.ever_had_remote_video || d.peer_camera_on;
  d.stage_visible = facts.camera_on || d.remote_video || d.peer_camera_on || (d.reconnect && video_in_play) ||
                    d.starting_video || (facts.have_peer_row && facts.peer_video_paused);
  d.placeholder = DecidePlaceholder(facts, d);
  return d;
}

CallLinkHealth CallMediaFrame::SampleLink(const CallMediaFacts& facts) {
  if (link_sampled_ms_ != 0 && facts.now_ms - link_sampled_ms_ < kLinkSampleMs) {
    return link_health_;
  }
  link_sampled_ms_ = facts.now_ms;
  const CallLinkHealth next = CallLinkHealthBetween(link_counters_, facts.link_counters);
  // Keep the last resend figure while a quiet interval (no Reliable sends) has none of its own.
  const double resend_pct = next.resend_pct >= 0.0 ? next.resend_pct : link_health_.resend_pct;
  link_health_ = next;
  link_health_.resend_pct = next.available ? resend_pct : -1.0;
  link_counters_ = facts.link_counters;
  return link_health_;
}

const CallMediaScreen& CallMediaFrame::Step(const CallScreen& screen, const CallMediaFacts& facts) {
  const CallMediaDecision d = Decide(screen, facts);
  CallMediaHealthInput in;
  in.engine = facts.engine;
  in.hop = facts.hop;
  in.reach_path_kind = facts.reach_path_kind;
  in.now_ms = facts.now_ms;
  in.reconnecting = d.reconnect || d.couldnt_connect;
  in.link = SampleLink(facts);
  const CallMediaHealthView health = EvaluateCallMediaHealth(in);
  return Finish(screen, facts, d, health);
}

const CallMediaScreen& CallMediaFrame::Step(const CallScreen& screen, const CallMediaFacts& facts,
                                            const CallMediaHealthView& health) {
  const CallMediaDecision d = Decide(screen, facts);
  return Finish(screen, facts, d, health);
}

const CallMediaScreen& CallMediaFrame::Finish(const CallScreen& screen, const CallMediaFacts& facts,
                                              const CallMediaDecision& d, const CallMediaHealthView& health) {
  view_.call_id = facts.call_id;
  view_.now_ms = facts.now_ms;
  view_.in_call = true;
  view_.media = d;
  view_.health = health;
  hint_clock_.Update(facts.call_id, health.asymmetry == CallAudioAsymmetry::SendingOnly, facts.now_ms);
  const char* hint_key = hint_clock_.KeyFor(health.asymmetry, screen.video);
  // The engine could not get the mic back even by rebuilding its audio devices: the other side hears silence until
  // captured audio returns (which clears it). Before any other hint: none of them is the cause then.
  view_.hint_key = facts.engine.capture_lost ? "call.quality.hint.mic_lost" : (hint_key ? hint_key : "");
  view_.line = DecideStatusLine(screen, facts, d, health, view_.hint_key, view_.timer_since_ms);
  // On hold the silence is by design: no "no audio" hint (unless the path is lost).
  const bool held = d.on_hold && !d.couldnt_connect && !d.path_lost;
  view_.hint_shown = !view_.hint_key.empty() && !held;

  CallStoryInput story;
  story.call_id = facts.call_id;
  story.kind = CallScreen::Kind::InCall;
  story.status = screen.status;
  story.group = screen.group;
  story.placed_here = screen.placed_here;
  story.media_connected = screen.media_connected;
  story.path_kind = health.path_kind;
  story.quality = health.quality;
  story.quality_bars = health.quality_bars;
  story.now_ms = facts.now_ms;
  StepStory(story, screen.status == Status::Reconnecting || d.reconnect);

  // What the page says of the hint (DEV/PLAN-call-failed-redial.md 9-10): one-way audio counts from when the steps went.
  const int64_t now = facts.now_ms;
  const bool steps = view_.story.connecting;
  if (steps) {
    steps_gone_ms_ = 0;
  } else if (steps_gone_ms_ == 0) {
    steps_gone_ms_ = now;
  }
  const bool sending_only = health.asymmetry == CallAudioAsymmetry::SendingOnly;
  if (!sending_only) {
    one_way_since_ms_ = 0;
  } else if (one_way_since_ms_ == 0) {
    one_way_since_ms_ = now;
  }
  const bool not_sending = health.asymmetry == CallAudioAsymmetry::ReceivingOnly;
  if (!not_sending) {
    not_sending_since_ms_ = 0;
  } else if (not_sending_since_ms_ == 0) {
    not_sending_since_ms_ = now;
  }
  CallHintPromptInput hint;
  hint.group = screen.group;
  hint.video = screen.video;
  hint.steps_shown = steps;
  hint.hint_key = view_.hint_key;
  hint.hint_shown = view_.hint_shown;
  hint.asymmetry = health.asymmetry;
  hint.capture_lost = facts.engine.capture_lost;
  hint.one_way_ms = sending_only && !steps ? now - std::max(one_way_since_ms_, steps_gone_ms_) : 0;
  hint.not_sending_ms = not_sending && !steps ? now - std::max(not_sending_since_ms_, steps_gone_ms_) : 0;
  hint.quiet = d.couldnt_connect || d.reconnect || screen.status == Status::CouldntConnect ||
               screen.status == Status::Disconnected || screen.status == Status::Reconnecting;
  hint.mic_blocked = facts.mic_blocked;
  hint.playout_stuck = facts.playout_stuck;
  hint.on_hold = held;
  hint.now_ms = now;
  const CallHintPrompt prompt = DecideCallHintPrompt(hint, last_pill_ms_);
  view_.center_hint_key = prompt.center_key;
  view_.pill_key = prompt.pill_key;
  view_.pill_alarm = prompt.pill_alarm;
  view_.hint_pilled = !screen.group;
  if (!prompt.pill_key.empty()) {
    last_pill_ms_[prompt.pill_key] = now;
  }
  return view_;
}

const CallMediaScreen& CallMediaFrame::StepAnswering(const CallScreen& screen, const int64_t now_ms) {
  view_ = {};
  view_.call_id = screen.ring_call_id;
  view_.now_ms = now_ms;
  // This device answered; the media is not up yet: the story starts at "You answered" (or a group's "joined").
  CallStoryInput story;
  story.call_id = screen.ring_call_id;
  story.kind = CallScreen::Kind::Answering;
  story.status = Status::Connecting;
  story.group = screen.ring_group;
  story.path_kind = "";
  story.now_ms = now_ms;
  StepStory(story, false);
  return view_;
}

void CallMediaFrame::StepStory(const CallStoryInput& in, const bool reconnecting) {
  view_.story = story_.Step(in);
  view_.reconnect = reconnect_.Step(in.call_id, reconnecting, in.now_ms);
}

} // namespace pbr
