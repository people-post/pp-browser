#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallControlClient.h"
#include "feature/calls/CallsThread.h"
#include "domain/messaging/CallListenAddrsLogic.h"
#include "domain/messaging/CallAnswererKickLogic.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"

#include "foundation/crypto/CryptoUtil.h"
#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/messaging/PairwiseFanoutLogic.h"
#include "domain/messaging/CallControlThreadLogic.h"
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
      topology_(sessions, contacts, media),
      control_(store, contacts, identity, sessions, psk_store, delivery_,
               [this]() -> std::optional<std::string> {
                 auto active = ActiveLocalCall();
                 if (active && *active && (*active)->origin_thread_id) {
                   return *(*active)->origin_thread_id;
                 }
                 return std::nullopt;
               }),
      key_exchange_(sessions, media_keys, control_), billing_(identity), reach_signals_(control_),
      workflow_(store, sessions, key_exchange_, billing_, live_calls_), peer_accounts_(contacts) {
  redirectLogger("CallSessionManager");
  topology_.SetMediaKeyStore(&media_keys_);
  key_exchange_.SetOnKeyReady([this](const std::string& call_id) {
    const auto direct_media = direct_media_.Get();
    if (direct_media->on_media_key_ready) {
      direct_media->on_media_key_ready(call_id);
    }
  });
  live_calls_.BindMediaResources(&media_, nullptr);
  live_calls_.BindHopDriver(&topology_);
  BindTopologyHostPorts();
  BindWorkflowHostPorts();
  BindReachSignalPorts();
}

void CallSessionManager::BindTopologyHostPorts() {
  CallTopologyController::HostPorts ports;
  ports.local_relay_identity = [this]() { return TopologyLocalIdentity(); };
  ports.leave_call = [this](const std::string& call_id) { return TopologyLeaveCall(call_id); };
  ports.call_media = [this](const std::string& call_id) { return live_calls_.Media(call_id); };
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
  ports.clear_media_peer_identity = [this]() { TopologyClearMediaPeerIdentity(); };
  ports.request_inbox_sync = [this]() { TopologyRequestInboxSync(); };
  topology_.SetHostPorts(std::move(ports));
}

