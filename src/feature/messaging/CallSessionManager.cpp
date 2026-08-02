#include "feature/messaging/CallSessionManager.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/SessionKeyDeriver.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "common/Utilities.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

void PrefetchReachForIdentities(const CallSessionManager::PrefetchPeerReachFn& fn,
                                const std::vector<std::string>& identities) {
  if (!fn) {
    return;
  }
  for (const std::string& id : identities) {
    if (!id.empty()) {
      fn(id);
    }
  }
}

void PrefetchReachForIdentity(const CallSessionManager::PrefetchPeerReachFn& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

} // namespace
CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       P2pMessagingService& p2p, IPskSessionStore& psk_store, CallMediaEngine& media)
    : store_(store), contacts_(contacts), identity_(identity), sessions_(sessions), media_keys_(media_keys),
      p2p_(p2p), psk_store_(psk_store), media_(media),
      topology_(*this, sessions, contacts, media), p2p_bridge_(*this, sessions, media) {
  redirectLogger("CallSessionManager");
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  topology_.SetMediaRelayDeps(std::move(deps));
}

void CallSessionManager::SetLibp2pMediaBridge(CallLibp2pMediaBridge* bridge) {
  libp2p_bridge_ = bridge;
}

void CallSessionManager::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity,
                                                  bool offerer) {
  if (libp2p_bridge_) {
    if (offerer) {
      libp2p_bridge_->ScheduleStartMediaAsOfferer(call_id, peer_identity);
    } else {
      libp2p_bridge_->ScheduleStartMediaAsAnswerer(call_id, peer_identity);
    }
    return;
  }
  if (offerer) {
    p2p_bridge_.ScheduleStartMediaAsOfferer(call_id, peer_identity);
  } else {
    p2p_bridge_.ScheduleStartMediaAsAnswerer(call_id, peer_identity);
  }
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
  topology_.RefreshAdaptation(call_id);
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

void CallSessionManager::SetPrefetchPeerReachability(PrefetchPeerReachFn callback) {
  prefetch_reach_ = std::move(callback);
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
  // Best-effort: one peer failure must not block CallSfuAttach / roster to the rest.
  for (const CallParticipant& row : *participants) {
    if (row.identity == skip_identity || row.state != CallParticipantState::Joined) {
      continue;
    }
    if (auto sent = SendCallDirectMessage(row.identity, type, detail_json, display); !sent) {
      log().warning << "FanOutToJoined send failed peer=" << row.identity << " type="
                    << CallControlTypeToWire(type) << " err=" << sent.error().message;
    }
  }
  return {};
}

Roe<void> CallSessionManager::FanOutToJoinedAndRinging(const std::string& call_id, const CallControlType type,
                                                       const std::string& detail_json, const std::string& display,
                                                       const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity == skip_identity) {
      continue;
    }
    if (row.state != CallParticipantState::Joined && row.state != CallParticipantState::Ringing &&
        row.state != CallParticipantState::Invited) {
      continue;
    }
    if (auto sent = SendCallDirectMessage(row.identity, type, detail_json, display); !sent) {
      log().warning << "FanOutToJoinedAndRinging send failed peer=" << row.identity << " type="
                    << CallControlTypeToWire(type) << " err=" << sent.error().message;
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

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  if (libp2p_bridge_) {
    libp2p_bridge_->StopLibp2pMedia(call_id);
  }
  p2p_bridge_.StopP2pMedia(call_id);
  topology_.OnMediaStopped(call_id);
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

  PrefetchReachForIdentities(prefetch_reach_, invitee_identities);

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
  // N≥3 requires media_relay soft-migrate (V021). Refuse mid-call guest invites when no hop
  // exists — otherwise Mac/Linux stay on 1:1 P2P while the invitee hangs on Connecting….
  const bool already_on_sfu =
      topology_.IsOnSfuForCall(call_id) ||
      ((*session)->sfu_hint && !(*session)->sfu_hint->empty());
  if (*joined >= 2 && !already_on_sfu && !topology_.HasMediaRelayHopCandidates()) {
    return Error("Adding a guest needs a media relay hop (enable Media relay on a Node/seed)");
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
  if (auto roster = BuildRosterDetail(call_id); roster) {
    invite.participants = std::move(roster->participants);
  }
  auto detail = CallControlCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  PrefetchReachForIdentity(prefetch_reach_, invitee_identity);
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

  auto joined_after = sessions_.CountJoined(call_id);
  const size_t n_joined = joined_after ? *joined_after : 0;
  if (!topology_.OnLocalAcceptJoined(call_id, n_joined, row.sfu_hint)) {
    if (row.sfu_hint && !row.sfu_hint->empty()) {
      row.sfu_hint.reset();
      (void)sessions_.UpsertSession(row);
    }
    // Must not Start media inside the Accept click / Rml callback — SDL audio + PC setup
    // can stall the UI so the ring dialog never dismisses.
    ScheduleStartDirectMedia(call_id, (*pending)->inviter_identity, false);
  }

  // Best-effort roster to inviter + other joined/ringing peers so co-invitees learn N.
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)SendCallDirectMessage((*pending)->inviter_identity, CallControlType::CallRoster, *roster_json,
                                  "Call roster");
      (void)FanOutToJoinedAndRinging(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }

  PrefetchReachForIdentity(prefetch_reach_, (*pending)->inviter_identity);

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
      // Notify Ringing/Invited invitees as well so offline inbox delivery can clear stale rings.
      (void)FanOutToJoinedAndRinging(call_id, CallControlType::CallEnded, *ended_json, "Call ended", *local);
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

Roe<std::optional<bool>> CallSessionManager::PeerVideoEnabledForCall(const std::string& call_id) const {
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
      return std::optional<bool>{row.media.video_enabled};
    }
  }
  return std::optional<bool>{};
}

