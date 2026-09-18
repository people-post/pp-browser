#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallMediaPaths.h"
#include "domain/messaging/CallListenAddrsLogic.h"
#include "domain/messaging/CallAnswererKickLogic.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SessionKeyDeriver.h"
#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/AnnounceLiveJoinHandoff.h"
#include "domain/messaging/BroadcastJoinTicket.h"
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

void PrefetchReachForIdentity(const CallSessionManager::PrefetchPeerReachFn& fn, const std::string& identity) {
  if (!identity.empty() && fn) {
    fn(identity);
  }
}

void NoteCapsForIdentity(CallSessionManager& sessions, ContactsStore& contacts,
                         const std::string& identity, const CallPeerCaps& caps,
                         const std::vector<std::string>& listen_multiaddrs) {
  if (!caps.present && listen_multiaddrs.empty()) {
    return;
  }
  std::vector<std::string> peer_ids = PeerIdsFromListenMultiaddrs(listen_multiaddrs);
  if (peer_ids.empty() && !identity.empty()) {
    if (auto found = contacts.FindByIdentity(identity, ContactIdKind::Account);
        found && found->has_value()) {
      const std::string peer_id = PeerIdFromContact(**found);
      if (!peer_id.empty()) {
        peer_ids.push_back(peer_id);
      }
    }
  }
  for (const std::string& peer_id : peer_ids) {
    if (caps.present) {
      sessions.NotePeerMediaRelayCap(peer_id, caps.media_relay);
    }
    if (IsAccountIdentityValue(identity)) {
      sessions.NoteMeshPeerIdForRelay(identity, peer_id);
    }
  }
}

} // namespace


CallSessionManager::CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                       CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                                       CallDeliveryPorts delivery, IPskSessionStore& psk_store, CallMediaEngine& media)
    : store_(store), contacts_(contacts), identity_(identity), sessions_(sessions), media_keys_(media_keys),
      delivery_(std::move(delivery)), psk_store_(psk_store), media_(media),
      topology_(sessions, contacts, media), broadcast_(sessions, contacts, media_keys),
      workflow_(store, identity, sessions, media_keys) {
  redirectLogger("CallSessionManager");
  topology_.SetMediaKeyStore(&media_keys_);
  BindTopologyHostPorts();
  BroadcastSessionCoordinator::HostPorts ports;
  ports.local_relay_identity = [this]() { return LocalRelayIdentity(); };
  ports.local_mesh_peer_id = [this]() -> std::string {
    if (!local_mesh_peer_id_) {
      return {};
    }
    return local_mesh_peer_id_();
  };
  ports.notify_ring_changed = [this]() { NotifyRingChanged(); };
  ports.sweep_expired_invites = [this]() { SweepExpiredInvites(); };
  ports.leave_call_if_active_except = [this](const std::string& keep) {
    return LeaveCallIfActiveExcept(keep);
  };
  ports.on_announce_viewer_joined = [this](const std::string& call_id,
                                           const std::optional<std::string>& sfu_hint) {
    return topology_.OnAnnounceViewerJoined(call_id, sfu_hint);
  };
  broadcast_.SetHostPorts(std::move(ports));
  BindWorkflowHostPorts();
}

void CallSessionManager::BindTopologyHostPorts() {
  CallTopologyController::HostPorts ports;
  ports.local_relay_identity = [this]() { return TopologyLocalIdentity(); };
  ports.leave_call = [this](const std::string& call_id) { return TopologyLeaveCall(call_id); };
  ports.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return TopologyFanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) {
    return TopologySendDirect(peer, type, detail, display);
  };
  ports.notify_ring_changed = [this]() { TopologyNotifyRingChanged(); };
  ports.set_last_media_error = [this](std::string message) { TopologySetLastMediaError(std::move(message)); };
  ports.set_media_activity = [this](std::string message) { TopologySetMediaActivity(std::move(message)); };
  ports.clear_media_activity = [this]() { TopologyClearMediaActivity(); };
  ports.note_media_attempted = [this](const std::string& call_id) { TopologyNoteMediaAttempted(call_id); };
  ports.bind_media_call_id = [this](const std::string& call_id) { TopologyBindMediaCallId(call_id); };
  ports.clear_media_peer_identity = [this]() { TopologyClearMediaPeerIdentity(); };
  ports.release_direct_media = [this]() { TopologyReleaseDirectMedia(); };
  ports.request_inbox_sync = [this]() { TopologyRequestInboxSync(); };
  topology_.SetHostPorts(std::move(ports));
}

