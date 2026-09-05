#include "feature/calls/CallSessionManager.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SessionKeyDeriver.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/messaging/SendRelayOptions.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactJson.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

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

} // namespace

void PrefetchReachForIdentity(const CallSessionManager::PrefetchPeerReachFn& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       CallDeliveryPorts delivery, IPskSessionStore& psk_store, CallMediaEngine& media)
    : store_(store), contacts_(contacts), identity_(identity), sessions_(sessions), media_keys_(media_keys),
      delivery_(std::move(delivery)), psk_store_(psk_store), media_(media),
      topology_(*this, sessions, contacts, media) {
  redirectLogger("CallSessionManager");
  topology_.SetMediaKeyStore(&media_keys_);
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  topology_.SetMediaRelayDeps(std::move(deps));
}

void CallSessionManager::SetCallMediaBridge(CallMediaBridge* bridge) {
  call_media_bridge_ = bridge;
}

void CallSessionManager::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity,
                                                  bool offerer) {
  if (!call_media_bridge_) {
    log().error << "ScheduleStartDirectMedia: mesh media bridge not configured call_id=" << call_id;
    last_media_error_ = "Call media unavailable";
    NotifyRingChanged();
    return;
  }
  log().info << "ScheduleStartDirectMedia libp2p role=" << (offerer ? "offerer" : "answerer")
                << " call_id=" << call_id << " peer=" << peer_identity;
  if (offerer) {
    call_media_bridge_->ScheduleStartMediaAsOfferer(call_id, peer_identity);
  } else {
    call_media_bridge_->ScheduleStartMediaAsAnswerer(call_id, peer_identity);
  }
}

CallHopHealth CallSessionManager::HopHealth() const {
  if (!topology_.IsSfuAttached()) {
    return {};
  }
  // Topology holds relay deps; sample via public IsSfuAttached + Media PathPressure elsewhere.
  return topology_.HopHealth();
}

bool CallSessionManager::IsSfuAttached() const {
  return topology_.IsSfuAttached();
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
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || !(*session)->video_allowed) {
      return Error("Video is not allowed for this call");
    }
  }
  topology_.RefreshAdaptation(call_id, enabled);
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

Roe<void> CallSessionManager::RequestVideoRefresh(const std::string& call_id,
                                                 const std::string& publisher_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (call_id.empty()) {
    return Error("No active call");
  }
  if (publisher_identity.empty() || publisher_identity == *local) {
    media_.RequestVideoKeyframe();
    return {};
  }
  CallVideoRefreshDetail detail;
  detail.call_id = call_id;
  detail.identity = publisher_identity;
  auto encoded = CallControlCodec::EncodeVideoRefresh(detail);
  if (!encoded) {
    return encoded.error();
  }
  return SendCallDirectMessage(publisher_identity, CallControlType::CallVideoRefresh, *encoded, "");
}

std::optional<std::string> CallSessionManager::TakeLastMediaError() {
  auto out = std::move(last_media_error_);
  last_media_error_.reset();
  return out;
}

void CallSessionManager::SetOnRingChanged(RingChangedFn callback) {
  on_ring_changed_ = std::move(callback);
}

void CallSessionManager::SetOnRingChangedMesh(RingChangedFn callback) {
  on_ring_changed_mesh_ = std::move(callback);
}

void CallSessionManager::SetPrefetchPeerReachability(PrefetchPeerReachFn callback) {
  prefetch_reach_ = std::move(callback);
}

