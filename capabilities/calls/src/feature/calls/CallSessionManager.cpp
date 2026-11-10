#include "feature/calls/CallSessionManager.h"

#include "lib/calls/CallSessionQueries.h"
#include "lib/calls/CallControlGate.h"
#include "feature/calls/CallControlClient.h"
#include "lib/calls/CallListenAddrsLogic.h"
#include "lib/calls/CallAnswererKickLogic.h"
#include "lib/calls/CallMediaPlannerSelectLogic.h"

#include "foundation/crypto/CryptoUtil.h"
#include "lib/calls/CallMediaAdaptation.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/delivery/DirectChatTargetFromContact.h"
#include "lib/calls/PeerCapsLogic.h"
#include "lib/delivery/SendRelayOptions.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/MeshHopPolicy.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

CallSessionManager::CallSessionManager(CallDevice& device)
    : device_(device),
      sessions_(device.Sessions()),
      live_calls_(device.Live()),
      call_media_(device.CallMediaOf()),
      workflow_(device.Workflow()),
      control_(device.Control()),
      key_exchange_(device.KeyExchange()),
      reach_signals_(device.ReachSignals()) {
  redirectLogger("CallSessionManager");
}

Roe<void> CallSessionManager::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  return device_.LeaveCall(call_id, reason);
}

void CallSessionManager::Apply(const CallLifecycleEvent ev, const std::string& call_id) {
  switch (ev) {
    case CallLifecycleEvent::AcceptClicked:
      ClickAccept(call_id);
      return;
    case CallLifecycleEvent::DeclineClicked:
      ClickDecline(call_id);
      return;
    case CallLifecycleEvent::LeaveClicked:
      ClickLeave(call_id);
      return;
    case CallLifecycleEvent::RetryClicked:
      device_.RestartMedia(call_id, false);
      return;
    case CallLifecycleEvent::InviteSeen:
    case CallLifecycleEvent::InviteCleared:
      // The ring is the LiveCall's: nothing to change, only to show.
      device_.NotifyCallStateChanged();
      return;
  }
}

void CallSessionManager::ClickAccept(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* ring = live_calls_.TheRing()) {
      call_id = ring->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "AcceptClicked ignored (no call_id)";
    return;
  }
  if (device_.AcceptingCallId() == call_id) {
    log().info << "AcceptClicked already in flight call_id=" << call_id;
    device_.NotifyCallStateChanged();
    return;
  }
  // Already answered (a double tap: the accept no longer waits for the park, so the first one may
  // have gone through by the second) — not a second accept.
  if (const LiveCall* call = live_calls_.Find(call_id); call && call->State() == LiveCallState::Joined) {
    log().info << "AcceptClicked ignored (already joined) call_id=" << call_id;
    device_.NotifyCallStateChanged();
    return;
  }
  // The ring holds the app's work running in the background (AppLifecycle::SetLiveSessionOn), so the
  // relay-fallback sends of the accept run there too. The workflow marks the call Accepting.
  log().info << "AcceptClicked call_id=" << call_id;
  OnAcceptResult(call_id, AcceptInvite(call_id));
}

void CallSessionManager::OnAcceptResult(const std::string& call_id, const Roe<void>& accepted) {
  if (!accepted) {
    // The workflow put the call back to ringing (or it ended meanwhile).
    log().warning << "AcceptInvite failed call_id=" << call_id << " err=" << accepted.error().message;
    device_.LastErrorMutable() = accepted.error().message;
    device_.NotifyCallStateChanged();
    return;
  }
  const LiveCall* active = live_calls_.Active();
  if (!active || active->Id() != call_id) {
    // B-CONFLICT: another accept (or a leave) moved on while this one was on the wire.
    log().info << "AcceptInvite result ignored stale call_id=" << call_id;
    return;
  }
  log().info << "AcceptInvite ok call_id=" << call_id;
  device_.KickAnswererAfterAccept(call_id);
  device_.NotifyCallStateChanged();
}

void CallSessionManager::ClickDecline(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* ring = live_calls_.TheRing()) {
      call_id = ring->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "DeclineClicked ignored (no call_id)";
    return;
  }
  if (auto declined = DeclineInvite(call_id); !declined) {
    log().warning << "DeclineInvite failed call_id=" << call_id << " err=" << declined.error().message;
    live_calls_.Close(call_id, LiveCallEndReason::Declined); // the ring goes either way
  }
  device_.NotifyCallStateChanged();
}