void CallSessionManager::BindWorkflowHostPorts() {
  CallSessionWorkflow::HostPorts ports;
  ports.wire.local_relay_identity = [this]() { return control_.LocalRelayIdentity(); };
  ports.wire.notify_ring_changed = [this]() { NotifyRingChanged(); };
  ports.wire.send_direct = [this](const std::string& peer, CallControlType type, const std::string& detail,
                             const std::string& display) {
    return control_.SendDirect(peer, type, detail, display);
  };
  ports.wire.ensure_control_thread = [this](const std::string& peer) { return control_.EnsureCallControlThread(peer); };
  ports.wire.fan_out_joined = [this](const std::string& call_id, CallControlType type, const std::string& detail,
                                const std::string& display, const std::string& skip) {
    return control_.FanOutToJoined(call_id, type, detail, display, skip);
  };
  ports.wire.fan_out_joined_and_ringing = [this](const std::string& call_id, CallControlType type,
                                            const std::string& detail, const std::string& display,
                                            const std::string& skip) {
    return control_.FanOutToJoinedAndRinging(call_id, type, detail, display, skip);
  };
  ports.wire.append_origin_history = [this](const std::string& thread_id, CallControlType type,
                                       const std::string& text, const std::string& detail) {
    return control_.AppendOriginHistory(thread_id, type, text, detail);
  };
  ports.wire.build_roster_detail = [this](const std::string& call_id) { return BuildRosterDetail(call_id); };
  ports.duplex.schedule_start_direct = [this](const std::string& call_id, const std::string& peer, bool offerer) {
    ScheduleStartDirectMedia(call_id, peer, offerer);
  };
  ports.hop.on_joined_count_observed = [this](const std::string& call_id, size_t n) {
    topology_.OnJoinedCountObserved(call_id, n);
  };
  ports.hop.plan_hop_for_invitees = [this](const std::vector<std::string>& invitees, const std::string& local) {
    return topology_.PlanHopForInvitees(invitees, local);
  };
  ports.hop.probe_invite_hops = [this](const std::string& call_id) { topology_.ProbeInviteHops(call_id); };
  ports.hop.hop_report_for_accept = [this](const std::string& call_id) {
    return topology_.HopReportForAccept(call_id);
  };
  ports.hop.note_accept_hop_report = [this](const std::string& call_id, const std::string& identity,
                                            const CallHopReport& report) {
    topology_.NoteAcceptHopReport(call_id, identity, report);
  };
  ports.hop.clear_sfu_attach_wait = [this]() { topology_.ClearSfuAttachWait(); };
  ports.hop.on_inbound_sfu_attach = [this](const std::string& call_id, const CallSfuAttachDetail& d,
                                           const std::string& sender) {
    return topology_.OnInboundSfuAttach(call_id, d, sender);
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
    const auto lifecycle = lifecycle_ports_.Get();
    if (lifecycle->set_direct_connecting) {
      lifecycle->set_direct_connecting(call_id);
    }
  };
  ports.chrome.note_outbound_started = [this](const std::string& call_id) {
    const auto lifecycle = lifecycle_ports_.Get();
    if (lifecycle->apply_outbound_started) {
      lifecycle->apply_outbound_started(call_id);
    }
  };
  ports.chrome.accepting_call_id = [this]() {
    const auto lifecycle = lifecycle_ports_.Get();
    return lifecycle->accepting_call_id ? lifecycle->accepting_call_id() : std::string{};
  };
  ports.chrome.active_call_id = [this]() {
    const auto lifecycle = lifecycle_ports_.Get();
    return lifecycle->active_call_id ? lifecycle->active_call_id() : std::string{};
  };
  ports.chrome.apply_remote_ended = [this](const std::string& call_id) {
    const auto lifecycle = lifecycle_ports_.Get();
    if (lifecycle->apply_remote_ended) {
      lifecycle->apply_remote_ended(call_id);
    }
  };
  ports.chrome.is_outbound_calling = [this]() {
    const auto lifecycle = lifecycle_ports_.Get();
    return lifecycle->is_outbound_calling && lifecycle->is_outbound_calling();
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
  ports.reach.note_call_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (call_peer_caps_sink_) {
      call_peer_caps_sink_(call_id, caps);
    }
  };
  ports.reach.prefetch_reach = [this](const std::string& identity) {
    PrefetchReachForIdentity(prefetch_reach_, identity);
  };
  ports.reach.ensure_circuit_ready = [this]() {
    if (ensure_circuit_ready_) {
      ensure_circuit_ready_();
    }
  };
  ports.reach.park_circuit = [this](int timeout_ms, std::function<void(bool)> done) {
    if (park_circuit_) {
      park_circuit_(timeout_ms, std::move(done));
    } else {
      done(false);
    }
  };
  ports.reach.note_mesh_peer_id_for_relay = [this](const std::string& relay, const std::string& peer_id) {
    NoteMeshPeerIdForRelay(relay, peer_id);
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

void CallSessionManager::BindReachSignalPorts() {
  CallReachSignals::CallPorts ports;
  ports.active_call_id = [this]() -> std::optional<std::string> {
    auto active = ActiveLocalCall();
    if (!active || !active->has_value()) {
      return std::nullopt;
    }
    return (*active)->call_id;
  };
  ports.call_peer = [this](const std::string& call_id) -> std::optional<std::string> {
    auto peer = PeerIdentityForCall(call_id);
    if (!peer || !peer->has_value()) {
      return std::nullopt;
    }
    return **peer;
  };
  ports.local_identity = [this]() -> std::string {
    auto local = P2pLocalIdentity();
    return local ? *local : std::string{};
  };
  ports.local_caps = [this]() -> std::optional<CallPeerCaps> {
    return local_peer_caps_ ? std::optional<CallPeerCaps>{local_peer_caps_()} : std::nullopt;
  };
  ports.local_listen_addrs = [this]() {
    return local_listen_multiaddrs_ ? local_listen_multiaddrs_() : std::vector<std::string>{};
  };
  ports.local_peer_id = [this]() { return local_mesh_peer_id_ ? local_mesh_peer_id_() : std::string{}; };
  ports.register_listen = [this](const std::string& key, const std::vector<std::string>& addrs) {
    if (register_peer_listen_multiaddrs_) {
      register_peer_listen_multiaddrs_(key, addrs);
    }
  };
  ports.on_peer_caps = [this](const std::string& call_id, const CallPeerCaps& caps) {
    if (call_peer_caps_sink_) {
      call_peer_caps_sink_(call_id, caps);
    }
  };
  reach_signals_.SetCallPorts(std::move(ports));
}

void CallSessionManager::SetInitiationBillingStore(InitiationBillingStore* store) {
  billing_.SetStore(store);
}

void CallSessionManager::SetMediaRelayDeps(MediaRelayDeps deps) {
  topology_.SetMediaRelayDeps(std::move(deps));
}

// Port setters swap a snapshot; the workflow / topology host ports (bound once in the ctor) read the
// current snapshot per call — never rebuilt under a running caller (thread-ownership t2a).
void CallSessionManager::SetDirectMediaPorts(CallDirectMediaPorts ports) {
  direct_media_.Set(std::move(ports));
}

void CallSessionManager::SetLifecyclePorts(CallSessionLifecyclePorts ports) {
  lifecycle_ports_.Set(std::move(ports));
}

void CallSessionManager::SetTopologyHopArmingPorts(CallHopArmingPorts ports) {
  topology_.SetHopArmingPorts(std::move(ports));
}

void CallSessionManager::SetMediaSeatPorts(CallMediaSeatPorts ports) {
  media_seat_ports_.Set(std::move(ports));
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
  return ports;
}

void CallSessionManager::TopologyOnMediaStoppedForSeat(const std::string& call_id) {
  topology_.OnMediaStopped(call_id);
}

void CallSessionManager::ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity,
                                                  bool offerer) {
  const auto lifecycle = lifecycle_ports_.Get();
  const bool allows =
      !lifecycle->allows_direct_path || lifecycle->allows_direct_path();
  if (!allows) {
    log().info << "ScheduleStartDirectMedia skipped (Status disallows Bridge) call_id=" << call_id
               << " status="
               << (lifecycle->status_name ? lifecycle->status_name() : "?")
               << " armed="
               << (lifecycle->armed_planner_name ? lifecycle->armed_planner_name() : "?");
    return;
  }
  // CSM is signaling-only for duplex start — the call's media coordinator takes the seat and has the
  // direct driver connect.
  log().info << "ScheduleStartDirectMedia libp2p role=" << (offerer ? "offerer" : "answerer")
                << " call_id=" << call_id << " peer=" << peer_identity;
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  if (auto begun = call_media ? call_media->BeginDirect(peer_identity, offerer)
                              : Roe<void>(Error("no live call for media"));
      !begun) {
    log().error << "ScheduleStartDirectMedia: " << begun.error().message << " call_id=" << call_id;
    last_media_error_ = "Call media unavailable";
    NotifyRingChanged();
  }
}

void CallSessionManager::KickAnswererDirectMediaIfArmed(const std::string& call_id) {
  const auto lifecycle = lifecycle_ports_.Get();
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
      !lifecycle->allows_direct_path || lifecycle->allows_direct_path();
  // IsActive alone — do not require tx/connected (capture lags StartSfu; dogfood e157 thrash).
  in.media_already_active_same_call = media_.IsActive() && media_.ActiveCallId() == call_id;
  in.peer_nonempty = !peer.empty();
  if (!ShouldKickAnswererDirectMedia(in)) {
    if (!in.allows_direct_path) {
      log().info << "KickAnswererDirectMediaIfArmed skip (Status disallows Bridge) call_id=" << call_id
                 << " status="
                 << (lifecycle->status_name ? lifecycle->status_name() : "?");
    } else if (in.media_already_active_same_call) {
      log().info << "KickAnswererDirectMediaIfArmed skip (media already active) call_id=" << call_id;
    } else {
      log().warning << "KickAnswererDirectMediaIfArmed no peer call_id=" << call_id;
    }
    return;
  }
  log().info << "KickAnswererDirectMediaIfArmed call_id=" << call_id << " peer=" << peer
             << " on_owner=" << (CallsThread::IsCurrent() ? 1 : 0);
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
  const auto direct_media = direct_media_.Get();
  if (!direct_media->media_path_kind) {
    return {};
  }
  return direct_media->media_path_kind();
}

bool CallSessionManager::IsSfuAttached() const {
  return topology_.IsSfuAttached();
}

CallMediaEngine& CallSessionManager::Media() {
  return media_;
}

Roe<void> CallSessionManager::SetLocalAudioMuted(bool muted) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = live_calls_.MediaRunningCallId();
  CallMediaCoordinator* call_media = call_id.empty() ? nullptr : live_calls_.Media(call_id);
  if (!call_media) {
    return Error("No active call media");
  }
  call_media->SetMuted(muted);
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.audio_muted = muted;
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)control_.FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::SetLocalVideoEnabled(bool enabled, const int display_rotation_degrees) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::string call_id = live_calls_.MediaRunningCallId();
  CallMediaCoordinator* call_media = call_id.empty() ? nullptr : live_calls_.Media(call_id);
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
  auto participant = sessions_.FindParticipant(call_id, *local);
  if (participant && participant->has_value()) {
    (*participant)->media.video_enabled = enabled && call_media->CameraOn();
    (void)sessions_.UpsertParticipant(**participant);
  }
  auto roster = BuildRosterDetail(call_id);
  if (roster) {
    auto roster_json = CallControlCodec::EncodeRoster(*roster);
    if (roster_json) {
      (void)control_.FanOutToJoined(call_id, CallControlType::CallRoster, *roster_json, "Call roster", *local);
    }
  }
  NotifyRingChanged();
  return {};
}

