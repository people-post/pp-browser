#include "feature/presenters/CallSystemUiBridge.h"

#include "common/Diag.h"
#include "common/Utilities.h"

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
  callbacks.pending_gone = [this]() { OnPendingGone(); };
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

bool CallSystemUiBridge::ChoiceFits(const std::string& call_id) const {
  return choice_call_id_.empty() || choice_call_id_ == call_id;
}

void CallSystemUiBridge::RecordChoice(const bool answer, const std::string& call_id) {
  if (answer) {
    Note("answer_pending");
  } else {
    Note("decline_pending");
  }
  answer_pending_at_ms_ = answer ? Now() : 0;
  decline_pending_at_ms_ = answer ? 0 : Now();
  choice_call_id_ = call_id;
}

void CallSystemUiBridge::ClearChoice() {
  answer_pending_at_ms_ = 0;
  decline_pending_at_ms_ = 0;
  choice_call_id_.clear();
}

void CallSystemUiBridge::ApplyDecline() {
  Note("decline_applied");
  if (actions_.decline) {
    actions_.decline();
  }
}

bool CallSystemUiBridge::Ringing(const std::string& call_id) const {
  return screen_.kind == CallScreen::Kind::Ringing && screen_.ring_call_id == call_id;
}

void CallSystemUiBridge::Sync(const CallScreen& screen, const std::string& active_call_id, const bool app_foreground) {
  screen_ = screen;
  active_call_id_ = active_call_id;
  RecheckOnNewRing(screen);
  if (!ui_.Available()) {
    return;
  }

  // The call on the system screen: still there, taken by the app, or over.
  if (!shown_.empty()) {
    const bool ringing = Ringing(shown_);
    const bool answering = screen.kind == CallScreen::Kind::Answering && screen.ring_call_id == shown_;
    const bool in_call = active_call_id == shown_;
    if (shown_answered_ && (answering || in_call)) {
      shown_taken_ = true;
    }
    if (!ringing && !answering && !in_call) {
      if (StillTaking()) {
        Note("answer_settling");
        return;
      }
      ui_.ReportEnded(shown_, ICallSystemUi::EndReason::Ended);
      shown_.clear();
    } else if (!ringing && !shown_answered_) {
      // Answered in the app: its own call screen shows the call.
      ui_.ReportEnded(shown_, ICallSystemUi::EndReason::AnsweredElsewhere);
      shown_.clear();
    }
  }

  // The app is already in a call no ring announced (a join answered on its own): the push's call is an orphan.
  if (shown_.empty() && ui_.HasPendingPushCall() &&
      (screen.kind == CallScreen::Kind::Answering || screen.kind == CallScreen::Kind::InCall)) {
    Note("pending_orphan");
    ui_.EndPendingPushCall(ICallSystemUi::EndReason::AnsweredElsewhere);
  }

  const bool ring = screen.kind == CallScreen::Kind::Ringing && !screen.ring_call_id.empty();
  // The push named this call and the user declined it there: it goes wherever it would ring (the app may be open
  // by now, the push's call gone with the decline).
  if (ring && !choice_call_id_.empty() && choice_call_id_ == screen.ring_call_id &&
      ChoiceLive(decline_pending_at_ms_)) {
    ClearChoice();
    ApplyDecline();
    return;
  }

  if (ring && screen.conflict) {
    LeaveRingToApp(screen.ring_call_id);
    return;
  }

  // A ring for the system screen.
  if (ring && TakesRing(app_foreground, screen.conflict)) {
    const bool fits = ChoiceFits(screen.ring_call_id);
    const bool declined = fits && ChoiceLive(decline_pending_at_ms_);
    const bool answered = fits && ChoiceLive(answer_pending_at_ms_);
    Note("ring_found");
    if (fits) {
      ClearChoice();
    } else if (ChoiceLive(answer_pending_at_ms_) || ChoiceLive(decline_pending_at_ms_)) {
      Note("choice_other_call"); // made on the call the push named: kept for its ring
    }
    if (declined) {
      // The user declined the push's call: the ring it announced goes too.
      ApplyDecline();
      return;
    }
    const std::string name = actions_.display_name ? actions_.display_name(screen.caller_identity) : std::string();
    ui_.ReportIncoming(screen.ring_call_id, name, screen.ring_video);
    shown_ = screen.ring_call_id;
    shown_answered_ = false;
    shown_taken_ = false;
    if (answered) {
      Note("accept_applied");
      AcceptShown(!screen.ring_video);
    }
  }
}

