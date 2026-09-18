#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallHopMigrateWorkflow.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallHopPlannerLogic.h"

#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/HopHintLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/messaging/SfuAttachFanout.h"
#include "domain/messaging/SfuAttachWaitLogic.h"
#include "foundation/i18n/LocalizationService.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/people/PeerDisplayLabel.h"
#include "foundation/platform/PlatformUserHints.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "common/SettledWait.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/Utilities.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

CallTopologyController::CallTopologyController(CallSessionStore& sessions, ContactsStore& contacts,
                                               CallMediaEngine& media)
    : hop_migrate_(std::make_unique<CallHopMigrateWorkflow>(sessions, media)),
      sessions_(sessions),
      contacts_(contacts),
      media_(media) {
  redirectLogger("CallTopologyController");
  BindHopMigratePortsAndOps();
}

CallTopologyController::~CallTopologyController() = default;

void CallTopologyController::BindHopMigratePortsAndOps() {
  hop_migrate_->SetMediaRelayDeps(&relay_deps_);
  CallHopMigrateWorkflow::TopologyOps ops;
  ops.apply = [this](CallHopPlannerEvent ev, const std::string& call_id) { Apply(ev, call_id); };
  ops.sync_sfu_subscriptions = [this](const std::string& call_id) { SyncSfuSubscriptions(call_id); };
  ops.refresh_adaptation = [this](const std::string& call_id) { RefreshAdaptation(call_id); };
  ops.ranked_media_hop_candidates = [this]() { return RankedMediaHopCandidates(); };
  ops.resolve_hop_multiaddr = [this](const std::string& hop_peer_id) {
    return ResolveHopMultiaddr(hop_peer_id);
  };
  ops.resolve_local_advertise_ma = [this](const std::string& local_peer_id) {
    return ResolveLocalAdvertiseMa(local_peer_id);
  };
  ops.infer_scope_for_call = [this](const std::string& call_id, const std::string& local_identity) {
    return InferScopeForCall(call_id, local_identity);
  };
  ops.lan_reachability_confirmed_for_call =
      [this](const std::string& call_id, const std::string& local_identity) {
        return LanReachabilityConfirmedForCall(call_id, local_identity);
      };
  ops.fan_out_sfu_attach_for_hop =
      [this](const std::string& call_id, const std::string& hop_peer_id,
             const std::string& local_identity) {
        FanOutSfuAttachForHop(call_id, hop_peer_id, local_identity);
      };
  ops.publisher_stream_id_for_local = [this]() { return PublisherStreamIdForLocal(); };
  ops.note_remote_publisher_from_attach = [this](const CallSfuAttachDetail& attach) {
    NoteRemotePublisherFromAttach(attach);
  };
  ops.announce_local_publisher = [this](const std::string& call_id,
                                        const CallSfuAttachDetail& hop_attach) {
    AnnounceLocalPublisher(call_id, hop_attach);
  };
  ops.is_active_call_for_topology = [this](const std::string& call_id) {
    return IsActiveCallForTopology(call_id);
  };
  ops.begin_sfu_attach_wait = [this](const std::string& call_id) { BeginSfuAttachWait(call_id); };
  ops.clear_sfu_attach_wait = [this]() { ClearSfuAttachWait(); };
  hop_migrate_->SetTopologyOps(std::move(ops));
}

void CallTopologyController::SetHostPorts(HostPorts ports) {
  hop_migrate_->SetHostPorts(MakeMigrateHostPorts(ports));
  host_ = std::move(ports);
}

void CallTopologyController::SetMediaRelayDeps(MediaRelayDeps deps) {
  relay_deps_ = std::move(deps);
  hop_migrate_->SetMediaRelayDeps(&relay_deps_);
  if (relay_deps_.relay) {
    relay_deps_.relay->SetClientTransportLostHandler([this]() {
      AppRuntime::PostUI([this]() { OnGuestSfuTransportLost(); });
    });
  }
}

void CallTopologyController::SetMediaKeyStore(CallMediaKeyStore* keys) {
  media_keys_ = keys;
  hop_migrate_->SetMediaKeyStore(keys);
}

void CallTopologyController::SetSeatPorts(CallTopologySeatPorts ports) {
  hop_migrate_->SetSeatPorts(MakeMigrateSeatPorts(ports));
  seat_ = std::move(ports);
}

void CallTopologyController::SetHopArmingPorts(CallHopArmingPorts ports) {
  hop_migrate_->SetArmingPorts(MakeMigrateArmingPorts(ports));
  arming_ = std::move(ports);
}

CallHopMigrateArmingPorts CallTopologyController::MakeMigrateArmingPorts(
    const CallHopArmingPorts& ports) const {
  CallHopMigrateArmingPorts out;
  out.migrate_ops_allowed = ports.hop_ops_allowed;
  out.soft_migrate_may_arm = ports.soft_migrate_may_arm;
  out.media_cancel_gen = ports.media_cancel_gen;
  out.report_progress = ports.report_progress;
  out.arming_debug_name = ports.arming_debug_name;
  return out;
}

CallHopMigrateHostPorts CallTopologyController::MakeMigrateHostPorts(const HostPorts& ports) const {
  CallHopMigrateHostPorts out;
  out.local_relay_identity = ports.local_relay_identity;
  out.fan_out_joined = ports.fan_out_joined;
  out.notify_ring_changed = ports.notify_ring_changed;
  out.set_last_media_error = ports.set_last_media_error;
  out.set_media_activity = ports.set_media_activity;
  out.clear_media_activity = ports.clear_media_activity;
  out.note_media_attempted = ports.note_media_attempted;
  out.bind_media_call_id = ports.bind_media_call_id;
  out.clear_media_peer_identity = ports.clear_media_peer_identity;
  out.release_direct_media = ports.release_direct_media;
  out.request_inbox_sync = ports.request_inbox_sync;
  return out;
}

CallHopMigrateSeatPorts CallTopologyController::MakeMigrateSeatPorts(
    const CallTopologySeatPorts& ports) const {
  CallHopMigrateSeatPorts out;
  out.is_bound = ports.is_bound;
  out.acquire = ports.acquire;
  out.allows_path_op = ports.allows_path_op;
  out.begin_attach = ports.begin_attach;
  out.end_attach_if_matching = ports.end_attach_if_matching;
  out.has_attach_in_flight = ports.has_attach_in_flight;
  out.attaching_hop = ports.attaching_hop;
  out.note_connecting = ports.note_connecting;
  out.note_start = ports.note_start;
  out.note_path = ports.note_path;
  out.note_live = ports.note_live;
  return out;
}

bool CallTopologyController::IsAwaitingSfuRecovery() const {
  return hop_migrate_->Sfu().awaiting_recovery || hop_migrate_->Flight().in_flight || !hop_migrate_->AttachWaitState().call_id.empty() ||
         hop_migrate_->Guest().reattach_in_flight;
}

bool CallTopologyController::IsSfuAttached() const {
  return hop_migrate_->Sfu().attached;
}

bool CallTopologyController::IsOnSfuForCall(const std::string& call_id) const {
  return hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
}

bool CallTopologyController::IsSoftMigrateInFlight() const {
  return hop_migrate_->Flight().in_flight;
}

bool CallTopologyController::IsSfuAttachWaitActive() const {
  return !hop_migrate_->AttachWaitState().call_id.empty();
}

std::vector<MeshHopCandidate> CallTopologyController::RankedMediaHopCandidates() const {
  std::vector<Contact> contacts;
  if (auto listed = contacts_.List()) {
    contacts = std::move(*listed);
  }
  std::vector<MeshDirectoryNode> directory_nodes;
  if (relay_deps_.list_directory_nodes) {
    directory_nodes = relay_deps_.list_directory_nodes();
  }
  std::vector<MeshDirectoryNode> dht_nodes;
  if (relay_deps_.list_dht_nodes) {
    dht_nodes = relay_deps_.list_dht_nodes();
  }
  const bool include_seeds = !relay_deps_.seed_dial_ok || relay_deps_.seed_dial_ok();
  auto merged = BuildCircuitHopList(contacts, directory_nodes, dht_nodes, relay_deps_.bootstrap_peers,
                                    relay_deps_.prefer_contacts, include_seeds);
  auto ranked = RankMediaHopsEscalating(std::move(merged), relay_deps_.prefer_contacts,
                                        relay_deps_.local_listen_multiaddr);
  if (relay_deps_.relay) {
    if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
      ranked = ExcludeSelfHop(std::move(ranked), *pid);
    }
  }
  if (relay_deps_.dial) {
    for (MeshHopCandidate& hop : ranked) {
      if (hop.multiaddr.empty()) {
        if (auto ma = relay_deps_.dial->PreferredMultiaddr(hop.peer_id)) {
          hop.multiaddr = *ma;
        }
      }
      hop.dialable = relay_deps_.dial->IsDialable(hop.peer_id);
      // Directory / bootstrap seeds with a published MA are SoftMigrate candidates even before
      // Amp address-book learn (cross-net PreferLocal fallback).
      if (!hop.dialable && !hop.multiaddr.empty() &&
          (hop.affinity == MeshHopAffinity::OrgSeed || hop.affinity == MeshHopAffinity::DirectoryNode ||
           hop.affinity == MeshHopAffinity::DhtDiscovered)) {
        hop.dialable = true;
      }
    }
  }
  // V030: contacts need media_relay ads; org seeds always eligible; PreferLocal added later.
  ranked = FilterHopsByMediaRelayAds(std::move(ranked), relay_deps_.peer_has_media_relay);
  if (relay_deps_.list_media_relay_peers) {
    const std::vector<std::string> advertised = relay_deps_.list_media_relay_peers();
    ranked = MergeAdvertisedMediaRelayHops(
        std::move(ranked), advertised, [this](const std::string& peer_id) -> std::string {
          if (!relay_deps_.dial) {
            return {};
          }
          if (auto ma = relay_deps_.dial->PreferredMultiaddr(peer_id)) {
            return *ma;
          }
          return {};
        });
    if (relay_deps_.dial) {
      for (MeshHopCandidate& hop : ranked) {
        if (hop.multiaddr.empty()) {
          if (auto ma = relay_deps_.dial->PreferredMultiaddr(hop.peer_id)) {
            hop.multiaddr = *ma;
          }
        }
        hop.dialable = relay_deps_.dial->IsDialable(hop.peer_id) || !hop.multiaddr.empty();
      }
    }
  }
  return ranked;
}

