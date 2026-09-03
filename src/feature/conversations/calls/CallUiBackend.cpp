#include "feature/conversations/calls/CallUiBackend.h"

#include "domain/media/CallMediaEngine.h"
#include "feature/conversations/calls/CallSessionManager.h"
#include "feature/conversations/calls/CallStack.h"

#include <stdexcept>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

const std::string& EmptyString() {
  static const std::string kEmpty;
  return kEmpty;
}

Error UnavailableError() {
  return Error("Call backend unavailable");
}

} // namespace

CallUiBackend::CallUiBackend(CallStack& stack) : stack_(stack) {}

bool CallUiBackend::Available() const {
  return stack_.Calls() != nullptr && stack_.Lifecycle() != nullptr;
}

const void* CallUiBackend::SessionsIdentity() const {
  return stack_.Calls();
}

void CallUiBackend::SetOnRingChanged(std::function<void()> callback) {
  if (auto* calls = stack_.Calls()) {
    calls->SetOnRingChanged(std::move(callback));
  }
}

void CallUiBackend::SetOnChromeRefresh(std::function<void()> callback) {
  if (auto* life = stack_.Lifecycle()) {
    life->SetOnChromeRefresh(std::move(callback));
  }
}

void CallUiBackend::SweepExpiredInvites() {
  if (auto* calls = stack_.Calls()) {
    calls->SweepExpiredInvites();
  }
}

void CallUiBackend::PollPendingSfuAttach() {
  if (auto* calls = stack_.Calls()) {
    calls->PollPendingSfuAttach();
  }
}

void CallUiBackend::PollP2pConnectHealth() {
  if (auto* calls = stack_.Calls()) {
    calls->PollP2pConnectHealth();
  }
}

std::optional<std::string> CallUiBackend::TakeLastMediaError() {
  if (auto* calls = stack_.Calls()) {
    return calls->TakeLastMediaError();
  }
  return std::nullopt;
}

std::string CallUiBackend::PeekMediaActivity() const {
  if (auto* calls = stack_.Calls()) {
    return calls->PeekMediaActivity();
  }
  return {};
}

void CallUiBackend::ClearMediaActivity() {
  if (auto* calls = stack_.Calls()) {
    calls->ClearMediaActivity();
  }
}

Roe<std::optional<PendingCallInvite>> CallUiBackend::TopPendingInvite() {
  if (auto* calls = stack_.Calls()) {
    return calls->TopPendingInvite();
  }
  return UnavailableError();
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

Roe<std::vector<CallParticipant>> CallUiBackend::ListJoinedParticipants(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->ListJoinedParticipants(call_id);
  }
  return UnavailableError();
}

bool CallUiBackend::IsAwaitingSfuRecovery() const {
  if (auto* calls = stack_.Calls()) {
    return calls->IsAwaitingSfuRecovery();
  }
  return false;
}

bool CallUiBackend::IsSoftMigrateInFlight() const {
  if (auto* calls = stack_.Calls()) {
    return calls->IsSoftMigrateInFlight();
  }
  return false;
}

bool CallUiBackend::IsSfuAttachWaitActive() const {
  if (auto* calls = stack_.Calls()) {
    return calls->IsSfuAttachWaitActive();
  }
  return false;
}

bool CallUiBackend::IsP2pConnectFailed() const {
  if (auto* calls = stack_.Calls()) {
    return calls->IsP2pConnectFailed();
  }
  return false;
}

bool CallUiBackend::P2pConnectMissingMic() const {
  if (auto* calls = stack_.Calls()) {
    return calls->P2pConnectMissingMic();
  }
  return false;
}

bool CallUiBackend::MediaAttemptedThisProcess(const std::string& call_id) const {
  if (auto* calls = stack_.Calls()) {
    return calls->MediaAttemptedThisProcess(call_id);
  }
  return false;
}

Roe<void> CallUiBackend::LeaveCall(const std::string& call_id) {
  if (auto* calls = stack_.Calls()) {
    return calls->LeaveCall(call_id);
  }
  return UnavailableError();
}

Roe<CallSession> CallUiBackend::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                          const std::vector<std::string>& invitee_identities) {
  if (auto* calls = stack_.Calls()) {
    return calls->StartCall(origin_thread_id, video_allowed, invitee_identities);
  }
  return UnavailableError();
}

Roe<void> CallUiBackend::InviteParticipant(const std::string& call_id,
                                           const std::string& invitee_identity) {
  if (auto* calls = stack_.Calls()) {
    return calls->InviteParticipant(call_id, invitee_identity);
  }
  return UnavailableError();
}

void CallUiBackend::StopCallMedia(const std::string& call_id) {
  if (auto* calls = stack_.Calls()) {
    calls->StopCallMedia(call_id);
  }
}

Roe<void> CallUiBackend::SetLocalAudioMuted(bool muted) {
  if (auto* calls = stack_.Calls()) {
    return calls->SetLocalAudioMuted(muted);
  }
  return UnavailableError();
}

Roe<void> CallUiBackend::SetLocalVideoEnabled(bool enabled) {
  if (auto* calls = stack_.Calls()) {
    return calls->SetLocalVideoEnabled(enabled);
  }
  return UnavailableError();
}

Roe<void> CallUiBackend::RequestVideoRefresh(const std::string& call_id,
                                            const std::string& publisher_identity) {
  if (auto* calls = stack_.Calls()) {
    return calls->RequestVideoRefresh(call_id, publisher_identity);
  }
  return UnavailableError();
}

CallMediaEngine& CallUiBackend::Media() {
  auto* calls = stack_.Calls();
  if (!calls) {
    throw std::runtime_error("CallUiBackend::Media unavailable");
  }
  return calls->Media();
}

CallHopHealth CallUiBackend::HopHealth() const {
  if (auto* calls = stack_.Calls()) {
    return calls->HopHealth();
  }
  return {};
}

const std::string& CallUiBackend::LastError() const {
  if (auto* life = stack_.Lifecycle()) {
    return life->LastError();
  }
  return EmptyString();
}

void CallUiBackend::ClearLastError() {
  if (auto* life = stack_.Lifecycle()) {
    life->ClearLastError();
  }
}

bool CallUiBackend::ShouldSuppressRing(const std::string& call_id) const {
  if (auto* life = stack_.Lifecycle()) {
    return life->ShouldSuppressRing(call_id);
  }
  return false;
}

CallPhase CallUiBackend::Phase() const {
  if (auto* life = stack_.Lifecycle()) {
    return life->Phase();
  }
  return CallPhase::Idle;
}

void CallUiBackend::Apply(CallLifecycleEvent ev, const std::string& call_id) {
  if (auto* life = stack_.Lifecycle()) {
    life->Apply(ev, call_id);
  }
}

void CallUiBackend::NoteRingCallId(const std::string& call_id) {
  if (auto* life = stack_.Lifecycle()) {
    life->NoteRingCallId(call_id);
  }
}

const std::string& CallUiBackend::LastRingCallId() const {
  if (auto* life = stack_.Lifecycle()) {
    return life->LastRingCallId();
  }
  return EmptyString();
}

const std::string& CallUiBackend::ActiveCallId() const {
  if (auto* life = stack_.Lifecycle()) {
    return life->ActiveCallId();
  }
  return EmptyString();
}

int64_t CallUiBackend::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  if (auto* calls = stack_.Calls()) {
    return calls->InitiationOfferMinorForPeer(peer_identity);
  }
  return 0;
}

void CallUiBackend::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  if (auto* calls = stack_.Calls()) {
    calls->SetPendingAcceptChargeDecision(decision);
  }
}

} // namespace pbr