void CallSystemUiBridge::RecheckOnNewRing(const CallScreen& screen) {
  if (screen.kind != CallScreen::Kind::Ringing || screen.ring_call_id.empty() ||
      screen.ring_call_id == rechecked_ring_call_id_) {
    return;
  }
  rechecked_ring_call_id_ = screen.ring_call_id;
  ui_.RecheckAvailable();
}

void CallSystemUiBridge::LeaveRingToApp(const std::string& ring_call_id) {
  const bool fits = ChoiceFits(ring_call_id);
  const bool declined = fits && ChoiceLive(decline_pending_at_ms_);
  if (fits && ChoiceLive(answer_pending_at_ms_)) {
    // Answered on the push's call before the ring showed: the user did not know it ends the call they are in.
    Note("answer_over_call");
    ClearChoice();
  } else if (declined) {
    ClearChoice();
  }
  if (ui_.HasPendingPushCall()) {
    Note("pending_over_call");
    ui_.EndPendingPushCall(ICallSystemUi::EndReason::AnsweredElsewhere); // the app's own ring takes it
  }
  if (declined) {
    ApplyDecline(); // declining it ends nothing
  }
}

void CallSystemUiBridge::AcceptShown(const bool voice_only) {
  shown_answered_ = true;
  shown_taken_ = false;
  answered_at_ms_ = Now();
  if (actions_.accept) {
    actions_.accept(shown_, voice_only); // redraws at once: Sync re-enters (StillTaking keeps the call)
  }
}

bool CallSystemUiBridge::StillTaking() const {
  return shown_answered_ && !shown_taken_ && Now() - answered_at_ms_ < kAnswerSettleMs;
}

bool CallSystemUiBridge::TakesRing(const bool app_foreground, const bool over_call) const {
  return ui_.Available() && shown_.empty() && !over_call && (!app_foreground || ui_.HasPendingPushCall());
}

bool CallSystemUiBridge::SystemRings(const std::string& call_id) const {
  return !call_id.empty() && shown_ == call_id && !shown_answered_;
}

void CallSystemUiBridge::OnMessagingReady() {
  ready_at_ms_ = Now();
  Note("messaging_ready");
  ui_.NotifyMessagingReady();
}

void CallSystemUiBridge::OnCallOver(const std::string& call_id) {
  if (call_id.empty() || !ui_.Available() || !ui_.HasPendingPushCall() || ui_.PendingPushCallId() != call_id) {
    return; // a push that named no call waits for the first ring: an over call says nothing about it
  }
  Note("pending_call_over");
  ui_.EndPendingPushCall(ICallSystemUi::EndReason::Ended); // pending_gone drops a choice made on it
}

void CallSystemUiBridge::OnPendingGone() {
  Note("pending_gone");
  ClearChoice();
}

void CallSystemUiBridge::OnAnswer(const std::string& call_id) {
  if (call_id.empty() || (call_id != shown_ && call_id != active_call_id_)) {
    // The push's call (named by its hint, or not): answered once its ring shows.
    RecordChoice(/*answer=*/true, call_id);
    return;
  }
  if (call_id != shown_ || !Ringing(call_id)) {
    return;
  }
  Note("answer_ring");
  AcceptShown(!screen_.ring_video);
}

void CallSystemUiBridge::OnEnd(const std::string& call_id) {
  if (call_id.empty() || (call_id != shown_ && call_id != active_call_id_)) {
    RecordChoice(/*answer=*/false, call_id); // the push's call: its ring is declined once it shows
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
