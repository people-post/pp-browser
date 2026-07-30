#include "feature/messaging/CallSessionManager.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/SessionKeyDeriver.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {

CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       P2pMessagingService& p2p, IPskSessionStore& psk_store, CallMediaEngine& media)
    : store_(store), contacts_(contacts), identity_(identity), sessions_(sessions), media_keys_(media_keys),
      p2p_(p2p), psk_store_(psk_store), media_(media) {
  redirectLogger("CallSessionManager");
}

CallMediaEngine& CallSessionManager::Media() {
  return media_;
}

Roe<void> CallSessionManager::SetLocalAudioMuted(bool muted) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return Error("No active call media");
  }
  media_.SetMuted(muted);
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.audio_muted = muted;
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::SetLocalVideoEnabled(bool enabled) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return Error("No active call media");
  }
  if (enabled) {
    if (auto cam = media_.SetCameraEnabled(true); !cam) {
      return cam.error();
    }
  } else {
    (void)media_.SetCameraEnabled(false);
  }
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.video_enabled = enabled && media_.IsCameraEnabled();
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

std::optional<std::string> CallSessionManager::TakeLastMediaError() {
  auto out = std::move(last_media_error_);
  last_media_error_.reset();
  return out;
}

void CallSessionManager::SetOnRingChanged(RingChangedFn callback) {
  on_ring_changed_ = std::move(callback);
}

void CallSessionManager::NotifyRingChanged() {
  if (on_ring_changed_) {
    on_ring_changed_();
  }
}

Roe<std::string> CallSessionManager::LocalRelayIdentity() const {
  auto identity = identity_.Get();
  if (!identity || identity->relay_user_id.empty()) {
    return Error("Local relay identity unavailable");
  }
  return identity->relay_user_id;
}

Roe<void> CallSessionManager::SendCallDirectMessage(const std::string& peer_identity, const CallControlType type,
                                                    const std::string& detail_json, const std::string& display) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, ContactIdKind::RelayUser)) {
    if (*contact) {
      contact_id = (*contact)->id;
      dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (dm_title.empty()) {
        dm_title = peer_identity;
      }
    }
  }

  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
  if (!thread) {
    return thread.error();
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::System;
  opts.payload_json =
      nlohmann::json({{"control_type", CallControlTypeToWire(type)}, {"detail", detail_json}}).dump();
  opts.generation = "system";
  opts.update_preview = false;
  auto sent = p2p_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<void> CallSessionManager::AppendOriginHistory(const std::string& thread_id, const CallControlType type,
                                                  const std::string& text, const std::string& detail_json) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto message = CallControlCodec::BuildSystemMessage(thread_id, type, text, detail_json, *local);
  if (!message) {
    return message.error();
  }
  if (auto appended = store_.AppendMessage(*message); !appended) {
    return appended.error();
  }
  return {};
}

Roe<CallRosterDetail> CallSessionManager::BuildRosterDetail(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  CallRosterDetail detail;
  detail.call_id = call_id;
  detail.media_epoch = (*session)->media_epoch;
  for (const CallParticipant& row : *participants) {
    CallRosterEntry entry;
    entry.identity = row.identity;
    entry.state = row.state;
    entry.audio_muted = row.media.audio_muted;
    entry.video_enabled = row.media.video_enabled;
    detail.participants.push_back(std::move(entry));
  }
  return detail;
}

Roe<void> CallSessionManager::FanOutToJoined(const std::string& call_id, const CallControlType type,
                                             const std::string& detail_json, const std::string& display,
                                             const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity == skip_identity || row.state != CallParticipantState::Joined) {
      continue;
    }
    if (auto sent = SendCallDirectMessage(row.identity, type, detail_json, display); !sent) {
      return sent.error();
    }
  }
  return {};
}