Roe<std::vector<CallParticipant>> CallSessionManager::ListJoinedParticipants(const std::string& call_id) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<CallParticipant> joined;
  joined.reserve(participants->size());
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined) {
      joined.push_back(row);
    }
  }
  return joined;
}

bool CallSessionManager::IsAwaitingSfuRecovery() const {
  return topology_.IsAwaitingSfuRecovery();
}

bool CallSessionManager::IsP2pConnectFailed() const {
  if (libp2p_bridge_ && libp2p_bridge_->IsLibp2pConnectFailed()) {
    return true;
  }
  return p2p_bridge_.IsP2pConnectFailed();
}

bool CallSessionManager::P2pConnectMissingMic() const {
  if (libp2p_bridge_ && libp2p_bridge_->IsLibp2pConnectFailed() && libp2p_bridge_->Libp2pConnectMissingMic()) {
    return true;
  }
  return p2p_bridge_.P2pConnectMissingMic();
}

void CallSessionManager::PollP2pConnectHealth() {
  if (libp2p_bridge_) {
    libp2p_bridge_->PollLibp2pConnectHealth();
  }
  p2p_bridge_.PollP2pConnectHealth();
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  if (libp2p_bridge_ && libp2p_bridge_->MediaAttempted(call_id)) {
    return libp2p_bridge_->RetryLibp2pMedia(call_id);
  }
  return p2p_bridge_.RetryP2pMedia(call_id);
}

void CallSessionManager::PollPendingSfuAttach() {
  topology_.PollPendingSfuAttach();
}