bool CallTopologyController::HasMediaRelayHopCandidates() const {
  if (!relay_deps_.relay || !relay_deps_.dial) {
    return false;
  }
  if (relay_deps_.prefer_local_as_hop && relay_deps_.relay->IsStarted()) {
    return true;
  }
  for (const MeshHopCandidate& hop : RankedMediaHopCandidates()) {
    if (hop.dialable) {
      return true;
    }
  }
  return false;
}

std::string CallTopologyController::ResolveHopMultiaddr(const std::string& hop_peer_id) const {
  if (hop_peer_id.empty()) {
    return {};
  }
  for (const MeshHopCandidate& hop : RankedMediaHopCandidates()) {
    if (hop.peer_id == hop_peer_id && !hop.multiaddr.empty()) {
      return hop.multiaddr;
    }
  }
  if (relay_deps_.dial) {
    if (auto ma = relay_deps_.dial->PreferredMultiaddr(hop_peer_id)) {
      return *ma;
    }
  }
  return {};
}

void CallTopologyController::BeginSfuAttachWait(const std::string& call_id) {
  hop_migrate_->AttachWaitState().call_id = call_id;
  hop_migrate_->AttachWaitState().deadline_ms = util::NowUnixMs() + kSfuAttachWaitDefaultMs;
  ArmAttachWaitTimer(call_id, hop_migrate_->AttachWaitState().deadline_ms);
}

void CallTopologyController::ClearSfuAttachWait() {
  CancelAttachWaitTimer();
  hop_migrate_->AttachWaitState().call_id.clear();
  hop_migrate_->AttachWaitState().deadline_ms = 0;
}

void CallTopologyController::CancelAttachWaitTimer() {
  if (hop_migrate_->AttachWaitState().timer_id != 0) {
    AppRuntime::CancelCoordinatorTimer(hop_migrate_->AttachWaitState().timer_id);
    hop_migrate_->AttachWaitState().timer_id = 0;
  }
}

void CallTopologyController::ArmAttachWaitTimer(const std::string& call_id, int64_t deadline_ms) {
  CancelAttachWaitTimer();
  if (call_id.empty() || deadline_ms <= 0) {
    return;
  }
  const int64_t delay = std::max<int64_t>(0, deadline_ms - util::NowUnixMs());
  const std::string captured = call_id;
  hop_migrate_->AttachWaitState().timer_id = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(delay), [this, captured]() {
        hop_migrate_->AttachWaitState().timer_id = 0;
        AppRuntime::PostUI([this, captured]() { OnAttachWaitTimerFire(captured); });
      });
}

void CallTopologyController::OnAttachWaitTimerFire(const std::string& call_id) {
  if (hop_migrate_->AttachWaitState().call_id != call_id) {
    return;
  }
  if (hop_migrate_->Sfu().attached) {
    ClearSfuAttachWait();
    return;
  }
  Apply(CallHopPlannerEvent::AttachWaitExpired, call_id);
  // Attach-wait expiry runs PollPendingSfuAttach for eject / recovery (timer primary path).
  PollPendingSfuAttach();
}

CallHopPlannerApplyContext CallTopologyController::BuildHopPlannerContext(
    const std::string& call_id, size_t effective_n, bool has_sfu_hint) const {
  CallHopPlannerApplyContext ctx;
  ctx.allows_hop_path = !arming_.IsBound() || arming_.hop_ops_allowed();
  ctx.should_arm_hop = ShouldArmHopPlanner(effective_n);
  ctx.has_sfu_hint = has_sfu_hint;
  ctx.soft_migrate_in_flight = hop_migrate_->Flight().in_flight;
  ctx.sfu_attached = hop_migrate_->Sfu().attached && media_.IsSfuMode() &&
                     (call_id.empty() || media_.ActiveCallId() == call_id);
  return ctx;
}

void CallTopologyController::SetHopPlannerPhase(const CallHopPlannerPhase next,
                                                const CallHopPlannerEvent ev,
                                                const std::string& call_id) {
  const CallHopPlannerPhase prev = hop_migrate_->Sfu().hop_planner_phase;
  hop_migrate_->Sfu().hop_planner_phase = next;
  log().info << "planner=Hop phase=" << CallHopPlannerPhaseName(prev) << "->"
             << CallHopPlannerPhaseName(next) << " event=" << CallHopPlannerEventName(ev)
             << " call_id=" << call_id
             << " migrate_gen=" << hop_migrate_->Flight().migrate_generation.load(std::memory_order_acquire);
}

void CallTopologyController::ReportHopProgress(const CallHopPlannerPhase phase,
                                               const std::string& call_id) {
  if (arming_.report_progress) {
    arming_.report_progress(phase, call_id);
  }
}

void CallTopologyController::Apply(CallHopPlannerEvent ev, const std::string& call_id) {
  size_t n_joined = 0;
  if (!call_id.empty()) {
    if (auto j = sessions_.CountJoined(call_id)) {
      n_joined = *j;
    }
  }
  size_t n_active = n_joined;
  if (!call_id.empty()) {
    if (auto all = sessions_.ListParticipants(call_id); all) {
      n_active = CountMediaPlannerActiveParticipants(*all);
    }
  }
  bool has_hint = false;
  if (!call_id.empty()) {
    if (auto session = sessions_.LoadSession(call_id);
        session && session->has_value() && (*session)->sfu_hint && !(*session)->sfu_hint->empty()) {
      has_hint = true;
    }
  }
  const CallHopPlannerApplyContext ctx =
      BuildHopPlannerContext(call_id, EffectiveMediaPlannerN(n_joined, n_active), has_hint);
  const CallHopPlannerPhaseOutcome out = DecideCallHopPlannerPhase(hop_migrate_->Sfu().hop_planner_phase, ev, ctx);
  if (out.decision == CallHopPlannerDecision::Ignore) {
    log().info << "planner=Hop ignore event=" << CallHopPlannerEventName(ev)
               << " phase=" << CallHopPlannerPhaseName(hop_migrate_->Sfu().hop_planner_phase) << " call_id=" << call_id
               << " allows_hop=" << (ctx.allows_hop_path ? 1 : 0);
    return;
  }
  if (out.decision == CallHopPlannerDecision::Transition && out.next != hop_migrate_->Sfu().hop_planner_phase) {
    SetHopPlannerPhase(out.next, ev, call_id);
  } else {
    log().info << "planner=Hop keep phase=" << CallHopPlannerPhaseName(hop_migrate_->Sfu().hop_planner_phase)
               << " event=" << CallHopPlannerEventName(ev) << " call_id=" << call_id;
  }
  if (ev == CallHopPlannerEvent::Stop) {
    CancelAttachWaitTimer();
  }
}

void CallTopologyController::ClearAwaitingSfuRecovery() {
  hop_migrate_->Sfu().awaiting_recovery = false;
}