void CallSessionManager::BindWorkflowHostPorts() {
  CallSessionWorkflow::HostPorts ports;
  ports.wire.local_relay_identity = [this]() { return LocalRelayIdentity(); };
  ports.wire.notify_ring_changed = [this]() { NotifyRingChanged(); };
  ports.wire.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) {
    return SendCallDirectMessage(peer, type, detail, display);
  };
  ports.wire.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return FanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.wire.fan_out_joined_and_ringing = [this](const std::string& call_id, CallControlType type,
                                            const std::string& detail, const std::string& display,
                                            const std::string& skip) {
    return FanOutToJoinedAndRinging(call_id, type, detail, display, skip);
  };
  ports.wire.append_origin_history = [this](const std::string& thread_id, CallControlType type,
                                       const std::string& text, const std::string& detail) {
    return AppendOriginHistory(thread_id, type, text, detail);
  };
  ports.wire.build_roster_detail = [this](const std::string& call_id) { return BuildRosterDetail(call_id); };
  ports.duplex.stop_media_if_call = [this](const std::string& call_id) { StopMediaIfCall(call_id); };
  ports.duplex.schedule_start_direct = [this](const std::string& call_id, const std::string& peer, bool offerer) {
    ScheduleStartDirectMedia(call_id, peer, offerer);
  };
  ports.duplex.on_media_key_ready = [this](const std::string& call_id) {
    if (direct_media_.on_media_key_ready) {
      direct_media_.on_media_key_ready(call_id);
    }
  };
  ports.duplex.media_is_active = [this]() { return media_.IsActive(); };
  ports.duplex.media_is_sfu_mode = [this]() { return media_.IsSfuMode(); };
  ports.duplex.media_active_call_id = [this]() { return media_.ActiveCallId(); };
  ports.duplex.media_request_keyframe = [this]() { media_.RequestVideoKeyframe(); };
  ports.duplex.media_stop = [this]() { media_.Stop(); };
  ports.hop.on_local_accept_joined = [this](const std::string& call_id, size_t n,
                                        const std::optional<std::string>& hint) {
    return topology_.OnLocalAcceptJoined(call_id, n, hint);
  };
  ports.hop.on_remote_accept_joined = [this](const std::string& call_id, size_t n, const std::string& peer) {
    return topology_.OnRemoteAcceptJoined(call_id, n, peer);
  };
  ports.hop.on_joined_count_observed = [this](const std::string& call_id, size_t n) {
    topology_.OnJoinedCountObserved(call_id, n);
  };
  ports.hop.clear_sfu_attach_wait = [this]() { topology_.ClearSfuAttachWait(); };
  ports.hop.on_inbound_sfu_attach = [this](const std::string& call_id, const CallSfuAttachDetail& d) {
    return topology_.OnInboundSfuAttach(call_id, d);
  };
  ports.hop.on_inbound_sfu_attach_failed = [this](const CallSfuAttachFailedDetail& d) {
    topology_.OnInboundSfuAttachFailed(d);
  };
  ports.hop.on_inbound_hop_refuse = [this](const CallHopRefuseDetail& d) { topology_.OnInboundHopRefuse(d); };
  ports.hop.is_on_sfu_for_call = [this](const std::string& call_id) {
    return topology_.IsOnSfuForCall(call_id);
  };
  ports.hop.has_media_relay_hop_candidates = [this]() {
    return topology_.HasMediaRelayHopCandidates();
  };
  ports.wire.clear_media_activity = [this]() { TopologyClearMediaActivity(); };
  ports.wire.sync_inbox_from_wake = [this]() {
    if (delivery_.sync_inbox_from_wake) {
      delivery_.sync_inbox_from_wake(true);
    }
  };
  ports.chrome.note_direct_connecting = [this](const std::string& call_id) {
    if (lifecycle_ports_.set_direct_connecting) {
      lifecycle_ports_.set_direct_connecting(call_id);
    }
  };
  ports.chrome.accepting_call_id = [this]() {
    return lifecycle_ports_.accepting_call_id ? lifecycle_ports_.accepting_call_id() : std::string{};
  };
  ports.chrome.active_call_id = [this]() {
    return lifecycle_ports_.active_call_id ? lifecycle_ports_.active_call_id() : std::string{};
  };
  ports.chrome.apply_remote_ended = [this](const std::string& call_id) {
    if (lifecycle_ports_.apply_remote_ended) {
      lifecycle_ports_.apply_remote_ended(call_id);
    }
  };
  ports.chrome.is_outbound_calling = [this]() {
    return lifecycle_ports_.is_outbound_calling && lifecycle_ports_.is_outbound_calling();
  };
  ports.reach.register_peer_listen = [this](const std::string& identity, const std::vector<std::string>& mas) {
    if (register_peer_listen_multiaddrs_) {
      register_peer_listen_multiaddrs_(identity, mas);
    }
  };
  ports.reach.note_caps_for_identity = [this](const std::string& identity, const CallPeerCaps& caps,
                                        const std::vector<std::string>& listen) {
    NoteCapsForIdentity(*this, contacts_, identity, caps, listen);
  };
  ports.reach.prefetch_reach = [this](const std::string& identity) {
    PrefetchReachForIdentity(prefetch_reach_, identity);
  };
  ports.reach.note_mesh_peer_id_for_relay = [this](const std::string& relay, const std::string& peer_id) {
    NoteMeshPeerIdForRelay(relay, peer_id);
  };
  ports.reach.resolve_peer_session_key = [this](const std::string& peer) { return ResolvePeerSessionKey(peer); };
  ports.reach.send_media_key = [this](const std::string& call_id, const std::string& peer, uint32_t epoch,
                                const std::string& key_id, const ByteVector& key) {
    return SendMediaKeyToPeer(call_id, peer, epoch, key_id, key);
  };
  ports.reach.local_listen_multiaddrs = [this]() -> std::vector<std::string> {
    return local_listen_multiaddrs_ ? local_listen_multiaddrs_() : std::vector<std::string>{};
  };
  ports.reach.local_peer_caps = [this]() -> CallPeerCaps {
    return local_peer_caps_ ? local_peer_caps_() : CallPeerCaps{};
  };
  ports.reach.local_mesh_peer_id = [this]() -> std::string {
    return local_mesh_peer_id_ ? local_mesh_peer_id_() : std::string{};
  };
  workflow_.SetHostPorts(std::move(ports));
}