Roe<std::optional<CallSession>> CallSessionManager::SessionForCall(const std::string& call_id) const {
  return sessions_.LoadSession(call_id);
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

void CallSessionManager::AbandonOrphanedCallsAfterRestart() {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }

  // Copy ids first — LeaveCall / EndCallLocal mutate the store.
  std::vector<std::string> joined_calls;
  std::vector<CallSession> other_live;
  if (auto active = sessions_.ListActiveSessions(); active) {
    for (const CallSession& session : *active) {
      auto participant = sessions_.FindParticipant(session.call_id, *local);
      if (participant && participant->has_value() &&
          (*participant)->state == CallParticipantState::Joined) {
        joined_calls.push_back(session.call_id);
      } else {
        other_live.push_back(session);
      }
    }
  }

  for (const std::string& call_id : joined_calls) {
    if (auto left = LeaveCall(call_id); !left) {
      auto session = sessions_.LoadSession(call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }
  for (CallSession& session : other_live) {
    if (session.state == CallSessionState::Ended) {
      continue;
    }
    (void)EndCallLocal(session, std::nullopt);
  }

  if (auto pending = sessions_.ListPendingInvitesForInvitee(*local); pending) {
    for (const PendingCallInvite& invite : *pending) {
      if (invite.status != "pending") {
        continue;
      }
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      auto session = sessions_.LoadSession(invite.call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }

  NotifyRingChanged();
}

bool CallSessionManager::MediaAttemptedThisProcess(const std::string& call_id) const {
  if (libp2p_bridge_ && libp2p_bridge_->MediaAttempted(call_id)) {
    return true;
  }
  return p2p_bridge_.MediaAttempted(call_id);
}

void CallSessionManager::ClearMediaCallbacks() {
  p2p_bridge_.ClearMediaCallbacks();
}

Roe<void> CallSessionManager::ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms) {
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
    // Already joined this call — ignore redelivered / duplicate invites (would demote to Ringing
    // and flicker ring chrome over the in-call banner).
    if (auto existing = sessions_.FindParticipant(invite->call_id, *local);
        existing && existing->has_value() && (*existing)->state == CallParticipantState::Joined) {
      log().info << "CallInvite ignored; already joined call_id=" << invite->call_id;
      return {};
    }
    const int64_t now = util::NowUnixMs();
    if (CallSessionLogic::ShouldDropStaleInvite(*invite, now, relay_created_at_ms, relay_server_time_ms)) {
      log().warning << "CallInvite dropped as stale call_id=" << invite->call_id
                    << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                    << " now=" << now
                    << " relay_created=" << (relay_created_at_ms ? std::to_string(*relay_created_at_ms) : "none")
                    << " relay_now=" << (relay_server_time_ms ? std::to_string(*relay_server_time_ms) : "none");
      return {};
    }
    // Near-live invite: re-arm ring TTL from local receipt (skew-safe). Relay-age gate already
    // dropped long-backlogged inbox rows when create/now samples were present.
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

    // Seed full roster from invite when present; fall back to inviter+self.
    if (!invite->participants.empty()) {
      for (const CallRosterEntry& entry : invite->participants) {
        if (entry.identity.empty()) {
          continue;
        }
        CallParticipant row;
        row.call_id = invite->call_id;
        row.identity = entry.identity;
        row.state = entry.state;
        row.media.audio_muted = entry.audio_muted;
        row.media.video_enabled = entry.video_enabled;
        if (entry.identity == *local) {
          row.state = CallParticipantState::Ringing;
        } else if (entry.identity == pending.inviter_identity &&
                   row.state != CallParticipantState::Joined) {
          row.state = CallParticipantState::Joined;
        }
        if (row.state == CallParticipantState::Joined) {
          // Prefer earlier stamp than late acceptors so soft-migrate initiator detection works.
          row.joined_at = session.created_at > 0 ? session.created_at : 1;
        }
        (void)sessions_.UpsertParticipant(row);
      }
    }
    CallParticipant inviter;
    inviter.call_id = invite->call_id;
    inviter.identity = pending.inviter_identity;
    inviter.state = CallParticipantState::Joined;
    inviter.joined_at = session.created_at > 0 ? session.created_at : 1;
    (void)sessions_.UpsertParticipant(inviter);
    CallParticipant self;
    self.call_id = invite->call_id;
    self.identity = *local;
    self.state = CallParticipantState::Ringing;
    (void)sessions_.UpsertParticipant(self);
    PrefetchReachForIdentity(prefetch_reach_, pending.inviter_identity);
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
      auto joined_after = sessions_.CountJoined(accept->call_id);
      const size_t n_joined = joined_after ? *joined_after : 0;
      if (!topology_.OnRemoteAcceptJoined(accept->call_id, n_joined, identity)) {
        ScheduleStartDirectMedia(accept->call_id, identity, true);
      }
      PrefetchReachForIdentity(prefetch_reach_, identity);
      // Fan roster so co-invitees / other joined peers see accurate N before SFU/P2P.
      if (auto roster = BuildRosterDetail(accept->call_id); roster) {
        if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
          (void)FanOutToJoinedAndRinging(accept->call_id, CallControlType::CallRoster, *roster_json, "Call roster",
                                         *local);
        }
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
    if (identity == *local) {
      // Ejected after failed soft-migrate (or remote Leave for us): clear local chrome/media.
      topology_.ClearSfuAttachWait();
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        std::optional<int64_t> duration;
        if ((*session)->created_at > 0) {
          duration = util::NowUnixMs() - (*session)->created_at;
        }
        return EndCallLocal(**session, duration);
      }
      StopMediaIfCall(leave->call_id);
      NotifyRingChanged();
      return {};
    }
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
      if (entry.identity.empty()) {
        continue;
      }
      // Do not resurrect Left/Declined peers from a stale roster fan-out (blocks re-invite).
      if (entry.state == CallParticipantState::Joined || entry.state == CallParticipantState::Ringing ||
          entry.state == CallParticipantState::Invited) {
        if (auto existing = sessions_.FindParticipant(roster->call_id, entry.identity);
            existing && existing->has_value()) {
          const CallParticipantState prior = (*existing)->state;
          if (prior == CallParticipantState::Left || prior == CallParticipantState::Declined ||
              prior == CallParticipantState::Missed) {
            continue;
          }
        }
      }
      CallParticipant participant;
      participant.call_id = roster->call_id;
      participant.identity = entry.identity;
      participant.state = entry.state;
      participant.media.audio_muted = entry.audio_muted;
      participant.media.video_enabled = entry.video_enabled;
      (void)sessions_.UpsertParticipant(participant);
    }
    // Mid-call invite: CallAccept only reaches the inviter; initiator SoftMigrates when roster
    // shows N≥3 (V021 sticky initiator / V022 payer).
    if (auto joined = sessions_.CountJoined(roster->call_id); joined) {
      topology_.OnJoinedCountObserved(roster->call_id, *joined);
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
    return p2p_bridge_.OnRemoteSdp(*sdp, sender_identity);
  }
  case CallControlType::CallIce: {
    auto ice = CallControlCodec::DecodeIce(detail_json);
    if (!ice) {
      return ice.error();
    }
    return p2p_bridge_.OnRemoteIce(*ice);
  }
  case CallControlType::CallSfuAttach: {
    auto attach = CallControlCodec::DecodeSfuAttach(detail_json);
    if (!attach) {
      return attach.error();
    }
    (void)topology_.OnInboundSfuAttach(attach->call_id, *attach);
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallEnded: {
    auto ended = CallControlCodec::DecodeEnded(detail_json);
    if (!ended) {
      return ended.error();
    }
    (void)sessions_.UpdateInviteStatus(ended->call_id, *local, "expired");
    auto session = sessions_.LoadSession(ended->call_id);
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      return EndCallLocal(**session, ended->duration_ms);
    }
    NotifyRingChanged();
    return {};
  }
  case CallControlType::CallStarted:
    return {};
  }
  return {};
}