void CallTopologyController::OnMediaStopped(const std::string& call_id) {
  // Invalidate in-flight SoftMigrate / AttachLocalToSfu so they cannot StartSfu after Leave
  // (Linux quit dogfood: double-free from SDL reopen during teardown).
  Apply(CallHopPlannerEvent::Stop, call_id);
  hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  if (seat_.IsBound()) {
    seat_.cancel_attach_for_call(call_id);
  }
  if (hop_migrate_->Sfu().attached && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  hop_migrate_->Sfu().attached = false;
  hop_migrate_->Sfu().awaiting_recovery = false;
  hop_migrate_->Flight().in_flight = false;
  hop_migrate_->Flight().call_id.clear();
  hop_migrate_->Flight().attaching_hop_peer_id.clear();
  hop_migrate_->Flight().attached_hop_peer_id.clear();
  hop_migrate_->Flight().pending_hop_prefer.clear();
  hop_migrate_->InboundGate().pending_attach.reset();
  hop_migrate_->InboundGate().pending_call_id.clear();
  hop_migrate_->InboundGate().last_fail_call_id.clear();
  hop_migrate_->InboundGate().last_fail_hop.clear();
  hop_migrate_->Publishers().local_stream_id = 0;
  hop_migrate_->Publishers().remote_stream_ids.clear();
  hop_migrate_->Guest().active_attach.reset();
  hop_migrate_->Guest().active_call_id.clear();
  hop_migrate_->Guest().reattach_attempts = 0;
  hop_migrate_->Guest().reattach_in_flight = false;
  host_.ClearMediaActivity();
  if (hop_migrate_->AttachWaitState().call_id == call_id) {
    ClearSfuAttachWait();
  }
}

void CallTopologyController::PollPendingSfuAttach() {
  const std::string call_id = hop_migrate_->AttachWaitState().call_id;
  SfuAttachWaitPollInput in;
  in.wait_active = !call_id.empty();
  in.now_ms = util::NowUnixMs();
  in.deadline_ms = hop_migrate_->AttachWaitState().deadline_ms;
  in.soft_migrate_in_flight = hop_migrate_->Flight().in_flight;
  in.sfu_attached_for_call =
      hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
  if (auto joined = sessions_.CountJoined(call_id)) {
    in.joined_count = *joined;
  }
  in.media_active_mesh_for_call =
      media_.IsActive() && media_.ActiveCallId() == call_id && !media_.IsSfuMode();

  switch (PollSfuAttachWait(in)) {
  case SfuAttachWaitPollResult::Idle:
  case SfuAttachWaitPollResult::Waiting:
    return;
  case SfuAttachWaitPollResult::ClearAttached:
  case SfuAttachWaitPollResult::ClearAsP2p:
    ClearSfuAttachWait();
    hop_migrate_->Sfu().awaiting_recovery = false;
    return;
  case SfuAttachWaitPollResult::TimeoutLeave:
    ClearSfuAttachWait();
    hop_migrate_->Sfu().awaiting_recovery = false;
    host_.SetLastMediaError(Tr("call.error.no_media_relay_hop"));
    log().warning << "SFU attach wait timed out call_id=" << call_id;
    (void)host_.leave_call(call_id);
    return;
  }
}

void CallTopologyController::EjectParticipantAfterMigrateFailure(const std::string& call_id,
                                                                 const std::string& identity,
                                                                 const std::string& reason) {
  if (identity.empty()) {
    return;
  }
  host_.SetLastMediaError(reason);
  log().warning << "Ejecting " << identity << " after soft-migrate failure: " << reason;

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = identity;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (detail) {
    (void)host_.send_direct(identity, CallControlType::CallLeave, *detail, "Left the call");
  }

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Left;
  participant.left_at = util::NowUnixMs();
  (void)sessions_.UpsertParticipant(participant);

  if (detail) {
    auto local = host_.local_relay_identity();
    if (local) {
      (void)host_.fan_out_joined(call_id, CallControlType::CallLeave, *detail, "Left the call",
                                         *local);
    }
  }
  host_.NotifyRingChanged();
}

uint32_t CallTopologyController::PublisherStreamIdForLocal() const {
  auto local = host_.local_relay_identity();
  if (!local) {
    return 1;
  }
  return PublisherStreamIdForIdentity(*local);
}

void CallTopologyController::RefreshAdaptation(const std::string& call_id) {
  RefreshAdaptation(call_id, media_.IsCameraEnabled());
}

void CallTopologyController::RefreshAdaptation(const std::string& call_id, bool camera_user_wants) {
  bool video_allowed = false;
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    video_allowed = (*session)->video_allowed;
  }
  CallAdaptationInput in;
  in.camera_user_wants = camera_user_wants && video_allowed;
  in.muted = media_.IsMuted();
  in.per_user_up_bps = hop_migrate_->Sfu().last_quote_a_up_bps;
  in.allow_video_hi = false;
  double pressure = media_.PathPressure();
  if (relay_deps_.relay) {
    pressure = std::max(pressure, relay_deps_.relay->PathPressure());
  }
  in.path_pressure = pressure;
  media_.NoteUplinkBudget(hop_migrate_->Sfu().last_quote_a_up_bps);
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

CallHopHealth CallTopologyController::HopHealth() const {
  if (!relay_deps_.relay || !hop_migrate_->Sfu().attached) {
    return {};
  }
  return relay_deps_.relay->HealthSnapshot();
}

bool CallTopologyController::IsMigrateGenerationCurrent(uint64_t gen) const {
  return gen == 0 || gen == hop_migrate_->Flight().migrate_generation.load(std::memory_order_acquire);
}

std::string CallTopologyController::ResolveLocalAdvertiseMa(const std::string& local_peer_id) const {
  const auto mas = ResolveLocalAdvertiseMas();
  if (mas.empty()) {
    return {};
  }
  std::string local_ma = mas.front();
  if (!local_peer_id.empty() && local_ma.find("/p2p/") == std::string::npos) {
    local_ma += "/p2p/" + local_peer_id;
  }
  return local_ma;
}

std::vector<std::string> CallTopologyController::ResolveLocalAdvertiseMas() const {
  if (relay_deps_.resolve_local_advertise) {
    const auto live = relay_deps_.resolve_local_advertise();
    if (!live.empty()) {
      return live;
    }
  }
  if (!relay_deps_.local_advertise_multiaddrs.empty()) {
    return relay_deps_.local_advertise_multiaddrs;
  }
  if (!relay_deps_.local_listen_multiaddr.empty()) {
    return {relay_deps_.local_listen_multiaddr};
  }
  return {};
}

CallHopScope CallTopologyController::InferScopeForCall(const std::string& call_id,
                                                       const std::string& local_identity) const {
  const std::vector<std::string> local_mas = ResolveLocalAdvertiseMas();

  std::unordered_map<std::string, std::vector<std::string>> remotes;
  if (relay_deps_.resolve_remote_listen_by_peer) {
    remotes = relay_deps_.resolve_remote_listen_by_peer();
  }
  // Restrict to joined remotes; missing entry → empty vector → Wide.
  if (auto participants = sessions_.ListParticipants(call_id)) {
    std::unordered_map<std::string, std::vector<std::string>> joined_remotes;
    for (const CallParticipant& p : *participants) {
      if (p.state != CallParticipantState::Joined || p.identity.empty() ||
          p.identity == local_identity) {
        continue;
      }
      auto it = remotes.find(p.identity);
      if (it != remotes.end()) {
        joined_remotes[p.identity] = it->second;
      } else {
        joined_remotes[p.identity] = {};
      }
    }
    remotes = std::move(joined_remotes);
  }
  return InferCallHopScope(local_mas, remotes);
}

bool CallTopologyController::LanReachabilityConfirmedForCall(
    const std::string& call_id, const std::string& local_identity) const {
  if (!relay_deps_.peer_lan_confirmed) {
    return false;
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return false;
  }
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  if (relay_deps_.resolve_remote_listen_by_peer) {
    remotes = relay_deps_.resolve_remote_listen_by_peer();
  }
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined || p.identity.empty() ||
        p.identity == local_identity) {
      continue;
    }
    if (relay_deps_.peer_lan_confirmed(p.identity)) {
      return true;
    }
    auto it = remotes.find(p.identity);
    if (it == remotes.end()) {
      continue;
    }
    for (const std::string& ma : it->second) {
      const auto p2p = ma.rfind("/p2p/");
      if (p2p == std::string::npos) {
        continue;
      }
      std::string peer_id = ma.substr(p2p + 5);
      const auto slash = peer_id.find('/');
      if (slash != std::string::npos) {
        peer_id.resize(slash);
      }
      if (!peer_id.empty() && relay_deps_.peer_lan_confirmed(peer_id)) {
        return true;
      }
    }
  }
  return false;
}

bool CallTopologyController::IsActiveCallForTopology(const std::string& call_id) const {
  if (call_id.empty()) {
    return false;
  }

  // SoftMigrate / WaitForAttach are exclusive while in flight (zombie Active disk rows).
  if (!hop_migrate_->Flight().call_id.empty()) {
    return hop_migrate_->Flight().call_id == call_id;
  }
  if (!hop_migrate_->AttachWaitState().call_id.empty()) {
    return hop_migrate_->AttachWaitState().call_id == call_id;
  }

  // V036: seat bind is the media-active answer — never veto via leftover engine ActiveCallId.
  if (seat_.IsBound()) {
    if (seat_.is_bound(call_id)) {
      return true;
    }
    const std::string bound = seat_.bound_call_id();
    if (!bound.empty()) {
      return false;
    }
  } else {
    // Guest SFU attach bind without seat (unit tests): exclusive if that call is still Active.
    auto session_still_active = [this](const std::string& id) -> bool {
      if (id.empty()) {
        return false;
      }
      if (auto active = sessions_.ListActiveSessions(); active) {
        for (const CallSession& session : *active) {
          if (session.call_id == id) {
            return true;
          }
        }
      }
      return false;
    };
    if (!hop_migrate_->Guest().active_call_id.empty()) {
      if (hop_migrate_->Guest().active_call_id == call_id) {
        return true;
      }
      if (session_still_active(hop_migrate_->Guest().active_call_id)) {
        return false;
      }
    }
  }

  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return false;
  }
  for (const CallSession& session : *active) {
    if (session.call_id == call_id) {
      return true;
    }
  }
  return false;
}