void CallSessionManager::SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback) {
  local_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::SetLocalPeerCapsProvider(LocalPeerCapsFn callback) {
  local_peer_caps_ = std::move(callback);
}

void CallSessionManager::SetLocalMeshPeerIdProvider(LocalMeshPeerIdFn callback) {
  local_mesh_peer_id_ = std::move(callback);
}

void CallSessionManager::SetRegisterPeerListenMultiaddrs(RegisterPeerListenMultiaddrsFn callback) {
  register_peer_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay) {
  if (peer_id.empty()) {
    return;
  }
  const bool was = PeerHasMediaRelayCap(peer_id);
  peer_media_relay_caps_[peer_id] = media_relay;
  // Phone initiator SoftMigrate may have run before this desktop Accept — nudge re-pick.
  if (media_relay && !was) {
    if (auto active = ActiveLocalCall(); active && active->has_value()) {
      if (topology_.IsSfuAttachWaitActive() || !topology_.IsSfuAttached()) {
        const std::string call_id = (*active)->call_id;
        AppRuntime::PostWorkerNormal([this, call_id]() {
          if (topology_.IsOnSfuForCall(call_id)) {
            return;
          }
          (void)topology_.MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
          NotifyRingChanged();
        });
      }
    }
  }
}

void CallSessionManager::NoteMeshPeerIdForRelay(const std::string& relay_identity,
                                                  const std::string& peer_id) {
  if (relay_identity.empty() || peer_id.empty() || !IsAccountIdentityValue(relay_identity)) {
    return;
  }
  peer_id_to_relay_[peer_id] = relay_identity;
  if (call_media_bridge_) {
    call_media_bridge_->NotePeerIdRelayMapping(peer_id, relay_identity);
  }
  auto found = contacts_.FindByIdentity(relay_identity, ContactIdKind::Account);
  if (!found || !found->has_value()) {
    // Non-contact call participants: in-memory map + bridge rebind is enough.
    log().info << "NoteMeshPeerIdForRelay map-only (no contact) peer_id=" << peer_id
               << " account=" << relay_identity;
    return;
  }
  Contact contact = **found;
  if (PeerIdFromContact(contact) == peer_id) {
    return;
  }
  bool has_peer = false;
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::PeerId && id.value == peer_id) {
      has_peer = true;
      break;
    }
  }
  if (has_peer) {
    return;
  }
  contact.ids.push_back(ContactId{ContactIdKind::PeerId, peer_id, false});
  contact.remote.ids = contact.ids;
  PromoteFlatFieldsToNested(contact);
  SyncContactMirrors(contact);
  if (auto saved = contacts_.Upsert(contact); !saved) {
    log().warning << "NoteMeshPeerIdForRelay contact upsert failed account=" << relay_identity
                  << " peer=" << peer_id << " err=" << saved.error().message;
    return;
  }
  log().info << "NoteMeshPeerIdForRelay learned peer_id=" << peer_id << " account=" << relay_identity;
}

bool CallSessionManager::PeerHasMediaRelayCap(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return false;
  }
  const auto it = peer_media_relay_caps_.find(peer_id);
  return it != peer_media_relay_caps_.end() && it->second;
}

std::vector<std::string> CallSessionManager::ListMediaRelayCapablePeerIds() const {
  std::vector<std::string> out;
  out.reserve(peer_media_relay_caps_.size());
  for (const auto& [peer_id, enabled] : peer_media_relay_caps_) {
    if (enabled && !peer_id.empty()) {
      out.push_back(peer_id);
    }
  }
  return out;
}

void CallSessionManager::NotifyRingChanged() {
  if (on_ring_changed_mesh_) {
    on_ring_changed_mesh_();
  }
  if (on_ring_changed_) {
    on_ring_changed_();
  }
}

Roe<std::string> CallSessionManager::LocalRelayIdentity() const {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  return identity->account_id;
}