void CallSessionManager::SetInitiationBillingStore(InitiationBillingStore* store) {
  workflow_.SetInitiationBillingStore(store);
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  topology_.SetMediaRelayDeps(std::move(deps));
}

void CallSessionManager::SetDirectMediaPorts(CallDirectMediaPorts ports) {
  direct_media_ = std::move(ports);
  BindWorkflowHostPorts();
}

void CallSessionManager::SetLifecyclePorts(CallSessionLifecyclePorts ports) {
  lifecycle_ports_ = std::move(ports);
  BindWorkflowHostPorts();
}

void CallSessionManager::SetTopologyHopArmingPorts(CallHopArmingPorts ports) {
  topology_.SetHopArmingPorts(std::move(ports));
}

void CallSessionManager::SetMediaSeatPorts(CallMediaSeatPorts ports) {
  media_seat_ports_ = std::move(ports);
  BindWorkflowHostPorts();
}

void CallSessionManager::SetTopologySeatPorts(CallTopologySeatPorts ports) {
  topology_.SetSeatPorts(std::move(ports));
}

CallMediaSeatPorts CallSessionManager::MakeSeatPorts(CallMediaSeat* seat) {
  CallMediaSeatPorts ports;
  if (!seat) {
    return ports;
  }
  ports.release = [seat](const std::string& call_id) { seat->Release(call_id); };
  ports.bind_hop_for_attach = [seat](const std::string& call_id) {
    if (call_id.empty()) {
      return;
    }
    CallHopPath::Ops ops;
    ops.acquire = [seat](const std::string& cid) { return seat->Acquire(cid); };
    ops.allows_path_op = [seat](const CallMediaSeat::Token& token) {
      return seat->AllowsPathOp(token);
    };
    (void)CallHopPath(std::move(ops)).BindForAttach(call_id);
  };
  return ports;
}