void CallSessionManager::ClickLeave(const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    if (const LiveCall* shown = live_calls_.Shown()) {
      call_id = shown->Id();
    }
  }
  if (call_id.empty()) {
    log().info << "LeaveClicked ignored (no call_id)";
    return;
  }
  if (auto left = device_.LeaveCall(call_id); !left) {
    log().warning << "LeaveCall failed call_id=" << call_id << " err=" << left.error().message;
    live_calls_.Close(call_id, LiveCallEndReason::LocalLeave); // the call leaves the screen either way
  }
  device_.NotifyCallStateChanged();
}

Roe<void> CallSessionManager::SetLocalAudioMuted(bool muted) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = call_media_.MediaRunningCallId();
  CallMedia* call_media = call_id.empty() ? nullptr : call_media_.Media(call_id);
  if (!call_media) {
    return Error("No active call media");
  }
  call_media->SetMuted(muted);
  (void)workflow_.PublishLocalMedia(call_id, *local, {muted, std::nullopt});
  return {};
}

Roe<void> CallSessionManager::SetLocalVideoEnabled(bool enabled, const int display_rotation_degrees) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = call_media_.MediaRunningCallId();
  CallMedia* call_media = call_id.empty() ? nullptr : call_media_.Media(call_id);
  if (!call_media) {
    return Error("No active call media");
  }
  if (enabled) {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || !(*session)->video_allowed) {
      return Error("Video is not allowed for this call");
    }
  }
  if (auto cam = call_media->SetCamera(enabled, display_rotation_degrees); !cam) {
    return cam.error();
  }
  (void)workflow_.PublishLocalMedia(call_id, *local, {std::nullopt, enabled && call_media->CameraOn()});
  return {};
}

Roe<void> CallSessionManager::SetLocalOnHold(const bool on_hold) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = call_media_.MediaRunningCallId();
  if (call_id.empty()) {
    return Error("No active call media");
  }
  CallSessionWorkflow::LocalMedia media;
  media.on_hold = on_hold;
  return workflow_.PublishLocalMedia(call_id, *local, media);
}

Roe<void> CallSessionManager::SetLocalVideoPaused(const bool paused, const int display_rotation_degrees) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = call_media_.MediaRunningCallId();
  CallMedia* call_media = call_id.empty() ? nullptr : call_media_.Media(call_id);
  if (!call_media) {
    video_paused_call_id_.clear();
    return Error("No active call media");
  }
  CallSessionWorkflow::LocalMedia media;
  if (paused) {
    if (!call_media->CameraOn()) {
      return {}; // nothing to pause
    }
    (void)call_media->SetCamera(false, 0);
    video_paused_call_id_ = call_id;
    media.video_enabled = false; // an older peer shows "camera off"
    media.video_paused = true;
  } else {
    if (video_paused_call_id_ != call_id) {
      video_paused_call_id_.clear();
      return {}; // not paused here (or another call's)
    }
    video_paused_call_id_.clear();
    auto cam = call_media->SetCamera(true, display_rotation_degrees);
    media.video_enabled = cam && call_media->CameraOn();
    media.video_paused = false;
  }
  return workflow_.PublishLocalMedia(call_id, *local, media);
}

Roe<void> CallSessionManager::RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (call_id.empty()) {
    return Error("No active call");
  }
  if (publisher_identity.empty() || publisher_identity == *local) {
    if (CallMedia* call_media = call_media_.Media(call_id)) {
      call_media->RequestKeyframe();
    }
    return {};
  }
  CallVideoRefreshDetail detail;
  detail.call_id = call_id;
  detail.identity = publisher_identity;
  auto encoded = CallControlCodec::EncodeVideoRefresh(detail);
  if (!encoded) {
    return encoded.error();
  }
  return control_.SendDirect(publisher_identity, CallControlType::CallVideoRefresh, *encoded, "");
}