Roe<void> CallSessionManager::SendCallDirectMessage(const std::string& peer_identity, const CallControlType type,
                                                    const std::string& detail_json, const std::string& display) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, ContactIdKind::Account)) {
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
  Object payload;
  payload.set("control_type", CallControlTypeToWire(type));
  payload.set("detail", detail_json);
  opts.payload_json = DumpJson(payload);
  opts.generation = "system";
  opts.update_preview = false;
  // Call-control must not sit behind PollInbox on Normal workers (MediaKey + Accept).
  opts.critical_lane = true;
  if (!delivery_.send_user_message) {
    return Error("Call delivery not bound");
  }
  auto sent = delivery_.send_user_message(thread->id, display, opts);
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
    entry.joined_at = row.joined_at;
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
    } else {
      log().info << "FanOutToJoined queued peer=" << row.identity
                 << " type=" << CallControlTypeToWire(type);
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
  target_key.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
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

void CallSessionManager::StopCallMedia(const std::string& call_id) {
  StopMediaIfCall(call_id);
}

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  // Detach media_relay BEFORE JoinCaptureThread. Capture may be blocked in BlockingWrite on the
  // SFU stream; OnMediaStopped closes it so Stop/quit can finish (group-call hang).
  topology_.OnMediaStopped(call_id);
  if (call_media_bridge_) {
    call_media_bridge_->StopMeshMedia(call_id);
  }
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

Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const bool video_allowed,
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
  // P001: block outbound dial when initiation offer > 0 and payment rails unavailable.
  if (initiation_billing_) {
    for (const std::string& invitee : invitee_identities) {
      if (invitee.empty() || invitee == *local || initiation_billing_->IsOpen(invitee)) {
        continue;
      }
      const InitiationPeerBilling billing = initiation_billing_->Get(invitee);
      const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
      if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
        return payable.error();
      }
    }
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
  session.media_mode = video_allowed ? CallMediaMode::Video : CallMediaMode::Voice;
  session.video_allowed = video_allowed;
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
  started.media_mode = session.media_mode;
  started.video_allowed = video_allowed;
  auto started_detail = CallControlCodec::EncodeStarted(started);
  if (!started_detail) {
    return started_detail.error();
  }
  const std::string started_text =
      video_allowed ? "Video call started" : "Voice call started";
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
    return Error("Adding a guest needs call hosting help (enable Help host calls on a computer that's helping the network)");
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
  pending.video_allowed = (*session)->video_allowed;
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
  invite.video_allowed = (*session)->video_allowed;
  invite.origin_thread_id = (*session)->origin_thread_id;
  invite.origin_group_id = (*session)->origin_group_id;
  invite.sfu_hint = (*session)->sfu_hint;
  invite.expires_at = pending.expires_at;
  if (auto roster = BuildRosterDetail(call_id); roster) {
    invite.participants = std::move(roster->participants);
  }
  // Embed epoch key in invite — separate CallMediaKey inbox rows are often ingested without
  // call-control side effects (BenignDuplicate / classifier), so Accept never sees the key.
  invite.media_epoch = (*session)->media_epoch;
  invite.media_key_id = (*session)->media_key_id;
  if (auto key_bytes = media_keys_.LoadEpochKey(call_id, (*session)->media_epoch);
      key_bytes && key_bytes->has_value()) {
    if (auto session_key = ResolvePeerSessionKey(invitee_identity)) {
      if (auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, **key_bytes, call_id, invite.media_epoch,
                                                       invite.media_key_id)) {
        invite.wrapped_key_b64 = *wrapped;
      } else {
        log().warning << "CallInvite media key wrap failed call_id=" << call_id
                      << " err=" << wrapped.error().message;
      }
    } else {
      log().warning << "CallInvite media key skip; no peer session key peer=" << invitee_identity;
    }
  }
  if (local_listen_multiaddrs_) {
    invite.listen_multiaddrs = local_listen_multiaddrs_();
  }
  if (local_mesh_peer_id_) {
    invite.libp2p_peer_id = local_mesh_peer_id_();
  }
  if (invite.libp2p_peer_id.empty()) {
    const auto ids = PeerIdsFromListenMultiaddrs(invite.listen_multiaddrs);
    if (!ids.empty()) {
      invite.libp2p_peer_id = ids.front();
    }
  }
  if (local_peer_caps_) {
    invite.caps = local_peer_caps_();
    invite.caps.present = true;
  }
  if (initiation_billing_ && !initiation_billing_->IsOpen(invitee_identity)) {
    const InitiationPeerBilling billing = initiation_billing_->Get(invitee_identity);
    const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
    if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
      return payable.error();
    }
    invite.offer_amount_minor = offer;
    invite.floor_minor = billing.floor_minor;
    invite.currency = kPricingCurrencyId;
    if (offer > 0) {
      (void)initiation_billing_->MarkOffered(invitee_identity, offer, billing.floor_minor);
    }
  }
  auto detail = CallControlCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  PrefetchReachForIdentity(prefetch_reach_, invitee_identity);
  if (auto sent = SendCallDirectMessage(invitee_identity, CallControlType::CallInvite, *detail, display); !sent) {
    return sent;
  }
  log().info << "CallInvite sent call_id=" << call_id << " peer=" << invitee_identity
             << " media_key_embedded=" << (invite.wrapped_key_b64.empty() ? 0 : 1)
             << " listen_addrs=" << invite.listen_multiaddrs.size();
  return {};
}

int64_t CallSessionManager::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  if (!initiation_billing_ || peer_identity.empty()) {
    return 0;
  }
  return initiation_billing_->Get(peer_identity).offer_minor;
}

void CallSessionManager::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  pending_accept_charge_ = decision;
  pending_accept_charge_set_ = true;
}