void CallSessionManager::TopologyOnMediaStoppedForSeat(const std::string& call_id) {
  topology_.OnMediaStopped(call_id);
}

void CallSessionManager::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity,
                                                  bool offerer) {
  const bool allows =
      !lifecycle_ports_.allows_direct_path || lifecycle_ports_.allows_direct_path();
  if (!allows) {
    log().info << "ScheduleStartDirectMedia skipped (Status disallows Bridge) call_id=" << call_id
               << " status="
               << (lifecycle_ports_.status_name ? lifecycle_ports_.status_name() : "?")
               << " armed="
               << (lifecycle_ports_.armed_planner_name ? lifecycle_ports_.armed_planner_name() : "?");
    return;
  }
  if (!direct_media_.schedule_start) {
    log().error << "ScheduleStartDirectMedia: mesh media bridge not configured call_id=" << call_id;
    last_media_error_ = "Call media unavailable";
    NotifyRingChanged();
    return;
  }
  // V036 Phase 3: CSM is signaling-only for duplex start — Direct path façade owns Acquire+Schedule.
  log().info << "ScheduleStartDirectMedia libp2p role=" << (offerer ? "offerer" : "answerer")
                << " call_id=" << call_id << " peer=" << peer_identity;
  direct_media_.schedule_start(call_id, peer_identity, offerer);
}

void CallSessionManager::KickAnswererDirectMediaIfArmed(const std::string& call_id) {
  if (call_id.empty()) {
    log().info << "KickAnswererDirectMediaIfArmed skip (empty call_id)";
    return;
  }
  std::string peer;
  if (!workflow_.PeekPendingAnswererKick(call_id, &peer)) {
    auto resolved = PeerIdentityForCall(call_id);
    if (resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  CallAnswererKickDecisionInput in;
  in.allows_direct_path =
      !lifecycle_ports_.allows_direct_path || lifecycle_ports_.allows_direct_path();
  // IsActive alone — do not require tx/connected (capture lags StartSfu; dogfood e157 thrash).
  in.media_already_active_same_call = media_.IsActive() && media_.ActiveCallId() == call_id;
  in.peer_nonempty = !peer.empty();
  if (!ShouldKickAnswererDirectMedia(in)) {
    if (!in.allows_direct_path) {
      log().info << "KickAnswererDirectMediaIfArmed skip (Status disallows Bridge) call_id=" << call_id
                 << " status="
                 << (lifecycle_ports_.status_name ? lifecycle_ports_.status_name() : "?");
    } else if (in.media_already_active_same_call) {
      log().info << "KickAnswererDirectMediaIfArmed skip (media already active) call_id=" << call_id;
    } else {
      log().warning << "KickAnswererDirectMediaIfArmed no peer call_id=" << call_id;
    }
    return;
  }
  log().info << "KickAnswererDirectMediaIfArmed call_id=" << call_id << " peer=" << peer
             << " on_ui=" << (AppRuntime::CurrentlyOnUI() ? 1 : 0);
  ScheduleStartDirectMedia(call_id, peer, false);
}

CallHopHealth CallSessionManager::HopHealth() const {
  if (!topology_.IsSfuAttached()) {
    return {};
  }
  // Topology holds relay deps; sample via public IsSfuAttached + Media PathPressure elsewhere.
  return topology_.HopHealth();
}

std::string CallSessionManager::MediaPathKind() const {
  if (!direct_media_.media_path_kind) {
    return {};
  }
  return direct_media_.media_path_kind();
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
  // Topology owns SoftMigrate nudge (N≥3 / attach-wait only — V038).
  if (media_relay && !was) {
    if (auto active = ActiveLocalCall(); active && active->has_value()) {
      topology_.OnPeerMediaRelayCapLearned((*active)->call_id, peer_id);
    }
  }
}

void CallSessionManager::NoteMeshPeerIdForRelay(const std::string& relay_identity,
                                                  const std::string& peer_id) {
  if (relay_identity.empty() || peer_id.empty() || !IsAccountIdentityValue(relay_identity)) {
    return;
  }
  peer_id_to_relay_[peer_id] = relay_identity;
  if (direct_media_.note_peer_id_relay_mapping) {
    direct_media_.note_peer_id_relay_mapping(peer_id, relay_identity);
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
  // V036 Phase 3: CSM signaling-only for duplex stop — seat.Release owns Detach-then-Stop.
  if (media_seat_ports_.release) {
    media_seat_ports_.release(call_id);
    return;
  }
  // Tests / incomplete wiring without a seat.
  topology_.OnMediaStopped(call_id);
  if (direct_media_.stop_mesh_media) {
    direct_media_.stop_mesh_media(call_id);
  }
}

Roe<void> CallSessionManager::LeaveCallIfActiveExcept(const std::string& keep_call_id) {
  return workflow_.LeaveCallIfActiveExcept(keep_call_id);
}


Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
  return workflow_.StartCall(origin_thread_id, video_allowed, invitee_identities);
}


Roe<void> CallSessionManager::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  return workflow_.InviteParticipant(call_id, invitee_identity);
}


int64_t CallSessionManager::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  return workflow_.InitiationOfferMinorForPeer(peer_identity);
}