void CallTopologyController::FanOutSfuAttachForHop(const std::string& call_id,
                                                   const std::string& hop_peer_id,
                                                   const std::string& local_identity) {
  if (hop_peer_id.empty()) {
    return;
  }
  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = hop_peer_id;
  attach.hop_multiaddr = ResolveHopMultiaddr(hop_peer_id);
  attach.publisher_stream_id = PublisherStreamIdForLocal();
  CallSfuAttachDetail fanout = BuildSfuAttachFanout(attach);
  if (auto encoded = CallControlCodec::EncodeSfuAttach(fanout)) {
    (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                       "Call SFU attach", local_identity);
  }
}

void CallTopologyController::FlushPendingHopPrefer(const std::string& call_id) {
  if (hop_migrate_->Flight().pending_hop_prefer.empty() || call_id.empty()) {
    return;
  }
  const std::string prefer = hop_migrate_->Flight().pending_hop_prefer;
  hop_migrate_->Flight().pending_hop_prefer.clear();
  if (!hop_migrate_->Flight().attached_hop_peer_id.empty() && prefer == hop_migrate_->Flight().attached_hop_peer_id) {
    auto local = host_.local_relay_identity();
    if (local) {
      FanOutSfuAttachForHop(call_id, prefer, *local);
    }
    SyncSfuSubscriptions(call_id);
    return;
  }
  if (hop_migrate_->Flight().in_flight) {
    hop_migrate_->Flight().pending_hop_prefer = prefer;
    return;
  }
  log().info << "Flush pending hop prefer=" << prefer << " call_id=" << call_id;
  const uint64_t gen = hop_migrate_->Flight().migrate_generation.load(std::memory_order_acquire);
  hop_migrate_->Flight().flight_gen = gen;
  hop_migrate_->Flight().in_flight = true;
  hop_migrate_->Flight().call_id = call_id;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::IceRecover, prefer, gen,
                             [this, call_id, gen](Roe<void> /*mig*/) {
    AppRuntime::PostUI([this, call_id, gen]() {
      if (hop_migrate_->Flight().flight_gen != gen && !IsMigrateGenerationCurrent(gen)) {
        return;
      }
      hop_migrate_->Flight().in_flight = false;
      hop_migrate_->Flight().call_id.clear();
      FlushPendingHopPrefer(call_id);
      FlushPendingInboundSfuAttach();
      host_.NotifyRingChanged();
    });
  });
}

void CallTopologyController::SubscribePublisherStream(uint32_t stream_id) {
  if (!relay_deps_.relay || stream_id == 0) {
    return;
  }
  if (hop_migrate_->Publishers().local_stream_id != 0 && stream_id == hop_migrate_->Publishers().local_stream_id) {
    return;
  }
  (void)relay_deps_.relay->Subscribe(stream_id, 0);
  (void)relay_deps_.relay->Subscribe(stream_id, 1);
  MaybeRequestPublisherKeyframe(stream_id);
}

void CallTopologyController::MaybeRequestPublisherKeyframe(uint32_t stream_id) {
  if (stream_id == 0 || !hop_migrate_->Publishers().video_refresh_sent.insert(stream_id).second) {
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return;
  }
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    if (PublisherStreamIdForIdentity(p.identity) != stream_id) {
      continue;
    }
    CallVideoRefreshDetail detail;
    detail.call_id = call_id;
    detail.identity = p.identity;
    auto encoded = CallControlCodec::EncodeVideoRefresh(detail);
    if (!encoded) {
      return;
    }
    (void)host_.send_direct(p.identity, CallControlType::CallVideoRefresh, *encoded, "");
    return;
  }
}

void CallTopologyController::NoteRemotePublisherFromAttach(const CallSfuAttachDetail& attach) {
  if (attach.publisher_stream_id == 0) {
    return;
  }
  if (hop_migrate_->Publishers().local_stream_id != 0 &&
      attach.publisher_stream_id == hop_migrate_->Publishers().local_stream_id) {
    return;
  }
  hop_migrate_->Publishers().remote_stream_ids.insert(attach.publisher_stream_id);
  if (hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == attach.call_id) {
    log().info << "SFU subscribe announced stream=" << attach.publisher_stream_id
               << " call_id=" << attach.call_id;
    SubscribePublisherStream(attach.publisher_stream_id);
  }
}

void CallTopologyController::AnnounceLocalPublisher(const std::string& call_id,
                                                    const CallSfuAttachDetail& hop_attach) {
  if (!hop_migrate_->Sfu().attached || hop_migrate_->Publishers().local_stream_id == 0) {
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  CallSfuAttachDetail announce = hop_attach;
  announce.call_id = call_id;
  announce.publisher_stream_id = hop_migrate_->Publishers().local_stream_id;
  announce.quote_id.clear();
  const CallSfuAttachDetail fanout = BuildSfuAttachFanout(announce);
  auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
  if (!encoded) {
    return;
  }
  log().info << "AnnounceLocalPublisher stream=" << hop_migrate_->Publishers().local_stream_id
             << " call_id=" << call_id;
  (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                     "Call SFU attach", *local);
  const std::string encoded_copy = *encoded;
  const std::string local_copy = *local;
  AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(2000), [this, call_id, encoded_copy, local_copy]() {
        if (!hop_migrate_->Sfu().attached || media_.ActiveCallId() != call_id) {
          return;
        }
        log().info << "AnnounceLocalPublisher re-fan-out call_id=" << call_id;
        (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, encoded_copy,
                                           "Call SFU attach", local_copy);
      });
}

void CallTopologyController::SyncSfuSubscriptions(const std::string& call_id) {
  if (!relay_deps_.relay || !hop_migrate_->Sfu().attached || !media_.IsSfuMode() || media_.ActiveCallId() != call_id) {
    return;
  }
  // Streams announced via CallSfuAttach (covers incomplete Joined roster).
  for (uint32_t stream : hop_migrate_->Publishers().remote_stream_ids) {
    SubscribePublisherStream(stream);
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return;
  }
  auto local = host_.local_relay_identity();
  int subscribed = 0;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    if (local && p.identity == *local) {
      continue;
    }
    const uint32_t stream = PublisherStreamIdForIdentity(p.identity);
    hop_migrate_->Publishers().remote_stream_ids.insert(stream);
    SubscribePublisherStream(stream);
    ++subscribed;
    log().info << "SFU subscribe peer=" << p.identity << " stream=" << stream << " call_id=" << call_id;
  }
  log().info << "SyncSfuSubscriptions call_id=" << call_id << " peers=" << subscribed
             << " announced=" << hop_migrate_->Publishers().remote_stream_ids.size();
}

Roe<void> CallTopologyController::MaybeSoftMigrateToSfu(const std::string& call_id,
                                                        SoftMigrateTrigger trigger,
                                                        const std::string& prefer_hop_peer_id,
                                                        uint64_t expected_gen) {
  return hop_migrate_->MaybeSoftMigrateToSfu(call_id, trigger, prefer_hop_peer_id, expected_gen);
}

void CallTopologyController::MaybeSoftMigrateToSfuAsync(const std::string& call_id,
                                                         SoftMigrateTrigger trigger,
                                                         const std::string& prefer_hop_peer_id,
                                                         uint64_t expected_gen,
                                                         std::function<void(Roe<void>)> on_done) {
  hop_migrate_->MaybeSoftMigrateToSfuAsync(call_id, trigger, prefer_hop_peer_id, expected_gen,
                                          std::move(on_done));
}

Roe<void> CallTopologyController::CompleteAttachLocalToSfu(
    const std::string& call_id, CallSfuAttachDetail attach, const bool self_hop, const int64_t a_up_bps,
    const uint64_t gen_at_start, const uint64_t cancel_gen_at_start,
    const std::shared_ptr<std::atomic<bool>>& sfu_frames_ready,
    const std::vector<uint8_t>& media_key, const uint32_t media_epoch) {
  return hop_migrate_->CompleteAttachLocalToSfu(call_id, std::move(attach), self_hop, a_up_bps,
                                               gen_at_start, cancel_gen_at_start, sfu_frames_ready,
                                               media_key, media_epoch);
}

void CallTopologyController::AttachLocalToSfuAsync(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach,
                                                   std::function<void(Roe<void>)> on_done) {
  hop_migrate_->AttachLocalToSfuAsync(call_id, attach, std::move(on_done));
}

Roe<void> CallTopologyController::AttachLocalToSfu(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach) {
  return hop_migrate_->AttachLocalToSfu(call_id, attach);
}

void CallTopologyController::OnGuestSfuTransportLost() {
  hop_migrate_->OnGuestSfuTransportLost();
}

Roe<void> CallTopologyController::ReattachGuestSfuTransport(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach) {
  return hop_migrate_->ReattachGuestSfuTransport(call_id, attach);
}

void CallTopologyController::ReattachGuestSfuTransportAsync(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach,
                                                            std::function<void(Roe<void>)> on_done) {
  hop_migrate_->ReattachGuestSfuTransportAsync(call_id, attach, std::move(on_done));
}

