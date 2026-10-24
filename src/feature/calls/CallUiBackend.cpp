#include "feature/calls/CallUiBackend.h"

#include "lib/media/CallMediaEngine.h"
#include "lib/media/CameraCaptureOrientation.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallStack.h"
#include "foundation/runtime/AppRuntime.h"

#include <stdexcept>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

Error UnavailableError() {
  return Error("Call backend unavailable");
}

std::function<void()> OnUi(std::function<void()> callback) {
  return [callback = std::move(callback)]() {
    if (!callback) {
      return;
    }
    if (AppRuntime::CurrentlyOnUI()) {
      callback();
    } else {
      AppRuntime::PostUI(callback);
    }
  };
}

} // namespace

CallUiBackend::CallUiBackend(CallStack& stack) : stack_(stack) {}

bool CallUiBackend::Available() const {
  return State()->available;
}

const void* CallUiBackend::SessionsIdentity() const {
  return State()->sessions_identity;
}

void CallUiBackend::SetOnRingChanged(std::function<void()> callback) {
  // GUI boundary: rings come from the calls owner (and receive paths); the GUI hears them on UI.
  stack_.RunOnOwner([ring = OnUi(std::move(callback))](CallSessionManager& calls) { calls.SetOnRingChanged(ring); });
}

void CallUiBackend::SetOnChromeRefresh(std::function<void()> callback) {
  stack_.SetOnChromeRefresh(std::move(callback));  // delivered on UI after the calls change
}

std::shared_ptr<const CallUiState> CallUiBackend::State() const {
  return stack_.UiState();
}

void CallUiBackend::OnOwner(std::function<void(CallSessionManager&)> op) {
  stack_.PostToSessions([op = std::move(op)](CallSessionManager* calls) {
    if (calls) {
      op(*calls);
    }
  });
}

template <typename R>
std::function<void(R)> CallUiBackend::ReplyOnUi(std::function<void(R)> on_done) {
  return [on_done = std::move(on_done)](R result) {
    if (!on_done) {
      return;
    }
    AppRuntime::PostUI([on_done, result = std::move(result)]() { on_done(result); });
  };
}

// --- intents -------------------------------------------------------------------------------------

void CallUiBackend::SweepExpiredInvites() {
  OnOwner([](CallSessionManager& calls) { calls.SweepExpiredInvites(); });
}

void CallUiBackend::PollP2pConnectHealth() {
  OnOwner([](CallSessionManager& calls) { calls.PollP2pConnectHealth(); });
}

void CallUiBackend::ClearMediaActivity() {
  OnOwner([](CallSessionManager& calls) { calls.ClearMediaActivity(); });
}

void CallUiBackend::Apply(CallLifecycleEvent ev, const std::string& call_id) {
  OnOwner([ev, call_id](CallSessionManager& calls) { calls.Apply(ev, call_id); });
}

void CallUiBackend::ClearLastError() {
  OnOwner([](CallSessionManager& calls) { calls.ClearLastError(); });
}

void CallUiBackend::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  OnOwner([call_id, reason](CallSessionManager& calls) { (void)calls.LeaveCall(call_id, reason); });
}

void CallUiBackend::StopCallMedia(const std::string& call_id) {
  OnOwner([call_id](CallSessionManager& calls) { calls.StopCallMedia(call_id); });
}

void CallUiBackend::RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity) {
  OnOwner([call_id, publisher_identity](CallSessionManager& calls) {
    (void)calls.RequestVideoRefresh(call_id, publisher_identity);
  });
}

void CallUiBackend::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  OnOwner([decision](CallSessionManager& calls) { calls.SetPendingAcceptChargeDecision(decision); });
}

void CallUiBackend::SetPendingAcceptVoiceOnly(const bool voice_only) {
  OnOwner([voice_only](CallSessionManager& calls) { calls.SetPendingAcceptVoiceOnly(voice_only); });
}

std::optional<std::string> CallUiBackend::TakeLastMediaError() {
  const auto state = State();
  if (!state->last_media_error) {
    taken_media_error_.reset();
    return std::nullopt;
  }
  if (taken_media_error_ == state->last_media_error) {
    return std::nullopt;  // shown already; the owner's clear has not published yet
  }
  taken_media_error_ = state->last_media_error;
  OnOwner([seen = *state->last_media_error](CallSessionManager& calls) { calls.ClearLastMediaErrorIf(seen); });
  return taken_media_error_;
}

std::optional<CallUiBackend::RemoteEnd> CallUiBackend::TakeRemoteEnd() {
  const auto state = State();
  if (state->remote_ended_call_id.empty() || state->remote_ended_call_id == taken_remote_ended_) {
    return std::nullopt;
  }
  taken_remote_ended_ = state->remote_ended_call_id;
  return RemoteEnd{state->remote_ended_call_id, state->remote_ended_declined};
}

void CallUiBackend::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                              const std::vector<std::string>& invitee_identities,
                              std::function<void(Roe<CallSession>)> on_done) {
  auto reply = ReplyOnUi<Roe<CallSession>>(std::move(on_done));
  stack_.PostToSessions([origin_thread_id, video_allowed, invitee_identities, reply](CallSessionManager* calls) {
    if (!calls) {
      reply(UnavailableError());
      return;
    }
    // The workflow admits the placed call (Deciding, LiveCalls::NoteOutboundStarted) itself.
    reply(calls->StartCall(origin_thread_id, video_allowed, invitee_identities));
  });
}