Roe<void> CallSessionManager::RequestVideoRefresh(const std::string& call_id,
                                                 const std::string& publisher_identity) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (call_id.empty()) {
    return Error("No active call");
  }
  if (publisher_identity.empty() || publisher_identity == *local) {
    if (CallMediaCoordinator* call_media = live_calls_.Media(call_id)) {
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

void CallSessionManager::ClearLastMediaErrorIf(const std::string& seen) {
  if (last_media_error_ && *last_media_error_ == seen) {
    last_media_error_.reset();
  }
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

void CallSessionManager::SetEnsureCircuitReady(EnsureCircuitReadyFn callback) {
  ensure_circuit_ready_ = std::move(callback);
}

void CallSessionManager::SetParkCircuit(ParkCircuitFn park) {
  park_circuit_ = std::move(park);
}

void CallSessionManager::SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback) {
  local_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::SetLocalPeerCapsProvider(LocalPeerCapsFn callback) {
  local_peer_caps_ = std::move(callback);
}

void CallSessionManager::SetCallPeerCapsSink(CallPeerCapsSink sink) { call_peer_caps_sink_ = std::move(sink); }

void CallSessionManager::SetLocalMeshPeerIdProvider(LocalMeshPeerIdFn callback) {
  local_mesh_peer_id_ = std::move(callback);
}

void CallSessionManager::SetRegisterPeerListenMultiaddrs(RegisterPeerListenMultiaddrsFn callback) {
  register_peer_listen_multiaddrs_ = std::move(callback);
}

void CallSessionManager::NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay) {
  // Topology owns SoftMigrate nudge (N≥3 / attach-wait only — V038).
  if (media_relay_caps_.Note(peer_id, media_relay)) {
    if (auto active = ActiveLocalCall(); active && active->has_value()) {
      topology_.OnPeerMediaRelayCapLearned((*active)->call_id, peer_id);
    }
  }
}

void CallSessionManager::NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id) {
  if (!peer_accounts_.Learn(relay_identity, peer_id)) {
    return;
  }
  const auto direct_media = direct_media_.Get();
  if (direct_media->note_peer_id_relay_mapping) {
    direct_media->note_peer_id_relay_mapping(peer_id, relay_identity);
  }
}

bool CallSessionManager::PeerHasMediaRelayCap(const std::string& peer_id) const {
  return media_relay_caps_.Has(peer_id);
}

std::vector<std::string> CallSessionManager::ListMediaRelayCapablePeerIds() const {
  return media_relay_caps_.ListCapable();
}

void CallSessionManager::NotifyRingChanged() {
  if (on_ring_changed_mesh_) {
    on_ring_changed_mesh_();
  }
  if (on_ring_changed_) {
    on_ring_changed_();
  }
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




void CallSessionManager::StopCallMedia(const std::string& call_id) {
  StopMediaIfCall(call_id);
}

void CallSessionManager::StopMediaIfCall(const std::string& call_id) {
  // CSM is signaling-only for duplex stop: the call registry releases the seat (Detach-then-Stop).
  live_calls_.StopMedia(call_id);
}



Roe<CallSession> CallSessionManager::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
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


int64_t CallSessionManager::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  return billing_.OfferFrom(peer_identity);
}


void CallSessionManager::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  workflow_.SetPendingAcceptChargeDecision(decision);
}


