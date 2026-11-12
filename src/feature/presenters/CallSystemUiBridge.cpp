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

void CallSystemUiBridge::RecordChoice(const bool answer) {
  if (answer) {
    Note("answer_pending");
  } else {
    Note("decline_pending");
  }
  answer_pending_at_ms_ = answer ? Now() : 0;
  decline_pending_at_ms_ = answer ? 0 : Now();
}

void CallSystemUiBridge::ClearChoice() {
  answer_pending_at_ms_ = 0;
  decline_pending_at_ms_ = 0;
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
      ui_.ReportEnded(shown_);
      shown_.clear();
    } else if (!ringing && !shown_answered_) {
      // Answered in the app: its own call screen shows the call.
      ui_.ReportEnded(shown_);
      shown_.clear();
    }
  }

  // The app is already in a call no ring announced (a join answered on its own): the push's call is an orphan.
  if (shown_.empty() && ui_.HasPendingPushCall() &&
      (screen.kind == CallScreen::Kind::Answering || screen.kind == CallScreen::Kind::InCall)) {
    Note("pending_orphan");
    ui_.EndPendingPushCall();
  }

  const bool ring = screen.kind == CallScreen::Kind::Ringing && !screen.ring_call_id.empty();
  if (ring && screen.conflict) {
    LeaveRingToApp(screen.ring_call_id);
    return;
  }

  // A ring for the system screen.
  if (ring && TakesRing(app_foreground, screen.conflict)) {
    (void)TakeRing(screen);
  } else if (ring && shown_.empty()) {
    seen_ring_call_id_ = screen.ring_call_id; // the app rings it where the user sees it
  }
}

bool CallSystemUiBridge::TakeRing(const CallScreen& screen) {
  const bool declined = ChoiceLive(decline_pending_at_ms_);
  const bool answered = ChoiceLive(answer_pending_at_ms_);
  // Only a call the user has not seen yet raises the app: once the app has rung it in front (from the start, or after
  // a hand-back) and the user left, it rings without pulling the app back up.
  const bool full_screen = screen.ring_call_id != seen_ring_call_id_;
  if (full_screen) {
    Note("ring_found");
  } else {
    Note("ring_to_system_again");
  }
  ClearChoice();
  if (declined) {
    // The user declined the push's call: the ring it announced goes too.
    ApplyDecline();
    return false;
  }
  const std::string name = actions_.display_name ? actions_.display_name(screen.caller_identity) : std::string();
  ui_.ReportIncoming(screen.ring_call_id, name, screen.ring_video, full_screen);
  seen_ring_call_id_ = screen.ring_call_id;
  shown_ = screen.ring_call_id;
  shown_answered_ = false;
  shown_taken_ = false;
  visible_since_ms_.reset();
  if (answered) {
    Note("accept_applied");
    AcceptShown(!screen.ring_video);
  }
  return true;
}

void CallSystemUiBridge::Reevaluate(const bool app_foreground) {
  if (!ui_.Available()) {
    return;
  }
  if (!shown_.empty()) {
    CallRingHandBackInput in;
    in.system_rings = Ringing(shown_);
    in.answered_on_system = shown_answered_;
    in.pending_push_call = ui_.HasPendingPushCall();
    if (in.system_rings && !in.answered_on_system && !in.pending_push_call) {
      in.ui = UiNow(app_foreground); // the platform is asked only when the rest holds
    }
    if (!CallRingHandBackConditions(in)) {
      visible_since_ms_.reset();
      return;
    }
    const int64_t now = Now();
    if (!visible_since_ms_) {
      visible_since_ms_ = now;
    }
    if (CallRingHandBack(in, now - *visible_since_ms_)) {
      HandBack();
    }
    return;
  }
  visible_since_ms_.reset();
  // The app rings it (given back, or never taken) and the user left the app: the system screen rings it again.
  const bool ring = screen_.kind == CallScreen::Kind::Ringing && !screen_.ring_call_id.empty();
  if (ring && TakesRing(app_foreground, screen_.conflict) && TakeRing(screen_) && actions_.ring_moved) {
    actions_.ring_moved();
  }
}

bool CallSystemUiBridge::WantsReevaluation() const {
  return !shown_.empty() && !shown_answered_;
}

bool CallSystemUiBridge::WantsRingTimer(const bool app_rings) const {
  return ui_.Available() && (WantsReevaluation() || app_rings);
}

void CallSystemUiBridge::HandBack() {
  const std::string call_id = shown_;
  Note("ring_handed_back");
  // The app's ring page and ringtone come up first (SystemRings is false while handing_back_), so there is no silent
  // gap; the notification goes after.
  handing_back_ = true;
  if (actions_.ring_moved) {
    actions_.ring_moved();
  }
  handing_back_ = false;
  if (shown_ != call_id) {
    return; // the redraw's frame already let it go (the ring ended meanwhile)
  }
  shown_.clear();
  visible_since_ms_.reset();
  ui_.ReportEnded(call_id); // only the notification goes: it tells the call nothing (not a decline, not an end)
}

bool CallSystemUiBridge::UiVisible(const bool app_foreground) const {
  return CallRingUiVisible(UiNow(app_foreground));
}

CallRingUi CallSystemUiBridge::UiNow(const bool app_foreground) const {
  CallRingUi ui;
  ui.app_foreground = app_foreground;
  if (!app_foreground) {
    return ui; // not seen whatever the screen says: not asked
  }
  if (actions_.screen_state) {
    const std::optional<DeviceScreenState> screen = actions_.screen_state();
    ui.screen_known = screen.has_value();
    if (screen) {
      ui.screen_interactive = screen->interactive;
      ui.keyguard_locked = screen->keyguard_locked;
    }
  } else {
    ui.screen_known = true;
    ui.screen_interactive = true;
    ui.keyguard_locked = false;
  }
  ui.pin_locked = actions_.pin_locked && actions_.pin_locked();
  return ui;
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
  const bool declined = ChoiceLive(decline_pending_at_ms_);
  if (ChoiceLive(answer_pending_at_ms_)) {
    // Answered on the push's call before the ring showed: the user did not know it ends the call they are in.
    Note("answer_over_call");
    ClearChoice();
  } else if (declined) {
    ClearChoice();
  }
  if (ui_.HasPendingPushCall()) {
    Note("pending_over_call");
    ui_.EndPendingPushCall(); // the app's own ring takes it
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
  return ui_.Available() && shown_.empty() && !over_call && (ui_.HasPendingPushCall() || !UiVisible(app_foreground));
}

bool CallSystemUiBridge::SystemRings(const std::string& call_id) const {
  return !call_id.empty() && shown_ == call_id && !shown_answered_ && !handing_back_;
}

void CallSystemUiBridge::OnMessagingReady() {
  ready_at_ms_ = Now();
  Note("messaging_ready");
  ui_.NotifyMessagingReady();
}

void CallSystemUiBridge::OnPendingGone() {
  Note("pending_gone");
  ClearChoice();
}

void CallSystemUiBridge::OnAnswer(const std::string& call_id) {
  if (call_id.empty()) {
    // The push's call: answered once its ring shows.
    RecordChoice(/*answer=*/true);
    return;
  }
  if (call_id != shown_ || !Ringing(call_id)) {
    return;
  }
  Note("answer_ring");
  AcceptShown(!screen_.ring_video);
}

void CallSystemUiBridge::OnEnd(const std::string& call_id) {
  if (call_id.empty()) {
    RecordChoice(/*answer=*/false); // the push's call: its ring is declined once it shows
    return;
  }
  if (call_id != shown_) {
    return;
  }
  shown_.clear(); // the system screen already ended it
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

} // namespace pbr