void CallTopologyController::TryRecoverViaSfu(const std::string& call_id) {
  if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
    return;
  }
  if (hop_migrate_->Flight().in_flight || (!hop_migrate_->AttachWaitState().call_id.empty() && hop_migrate_->AttachWaitState().call_id == call_id)) {
    return;
  }
  hop_migrate_->Sfu().awaiting_recovery = true;
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.finding_media_path"));
  host_.NotifyRingChanged();
  const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  hop_migrate_->Flight().flight_gen = gen;
  hop_migrate_->Flight().in_flight = true;
  hop_migrate_->Flight().call_id = call_id;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::IceRecover, {}, gen,
                             [this, call_id, gen](Roe<void> migrated) {
    const bool attached = hop_migrate_->Sfu().attached && media_.IsSfuMode();
    AppRuntime::PostUI([this, call_id, migrated, attached, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      hop_migrate_->Flight().in_flight = false;
      hop_migrate_->Flight().call_id.clear();
      if (attached || (hop_migrate_->Sfu().attached && media_.IsSfuMode())) {
        hop_migrate_->Sfu().awaiting_recovery = false;
        ClearSfuAttachWait();
        SyncSfuSubscriptions(call_id);
        host_.NotifyRingChanged();
        return;
      }
      if (!migrated) {
        hop_migrate_->Sfu().awaiting_recovery = false;
        const std::string msg =
            migrated.error().message.empty()
                ? Tr("call.error.no_media_relay_hop")
                : migrated.error().message;
        host_.SetLastMediaError(msg);
        log().warning << "ICE-fail SFU recovery failed: " << msg;
        (void)host_.leave_call(call_id);
        host_.NotifyRingChanged();
        return;
      }
      host_.NotifyRingChanged();
    });
  });
}

bool CallTopologyController::OnAnnounceViewerJoined(const std::string& call_id,
                                                   const std::optional<std::string>& sfu_hint) {
  if (sfu_hint && !sfu_hint->empty()) {
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = *sfu_hint;
    attach.hop_multiaddr = ResolveHopMultiaddr(*sfu_hint);
    attach.publisher_stream_id = PublisherStreamIdForLocal();
    host_.note_media_attempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.NotifyRingChanged();
    const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    hop_migrate_->Flight().flight_gen = gen;
    hop_migrate_->Flight().in_flight = true;
    hop_migrate_->Flight().call_id = call_id;
    AttachLocalToSfuAsync(call_id, attach, [this, call_id, gen](Roe<void> ok) {
      AppRuntime::PostUI([this, call_id, ok, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        hop_migrate_->Flight().in_flight = false;
        hop_migrate_->Flight().call_id.clear();
        if (!ok) {
          if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
            SyncSfuSubscriptions(call_id);
            host_.NotifyRingChanged();
            return;
          }
          log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
          host_.SetLastMediaError(ok.error().message);
          ClearSfuAttachWait();
          (void)host_.leave_call(call_id);
        } else {
          hop_migrate_->InboundGate().pending_attach.reset();
          hop_migrate_->InboundGate().pending_call_id.clear();
        }
        FlushPendingInboundSfuAttach();
        host_.NotifyRingChanged();
      });
    });
    return true;
  }
  ClearSfuAttachWait();
  hop_migrate_->Sfu().awaiting_recovery = false;
  log().info << "OnAnnounceViewerJoined defer media (no sfu_hint) call_id=" << call_id;
  return false;
}

bool CallTopologyController::OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                                 const std::optional<std::string>& sfu_hint) {
  Apply(CallHopPlannerEvent::LocalAcceptN3, call_id);
  if (n_joined >= 3 && sfu_hint && !sfu_hint->empty()) {
    ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = *sfu_hint;
    attach.hop_multiaddr = ResolveHopMultiaddr(*sfu_hint);
    attach.publisher_stream_id = PublisherStreamIdForLocal();
    host_.note_media_attempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.NotifyRingChanged();
    if (hop_migrate_->Flight().in_flight &&
        (hop_migrate_->Flight().call_id.empty() || hop_migrate_->Flight().call_id == call_id)) {
      // Inbound CallSfuAttach already dialing — do not bump hop_migrate_->Flight().migrate_generation.
      log().info << "OnLocalAcceptJoined keep in-flight attach (invite hint) call_id=" << call_id;
      hop_migrate_->InboundGate().pending_attach = attach;
      hop_migrate_->InboundGate().pending_call_id = call_id;
      return true;
    }
    const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    hop_migrate_->Flight().flight_gen = gen;
    hop_migrate_->Flight().in_flight = true;
    hop_migrate_->Flight().call_id = call_id;
    AttachLocalToSfuAsync(call_id, attach, [this, call_id, gen](Roe<void> ok) {
      AppRuntime::PostUI([this, call_id, ok, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        hop_migrate_->Flight().in_flight = false;
        hop_migrate_->Flight().call_id.clear();
        if (!ok) {
          if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
            SyncSfuSubscriptions(call_id);
            host_.NotifyRingChanged();
            return;
          }
          log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
          host_.SetLastMediaError(ok.error().message);
          ClearSfuAttachWait();
          (void)host_.leave_call(call_id);
        } else {
          hop_migrate_->InboundGate().pending_attach.reset();
          hop_migrate_->InboundGate().pending_call_id.clear();
        }
        FlushPendingInboundSfuAttach();
        host_.NotifyRingChanged();
      });
    });
    return true;
  }
  if (ShouldArmHopPlanner(n_joined)) {
    host_.note_media_attempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.setting_up_group"));
    host_.NotifyRingChanged();
    if (hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
      ClearSfuAttachWait();
      SyncSfuSubscriptions(call_id);
      Apply(CallHopPlannerEvent::AttachSucceeded, call_id);
      ReportHopProgress(CallHopPlannerPhase::Live, call_id);
      return true;
    }
    // Dogfood: CallSfuAttach often starts AcceptAndAttach before AcceptInvite finishes.
    // Bumping hop_migrate_->Flight().migrate_generation here aborts that worker before StartSfu — caller shows
    // Connected, guest stuck Connecting with streams=0.
    if (hop_migrate_->Flight().in_flight &&
        (hop_migrate_->Flight().call_id.empty() || hop_migrate_->Flight().call_id == call_id)) {
      log().info << "OnLocalAcceptJoined keep in-flight SoftMigrate/attach call_id=" << call_id;
      ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
      return true;
    }
    // PreferLocal Node may PickHop on LocalJoinedWithoutHint; phones/guests WaitForAttach.
    // WaitForAttach must not bump gen or hold hop_migrate_->Flight().in_flight (defers inbound attach).
    const bool may_prefer_local =
        relay_deps_.prefer_local_as_hop && relay_deps_.relay && relay_deps_.relay->IsStarted();
    if (!may_prefer_local) {
      log().info << "OnLocalAcceptJoined WaitForAttach call_id=" << call_id << " n=" << n_joined;
      ReportHopProgress(CallHopPlannerPhase::WaitingAttach, call_id);
      FlushPendingInboundSfuAttach();
      return true;
    }
    ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
    const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    hop_migrate_->Flight().flight_gen = gen;
    hop_migrate_->Flight().in_flight = true;
    hop_migrate_->Flight().call_id = call_id;
    MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::LocalJoinedWithoutHint, {}, gen,
                               [this, call_id, gen](Roe<void> mig) {
      const bool attached = hop_migrate_->Sfu().attached;
      AppRuntime::PostUI([this, call_id, mig, attached, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        hop_migrate_->Flight().in_flight = false;
        hop_migrate_->Flight().call_id.clear();
        if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          SyncSfuSubscriptions(call_id);
          hop_migrate_->InboundGate().pending_attach.reset();
          hop_migrate_->InboundGate().pending_call_id.clear();
          host_.NotifyRingChanged();
          return;
        }
        if (!mig) {
          log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
          host_.SetLastMediaError(mig.error().message);
          ClearSfuAttachWait();
          (void)host_.leave_call(call_id);
        } else if (!attached && !hop_migrate_->Sfu().attached) {
          // Wait for CallSfuAttach (LocalJoinedWithoutHint → WaitForAttach).
          FlushPendingInboundSfuAttach();
        } else {
          ClearSfuAttachWait();
        }
        host_.NotifyRingChanged();
      });
    });
    return true;
  }
  ClearSfuAttachWait();
  hop_migrate_->Sfu().awaiting_recovery = false;
  // Cancel any leftover SoftMigrate / inbound AcceptAndAttach so it cannot StartSfu on this 1:1
  // after ScheduleStartDirectMedia (dogfood: stale gen StartSfu → brief hop audio → "direct").
  hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  hop_migrate_->Flight().in_flight = false;
  hop_migrate_->Flight().call_id.clear();
  hop_migrate_->Flight().attaching_hop_peer_id.clear();
  hop_migrate_->InboundGate().pending_attach.reset();
  hop_migrate_->InboundGate().pending_call_id.clear();
  host_.ClearMediaActivity();
  log().info << "OnLocalAcceptJoined → P2P ScheduleStart call_id=" << call_id
             << " n=" << n_joined;
  return false;
}