Roe<ByteVector> CallSessionManager::ResolvePeerSessionKey(const std::string& peer_identity) const {
  ChatTargetKey target_key;
  target_key.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  target_key.peer_identity_value = peer_identity;
  target_key.channel = CryptoChannel::E2ePublic;

  auto record = psk_store_.Load(target_key);
  if (!record) {
    return record.error();
  }
  if (!record->has_value()) {
    return Error("No PSK session for peer");
  }
  const uint32_t active_epoch = (*record)->session_epoch;
  auto master_psk_b64 = psk_store_.ResolveMasterPskForEpoch(target_key, active_epoch);
  if (!master_psk_b64) {
    return master_psk_b64.error();
  }
  if (!master_psk_b64->has_value()) {
    return Error("No PSK for active session epoch");
  }
  auto master_psk = Base64Decode(**master_psk_b64);
  if (!master_psk) {
    return master_psk.error();
  }
  return SessionKeyDeriver::Derive(*master_psk, CryptoChannel::E2ePublic, active_epoch);
}

Roe<void> CallSessionManager::SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                                                 const uint32_t media_epoch, const std::string& media_key_id,
                                                 const ByteVector& key_bytes) {
  auto session_key = ResolvePeerSessionKey(peer_identity);
  if (!session_key) {
    return session_key.error();
  }
  auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, key_bytes, call_id, media_epoch, media_key_id);
  if (!wrapped) {
    return wrapped.error();
  }
  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = media_epoch;
  key_detail.media_key_id = media_key_id;
  key_detail.wrapped_key_b64 = *wrapped;
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  if (!key_json) {
    return key_json.error();
  }
  return SendCallDirectMessage(peer_identity, CallControlType::CallMediaKey, *key_json, "Call media key");
}

void CallSessionManager::BindMediaCallbacks(const std::string& peer_identity) {
  media_peer_identity_ = peer_identity;

  media_.SetOnLocalDescription([this](const CallMediaEngine::LocalDescription& local) {
    const std::string call_id = media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    CallSdpDetail detail;
    detail.call_id = call_id;
    if (auto local_identity = LocalRelayIdentity()) {
      detail.identity = *local_identity;
    }
    detail.sdp_type = local.type;
    detail.sdp = local.sdp;
    auto encoded = CallControlCodec::EncodeSdp(detail);
    if (!encoded) {
      return;
    }
    (void)SendCallDirectMessage(media_peer_identity_, CallControlType::CallSdp, *encoded, "Call signaling");
  });

  media_.SetOnIceCandidate([this](const CallMediaEngine::IceCandidate& ice) {
    const std::string call_id = media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    CallIceDetail detail;
    detail.call_id = call_id;
    if (auto local_identity = LocalRelayIdentity()) {
      detail.identity = *local_identity;
    }
    detail.candidate = ice.candidate;
    detail.mid = ice.mid;
    auto encoded = CallControlCodec::EncodeIce(detail);
    if (!encoded) {
      return;
    }
    (void)SendCallDirectMessage(media_peer_identity_, CallControlType::CallIce, *encoded, "Call signaling");
  });

  media_.SetOnStateChanged([this](const std::string&) { NotifyRingChanged(); });
}

void CallSessionManager::ClearMediaCallbacks() {
  media_.SetOnLocalDescription({});
  media_.SetOnIceCandidate({});
  media_.SetOnStateChanged({});
  media_peer_identity_.clear();
}

Roe<void> CallSessionManager::StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity) {
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Offerer);
}

Roe<void> CallSessionManager::StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity) {
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Answerer);
}

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  media_peer_identity_.clear();
}