void CallSessionManager::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  workflow_.SetPendingAcceptChargeDecision(decision);
}


Roe<void> CallSessionManager::AcceptInvite(const std::string& call_id,
                                           InitiationChargeDecision charge_decision) {
  return workflow_.AcceptInvite(call_id, charge_decision);
}



Roe<PendingCallInvite> CallSessionManager::ArmJoinFromLiveAnnounce(const AnnounceLiveJoinPlan& plan,
                                                                   const ArmLiveAnnounceJoinOpts& opts) {
  return broadcast_.ArmJoinFromLiveAnnounce(plan, opts);
}


Roe<void> CallSessionManager::AcceptLiveAnnounceJoin(const std::string& call_id) {
  return broadcast_.AcceptLiveAnnounceJoin(call_id);
}


Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  return workflow_.DeclineInvite(call_id);
}


Roe<void> CallSessionManager::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  return workflow_.MaybeRotateMediaKey(call_id, leaver_identity);
}


Roe<void> CallSessionManager::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms) {
  return workflow_.EndCallLocal(session, duration_ms);
}


Roe<void> CallSessionManager::LeaveCall(const std::string& call_id) {
  return workflow_.LeaveCall(call_id);
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
  return workflow_.ActiveLocalCall();
}

Roe<std::optional<PendingCallInvite>> CallSessionManager::TopPendingInvite() {
  return workflow_.TopPendingInvite();
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
  return direct_media_.is_connect_failed && direct_media_.is_connect_failed();
}

bool CallSessionManager::P2pConnectMissingMic() const {
  return direct_media_.connect_missing_mic && direct_media_.connect_missing_mic();
}

void CallSessionManager::PollP2pConnectHealth() {
  if (direct_media_.poll_connect_health) {
    direct_media_.poll_connect_health();
  }
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  if (direct_media_.media_attempted && direct_media_.retry_mesh_media &&
      direct_media_.media_attempted(call_id)) {
    return direct_media_.retry_mesh_media(call_id);
  }
  return Error("Call media retry unavailable");
}

Roe<std::optional<CallSession>> CallSessionManager::SessionForCall(const std::string& call_id) const {
  return sessions_.LoadSession(call_id);
}

void CallSessionManager::SweepExpiredInvites() {
  workflow_.SweepExpiredInvites();
}


void CallSessionManager::AbandonOrphanedCallsAfterRestart() {
  workflow_.AbandonOrphanedCallsAfterRestart();
}