bool CallTopologyController::OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                                  const std::string& joiner_identity) {
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "OnRemoteAcceptJoined ignored (not active call) call_id=" << call_id
               << " joiner=" << joiner_identity << " media_active=" << media_.ActiveCallId();
    // true → caller must not ScheduleStartDirectMedia for a stale/zombie call.
    return true;
  }
  if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    log().info << "OnRemoteAcceptJoined call_id=" << call_id << " n=" << n_joined
               << " joiner=" << joiner_identity << " sfu=" << (hop_migrate_->Sfu().attached ? 1 : 0)
               << " inflight=" << (hop_migrate_->Flight().in_flight ? 1 : 0);
    // Already on SFU (2nd concurrent accept): refresh subscriptions and re-fan-out attach so the
    // late joiner (missed SoftMigrate fan-out while still Ringing) can WaitForAttach → attach.
    if (hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
      SyncSfuSubscriptions(call_id);
      if (relay_deps_.relay && relay_deps_.relay->IsLocalHopAttached()) {
        if (auto hop = relay_deps_.relay->LocalPeerIdBase58(); hop) {
          CallSfuAttachDetail attach;
          attach.call_id = call_id;
          attach.hop_peer_id = *hop;
          attach.hop_multiaddr = ResolveHopMultiaddr(*hop);
          attach.publisher_stream_id = PublisherStreamIdForLocal();
          CallSfuAttachDetail fanout = BuildSfuAttachFanout(attach);
          if (auto encoded = CallControlCodec::EncodeSfuAttach(fanout); encoded) {
            if (auto local = host_.local_relay_identity(); local) {
              log().info << "OnRemoteAcceptJoined re-fan-out CallSfuAttach joiner="
                         << joiner_identity;
              (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                                 "Call SFU attach", *local);
            }
          }
        }
      }
      ClearSfuAttachWait();
      host_.NotifyRingChanged();
      return true;
    }
    // Overlapping Accept: keep the in-flight SoftMigrate; bumping gen would Detach mid-attach.
    if (hop_migrate_->Flight().in_flight) {
      log().info << "OnRemoteAcceptJoined defer SoftMigrate (already in flight) joiner="
                 << joiner_identity;
      BeginSfuAttachWait(call_id);
      host_.SetMediaActivity(Tr("call.status.setting_up_group"));
      host_.NotifyRingChanged();
      return true;
    }
    host_.note_media_attempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.setting_up_group"));
    host_.NotifyRingChanged();
    Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
    ReportHopProgress(CallHopPlannerPhase::Migrating, call_id);
    const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    hop_migrate_->Flight().flight_gen = gen;
    hop_migrate_->Flight().in_flight = true;
    hop_migrate_->Flight().call_id = call_id;
    MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::RemoteAcceptObserved, {}, gen,
                               [this, call_id, joiner_identity, gen](Roe<void> mig) {
      AppRuntime::PostUI([this, call_id, joiner_identity, mig, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        hop_migrate_->Flight().in_flight = false;
        hop_migrate_->Flight().call_id.clear();
        if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          SyncSfuSubscriptions(call_id);
          hop_migrate_->InboundGate().pending_attach.reset();
          hop_migrate_->InboundGate().pending_call_id.clear();
          host_.NotifyRingChanged();
          return;
        }
        if (!mig) {
          log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
          EjectParticipantAfterMigrateFailure(
              call_id, joiner_identity,
              mig.error().message.empty()
                ? Tr("call.error.no_media_relay_hop")
                : mig.error().message);
        }
        // Non-initiator WaitForAttach: keep wait; apply deferred CallSfuAttach from owner.
        FlushPendingInboundSfuAttach();
        host_.NotifyRingChanged();
      });
    });
    return true;
  }
  log().info << "OnRemoteAcceptJoined stay P2P call_id=" << call_id << " n=" << n_joined
             << " joiner=" << joiner_identity;
  ClearSfuAttachWait();
  hop_migrate_->Sfu().awaiting_recovery = false;
  host_.ClearMediaActivity();
  return false;
}

void CallTopologyController::OnPeerMediaRelayCapLearned(const std::string& call_id,
                                                        const std::string& peer_id) {
  if (call_id.empty()) {
    return;
  }
  size_t n_joined = 0;
  if (auto joined = sessions_.CountJoined(call_id)) {
    n_joined = *joined;
  }
  size_t n_active = n_joined;
  if (auto all = sessions_.ListParticipants(call_id); all) {
    n_active = CountMediaPlannerActiveParticipants(*all);
  }
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = EffectiveMediaPlannerN(n_joined, n_active);
  in.sfu_attach_wait_active = IsSfuAttachWaitActive();
  in.sfu_attached = IsSfuAttached();
  in.already_on_sfu_for_call = IsOnSfuForCall(call_id);
  if (!ShouldNudgeSoftMigrateOnRelayCap(in)) {
    log().info << "SoftMigrate relay-cap nudge skipped (1:1 stay Direct) call_id=" << call_id
               << " n=" << in.effective_n << " peer=" << peer_id;
    return;
  }
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  MaybeSoftMigrateToSfuAsync(
      call_id, SoftMigrateTrigger::JoinedCountObserved, {}, 0,
      [this](Roe<void> mig) {
        if (!mig) {
          log().warning << "SoftMigrate (relay-cap nudge) failed: " << mig.error().message;
        }
        AppRuntime::PostUI([this]() { host_.NotifyRingChanged(); });
      });
}

void CallTopologyController::OnJoinedCountObserved(const std::string& call_id, size_t n_joined) {
  if (!CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    return;
  }
  if (hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    // Roster grew while already on SFU — ensure we subscribe to any late Joined peers.
    SyncSfuSubscriptions(call_id);
    return;
  }
  if (hop_migrate_->Flight().in_flight) {
    log().info << "OnJoinedCountObserved skip SoftMigrate (in flight) n=" << n_joined
               << " call_id=" << call_id;
    return;
  }
  // Already waiting for owner CallSfuAttach — re-entry only bumps gen and thrashs inbox.
  if (!hop_migrate_->AttachWaitState().call_id.empty() && hop_migrate_->AttachWaitState().call_id == call_id) {
    log().info << "OnJoinedCountObserved skip SoftMigrate (attach wait) n=" << n_joined
               << " call_id=" << call_id;
    host_.RequestInboxSync();
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  const bool first_attach =
      !session || !session->has_value() || !(*session)->sfu_hint || (*session)->sfu_hint->empty();
  if (!first_attach) {
    return;
  }

  host_.note_media_attempted(call_id);
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.setting_up_group"));
  host_.NotifyRingChanged();
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  ReportHopProgress(CallHopPlannerPhase::Migrating, call_id);
  const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  hop_migrate_->Flight().flight_gen = gen;
  hop_migrate_->Flight().in_flight = true;
  hop_migrate_->Flight().call_id = call_id;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::JoinedCountObserved, {}, gen,
                             [this, call_id, gen](Roe<void> mig) {
    AppRuntime::PostUI([this, call_id, mig, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      hop_migrate_->Flight().in_flight = false;
      hop_migrate_->Flight().call_id.clear();
      if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
        ClearSfuAttachWait();
        SyncSfuSubscriptions(call_id);
        hop_migrate_->InboundGate().pending_attach.reset();
        hop_migrate_->InboundGate().pending_call_id.clear();
        host_.NotifyRingChanged();
        return;
      }
      if (!mig) {
        log().warning << "MaybeSoftMigrateToSfu (roster) failed: " << mig.error().message;
        host_.SetLastMediaError(mig.error().message);
        // Do not LeaveCall here — attach-wait / inviter eject paths handle failure.
      } else {
        FlushPendingInboundSfuAttach();
        if (!hop_migrate_->Sfu().attached) {
          host_.RequestInboxSync();
        }
      }
      host_.NotifyRingChanged();
    });
  });
}