void CallSessionManager::StopCallMedia(const std::string& call_id) {
  device_.StopMediaIfCall(call_id);
}

Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
  // Calling someone whose call is ringing here is answering it (they got there first).
  if (invitee_identities.size() == 1) {
    if (auto pending = workflow_.TopPendingInvite();
        pending && pending->has_value() && (*pending)->inviter_identity == invitee_identities.front() &&
        !((*pending)->origin_group_id && !(*pending)->origin_group_id->empty())) {
      const std::string call_id = (*pending)->call_id;
      log().info << "StartCall to " << invitee_identities.front()
                 << " while their call rings — answering call_id=" << call_id;
      workflow_.SetPendingAcceptVoiceOnly(!video_allowed);
      ClickAccept(call_id);
      if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
        return **session;
      }
      return Error("Call session not found");
    }
  }
  auto started = workflow_.StartCall(origin_thread_id, video_allowed, invitee_identities);
  if (started) {
    // Circuit path often chooses R1 before Invite (product-stack / hard-w5); flush wire announce.
    reach_signals_.FlushCircuitR1();
  }
  return started;
}

Roe<void> CallSessionManager::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  return workflow_.InviteParticipant(call_id, invitee_identity);
}

Roe<void> CallSessionManager::RemoveFromCall(const std::string& call_id, const std::string& identity) {
  return workflow_.RemoveFromCall(call_id, identity);
}

void CallSessionManager::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  workflow_.SetPendingAcceptChargeDecision(decision);
}

void CallSessionManager::SetPendingAcceptVoiceOnly(const bool voice_only) {
  workflow_.SetPendingAcceptVoiceOnly(voice_only);
}

Roe<void> CallSessionManager::AcceptInvite(const std::string& call_id, const InitiationChargeDecision charge_decision) {
  Roe<void> accepted = workflow_.AcceptInvite(call_id, charge_decision);
  if (accepted) {
    TellOwnDevicesAnswered(call_id, true);
  }
  return accepted;
}

void CallSessionManager::TellOwnDevicesAnswered(const std::string& call_id, const bool accepted) {
  if (!device_.Delivery().notify_own_devices) {
    return;
  }
  auto local = device_.LocalIdentity();
  auto detail = CallControlCodec::EncodeAnsweredElsewhere(call_id, accepted);
  if (!local || !detail) {
    return;
  }
  auto control = CallControlCodec::BuildSystemMessage(
      "", CallControlType::CallAnsweredElsewhere,
      accepted ? "Answered on another device" : "Declined on another device", *detail, *local);
  if (!control) {
    return;
  }
  if (auto told = device_.Delivery().notify_own_devices(*control); !told) {
    log().info << "own devices not told the call was answered call_id=" << call_id << ": " << told.error().message;
  }
}

Roe<void> CallSessionManager::HandleAnsweredElsewhere(const std::string& detail_json,
                                                      const std::string& sender_identity,
                                                      const std::string& local_identity) {
  if (sender_identity != local_identity) {
    log().info << "call_answered_elsewhere dropped: not from our own account (" << sender_identity << ")";
    return {};
  }
  auto answered = CallControlCodec::DecodeAnsweredElsewhere(detail_json);
  if (!answered) {
    return answered.error();
  }
  return workflow_.StopRingingAnsweredElsewhere(answered->first, local_identity);
}

Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  auto declined = workflow_.DeclineInvite(call_id);
  if (declined) {
    TellOwnDevicesAnswered(call_id, false);
  }
  return declined;
}

void CallSessionManager::SweepExpiredInvites() {
  workflow_.SweepExpiredInvites();
}

void CallSessionManager::AbandonOrphanedCallsAfterRestart() {
  workflow_.AbandonOrphanedCallsAfterRestart();
}

void CallSessionManager::LeaveIfOrphaned(const std::string& call_id) {
  // A call this process never had (force-quit / restart), with nothing running for it. A call it had
  // — open, or ended with its leave still on the way — is not an orphan.
  if (call_id.empty() || live_calls_.Find(call_id) || device_.Engine().IsActive() || device_.IsAwaitingSfuRecovery()) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state != CallSessionState::Active) {
    return;
  }
  (void)device_.LeaveCall(call_id, LiveCallEndReason::Orphaned);
}

Roe<void> CallSessionManager::HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                                  const ThreadMessage& message,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms,
                                                  const std::string& local_identity) {
  auto answer = workflow_.HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms,
                                              relay_server_time_ms, local_identity);
  if (!answer) {
    return answer.error();
  }
  if (*answer) {
    ClickAccept(**answer); // calling each other: theirs won, ours gave way
  }
  return {};
}