bool CallSessionManager::MediaAttemptedThisProcess(const std::string& call_id) const {
  return direct_media_.media_attempted && direct_media_.media_attempted(call_id);
}

void CallSessionManager::ClearMediaCallbacks() {
}

Roe<void> CallSessionManager::HandleInboundInvite(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const ThreadMessage& message,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms,
                                                  const std::string& local_identity) {
  return workflow_.HandleInboundInvite(detail_json, sender_identity, message, relay_created_at_ms, relay_server_time_ms, local_identity);
}


Roe<void> CallSessionManager::HandleInboundAccept(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const std::string& local_identity) {
  return workflow_.HandleInboundAccept(detail_json, sender_identity, local_identity);
}


Roe<void> CallSessionManager::HandleInboundDecline(const std::string& detail_json,
                                                   const std::string& sender_identity) {
  return workflow_.HandleInboundDecline(detail_json, sender_identity);
}


Roe<void> CallSessionManager::HandleInboundLeave(const std::string& detail_json,
                                                 const std::string& sender_identity,
                                                 const std::string& local_identity) {
  return workflow_.HandleInboundLeave(detail_json, sender_identity, local_identity);
}


Roe<void> CallSessionManager::HandleInboundRoster(const std::string& detail_json) {
  return workflow_.HandleInboundRoster(detail_json);
}


Roe<void> CallSessionManager::HandleInboundMediaKey(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  return workflow_.HandleInboundMediaKey(detail_json, sender_identity);
}


Roe<void> CallSessionManager::HandleInboundSfuAttach(const std::string& detail_json) {
  return workflow_.HandleInboundSfuAttach(detail_json);
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


Roe<void> CallSessionManager::HandleInboundEnded(const std::string& detail_json,
                                                 const std::string& local_identity) {
  return workflow_.HandleInboundEnded(detail_json, local_identity);
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
  if (direct_media_.note_media_attempted) {
    direct_media_.note_media_attempted(call_id);
  }
}

void CallSessionManager::TopologyBindMediaCallId(const std::string& call_id) {
  // Hop path bind — CallHopPath façade (Acquire under seat) via ports.
  if (media_seat_ports_.bind_hop_for_attach) {
    media_seat_ports_.bind_hop_for_attach(call_id);
  }
}

void CallSessionManager::TopologyClearMediaPeerIdentity() {
}

void CallSessionManager::TopologyReleaseDirectMedia() {
  // SoftMigrate path replace: Direct path ReleaseTransport under current seat token.
  if (direct_media_.release_direct_transport) {
    direct_media_.release_direct_transport();
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

Roe<std::optional<std::string>> CallSessionManager::MeshPeerIdForAccount(const std::string& account) const {
  if (account.empty() || account.rfind("account:", 0) != 0) {
    return std::optional<std::string>{};
  }
  for (const auto& [peer_id, relay] : peer_id_to_relay_) {
    if (relay == account && !peer_id.empty()) {
      return std::optional<std::string>{peer_id};
    }
  }
  auto found = contacts_.FindByIdentity(account, ContactIdKind::Account);
  if (!found) {
    return found.error();
  }
  if (found->has_value()) {
    const std::string peer_id = PeerIdFromContact(**found);
    if (!peer_id.empty()) {
      return std::optional<std::string>{peer_id};
    }
  }
  return std::optional<std::string>{};
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
  CallExpectGroupSfuInput in;
  in.awaiting_sfu_recovery = topology_.IsAwaitingSfuRecovery();
  in.sfu_attached = topology_.IsSfuAttached();
  if (auto n = sessions_.CountJoined(call_id)) {
    in.joined_count = *n;
  }
  if (auto all = sessions_.ListParticipants(call_id); all) {
    in.active_roster_count = CountMediaPlannerActiveParticipants(*all);
  } else {
    in.active_roster_count = in.joined_count;
  }
  if (auto session = sessions_.LoadSession(call_id);
      session && *session && (*session)->sfu_hint && !(*session)->sfu_hint->empty()) {
    in.has_sfu_hint = true;
  }
  return ShouldExpectGroupSfuMigration(in);
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