Roe<void> CallTopologyController::OnInboundSfuAttach(const std::string& call_id,
                                                     const CallSfuAttachDetail& attach) {
  log().info << "OnInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " sfu=" << (hop_migrate_->Sfu().attached ? 1 : 0) << " inflight=" << (hop_migrate_->Flight().in_flight ? 1 : 0);
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "OnInboundSfuAttach ignored (not active call) call_id=" << call_id
               << " media_active=" << media_.ActiveCallId();
    return {};
  }
  // 1:1 must stay on call-media duplex. Owner SoftMigrate / inflated roster can still fan
  // CallSfuAttach; accepting it races ScheduleStartDirectMedia (intermittent hop sound → "direct").
  // Allow only when Status arms Topology (V037) or we are waiting / SoftMigrating this call.
  size_t n_joined = 0;
  if (auto joined = sessions_.CountJoined(call_id)) {
    n_joined = *joined;
  }
  const bool status_allows_hop = !arming_.IsBound() || arming_.hop_ops_allowed();
  const bool expect_group_attach =
      status_allows_hop &&
      (CallMediaTopology::ShouldUseMediaRelay(n_joined) || hop_migrate_->AttachWaitState().call_id == call_id ||
       (hop_migrate_->Flight().in_flight && hop_migrate_->Flight().call_id == call_id) || hop_migrate_->Sfu().attached);
  if (!expect_group_attach) {
    log().info << "OnInboundSfuAttach ignored (1:1 / hop not armed) call_id=" << call_id
               << " n_joined=" << n_joined << " hop=" << attach.hop_peer_id
               << " arming="
               << (arming_.IsBound() && arming_.arming_debug_name ? arming_.arming_debug_name()
                                                                  : "null");
    return {};
  }
  Apply(CallHopPlannerEvent::SfuAttachInbound, call_id);
  if (hop_migrate_->Sfu().hop_planner_phase == CallHopPlannerPhase::Attaching) {
    ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  }
  if (hop_migrate_->Sfu().attached && media_.ActiveCallId() == call_id &&
      (media_.IsSfuMode() || hop_migrate_->Flight().attached_hop_peer_id == attach.hop_peer_id)) {
    // Already attached (duplicate fan-out / late roster / peer publisher announce).
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    ClearSfuAttachWait();
    host_.ClearMediaActivity();
    host_.NotifyRingChanged();
    return {};
  }
  // Same hop already dialing — coalesce even if hop_migrate_->Flight().in_flight briefly cleared.
  if (seat_.IsBound() && seat_.has_attach_in_flight()) {
    if (seat_.attaching_hop() == attach.hop_peer_id) {
      log().info << "OnInboundSfuAttach coalesce (seat attaching same hop) call_id=" << call_id
                 << " hop=" << attach.hop_peer_id;
    } else {
      hop_migrate_->InboundGate().pending_attach = attach;
      hop_migrate_->InboundGate().pending_call_id = call_id;
      log().info << "OnInboundSfuAttach deferred (seat attach in flight) call_id=" << call_id
                 << " in_flight_hop=" << seat_.attaching_hop()
                 << " requested=" << attach.hop_peer_id;
    }
    BeginSfuAttachWait(call_id);
    return {};
  }
  if (!hop_migrate_->Flight().attaching_hop_peer_id.empty()) {
    if (hop_migrate_->Flight().attaching_hop_peer_id == attach.hop_peer_id) {
      log().info << "OnInboundSfuAttach coalesce (attaching same hop) call_id=" << call_id
                 << " hop=" << attach.hop_peer_id;
    } else {
      // Different hop while AcceptAndAttach in flight — defer; do not parallel Detach.
      hop_migrate_->InboundGate().pending_attach = attach;
      hop_migrate_->InboundGate().pending_call_id = call_id;
      log().info << "OnInboundSfuAttach deferred (attach in flight) call_id=" << call_id
                 << " in_flight_hop=" << hop_migrate_->Flight().attaching_hop_peer_id << " requested=" << attach.hop_peer_id;
    }
    BeginSfuAttachWait(call_id);
    return {};
  }
  // SoftMigrate PickHop may be mid-AcceptAndAttach. Bumping gen Detach's that stream and races
  // libp2p asio (Moto SIGSEGV on pp-worker). Defer until SoftMigrate clears in-flight.
  if (hop_migrate_->Flight().in_flight) {
    if (!hop_migrate_->Flight().call_id.empty() && call_id != hop_migrate_->Flight().call_id) {
      log().info << "OnInboundSfuAttach ignored (SoftMigrate in flight for other call)"
                 << " pending_call=" << hop_migrate_->Flight().call_id << " call_id=" << call_id;
      return {};
    }
    hop_migrate_->InboundGate().pending_attach = attach;
    hop_migrate_->InboundGate().pending_call_id = call_id;
    log().info << "OnInboundSfuAttach deferred (SoftMigrate in flight) call_id=" << call_id;
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.NotifyRingChanged();
    return {};
  }

  // Cross-net: PreferLocal often fans a private LAN MA. Fail fast and ask owner to re-pick a
  // shared public hop instead of waiting for media-relay timeout.
  if (!attach.hop_multiaddr.empty() && MultiaddrHasPrivateIpv4Host(attach.hop_multiaddr)) {
    const auto local_mas = ResolveLocalAdvertiseMas();
    const bool may_dial = GuestMayDialPrivateHopMa(attach.hop_multiaddr, local_mas);
    const bool lan_hop =
        relay_deps_.peer_lan_confirmed && relay_deps_.peer_lan_confirmed(attach.hop_peer_id);
    if (!may_dial || !lan_hop) {
      log().warning << "OnInboundSfuAttach skip private hop MA hop=" << attach.hop_peer_id
                    << " ma=" << attach.hop_multiaddr << " may_dial=" << (may_dial ? 1 : 0)
                    << " lan_hop=" << (lan_hop ? 1 : 0);
      BeginSfuAttachWait(call_id);
      host_.SetMediaActivity(Tr("call.status.looking_for_another_path"));
      host_.NotifyRingChanged();
      ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id,
                                       "hop multiaddr not reachable (private)");
      return {};
    }
  }

  BeginSfuAttachWait(call_id);
  if (seat_.IsBound()) {
    seat_.note_connecting(call_id);
  }
  host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
  host_.NotifyRingChanged();
  const uint64_t gen = hop_migrate_->Flight().migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  hop_migrate_->Flight().flight_gen = gen;
  hop_migrate_->Flight().in_flight = true;
  hop_migrate_->Flight().call_id = call_id;
  AttachLocalToSfuAsync(call_id, attach, [this, call_id, attach, gen](Roe<void> ok) {
    if (!IsMigrateGenerationCurrent(gen)) {
      log().info << "OnInboundSfuAttach worker gen moved want=" << gen
                 << " have=" << hop_migrate_->Flight().migrate_generation.load(std::memory_order_acquire)
                 << " attached=" << (hop_migrate_->Sfu().attached ? 1 : 0) << " ok=" << (ok ? 1 : 0);
      AppRuntime::PostUI([this, gen, call_id, attach, ok]() {
        const bool duplex =
            hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
        // Attach finished successfully (StartSfu may still be settling on another worker).
        // Never ReportSfuAttachFailed here — that makes the owner RefuseGuest → CallHopRefuse →
        // LeaveCall mid-call (dogfood: Connected then aborted).
        if (ok || duplex) {
          if (hop_migrate_->Flight().flight_gen == gen || duplex) {
            hop_migrate_->Flight().in_flight = false;
            hop_migrate_->Flight().call_id.clear();
          }
          if (duplex) {
            hop_migrate_->InboundGate().pending_attach.reset();
            hop_migrate_->InboundGate().pending_call_id.clear();
            ClearSfuAttachWait();
            SyncSfuSubscriptions(call_id);
            host_.ClearMediaActivity();
          } else if (hop_migrate_->Flight().flight_gen == gen) {
            FlushPendingInboundSfuAttach();
          }
          host_.NotifyRingChanged();
          return;
        }
        if (hop_migrate_->Flight().flight_gen == gen) {
          hop_migrate_->Flight().in_flight = false;
          hop_migrate_->Flight().call_id.clear();
          FlushPendingInboundSfuAttach();
        }
        // True failure under a superseded gen — wait for a fresh fan-out, do not refuse.
        BeginSfuAttachWait(call_id);
        host_.NotifyRingChanged();
      });
      return;
    }
    AppRuntime::PostUI([this, call_id, attach, ok, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      hop_migrate_->Flight().in_flight = false;
      hop_migrate_->Flight().call_id.clear();
      if (!ok) {
        if (hop_migrate_->Sfu().attached && media_.IsSfuMode()) {
          SyncSfuSubscriptions(call_id);
          host_.NotifyRingChanged();
          return;
        }
        host_.SetLastMediaError(ok.error().message);
        log().warning << "AttachLocalToSfu (inbound) failed: " << ok.error().message;
        // V029: ask owner to re-pick or refuse — keep attach-wait for a re-fan-out.
        ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id, ok.error().message);
        BeginSfuAttachWait(call_id);
        host_.NotifyRingChanged();
        return;
      }
      hop_migrate_->InboundGate().pending_attach.reset();
      hop_migrate_->InboundGate().pending_call_id.clear();
      SyncSfuSubscriptions(call_id);
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
    });
  });
  return {};
}

void CallTopologyController::FlushPendingInboundSfuAttach() {
  if (!hop_migrate_->InboundGate().pending_attach || hop_migrate_->InboundGate().pending_call_id.empty()) {
    return;
  }
  const std::string call_id = hop_migrate_->InboundGate().pending_call_id;
  const CallSfuAttachDetail attach = *hop_migrate_->InboundGate().pending_attach;
  hop_migrate_->InboundGate().pending_attach.reset();
  hop_migrate_->InboundGate().pending_call_id.clear();
  if (hop_migrate_->Sfu().attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    return;
  }
  log().info << "FlushPendingInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id;
  (void)OnInboundSfuAttach(call_id, attach);
}