void CallSessionManager::SetPendingAcceptVoiceOnly(const bool voice_only) {
  workflow_.SetPendingAcceptVoiceOnly(voice_only);
}


void CallSessionManager::AcceptInviteAsync(const std::string& call_id, std::function<void(Roe<void>)> on_done,
                                           InitiationChargeDecision charge_decision) {
  workflow_.AcceptInviteAsync(call_id, charge_decision, std::move(on_done));
}





Roe<void> CallSessionManager::DeclineInvite(const std::string& call_id) {
  return workflow_.DeclineInvite(call_id);
}


Roe<void> CallSessionManager::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  return workflow_.MaybeRotateMediaKey(call_id, leaver_identity);
}


Roe<void> CallSessionManager::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms,
                                           const LiveCallEndReason reason) {
  auto result = workflow_.EndCallLocal(session, duration_ms, reason);
  MaybeCatchUpAfterCall();
  return result;
}


Roe<void> CallSessionManager::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  auto result = workflow_.LeaveCall(call_id, reason);
  MaybeCatchUpAfterCall();
  return result;
}

void CallSessionManager::MaybeCatchUpAfterCall() {
  if (!delivery_.catch_up_after_call) {
    return;
  }
  auto active = ActiveLocalCall();
  if (active && *active) {
    return;
  }
  delivery_.catch_up_after_call();
}


