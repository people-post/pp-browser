#include "feature/presenters/CallLocalHold.h"

#include <optional>
#include <utility>

namespace pbr {

CallLocalHold::CallLocalHold(Ports ports) : ports_(std::move(ports)) {
  redirectLogger("CallLocalHold");
}

void CallLocalHold::Sync(const Facts& facts) {
  if (facts.call_id != call_id_) {
    BeginCall(facts);
  }
  const bool in_call = !facts.call_id.empty();
  const bool held = in_call && facts.audio_taken;
  if (held && on_hold_) {
    WatchUserUnmute();
  }
  if (held != on_hold_) {
    Transition(held, in_call, facts.now_ms, facts.mono_ms);
  }
  PublishIfOwed(in_call, facts.now_ms);
  if (in_call) {
    if (settle_until_ != 0 && settle_until_ != kNever && !bg_seen_ && facts.mono_ms >= settle_until_) {
      settle_until_ = 0;
      log().info << "camera settled: the app has stayed active for " << kCameraSettleMs << " ms";
    }
    ReadBack(facts.mono_ms);
    Reconcile(facts.mono_ms);
  }
}

void CallLocalHold::BeginCall(const Facts& facts) {
  call_id_ = facts.call_id;
  ++call_gen_;
  told_on_hold_ = false; // this call has been told nothing: a hold that goes on into it is told afresh
  publishing_ = false;
  next_publish_ms_ = 0;
  // The camera: nothing wished, nothing asked, no close on record. The engine is NOT read here: an automatic camera-on
  // may be in flight, and a read now would turn it into "off" (B225).
  wants_ = false;
  asked_on_ = false;
  asked_mono_ = kLongAgo;
  closed_mono_ = kLongAgo;
  paused_sent_ = false;
  hold_lifted_ = false;
  open_by_user_ = false;
  ++request_seq_;
  active_ = facts.foreground;
  bg_seen_ = !facts.visible;
  settle_until_ = (bg_seen_ || !facts.foreground) ? kNever : 0;
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

void CallLocalHold::Transition(const bool held, const bool in_call, const int64_t now_ms, const int64_t mono_ms) {
  on_hold_ = held;
  hold_lifted_ = false;
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
    // B221: the mic change and the hold reach the others as ONE roster (rosters carry no version, so two sent back to
    // back can land in either order, and the stale one would keep the peer on "away"). It rides on the hold's publish
    // when that goes out now; otherwise (one still in flight, or nothing to tell) the mic is changed at once — a mute
    // never waits.
    if (!publishing_ && told_on_hold_ != held) {
      pending_mute_ = mute;
    } else {
      ports_.set_mic_muted(*mute);
    }
    if (!*mute) {
      ports_.note("call.status.on_hold_ended"); // the hold opened the mic again: say so
    }
  }
  // Told before the camera moves: every roster that leaves after this already says the final on_hold.
  PublishIfOwed(in_call, now_ms);
  if (pending_mute_) { // the publish did not go out after all
    ports_.set_mic_muted(*std::exchange(pending_mute_, std::nullopt));
  }
  // The camera rests meanwhile and comes back at the end through the same rule as everywhere (the gate and the
  // settle still apply: in the background at the end, it waits for a real return).
  if (in_call) {
    Reconcile(mono_ms);
  }
}

void CallLocalHold::PublishIfOwed(const bool in_call, const int64_t now_ms) {
  if (!in_call || publishing_ || told_on_hold_ == on_hold_ || now_ms < next_publish_ms_) {
    return;
  }
  publishing_ = true;
  next_publish_ms_ = now_ms + kPublishRetryMs;
  const bool on_hold = on_hold_;
  const std::optional<bool> mute = std::exchange(pending_mute_, std::nullopt);
  ports_.publish_hold(
      on_hold, mute, [this, weak = std::weak_ptr<int>(alive_), gen = call_gen_, on_hold](Roe<void> told) {
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

void CallLocalHold::OnEnteredBackground(const int64_t mono_ms) {
  if (settle_until_ != 0 && settle_until_ != kNever) {
    log().info << "camera resume cancelled: back-to-background";
  }
  bg_seen_ = true;
  settle_until_ = kNever;
  Reconcile(mono_ms);
}

void CallLocalHold::OnWillEnterForeground() {
  bg_seen_ = false;
}

void CallLocalHold::OnInactive() {
  active_ = false;
  if (settle_until_ != 0 && settle_until_ != kNever) {
    log().info << "camera resume postponed: inactive before the settle ended";
    settle_until_ = kNever;
  }
  // Nothing else moves: an inactive app keeps its camera.
}

void CallLocalHold::OnActive(const int64_t mono_ms) {
  active_ = true;
  if (bg_seen_) {
    // The lock screen sends a lone "active" in the middle of going to the background: with no real return before it,
    // it must not open the camera.
    log().info << "camera resume ignored: active without foreground";
    return;
  }
  if (settle_until_ != 0) {
    settle_until_ = mono_ms + kCameraSettleMs;
    log().info << "camera resume scheduled: the app must stay active for " << kCameraSettleMs << " ms";
  }
  Reconcile(mono_ms);
}

void CallLocalHold::OnMuteTapped() {
  if (on_hold_) {
    hold_mute_.UserToggled();
  }
}

void CallLocalHold::SetWanted(const int64_t mono_ms, const bool user) {
  wants_ = true;
  open_by_user_ = user;
  if (user && bg_seen_ && active_) {
    // The way out when the real-return signal never came: the app is active and the user is looking at it. The open
    // still waits kCameraSettleMs from this tap (and from our last close).
    log().info << "camera resume: tap while active, no real return seen";
    bg_seen_ = false;
    settle_until_ = mono_ms + kCameraSettleMs;
  }
}

void CallLocalHold::OnCameraTapped(const int64_t mono_ms) {
  if (on_hold_ && !hold_lifted_) {
    // The hold only rested the camera: a tap takes the rest off and the user keeps it from here.
    hold_lifted_ = true;
    SetWanted(mono_ms, true);
  } else if (wants_) {
    wants_ = false;
  } else {
    SetWanted(mono_ms, true);
  }
  if (!call_id_.empty()) {
    Reconcile(mono_ms);
  }
}

void CallLocalHold::OnAutoCameraOn(const int64_t mono_ms) {
  SetWanted(mono_ms, false);
  if (!call_id_.empty()) {
    Reconcile(mono_ms);
  }
}

void CallLocalHold::OnCameraClosedByEngine() {
  wants_ = false;
  asked_on_ = false;
  paused_sent_ = false;
  ++request_seq_;
  // closed_mono_ stays: the engine closed it, not us, so the user may open it again at once.
}

void CallLocalHold::OnCameraClosedByUs(const int64_t mono_ms) {
  wants_ = false;
  asked_on_ = false;
  paused_sent_ = false;
  closed_mono_ = mono_ms;
  ++request_seq_;
}

void CallLocalHold::ReadBack(const int64_t mono_ms) {
  // A safety net, not the main path: only in an active, settled app (not while a permission prompt has it inactive),
  // and only once the request is long past in flight.
  if (!asked_on_ || !active_ || !Settled(mono_ms) || mono_ms - asked_mono_ < kEngineReadBackMs ||
      (ports_.engine_camera_on && ports_.engine_camera_on())) {
    return;
  }
  log().warning << "camera closed by the engine without a report: the wish follows it";
  wants_ = false;
  asked_on_ = false;
  paused_sent_ = false;
  ++request_seq_;
}

void CallLocalHold::Reconcile(const int64_t mono_ms) {
  if (call_id_.empty()) {
    return;
  }
  if (!wants_) {
    // A close is never delayed. A camera we only rested still has "paused" on the roster: clear it too.
    if (asked_on_ || paused_sent_) {
      const bool device_close = asked_on_;
      ++request_seq_;
      ports_.set_camera_enabled(false, {});
      asked_on_ = false;
      paused_sent_ = false;
      if (device_close) {
        closed_mono_ = mono_ms;
        log().info << "camera off: the user turned it off";
      }
    }
    return;
  }
  const bool want_open = !(on_hold_ && !hold_lifted_) && Settled(mono_ms);
  if (want_open && !asked_on_) {
    if (mono_ms - closed_mono_ < kCameraSettleMs) {
      return; // only opening is gated: CameraResuming() says so
    }
    const bool back = paused_sent_;
    const uint64_t seq = ++request_seq_;
    asked_on_ = true;
    asked_mono_ = mono_ms;
    paused_sent_ = false;
    log().info << (back ? "camera back: the app has settled" : "camera on");
    ports_.set_camera_enabled(
        true, [this, weak = std::weak_ptr<int>(alive_), gen = call_gen_, seq, back](Roe<void> done) {
          if (done || !weak.lock() || gen != call_gen_ || seq != request_seq_) {
            return;
          }
          log().info << "camera open refused: " << done.error().message;
          wants_ = false;
          asked_on_ = false;
          paused_sent_ = back; // the others still read "paused": the close that follows (wish off) clears it
          if (open_by_user_ && ports_.camera_open_failed) {
            ports_.camera_open_failed();
          }
        });
  } else if (!want_open && asked_on_) {
    ++request_seq_;
    const char* why = bg_seen_                      ? "app in the background"
                      : (on_hold_ && !hold_lifted_) ? "call on hold"
                                                    : "app not settled";
    log().info << "camera paused: " << why;
    ports_.pause_camera();
    asked_on_ = false;
    asked_mono_ = mono_ms;
    closed_mono_ = mono_ms;
    paused_sent_ = true;
  }
}

} // namespace pbr