Roe<void> CallSessionManager::LeaveCallIfActiveExcept(const std::string& keep_call_id) {
  // Drain any conflicting Joined sessions (normally at most one).
  for (int i = 0; i < 4; ++i) {
    auto active = ActiveLocalCall();
    if (!active) {
      return active.error();
    }
    if (!active->has_value() || (*active)->call_id == keep_call_id) {
      return {};
    }
    if (auto left = LeaveCall((*active)->call_id); !left) {
      log().warning << "LeaveCallIfActiveExcept failed for " << (*active)->call_id << ": "
                    << left.error().message;
      return left.error();
    }
  }
  return {};
}

Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const CallMediaMode mode,
                                               const std::vector<std::string>& invitee_identities) {
  if (invitee_identities.empty()) {
    return Error("At least one invitee required");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto pending = TopPendingInvite();
  if (!pending) {
    return pending.error();
  }
  if (pending->has_value()) {
    return Error("Decline the incoming call first");
  }
  auto thread = store_.GetThread(origin_thread_id);
  if (!thread || !*thread) {
    return Error("Origin thread not found");
  }
  if ((*thread)->kind != ThreadKind::Direct && (*thread)->kind != ThreadKind::Group) {
    return Error("Calls require a direct or group thread");
  }

  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const std::string call_id = GenerateCallId();
  auto media_key_id = media_keys_.PutEpochKey(call_id, 1, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }

  const int64_t now = util::NowUnixMs();
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = origin_thread_id;
  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    session.origin_group_id = *(*thread)->group_id;
  }
  session.media_mode = mode;
  session.state = CallSessionState::Ringing;
  session.created_at = now;
  session.media_epoch = 1;
  session.media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallStartedDetail started;
  started.call_id = call_id;
  started.media_mode = mode;
  auto started_detail = CallControlCodec::EncodeStarted(started);
  if (!started_detail) {
    return started_detail.error();
  }
  const std::string started_text = mode == CallMediaMode::Video ? "Video call started" : "Voice call started";
  if (auto hist = AppendOriginHistory(origin_thread_id, CallControlType::CallStarted, started_text, *started_detail);
      !hist) {
    return hist.error();
  }

  for (const std::string& invitee : invitee_identities) {
    if (invitee.empty() || invitee == *local) {
      continue;
    }
    if (auto invited = InviteParticipant(call_id, invitee); !invited) {
      return invited.error();
    }
  }

  NotifyRingChanged();
  return session;
}

Roe<void> CallSessionManager::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    return Error("Call has ended");
  }
  auto joined = sessions_.CountJoined(call_id);
  if (!joined) {
    return joined.error();
  }
  if (!CallSessionLogic::CanAcceptJoin(*joined)) {
    return Error("Call is full");
  }

  const int64_t now = util::NowUnixMs();
  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = invitee_identity;
  participant.state = CallParticipantState::Ringing;
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }

  PendingCallInvite pending;
  pending.call_id = call_id;
  pending.inviter_identity = *local;
  pending.invitee_identity = invitee_identity;
  pending.media_mode = (*session)->media_mode;
  pending.origin_thread_id = (*session)->origin_thread_id;
  pending.origin_group_id = (*session)->origin_group_id;
  pending.sfu_hint = (*session)->sfu_hint;
  pending.expires_at = now + kDefaultCallInviteTtlMs;
  pending.created_at = now;
  pending.status = "pending";
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }

  CallInviteDetail invite;
  invite.call_id = call_id;
  invite.inviter_identity = *local;
  invite.invitee_identity = invitee_identity;
  invite.media_mode = (*session)->media_mode;
  invite.origin_thread_id = (*session)->origin_thread_id;
  invite.origin_group_id = (*session)->origin_group_id;
  invite.sfu_hint = (*session)->sfu_hint;
  invite.expires_at = pending.expires_at;
  auto detail = CallControlCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  return SendCallDirectMessage(invitee_identity, CallControlType::CallInvite, *detail, display);
}