Roe<std::vector<PendingCallInvite>> CallSessionManager::ListPendingInvites() {
  SweepExpiredInvites();
  auto local = control_.LocalRelayIdentity();
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
  auto local = control_.LocalRelayIdentity();
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
  auto local = control_.LocalRelayIdentity();
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

Roe<bool> CallSessionManager::AwaitingExplicitAnswerForCall(const std::string& call_id) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& p : *participants) {
    auto invite = sessions_.LoadPendingInvite(call_id, p.identity);
    if (invite && invite->has_value() && (*invite)->status == "accepted_implicit") {
      return true;
    }
  }
  return false;
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
  const auto direct_media = direct_media_.Get();
  return direct_media->is_connect_failed && direct_media->is_connect_failed();
}

bool CallSessionManager::P2pConnectMissingMic() const {
  const auto direct_media = direct_media_.Get();
  return direct_media->connect_missing_mic && direct_media->connect_missing_mic();
}

void CallSessionManager::PollP2pConnectHealth() {
  const auto direct_media = direct_media_.Get();
  if (direct_media->poll_connect_health) {
    direct_media->poll_connect_health();
  }
}

Roe<void> CallSessionManager::RetryP2pMedia(const std::string& call_id) {
  const auto direct_media = direct_media_.Get();
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  if (call_media && direct_media->media_attempted && direct_media->media_attempted(call_id)) {
    return call_media->Retry();
  }
  return Error("Call media retry unavailable");
}

