#include "feature/presenters/CallSystemUiBridge.h"

#include "common/Diag.h"
#include "common/Utilities.h"
#include "lib/media/AudioSession.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

void Note(const ShareWord what) {
  DiagLine("call.system").Add("what", what).Emit();
}

} // namespace

CallSystemUiBridge::CallSystemUiBridge(ICallSystemUi& ui, Actions actions) : ui_(ui), actions_(std::move(actions)) {
  ICallSystemUi::Callbacks callbacks;
  callbacks.answer = [this](const std::string& call_id) { OnAnswer(call_id); };
  callbacks.end = [this](const std::string& call_id) { OnEnd(call_id); };
  callbacks.set_muted = [this](const std::string& call_id, const bool muted) { OnSetMuted(call_id, muted); };
  callbacks.audio_session = [this](const bool active) { OnAudioSession(active); };
  ui_.SetCallbacks(std::move(callbacks));
}

CallSystemUiBridge::~CallSystemUiBridge() {
  ui_.SetCallbacks({});
}

int64_t CallSystemUiBridge::Now() const {
  return actions_.now_ms ? actions_.now_ms() : util::NowUnixMs();
}

bool CallSystemUiBridge::ChoiceLive(const int64_t at_ms) const {
  if (at_ms == 0) {
    return false;
  }
  const int64_t now = Now();
  return now - at_ms <= kPendingMaxMs && now - std::max(at_ms, ready_at_ms_) <= kPendingChoiceMs;
}

bool CallSystemUiBridge::Ringing(const std::string& call_id) const {
  return screen_.kind == CallScreen::Kind::Ringing && screen_.ring_call_id == call_id;
}

void CallSystemUiBridge::Sync(const CallScreen& screen, const std::string& active_call_id, const bool app_foreground) {
  screen_ = screen;
  active_call_id_ = active_call_id;
  if (!ui_.Available()) {
    return;
  }

  // The call on the system screen: still there, taken by the app, or over.
  if (!shown_.empty()) {
    const bool ringing = Ringing(shown_);
    const bool answering = screen.kind == CallScreen::Kind::Answering && screen.ring_call_id == shown_;
    const bool in_call = active_call_id == shown_;
    if (!ringing && !answering && !in_call) {
      ui_.ReportEnded(shown_, ICallSystemUi::EndReason::Ended);
      shown_.clear();
    } else if (!ringing && !shown_answered_) {
      // Answered in the app: its own call screen shows the call.
      ui_.ReportEnded(shown_, ICallSystemUi::EndReason::AnsweredElsewhere);
      shown_.clear();
    }
  }

  // A ring for the system screen.
  if (screen.kind == CallScreen::Kind::Ringing && !screen.ring_call_id.empty() && TakesRing(app_foreground)) {
    const bool declined = ChoiceLive(decline_pending_at_ms_);
    const bool answered = ChoiceLive(answer_pending_at_ms_);
    Note("ring_found");
    decline_pending_at_ms_ = 0;
    answer_pending_at_ms_ = 0;
    if (declined) {
      // The user declined the push's call: the ring it announced goes too.
      Note("decline_applied");
      if (actions_.decline) {
        actions_.decline();
      }
      return;
    }
    const std::string name = actions_.display_name ? actions_.display_name(screen.caller_identity) : std::string();
    ui_.ReportIncoming(screen.ring_call_id, name, screen.ring_video);
    shown_ = screen.ring_call_id;
    shown_answered_ = false;
    if (answered) {
      shown_answered_ = true;
      Note("accept_applied");
      if (actions_.accept) {
        actions_.accept(!screen.ring_video);
      }
    }
  }
}

bool CallSystemUiBridge::TakesRing(const bool app_foreground) const {
  return ui_.Available() && shown_.empty() && (!app_foreground || ui_.HasPendingPushCall());
}

bool CallSystemUiBridge::SystemRings(const std::string& call_id) const {
  return !call_id.empty() && shown_ == call_id && !shown_answered_;
}

void CallSystemUiBridge::OnMessagingReady() {
  ready_at_ms_ = Now();
  Note("messaging_ready");
  ui_.NotifyMessagingReady();
}

void CallSystemUiBridge::OnAudioSession(const bool active) {
  AudioSession::NotifySystemActivation(active);
  if (active) {
    Note("audio_on");
  } else {
    Note("audio_off");
    AudioSession::SetSystemManaged(false);
  }
}

void CallSystemUiBridge::OnAnswer(const std::string& call_id) {
  if (call_id.empty()) {
    Note("answer_pending");
    AudioSession::SetSystemManaged(true);
    answer_pending_at_ms_ = Now();
    decline_pending_at_ms_ = 0;
    return;
  }
  if (call_id != shown_ || !Ringing(call_id)) {
    return;
  }
  Note("answer_ring");
  AudioSession::SetSystemManaged(true);
  shown_answered_ = true;
  if (actions_.accept) {
    actions_.accept(!screen_.ring_video);
  }
}

void CallSystemUiBridge::OnEnd(const std::string& call_id) {
  if (call_id.empty()) {
    Note("decline_pending");
    decline_pending_at_ms_ = Now();
    answer_pending_at_ms_ = 0;
    return;
  }
  if (call_id != shown_) {
    return;
  }
  shown_.clear();  // the system screen already ended it
  if (Ringing(call_id)) {
    if (actions_.decline) {
      actions_.decline();
    }
  } else if (active_call_id_ == call_id || screen_.ring_call_id == call_id) {
    if (actions_.leave) {
      actions_.leave();
    }
  }
}

void CallSystemUiBridge::OnSetMuted(const std::string& call_id, const bool muted) {
  if (call_id.empty() || call_id != active_call_id_ || screen_.kind != CallScreen::Kind::InCall) {
    return;
  }
  if (screen_.muted != muted && actions_.toggle_mute) {
    actions_.toggle_mute();
  }
}

} // namespace pbr