Roe<void> CallSessionManager::AcceptInvite(const std::string& call_id,
                                           InitiationChargeDecision charge_decision) {
  if (pending_accept_charge_set_) {
    charge_decision = pending_accept_charge_;
    pending_accept_charge_set_ = false;
    pending_accept_charge_ = InitiationChargeDecision::Waive;
  }
  log().info << "AcceptInvite start call_id=" << call_id
             << " charge=" << InitiationChargeDecisionToWire(charge_decision);
  auto local = LocalRelayIdentity();
  if (!local) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << local.error().message;
    return local.error();
  }
  SweepExpiredInvites();
  if (auto cleared = LeaveCallIfActiveExcept(call_id); !cleared) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << cleared.error().message;
    return cleared.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Pending call invite not found";
    return Error("Pending call invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, *local, "expired");
    NotifyRingChanged();
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Call invite expired";
    return Error("Call invite expired");
  }

  const std::string inviter = (*pending)->inviter_identity;
  int64_t offer_minor = 0;
  if (initiation_billing_) {
    offer_minor = initiation_billing_->Get(inviter).offer_minor;
  }
  if (charge_decision == InitiationChargeDecision::TakeAll && offer_minor > 0 && !PaymentRailsAvailable()) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=payment_unavailable take_all";
    return Error("payment_unavailable: cannot collect charge yet");
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
    row.video_allowed = (*pending)->video_allowed;
    row.state = CallSessionState::Ringing;
    row.created_at = (*pending)->created_at;
    row.media_epoch = 1;
    row.sfu_hint = (*pending)->sfu_hint;
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t joined_count = joined ? *joined : 0;
  if (!CallSessionLogic::CanAcceptJoin(joined_count)) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Call is full";
    return Error("Call is full");
  }

  const int64_t now = util::NowUnixMs();
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << saved.error().message;
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << saved.error().message;
    return saved.error();
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "accepted");

  // Runs on Accept worker thread (never Browser IO). Do not wait on ListenOn / PollInbox here.
  // ScheduleStart* only posts StartSfu onto UI — never run the engine on this thread.
  auto joined_after = sessions_.CountJoined(call_id);
  size_t n_joined = joined_after ? *joined_after : 0;
  // Group invite seeds other callees as Ringing. CountJoined alone stays at 2 until they Accept,
  // so we SoftMigrate/WaitForAttach when Joined+Ringing+Invited already implies N≥3.
  if (auto all = sessions_.ListParticipants(call_id); all) {
    size_t n_active = 0;
    for (const CallParticipant& p : *all) {
      if (p.state == CallParticipantState::Joined || p.state == CallParticipantState::Ringing ||
          p.state == CallParticipantState::Invited) {
        ++n_active;
      }
    }
    if (n_active > n_joined) {
      n_joined = n_active;
    }
  }
  if (!topology_.OnLocalAcceptJoined(call_id, n_joined, row.sfu_hint)) {
    if (row.sfu_hint && !row.sfu_hint->empty()) {
      row.sfu_hint.reset();
      (void)sessions_.UpsertSession(row);
    }
    ScheduleStartDirectMedia(call_id, inviter, false);
  }
  NotifyRingChanged();

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = *local;
  accept.video_enabled = false;
  // P001: recipient chooses waive (0) or take_all (rails checked above).
  if (initiation_billing_) {
    accept.offer_amount_minor = offer_minor;
    accept.charge_decision = InitiationChargeDecisionToWire(charge_decision);
    (void)initiation_billing_->MarkOpen(inviter);
  }
  if (local_listen_multiaddrs_) {
    accept.listen_multiaddrs = local_listen_multiaddrs_();
  }
  if (local_mesh_peer_id_) {
    accept.libp2p_peer_id = local_mesh_peer_id_();
  }
  if (accept.libp2p_peer_id.empty()) {
    const auto ids = PeerIdsFromListenMultiaddrs(accept.listen_multiaddrs);
    if (!ids.empty()) {
      accept.libp2p_peer_id = ids.front();
    }
  }
  if (local_peer_caps_) {
    accept.caps = local_peer_caps_();
    accept.caps.present = true;
  }
  auto detail = CallControlCodec::EncodeAccept(accept);
  if (!detail) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << detail.error().message;
    return detail.error();
  }
  if (auto sent = SendCallDirectMessage(inviter, CallControlType::CallAccept, *detail, "Call accepted"); !sent) {
    log().warning << "CallAccept send failed call_id=" << call_id << " err=" << sent.error().message;
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << sent.error().message;
    return sent.error();
  }

  // Pull CallMediaKey ASAP — do not wait for the next UI-tick poll (Accept worker path).
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }

  // Roster / prefetch after Accept returns — keep Accept worker snappy (no Accept hang UX).
  const std::string accept_call_id = call_id;
  const std::string accept_inviter = inviter;
  const std::string accept_local = *local;
  AppRuntime::PostWorkerNormal([this, accept_call_id, accept_inviter, accept_local]() {
    if (auto roster = BuildRosterDetail(accept_call_id); roster) {
      if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
        (void)SendCallDirectMessage(accept_inviter, CallControlType::CallRoster, *roster_json, "Call roster");
        (void)FanOutToJoinedAndRinging(accept_call_id, CallControlType::CallRoster, *roster_json, "Call roster",
                                       accept_local);
      }
    }
    PrefetchReachForIdentity(prefetch_reach_, accept_inviter);
  });

  log().info << "AcceptInvite end call_id=" << call_id << " ok";
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

