#include "feature/presenters/CallLocalHold.h"

#include <utility>

namespace pbr {

CallLocalHold::CallLocalHold(Ports ports) : ports_(std::move(ports)) {
  redirectLogger("CallLocalHold");
}

void CallLocalHold::Sync(const Facts& facts) {
  if (facts.call_id != call_id_) {
    BeginCall(facts.call_id);
  }
  const bool in_call = !facts.call_id.empty();
  const bool held = in_call && facts.audio_taken;
  if (held && on_hold_) {
    WatchUserUnmute();
  }
  if (held != on_hold_) {
    Transition(held, in_call, facts.foreground);
  }
  PublishIfOwed(in_call, facts.now_ms);
}

void CallLocalHold::BeginCall(const std::string& call_id) {
  call_id_ = call_id;
  ++call_gen_;
  told_on_hold_ = false; // this call has been told nothing: a hold that goes on into it is told afresh
  publishing_ = false;
  next_publish_ms_ = 0;
}

void CallLocalHold::WatchUserUnmute() {
  // SetLocalAudioMuted posts to the sessions thread: only once the hold's mute has been seen applied does an open mic
  // mean the user unmuted (by any path); before that it is just the post in flight (code review 2026-11-06).
  if (ports_.mic_muted()) {
    hold_mute_seen_applied_ = true;
  } else if (hold_mute_seen_applied_) {
    hold_mute_.UserToggled();
  }
}

void CallLocalHold::Transition(const bool held, const bool in_call, const bool foreground) {
  on_hold_ = held;
  hold_mute_seen_applied_ = false;
  next_publish_ms_ = 0; // a change is told at once
  if (held) {
    // The other call rings as a banner on iPhones, so the user still sees PP: the long pill says what PP did; the
    // status line keeps saying it meanwhile (Kenneth 2026-11-06: the pill is the one style for every notice).
    ports_.note("call.status.on_hold_local");
  }
  log().info << "call on hold=" << (held ? 1 : 0) << " (the OS " << (held ? "took" : "gave back") << " the audio)";
  // Privacy: nothing of the other call may reach this one through our mic; a mute the user made before stays.
  if (const auto mute = hold_mute_.Next(held, in_call && ports_.mic_muted()); mute && in_call) {
    ports_.set_mic_muted(*mute);
    if (!*mute) {
      ports_.note("call.status.on_hold_ended"); // the hold opened the mic again: say so
    }
  }
  // The camera rests meanwhile, as in the background; still in the background at the end, OnAppForeground brings it.
  if (held && !camera_paused_ && ports_.camera_on()) {
    camera_paused_ = true;
    ports_.set_camera_paused(true);
  } else if (!held && camera_paused_ && foreground) {
    camera_paused_ = false;
    ports_.set_camera_paused(false);
  }
}

void CallLocalHold::PublishIfOwed(const bool in_call, const int64_t now_ms) {
  if (!in_call || publishing_ || told_on_hold_ == on_hold_ || now_ms < next_publish_ms_) {
    return;
  }
  publishing_ = true;
  next_publish_ms_ = now_ms + kPublishRetryMs;
  const bool on_hold = on_hold_;
  ports_.publish_hold(on_hold, [this, weak = std::weak_ptr<int>(alive_), gen = call_gen_, on_hold](Roe<void> told) {
    if (!weak.lock() || gen != call_gen_) {
      return; // gone, or about another call
    }
    publishing_ = false;
    if (told) {
      told_on_hold_ = on_hold;
    } else {
      // No call media yet (ringing out): the next tick past the retry time tells it again.
      log().debug << "call on hold=" << (on_hold ? 1 : 0) << " not told yet: " << told.error().message;
    }
  });
}

void CallLocalHold::OnAppBackground() {
  if (!ports_.camera_on()) {
    return;
  }
  camera_paused_ = true;
  log().info << "camera paused: app in the background";
  ports_.set_camera_paused(true);
}

void CallLocalHold::OnAppForeground() {
  if (!camera_paused_ || on_hold_) { // on hold: the camera comes back when the hold ends
    return;
  }
  camera_paused_ = false;
  log().info << "camera back: app in front";
  ports_.set_camera_paused(false);
}

void CallLocalHold::OnMuteTapped() {
  if (on_hold_) {
    hold_mute_.UserToggled();
  }
}

bool CallLocalHold::TakeCameraTap() {
  if (!on_hold_ || !camera_paused_) {
    return false;
  }
  // The hold only paused the camera (it still counts as on): a tap brings it back now, and the user keeps it from here.
  camera_paused_ = false;
  ports_.set_camera_paused(false);
  return true;
}

} // namespace pbr