Roe<std::string> CallSessionManager::TopologyLocalIdentity() const {
  return LocalRelayIdentity();
}

Roe<void> CallSessionManager::TopologyLeaveCall(const std::string& call_id) {
  return LeaveCall(call_id);
}

Roe<void> CallSessionManager::TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                                     const std::string& detail_json, const std::string& display,
                                                     const std::string& skip_identity) {
  return FanOutToJoined(call_id, type, detail_json, display, skip_identity);
}

Roe<void> CallSessionManager::TopologySendDirect(const std::string& peer_identity, CallControlType type,
                                                 const std::string& detail_json, const std::string& display) {
  return SendCallDirectMessage(peer_identity, type, detail_json, display);
}

void CallSessionManager::TopologyNotifyRingChanged() {
  NotifyRingChanged();
}

void CallSessionManager::TopologySetLastMediaError(std::string message) {
  last_media_error_ = std::move(message);
}

void CallSessionManager::TopologyNoteMediaAttempted(const std::string& call_id) {
  if (libp2p_bridge_) {
    libp2p_bridge_->NoteMediaAttempted(call_id);
  }
  p2p_bridge_.NoteMediaAttempted(call_id);
}

void CallSessionManager::TopologyBindMediaCallId(const std::string& call_id) {
  p2p_bridge_.BindMediaCallId(call_id);
}

void CallSessionManager::TopologyClearMediaPeerIdentity() {
  p2p_bridge_.ClearMediaPeerIdentity();
}

Roe<std::string> CallSessionManager::P2pLocalIdentity() const {
  return LocalRelayIdentity();
}

Roe<void> CallSessionManager::P2pSendDirect(const std::string& peer_identity, CallControlType type,
                                            const std::string& detail_json, const std::string& display) {
  return SendCallDirectMessage(peer_identity, type, detail_json, display);
}

void CallSessionManager::P2pNotifyRingChanged() {
  NotifyRingChanged();
}

void CallSessionManager::P2pSetLastMediaError(std::string message) {
  last_media_error_ = std::move(message);
}

Roe<std::optional<std::string>> CallSessionManager::P2pPeerIdentityForCall(const std::string& call_id) const {
  return PeerIdentityForCall(call_id);
}

bool CallSessionManager::P2pIsAwaitingSfuRecovery() const {
  return topology_.IsAwaitingSfuRecovery();
}

void CallSessionManager::P2pOnGroupIceFailed(const std::string& call_id) {
  topology_.TryRecoverViaSfu(call_id);
}

void CallSessionManager::P2pClearAwaitingSfuRecovery() {
  topology_.ClearAwaitingSfuRecovery();
}

} // namespace pbr