std::vector<std::string> CallTopologyController::DialableHopPeerIds() const {
  std::vector<std::string> out;
  for (const MeshHopCandidate& hop : RankedMediaHopCandidates()) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (hop.dialable || (relay_deps_.dial && relay_deps_.dial->IsDialable(hop.peer_id))) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

void CallTopologyController::ReportSfuAttachFailedToInitiator(const std::string& call_id,
                                                              const std::string& failed_hop,
                                                              const std::string& error) {
  if (!call_id.empty() && call_id == hop_migrate_->InboundGate().last_fail_call_id &&
      failed_hop == hop_migrate_->InboundGate().last_fail_hop) {
    log().debug << "ReportSfuAttachFailed deduped call_id=" << call_id << " hop=" << failed_hop;
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return;
  }
  std::vector<SoftMigrateJoinedPeer> joined_peers;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    SoftMigrateJoinedPeer peer;
    peer.identity = p.identity;
    peer.joined_at = p.joined_at;
    joined_peers.push_back(std::move(peer));
  }
  const std::string initiator = SelectCallInitiator(joined_peers);
  if (initiator.empty() || initiator == *local) {
    // Local is initiator (or unknown): cannot ask self — leave with friendly copy.
    host_.SetLastMediaError(Tr("call.error.hop_unreachable_guest"));
    ClearSfuAttachWait();
    (void)host_.leave_call(call_id);
    return;
  }

  CallSfuAttachFailedDetail detail;
  detail.call_id = call_id;
  detail.identity = *local;
  detail.failed_hop_peer_id = failed_hop;
  detail.error = error;
  detail.preferred_hop_peer_ids =
      CapGuestHopPreferences(DialableHopPeerIds(), failed_hop, kMaxGuestHopPreferences);
  auto encoded = CallControlCodec::EncodeSfuAttachFailed(detail);
  if (!encoded) {
    return;
  }
  hop_migrate_->InboundGate().last_fail_call_id = call_id;
  hop_migrate_->InboundGate().last_fail_hop = failed_hop;
  log().info << "ReportSfuAttachFailed to initiator=" << initiator << " prefs="
             << detail.preferred_hop_peer_ids.size();
  host_.SetMediaActivity(Tr("call.status.looking_for_another_path"));
  host_.NotifyRingChanged();
  (void)host_.send_direct(initiator, CallControlType::CallSfuAttachFailed, *encoded,
                                 "Call hop attach failed");
}

void CallTopologyController::RefuseGuestNoSharedHop(const std::string& call_id,
                                                    const std::string& guest_identity) {
  // Call-control arrives on Browser IO (PollInbox); eject/UI must not run there — SoftMigrate
  // dogfood: malloc corruption / abort when refusing Samsung mid PreferLocal.
  AppRuntime::PostUI([this, call_id, guest_identity]() {
    const std::string message = Tr("call.error.hop_unreachable_guest");
    CallHopRefuseDetail refuse;
    refuse.call_id = call_id;
    refuse.identity = guest_identity;
    refuse.reason = "no_shared_hop";
    refuse.message = message;
    auto encoded = CallControlCodec::EncodeHopRefuse(refuse);
    if (encoded) {
      (void)host_.send_direct(guest_identity, CallControlType::CallHopRefuse, *encoded, message);
    }
    std::string display = guest_identity;
    if (auto hit = contacts_.FindByIdentity(guest_identity); hit && hit->has_value()) {
      const std::string title = FormatContactTitle(**hit);
      if (!title.empty()) {
        display = title;
      }
    }
    const std::string owner_toast = Tr("call.error.hop_unreachable_owner", {{"name", display}});
    EjectParticipantAfterMigrateFailure(call_id, guest_identity, owner_toast);
  });
}

void CallTopologyController::OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail) {
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  auto participants = sessions_.ListParticipants(detail.call_id);
  if (!participants) {
    return;
  }
  std::vector<SoftMigrateJoinedPeer> joined_peers;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    SoftMigrateJoinedPeer peer;
    peer.identity = p.identity;
    peer.joined_at = p.joined_at;
    joined_peers.push_back(std::move(peer));
  }
  if (SelectCallInitiator(joined_peers) != *local) {
    return; // only sticky initiator handles hop hints
  }

  const std::string guest = detail.identity.empty() ? std::string{} : detail.identity;
  const auto decision = DecideHopHintOwnerAction(detail.preferred_hop_peer_ids, DialableHopPeerIds(),
                                                 detail.failed_hop_peer_id);
  if (decision.action == HopHintOwnerAction::RefuseGuest || guest.empty()) {
    log().warning << "Hop hint refuse guest=" << guest << " failed_hop=" << detail.failed_hop_peer_id;
    if (!guest.empty()) {
      RefuseGuestNoSharedHop(detail.call_id, guest);
    }
    return;
  }

  std::string local_pid;
  if (relay_deps_.relay) {
    if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
      local_pid = *pid;
    }
  }
  // PreferLocal LAN hop often unreachable for cross-net guests. If guest/owner share another
  // dialable hop (directory/org seed), SoftMigrate off PreferLocal onto that hop.
  if (relay_deps_.relay && relay_deps_.relay->IsLocalHopAttached()) {
    const bool public_repick =
        !decision.preferred_hop_peer_id.empty() && decision.preferred_hop_peer_id != local_pid;
    if (!public_repick) {
      log().warning << "Hop hint refuse (keep PreferLocal) guest=" << guest
                    << " failed_hop=" << detail.failed_hop_peer_id
                    << " prefer=" << decision.preferred_hop_peer_id;
      if (!guest.empty()) {
        RefuseGuestNoSharedHop(detail.call_id, guest);
      }
      return;
    }
    log().info << "Hop hint PreferLocal → shared public hop re-pick prefer="
               << decision.preferred_hop_peer_id << " guest=" << guest;
  }

  // Already SoftMigrated onto the guest prefer hop: re-fan-out only (do not Detach again).
  if (hop_migrate_->Sfu().attached && !decision.preferred_hop_peer_id.empty()) {
    const std::string current =
        !hop_migrate_->Flight().attached_hop_peer_id.empty() ? hop_migrate_->Flight().attached_hop_peer_id : std::string{};
    auto session = sessions_.LoadSession(detail.call_id);
    const std::string hint =
        (session && session->has_value() && (*session)->sfu_hint) ? *(*session)->sfu_hint : "";
    if (decision.preferred_hop_peer_id == current || decision.preferred_hop_peer_id == hint) {
      log().info << "Hop hint no-op: already on prefer hop=" << decision.preferred_hop_peer_id
                 << " guest=" << guest;
      FanOutSfuAttachForHop(detail.call_id, decision.preferred_hop_peer_id, *local);
      SyncSfuSubscriptions(detail.call_id);
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
      return;
    }
  }
  if (hop_migrate_->Flight().in_flight) {
    hop_migrate_->Flight().pending_hop_prefer = decision.preferred_hop_peer_id;
    log().info << "Hop hint coalesced prefer=" << hop_migrate_->Flight().pending_hop_prefer << " guest=" << guest;
    return;
  }

  log().info << "Hop hint re-pick prefer=" << decision.preferred_hop_peer_id << " guest=" << guest;
  host_.SetMediaActivity(Tr("call.status.switching_media_path"));
  host_.NotifyRingChanged();
  BeginSfuAttachWait(detail.call_id);
  // V035: do not bump hop_migrate_->Flight().migrate_generation on hop-hint (Leave/teardown only).
  const uint64_t gen = hop_migrate_->Flight().migrate_generation.load(std::memory_order_acquire);
  hop_migrate_->Flight().flight_gen = gen;
  hop_migrate_->Flight().in_flight = true;
  hop_migrate_->Flight().call_id = detail.call_id;
  const std::string prefer = decision.preferred_hop_peer_id;
  MaybeSoftMigrateToSfuAsync(detail.call_id, SoftMigrateTrigger::IceRecover, prefer, gen,
                             [this, call_id = detail.call_id, guest, gen](Roe<void> mig) {
    AppRuntime::PostUI([this, call_id, mig, guest, gen]() {
      if (hop_migrate_->Flight().flight_gen != gen) {
        return;
      }
      hop_migrate_->Flight().in_flight = false;
      hop_migrate_->Flight().call_id.clear();
      if (!mig) {
        if (mig.error().message == "keep_prefer_local") {
          RefuseGuestNoSharedHop(call_id, guest);
          return;
        }
        log().warning << "Hop hint re-pick failed: " << mig.error().message;
        RefuseGuestNoSharedHop(call_id, guest);
        return;
      }
      SyncSfuSubscriptions(call_id);
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
      FlushPendingHopPrefer(call_id);
      FlushPendingInboundSfuAttach();
    });
  });
}

void CallTopologyController::OnInboundHopRefuse(const CallHopRefuseDetail& detail) {
  if (!IsActiveCallForTopology(detail.call_id)) {
    log().info << "CallHopRefuse ignored (not active call) call_id=" << detail.call_id;
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  if (!detail.identity.empty() && detail.identity != *local) {
    return;
  }
  const std::string message =
      detail.message.empty() ? Tr("call.error.hop_unreachable_guest") : detail.message;
  host_.SetLastMediaError(message);
  log().warning << "CallHopRefuse call_id=" << detail.call_id << " reason=" << detail.reason;
  ClearSfuAttachWait();
  hop_migrate_->Sfu().awaiting_recovery = false;
  (void)host_.leave_call(detail.call_id);
  host_.NotifyRingChanged();
}

} // namespace pbr