Roe<void> CallSessionManager::AcceptInvite(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  SweepExpiredInvites();
  if (auto cleared = LeaveCallIfActiveExcept(call_id); !cleared) {
    return cleared.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, *local, "expired");
    NotifyRingChanged();
    return Error("Call invite expired");
  }

  auto session = sessions_.LoadSession(call_id);
  CallSession row;
  if (session && session->has_value()) {
    row = **session;
  } else {
    row.call_id = call_id;
    row.origin_thread_id = (*pending)->origin_thread_id;
    row.origin_group_id = (*pending)->origin_group_id;
    row.media_mode = (*pending)->media_mode;
    row.state = CallSessionState::Ringing;
    row.created_at = (*pending)->created_at;
    row.media_epoch = 1;
    row.sfu_hint = (*pending)->sfu_hint;
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t joined_count = joined ? *joined : 0;
  if (!CallSessionLogic::CanAcceptJoin(joined_count)) {
    return Error("Call is full");
  }

  const int64_t now = util::NowUnixMs();
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "accepted");

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = *local;
  accept.video_enabled = false;
  auto detail = CallControlCodec::EncodeAccept(accept);
  if (!detail) {
    return detail.error();
  }
  if (auto sent = SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallAccept, *detail,
                                        "Call accepted");
      !sent) {
    return sent.error();
  }
  if (auto started = StartMediaAsAnswerer(call_id, (*pending)->inviter_identity); !started) {
    log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
    return started.error();
  }

  // Best-effort roster to inviter; full fan-out when we are coordinator later.
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallRoster, *roster_json,
                                  "Call roster");
    }
  }

  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "declined");

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = *local;
  participant.state = CallParticipantState::Declined;
  (void)sessions_.UpsertParticipant(participant);

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = *local;
  auto detail = CallControlCodec::EncodeDecline(decline);
  if (!detail) {
    return detail.error();
  }
  if (auto sent = SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallDecline, *detail,
                                        "Call declined");
      !sent) {
    return sent.error();
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<std::string> remaining;
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined && row.identity != leaver_identity) {
      remaining.push_back(row.identity);
    }
  }
  if (remaining.empty()) {
    return {};
  }
  auto coordinator = CallSessionLogic::SelectEpochCoordinator(remaining);
  if (!coordinator || *coordinator != *local) {
    return {};
  }

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const uint32_t new_epoch = (*session)->media_epoch + 1;
  auto media_key_id = media_keys_.PutEpochKey(call_id, new_epoch, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }
  (*session)->media_epoch = new_epoch;
  (*session)->media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }

  auto roster = BuildRosterDetail(call_id);
  if (!roster) {
    return roster.error();
  }
  roster->media_epoch = new_epoch;
  auto roster_json = CallControlCodec::EncodeRoster(*roster);
  if (!roster_json) {
    return roster_json.error();
  }
  for (const std::string& peer : remaining) {
    if (peer == *local) {
      continue;
    }
    (void)SendMediaKeyToPeer(call_id, peer, new_epoch, *media_key_id, *key);
    (void)SendCallDirectMessage(peer, CallControlType::CallRoster, *roster_json, "Call roster");
  }
  return {};
}

Roe<void> CallSessionManager::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms) {
  StopMediaIfCall(session.call_id);
  session.state = CallSessionState::Ended;
  session.ended_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }
  if (session.origin_thread_id) {
    CallEndedDetail ended;
    ended.call_id = session.call_id;
    ended.duration_ms = duration_ms;
    auto detail = CallControlCodec::EncodeEnded(ended);
    if (detail) {
      (void)AppendOriginHistory(*session.origin_thread_id, CallControlType::CallEnded, "Call ended", *detail);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::LeaveCall(const std::string& call_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    return {};
  }
  StopMediaIfCall(call_id);

  const int64_t now = util::NowUnixMs();
  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Left;
  self.left_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = *local;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (!detail) {
    return detail.error();
  }
  (void)FanOutToJoined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);

  auto joined = sessions_.CountJoined(call_id);
  const size_t remaining = joined ? *joined : 0;
  const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
  if (next == CallSessionState::Ended) {
    std::optional<int64_t> duration;
    if ((*session)->created_at > 0) {
      duration = now - (*session)->created_at;
    }
    CallEndedDetail ended;
    ended.call_id = call_id;
    ended.duration_ms = duration;
    auto ended_json = CallControlCodec::EncodeEnded(ended);
    if (ended_json) {
      (void)FanOutToJoined(call_id, CallControlType::CallEnded, *ended_json, "Call ended", *local);
    }
    return EndCallLocal(**session, duration);
  }

  (*session)->state = next;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }
  (void)MaybeRotateMediaKey(call_id, *local);
  NotifyRingChanged();
  return {};
}

Roe<std::vector<PendingCallInvite>> CallSessionManager::ListPendingInvites() {
  SweepExpiredInvites();
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  return sessions_.ListPendingInvitesForInvitee(*local);
}

Roe<std::optional<CallSession>> CallSessionManager::ActiveLocalCall() const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return active.error();
  }
  for (const CallSession& session : *active) {
    auto participant = sessions_.FindParticipant(session.call_id, *local);
    if (participant && participant->has_value() && (*participant)->state == CallParticipantState::Joined) {
      return std::optional<CallSession>{session};
    }
  }
  return std::optional<CallSession>{};
}

Roe<std::optional<PendingCallInvite>> CallSessionManager::TopPendingInvite() {
  auto pending = ListPendingInvites();
  if (!pending) {
    return pending.error();
  }
  if (pending->empty()) {
    return std::optional<PendingCallInvite>{};
  }
  return std::optional<PendingCallInvite>{pending->front()};
}