Roe<std::optional<bool>> CallSessionManager::VideoAllowedForCall(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session) {
    return session.error();
  }
  if (!session->has_value()) {
    return std::optional<bool>{};
  }
  return std::optional<bool>{(*session)->video_allowed};
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

bool CallSessionManager::IsSoftMigrateInFlight() const {
  return topology_.IsSoftMigrateInFlight();
}

bool CallSessionManager::IsSfuAttachWaitActive() const {
  return topology_.IsSfuAttachWaitActive();
}

bool CallSessionManager::IsP2pConnectFailed() const {
  return call_media_bridge_ && call_media_bridge_->IsMeshConnectFailed();
}

bool CallSessionManager::P2pConnectMissingMic() const {
  return call_media_bridge_ && call_media_bridge_->IsMeshConnectFailed() && call_media_bridge_->MeshConnectMissingMic();
}

void CallSessionManager::PollP2pConnectHealth() {
  if (call_media_bridge_) {
    call_media_bridge_->PollMeshConnectHealth();
  }
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  if (call_media_bridge_ && call_media_bridge_->MediaAttempted(call_id)) {
    return call_media_bridge_->RetryMeshMedia(call_id);
  }
  return Error("Call media retry unavailable");
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
  return call_media_bridge_ && call_media_bridge_->MediaAttempted(call_id);
}

void CallSessionManager::ClearMediaCallbacks() {
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
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }

  switch (*type) {
  case CallControlType::CallInvite:
    return HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms, relay_server_time_ms,
                               *local);
  case CallControlType::CallAccept:
    return HandleInboundAccept(detail_json, sender_identity, *local);
  case CallControlType::CallDecline:
    return HandleInboundDecline(detail_json, sender_identity);
  case CallControlType::CallLeave:
    return HandleInboundLeave(detail_json, sender_identity, *local);
  case CallControlType::CallRoster:
    return HandleInboundRoster(detail_json);
  case CallControlType::CallMediaKey:
    return HandleInboundMediaKey(detail_json, sender_identity);
  case CallControlType::CallSdp:
  case CallControlType::CallIce:
    log().debug << "Ignoring legacy call_sdp/call_ice from " << sender_identity;
    return {};
  case CallControlType::CallSfuAttach:
    return HandleInboundSfuAttach(detail_json);
  case CallControlType::CallSfuAttachFailed:
    return HandleInboundSfuAttachFailed(detail_json, sender_identity);
  case CallControlType::CallHopRefuse:
    return HandleInboundHopRefuse(detail_json);
  case CallControlType::CallVideoRefresh:
    return HandleInboundVideoRefresh(detail_json, sender_identity);
  case CallControlType::CallEnded:
    return HandleInboundEnded(detail_json, *local);
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

void CallSessionManager::TopologySetMediaActivity(std::string message) {
  media_activity_ = std::move(message);
}

void CallSessionManager::TopologyClearMediaActivity() {
  media_activity_.clear();
}

std::string CallSessionManager::PeekMediaActivity() const {
  return media_activity_;
}

void CallSessionManager::ClearMediaActivity() {
  media_activity_.clear();
}

void CallSessionManager::TopologyNoteMediaAttempted(const std::string& call_id) {
  if (call_media_bridge_) {
    call_media_bridge_->NoteMediaAttempted(call_id);
  }
}

void CallSessionManager::TopologyBindMediaCallId(const std::string& /*call_id*/) {
}

void CallSessionManager::TopologyClearMediaPeerIdentity() {
}