Roe<void> CallSessionManager::ResumeP2pMedia(const std::string& call_id) {
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  return call_media ? call_media->ResumeFromInbound() : Roe<void>(Error("Call media resume unavailable"));
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
  const auto direct_media = direct_media_.Get();
  return direct_media->media_attempted && direct_media->media_attempted(call_id);
}

void CallSessionManager::ClearMediaCallbacks() {
  // Drop deferred Accept roster fan-out before CallStack drains/resets CSM (PR #216 follow-up).
  workflow_.InvalidateDeferredOps();
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
  auto local = control_.LocalRelayIdentity();
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
    return key_exchange_.HandleInbound(detail_json, sender_identity);
  case CallControlType::CallSdp:
  case CallControlType::CallIce:
    log().debug << "Ignoring legacy call_sdp/call_ice from " << sender_identity;
    return {};
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
  case CallControlType::CallStarted:
    return {};
  }
  return {};
}


Roe<std::string> CallSessionManager::TopologyLocalIdentity() const {
  return control_.LocalRelayIdentity();
}

Roe<void> CallSessionManager::TopologyLeaveCall(const std::string& call_id) {
  // Topology leaves when the group media path cannot be kept (attach-wait timeout, failed migrate).
  return LeaveCall(call_id, LiveCallEndReason::MediaUnavailable);
}

Roe<void> CallSessionManager::TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                                     const std::string& detail_json, const std::string& display,
                                                     const std::string& skip_identity) {
  return control_.FanOutToJoined(call_id, type, detail_json, display, skip_identity);
}

Roe<void> CallSessionManager::TopologySendDirect(const std::string& peer_identity, CallControlType type,
                                                 const std::string& detail_json, const std::string& display) {
  return control_.SendDirect(peer_identity, type, detail_json, display);
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
  const auto direct_media = direct_media_.Get();
  if (direct_media->note_media_attempted) {
    direct_media->note_media_attempted(call_id);
  }
}

void CallSessionManager::TopologyClearMediaPeerIdentity() {
}

void CallSessionManager::TopologyRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

Roe<std::string> CallSessionManager::P2pLocalIdentity() const {
  return control_.LocalRelayIdentity();
}

Roe<void> CallSessionManager::P2pSendDirect(const std::string& peer_identity, CallControlType type,
                                            const std::string& detail_json, const std::string& display) {
  return control_.SendDirect(peer_identity, type, detail_json, display);
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
  return peer_accounts_.PeerIdForAccount(account);
}

Roe<std::optional<std::string>> CallSessionManager::RelayIdentityForMeshPeerId(
    const std::string& call_id, const std::string& peer_id) const {
  // Prefer the call's participants whose contact carries the inbound stream's PeerId.
  std::vector<std::string> participants;
  if (!call_id.empty() && !peer_id.empty()) {
    auto rows = sessions_.ListParticipants(call_id);
    if (!rows) {
      return rows.error();
    }
    for (const CallParticipant& row : *rows) {
      participants.push_back(row.identity);
    }
  }
  return peer_accounts_.AccountForPeerId(peer_id, participants);
}

void CallSessionManager::P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) {
  if (call_id.empty() || peer_identity.empty()) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return;
  }
  (void)key_exchange_.SendCurrent(call_id, peer_identity);
}

void CallSessionManager::P2pNoteInboundHello(const std::string& call_id, const std::string& identity,
                                             const std::string& peer_id) {
  auto local = control_.LocalRelayIdentity();
  if (!local) {
    return;
  }
  if (auto applied = workflow_.ApplyImplicitAccept(call_id, identity, peer_id, *local); !applied) {
    log().warning << "implicit accept failed call_id=" << call_id << " err=" << applied.error().message;
  }
}

void CallSessionManager::P2pRequestInboxSync() {
  if (delivery_.sync_inbox_from_wake) {
    delivery_.sync_inbox_from_wake(true);
  }
}

} // namespace pbr