Roe<std::optional<std::string>> CallSessionManager::PeerIdentityForCall(const std::string& call_id) const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != *local && !row.identity.empty()) {
      return std::optional<std::string>{row.identity};
    }
  }
  return std::optional<std::string>{};
}

void CallSessionManager::SweepExpiredInvites() {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }
  auto pending = sessions_.ListPendingInvitesForInvitee(*local);
  if (!pending) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  bool changed = false;
  for (PendingCallInvite& invite : *pending) {
    if (invite.status != "pending") {
      continue;
    }
    if (CallSessionLogic::IsInviteExpired(invite, now)) {
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      changed = true;
    }
  }
  if (changed) {
    NotifyRingChanged();
  }
}

Roe<void> CallSessionManager::ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity) {
  auto type = CallControlCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Call control missing detail");
  }
  const std::string detail_json = payload["detail"].get<std::string>();
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }

  switch (*type) {
  case CallControlType::CallInvite: {
    auto invite = CallControlCodec::DecodeInvite(detail_json);
    if (!invite) {
      return invite.error();
    }
    // Do not trust caller wall-clock for ring lifetime (clock skew drops live invites while the
    // system message still persists → unread badge, no Accept UI). Re-arm from local receipt time.
    const int64_t now = util::NowUnixMs();
    if (CallSessionLogic::IsInviteExpired(*invite, now)) {
      log().warning << "CallInvite past wire expires_at; re-arming locally call_id=" << invite->call_id
                    << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                    << " now=" << now;
    }
    PendingCallInvite pending;
    pending.call_id = invite->call_id;
    pending.inviter_identity = invite->inviter_identity.empty() ? sender_identity : invite->inviter_identity;
    // Always key pending rows to local identity so ListPendingInvitesForInvitee matches.
    pending.invitee_identity = *local;
    pending.media_mode = invite->media_mode;
    pending.origin_thread_id = invite->origin_thread_id;
    pending.origin_group_id = invite->origin_group_id;
    pending.sfu_hint = invite->sfu_hint;
    pending.expires_at = now + kDefaultCallInviteTtlMs;
    pending.created_at = message.timestamp > 0 ? message.timestamp : now;
    pending.status = "pending";
    if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
      return saved.error();
    }

    CallSession session;
    session.call_id = invite->call_id;
    session.origin_thread_id = invite->origin_thread_id;
    session.origin_group_id = invite->origin_group_id;
    session.media_mode = invite->media_mode;
    session.state = CallSessionState::Ringing;
    session.created_at = pending.created_at;
    session.media_epoch = 1;
    session.sfu_hint = invite->sfu_hint;
    (void)sessions_.UpsertSession(session);

    CallParticipant inviter;
    inviter.call_id = invite->call_id;
    inviter.identity = pending.inviter_identity;
    inviter.state = CallParticipantState::Joined;
    (void)sessions_.UpsertParticipant(inviter);
    CallParticipant self;
    self.call_id = invite->call_id;
    self.identity = *local;
    self.state = CallParticipantState::Ringing;
    (void)sessions_.UpsertParticipant(self);
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallAccept: {
    auto accept = CallControlCodec::DecodeAccept(detail_json);
    if (!accept) {
      return accept.error();
    }
    const std::string identity = accept->identity.empty() ? sender_identity : accept->identity;
    CallParticipant participant;
    participant.call_id = accept->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Joined;
    participant.media.audio_muted = accept->audio_muted;
    participant.media.video_enabled = accept->video_enabled;
    participant.joined_at = util::NowUnixMs();
    if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
      return saved.error();
    }
    auto session = sessions_.LoadSession(accept->call_id);
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      (*session)->state = CallSessionLogic::TransitionOnRemoteJoined((*session)->state);
      (void)sessions_.UpsertSession(**session);
    }
    (void)sessions_.UpdateInviteStatus(accept->call_id, identity, "accepted");

    if (session && session->has_value()) {
      const uint32_t epoch = (*session)->media_epoch;
      auto key_bytes = media_keys_.LoadEpochKey(accept->call_id, epoch);
      if (key_bytes && key_bytes->has_value()) {
        (void)SendMediaKeyToPeer(accept->call_id, identity, epoch, (*session)->media_key_id, **key_bytes);
      }
      if (auto started = StartMediaAsOfferer(accept->call_id, identity); !started) {
        // Accept already applied — keep session, surface error for UI toast.
        log().warning << "StartMediaAsOfferer failed: " << started.error().message;
        last_media_error_ = started.error().message;
        NotifyRingChanged();
      }
    }

    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallDecline: {
    auto decline = CallControlCodec::DecodeDecline(detail_json);
    if (!decline) {
      return decline.error();
    }
    const std::string identity = decline->identity.empty() ? sender_identity : decline->identity;
    CallParticipant participant;
    participant.call_id = decline->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Declined;
    (void)sessions_.UpsertParticipant(participant);
    (void)sessions_.UpdateInviteStatus(decline->call_id, identity, "declined");
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallLeave: {
    auto leave = CallControlCodec::DecodeLeave(detail_json);
    if (!leave) {
      return leave.error();
    }
    const std::string identity = leave->identity.empty() ? sender_identity : leave->identity;
    CallParticipant participant;
    participant.call_id = leave->call_id;
    participant.identity = identity;
    participant.state = CallParticipantState::Left;
    participant.left_at = util::NowUnixMs();
    (void)sessions_.UpsertParticipant(participant);
    auto joined = sessions_.CountJoined(leave->call_id);
    auto session = sessions_.LoadSession(leave->call_id);
    if (session && session->has_value()) {
      const size_t remaining = joined ? *joined : 0;
      const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
      if (next == CallSessionState::Ended) {
        std::optional<int64_t> duration;
        if ((*session)->created_at > 0) {
          duration = util::NowUnixMs() - (*session)->created_at;
        }
        return EndCallLocal(**session, duration);
      }
      (*session)->state = next;
      (void)sessions_.UpsertSession(**session);
      (void)MaybeRotateMediaKey(leave->call_id, identity);
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallRoster: {
    auto roster = CallControlCodec::DecodeRoster(detail_json);
    if (!roster) {
      return roster.error();
    }
    auto session = sessions_.LoadSession(roster->call_id);
    if (session && session->has_value()) {
      (*session)->media_epoch = roster->media_epoch;
      (void)sessions_.UpsertSession(**session);
    }
    for (const CallRosterEntry& entry : roster->participants) {
      CallParticipant participant;
      participant.call_id = roster->call_id;
      participant.identity = entry.identity;
      participant.state = entry.state;
      participant.media.audio_muted = entry.audio_muted;
      participant.media.video_enabled = entry.video_enabled;
      (void)sessions_.UpsertParticipant(participant);
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallMediaKey: {
    auto key = CallControlCodec::DecodeMediaKey(detail_json);
    if (!key) {
      return key.error();
    }
    auto session = sessions_.LoadSession(key->call_id);
    if (session && session->has_value()) {
      (*session)->media_epoch = key->media_epoch;
      (*session)->media_key_id = key->media_key_id;
      (void)sessions_.UpsertSession(**session);
    }
    if (!key->wrapped_key_b64.empty()) {
      auto session_key = ResolvePeerSessionKey(sender_identity);
      if (session_key) {
        auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, key->wrapped_key_b64, key->call_id,
                                                          key->media_epoch, key->media_key_id);
        if (unwrapped) {
          (void)media_keys_.PutEpochKey(key->call_id, key->media_epoch, *unwrapped);
        }
      }
    }
    return {};
  }
  case CallControlType::CallSdp: {
    auto sdp = CallControlCodec::DecodeSdp(detail_json);
    if (!sdp) {
      return sdp.error();
    }
    return media_.SetRemoteDescription(sdp->sdp_type, sdp->sdp);
  }
  case CallControlType::CallIce: {
    auto ice = CallControlCodec::DecodeIce(detail_json);
    if (!ice) {
      return ice.error();
    }
    return media_.AddRemoteIceCandidate(ice->candidate, ice->mid);
  }
  case CallControlType::CallEnded: {
    auto ended = CallControlCodec::DecodeEnded(detail_json);
    if (!ended) {
      return ended.error();
    }
    auto session = sessions_.LoadSession(ended->call_id);
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      return EndCallLocal(**session, ended->duration_ms);
    }
    return {};
  }
  case CallControlType::CallStarted:
    return {};
  }
  return {};
}

} // namespace pbr