void CallSessionManager::TopologyReleaseDirectMedia() {
  if (call_media_bridge_) {
    call_media_bridge_->ReleaseDirectTransport();
  }
}

void CallSessionManager::TopologyRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
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

Roe<std::optional<std::string>> CallSessionManager::RelayIdentityForMeshPeerId(
    const std::string& call_id, const std::string& peer_id) const {
  if (peer_id.empty()) {
    return std::optional<std::string>{};
  }
  if (const auto it = peer_id_to_relay_.find(peer_id); it != peer_id_to_relay_.end()) {
    return std::optional<std::string>{it->second};
  }
  auto account_from_contact = [](const Contact& contact) -> std::string {
    if (auto account = ContactAccountId(contact)) {
      return *account;
    }
    return {};
  };
  // Prefer a call participant whose contact PeerId matches the inbound stream peer.
  if (!call_id.empty()) {
    auto participants = sessions_.ListParticipants(call_id);
    if (!participants) {
      return participants.error();
    }
    for (const CallParticipant& row : *participants) {
      if (row.identity.empty()) {
        continue;
      }
      auto found = contacts_.FindByIdentity(row.identity, ContactIdKind::Account);
      if (!found) {
        return found.error();
      }
      if (!found->has_value()) {
        continue;
      }
      if (PeerIdFromContact(**found) == peer_id) {
        return std::optional<std::string>{row.identity};
      }
    }
  }
  // Fallback: any contact with this PeerId (or /p2p/ PeerId in multiaddrs).
  auto listed = contacts_.List();
  if (!listed) {
    return listed.error();
  }
  for (const Contact& contact : *listed) {
    if (PeerIdFromContact(contact) != peer_id) {
      continue;
    }
    const std::string account = account_from_contact(contact);
    if (!account.empty()) {
      return std::optional<std::string>{account};
    }
  }
  return std::optional<std::string>{};
}

void CallSessionManager::P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) {
  if (call_id.empty() || peer_identity.empty()) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return;
  }
  const uint32_t epoch = (*session)->media_epoch;
  auto key_bytes = media_keys_.LoadEpochKey(call_id, epoch);
  if (!key_bytes || !key_bytes->has_value()) {
    log().warning << "P2pResendMediaKey missing key call_id=" << call_id << " epoch=" << epoch;
    return;
  }
  if (auto sent = SendMediaKeyToPeer(call_id, peer_identity, epoch, (*session)->media_key_id, **key_bytes); !sent) {
    log().warning << "P2pResendMediaKey send failed call_id=" << call_id << " err=" << sent.error().message;
    return;
  }
  log().info << "P2pResendMediaKey sent call_id=" << call_id << " peer=" << peer_identity
                << " epoch=" << epoch;
}

void CallSessionManager::P2pRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

bool CallSessionManager::P2pIsAwaitingSfuRecovery() const {
  return topology_.IsAwaitingSfuRecovery();
}

bool CallSessionManager::P2pExpectGroupSfuMigration(const std::string& call_id) const {
  if (call_id.empty()) {
    return false;
  }
  if (topology_.IsAwaitingSfuRecovery() || topology_.IsSfuAttached()) {
    return true;
  }
  // SoftMigrate can start on the hop when Joined+Ringing+Invited ≥ 3 while a guest's
  // CountJoined is still 2 (Samsung Accept not on roster yet). Treat that as expect.
  if (auto all = sessions_.ListParticipants(call_id); all) {
    size_t n_active = 0;
    for (const CallParticipant& p : *all) {
      if (p.state == CallParticipantState::Joined || p.state == CallParticipantState::Ringing ||
          p.state == CallParticipantState::Invited) {
        ++n_active;
      }
    }
    if (n_active >= 3) {
      return true;
    }
  } else if (auto n = sessions_.CountJoined(call_id); n && *n >= 3) {
    return true;
  }
  if (auto session = sessions_.LoadSession(call_id);
      session && *session && (*session)->sfu_hint && !(*session)->sfu_hint->empty()) {
    return true;
  }
  return false;
}

void CallSessionManager::P2pNoteExpectSfuAttach(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  topology_.BeginSfuAttachWait(call_id);
}

bool CallSessionManager::P2pIsSfuAttached() const {
  return topology_.IsSfuAttached();
}

void CallSessionManager::P2pClearAwaitingSfuRecovery() {
  topology_.ClearAwaitingSfuRecovery();
}

} // namespace pbr