Roe<void> CallSessionManager::HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                                  const std::string& local_identity) {
  return workflow_.HandleInboundAccept(detail_json, sender_identity, local_identity);
}

Roe<void> CallSessionManager::HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity) {
  return workflow_.HandleInboundDecline(detail_json, sender_identity);
}

Roe<void> CallSessionManager::HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                                                 const std::string& local_identity) {
  return workflow_.HandleInboundLeave(detail_json, sender_identity, local_identity);
}

Roe<void> CallSessionManager::HandleInboundMediaKey(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  auto taken = key_exchange_.HandleInbound(detail_json, sender_identity);
  if (!taken) {
    return taken.error();
  }
  if (*taken) {
    workflow_.AdoptMediaEpoch((*taken)->call_id, (*taken)->epoch, (*taken)->key_id);
  }
  return {};
}

Roe<void> CallSessionManager::HandleInboundRoster(const std::string& detail_json) {
  return workflow_.HandleInboundRoster(detail_json);
}

Roe<void> CallSessionManager::HandleInboundSfuAttach(const std::string& detail_json,
                                                     const std::string& sender_identity) {
  return workflow_.HandleInboundSfuAttach(detail_json, sender_identity);
}

Roe<void> CallSessionManager::HandleInboundSfuAttachFailed(const std::string& detail_json,
                                                           const std::string& sender_identity) {
  return workflow_.HandleInboundSfuAttachFailed(detail_json, sender_identity);
}

Roe<void> CallSessionManager::HandleInboundHopRefuse(const std::string& detail_json) {
  return workflow_.HandleInboundHopRefuse(detail_json);
}

Roe<void> CallSessionManager::HandleInboundVideoRefresh(const std::string& detail_json,
                                                        const std::string& sender_identity) {
  return workflow_.HandleInboundVideoRefresh(detail_json, sender_identity);
}

Roe<void> CallSessionManager::HandleInboundEnded(const std::string& detail_json, const std::string& local_identity) {
  return workflow_.HandleInboundEnded(detail_json, local_identity);
}

bool CallSessionManager::AllowsInboundCall(const std::string& caller) const {
  switch (inbound_call_audience_) {
    case mesh::InboundAudience::Everyone:
      return true;
    case mesh::InboundAudience::Nobody:
      return false;
    case mesh::InboundAudience::ContactsOnly:
      // No gate wired (tests, tools): nothing to judge by — let it ring.
      return !device_.MeshGlue().address_disclosure ||
             device_.MeshGlue().address_disclosure->Snapshot()->contacts.contains(caller);
  }
  return true;
}

bool CallSessionManager::SenderMaySend(const CallControlType type, const std::string& detail_json,
                                       const std::string& sender_identity) const {
  switch (WhoMaySend(type)) {
    case CallControlSender::Anyone:
      if (type == CallControlType::CallInvite && !AllowsInboundCall(sender_identity)) {
        // Silent: a decline would tell the caller we exist and are online.
        log().info << "call invite dropped (who can call me: " << mesh::InboundAudienceName(inbound_call_audience_)
                   << ") from=" << sender_identity;
        return false;
      }
      return true;
    case CallControlSender::OwnAccount:
      return true; // M021 d5: HandleAnsweredElsewhere takes it from our own account only
    case CallControlSender::OfferedOrInCall:
      if (SenderMayJoin(detail_json, sender_identity) || SenderIsInCall(detail_json, sender_identity)) {
        return true;
      }
      break;
    case CallControlSender::InCall:
      if (SenderIsInCall(detail_json, sender_identity)) {
        return true;
      }
      break;
    case CallControlSender::Nobody:
      log().debug << "call control not signaling here type=" << static_cast<int>(type) << " from=" << sender_identity;
      return false;
  }
  log().info << "call control dropped (sender not in the call) type=" << static_cast<int>(type)
             << " from=" << sender_identity;
  return false;
}