void CallUiBackend::InviteParticipant(const std::string& call_id, const std::string& invitee_identity,
                                      std::function<void(Roe<void>)> on_done) {
  auto reply = ReplyOnUi<Roe<void>>(std::move(on_done));
  stack_.PostToSessions([call_id, invitee_identity, reply](CallSessionManager* calls) {
    reply(calls ? calls->InviteParticipant(call_id, invitee_identity) : Roe<void>(UnavailableError()));
  });
}

void CallUiBackend::SetLocalAudioMuted(bool muted, std::function<void(Roe<void>)> on_done) {
  auto reply = ReplyOnUi<Roe<void>>(std::move(on_done));
  stack_.PostToSessions([muted, reply](CallSessionManager* calls) {
    reply(calls ? calls->SetLocalAudioMuted(muted) : Roe<void>(UnavailableError()));
  });
}

void CallUiBackend::SetLocalVideoEnabled(bool enabled, std::function<void(Roe<void>)> on_done) {
  // L012: the display rotation comes from UIKit on iOS — read it here, on UI, not on the owner.
  const int rotation = enabled ? CameraDisplayRotationDegrees() : 0;
  auto reply = ReplyOnUi<Roe<void>>(std::move(on_done));
  stack_.PostToSessions([enabled, rotation, reply](CallSessionManager* calls) {
    reply(calls ? calls->SetLocalVideoEnabled(enabled, rotation) : Roe<void>(UnavailableError()));
  });
}

CallUiBackend::Surface CallUiBackend::Settle() {
  Surface surface;
  if (!Available()) {
    return surface;
  }
  auto top = TopPendingInvite();
  auto active = ActiveLocalCall();
  if (active && active->has_value()) {
    surface.active = **active;
  }
  if (top && top->has_value()) {
    const PendingCallInvite& invite = **top;
    const bool same_call_active = surface.active && surface.active->call_id == invite.call_id;
    if (!same_call_active) {
      surface.invite = invite;
      if (ShouldSuppressRing(invite.call_id)) {
        surface.kind = Surface::Kind::Answering;  // accept in flight (CALLS.md)
        return surface;
      }
      if (Phase() == CallPhase::Idle) {
        Apply(CallLifecycleEvent::InviteSeen, invite.call_id);
      }
      surface.kind = Surface::Kind::Ringing;
      return surface;
    }
    // Same-call pending (duplicate invite / inbox race): the call, not a ring.
  }
  if (surface.active) {
    const CallSession& call = *surface.active;
    // LeaveClicked sets Idle before LeaveCall IO finishes: do not resurrect the panel from the
    // still-Active disk row (Samsung: End looked hung / "couldn't connect" stuck).
    if (Phase() == CallPhase::Idle) {
      if (call.state == CallSessionState::Active && !Media().IsActive() &&
          !MediaAttemptedThisProcess(call.call_id) && !IsAwaitingSfuRecovery()) {
        LeaveCall(call.call_id, LiveCallEndReason::Orphaned);  // a true orphan: force-quit / restart
      }
      surface.active.reset();
      return surface;
    }
    // Direct connect failed: the state callback may not have marked it yet (ordering). Never leave
    // on `failed` here: that erased the session before the user could retry.
    if (Media().IsActive() && Media().ActiveCallId() == call.call_id &&
        Media().ConnectionState() == "failed" && !IsAwaitingSfuRecovery() && !Media().IsSfuMode() &&
        !IsP2pConnectFailed()) {
      PollP2pConnectHealth();
    }
    surface.kind = Surface::Kind::InCall;
    return surface;
  }
  if (Phase() == CallPhase::Ringing) {
    Apply(CallLifecycleEvent::InviteCleared, {});
  }
  return surface;
}

// --- durable state -------------------------------------------------------------------------------

Roe<std::optional<PendingCallInvite>> CallUiBackend::TopPendingInvite() {
  if (auto* calls = stack_.Calls()) {
    return calls->PeekTopPendingInvite();
  }
  return UnavailableError();
}

std::optional<std::string> CallUiBackend::JoinableCallInThread(const std::string& thread_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->JoinableCallInThread(thread_id);
  }
  return std::nullopt;
}

Roe<std::optional<CallSession>> CallUiBackend::ActiveLocalCall() {
  if (auto* calls = stack_.Calls()) {
    return calls->ActiveLocalCall();
  }
  return UnavailableError();
}

Roe<std::optional<std::string>> CallUiBackend::PeerIdentityForCall(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->PeerIdentityForCall(call_id);
  }
  return UnavailableError();
}

Roe<std::optional<bool>> CallUiBackend::PeerVideoEnabledForCall(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->PeerVideoEnabledForCall(call_id);
  }
  return UnavailableError();
}

Roe<std::optional<bool>> CallUiBackend::VideoAllowedForCall(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->VideoAllowedForCall(call_id);
  }
  return UnavailableError();
}

Roe<bool> CallUiBackend::AwaitingExplicitAnswerForCall(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->AwaitingExplicitAnswerForCall(call_id);
  }
  return UnavailableError();
}

Roe<std::vector<CallParticipant>> CallUiBackend::ListJoinedParticipants(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->ListJoinedParticipants(call_id);
  }
  return UnavailableError();
}

int64_t CallUiBackend::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  if (auto* calls = stack_.Calls()) {
    return calls->InitiationOfferMinorForPeer(peer_identity);
  }
  return 0;
}

bool CallUiBackend::MediaAttemptedThisProcess(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->MediaAttemptedThisProcess(call_id);
  }
  return false;
}

CallMediaEngine& CallUiBackend::Media() {
  auto* calls = stack_.Calls();
  if (!calls) {
    throw std::runtime_error("CallUiBackend::Media unavailable");
  }
  return calls->Media();
}

} // namespace pbr