Roe<void> CallSessionManager::ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms) {
  auto type = CallControlCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  auto payload = TryParseObject(message.payload_json);
  auto detail = payload ? payload->getString("detail") : std::nullopt;
  if (!detail) {
    return Error("Call control missing detail");
  }
  const std::string detail_json = *detail;
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  // V057 4: who may send this type (CallControlGate); anyone else is dropped silently.
  if (!SenderMaySend(*type, detail_json, sender_identity)) {
    return {};
  }
  switch (*type) {
    case CallControlType::CallInvite:
      return HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms, relay_server_time_ms,
                                 *local);
    case CallControlType::CallAccept:
      return HandleInboundAccept(detail_json, sender_identity, *local);
    case CallControlType::CallJoinRequest:
      return workflow_.HandleInboundJoinRequest(detail_json, sender_identity, *local);
    case CallControlType::CallDecline:
      return HandleInboundDecline(detail_json, sender_identity);
    case CallControlType::CallLeave:
      return HandleInboundLeave(detail_json, sender_identity, *local);
    case CallControlType::CallRoster:
      return HandleInboundRoster(detail_json);
    case CallControlType::CallMediaKey:
      return HandleInboundMediaKey(detail_json, sender_identity);
    case CallControlType::CallSfuAttach:
      return HandleInboundSfuAttach(detail_json, sender_identity);
    case CallControlType::CallSfuAttachFailed:
      return HandleInboundSfuAttachFailed(detail_json, sender_identity);
    case CallControlType::CallHopRefuse:
      return HandleInboundHopRefuse(detail_json);
    case CallControlType::CallVideoRefresh:
      return HandleInboundVideoRefresh(detail_json, sender_identity);
    case CallControlType::CallCircuitR1:
      return reach_signals_.HandleInboundCircuitR1(detail_json);
    case CallControlType::CallCapsUpdate:
      return reach_signals_.HandleInboundCapsUpdate(detail_json);
    case CallControlType::CallPunchOffer:
      return reach_signals_.HandleInboundPunchOffer(detail_json, sender_identity);
    case CallControlType::CallPunchAnswer:
      return reach_signals_.HandleInboundPunchAnswer(detail_json);
    case CallControlType::CallEnded:
      return HandleInboundEnded(detail_json, *local);
    case CallControlType::CallAnsweredElsewhere:
      return HandleAnsweredElsewhere(detail_json, sender_identity, *local);
    case CallControlType::CallSdp:
    case CallControlType::CallIce:
    case CallControlType::CallSummary:
      return {}; // the gate dropped them (not signaling here)
    case CallControlType::CallConnectGiveUp:
      return HandleInboundConnectGiveUp(detail_json, sender_identity);
    case CallControlType::CallRemove:
      return workflow_.HandleInboundRemove(detail_json, sender_identity, *local);
  }
  return {};
}

Roe<void> CallSessionManager::HandleInboundConnectGiveUp(const std::string& detail_json,
                                                         const std::string& sender_identity) {
  auto detail = CallControlCodec::DecodeConnectGiveUp(detail_json);
  if (!detail) {
    return detail.error();
  }
  const LiveCall* call = live_calls_.Find(detail->call_id);
  const std::optional<std::string> peer = call ? call->SolePeer() : std::nullopt;
  CallMedia* call_media = call_media_.Find(detail->call_id);
  if (!peer || *peer != sender_identity || !call_media || !call_media->DirectIfBuilt()) {
    log().info << "CallConnectGiveUp ignored call_id=" << detail->call_id << " from=" << sender_identity;
    return {};
  }
  log().info << "CallConnectGiveUp inbound call_id=" << detail->call_id << " from=" << sender_identity;
  call_media->OnPeerGaveUpReaching();
  return {};
}

bool CallSessionManager::SenderMayJoin(const std::string& detail_json, const std::string& sender_identity) const {
  auto detail = TryParseObject(detail_json);
  auto call_id = detail ? detail->getString("call_id") : std::nullopt;
  return call_id && call_queries::SenderMayJoin(sessions_, *call_id, sender_identity);
}

bool CallSessionManager::SenderIsInCall(const std::string& detail_json, const std::string& sender_identity) const {
  auto detail = TryParseObject(detail_json);
  auto call_id = detail ? detail->getString("call_id") : std::nullopt;
  return call_id && call_queries::SenderIsInCall(sessions_, *call_id, sender_identity);
}

} // namespace pbr
