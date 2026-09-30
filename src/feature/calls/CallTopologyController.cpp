#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallsThread.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallHopPlannerLogic.h"

#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "foundation/i18n/LocalizationService.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/people/PeerDisplayLabel.h"
#include "foundation/platform/PlatformUserHints.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/Utilities.h"
#include "common/directory/MeshHopDial.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

CallTopologyController::CallTopologyController(CallSessionStore& sessions, ContactsStore& contacts,
                                               CallMediaEngine& media)
    : hop_migrate_(sessions, media),
      flight_(hop_migrate_.Flight()),
      attach_wait_(hop_migrate_.AttachWaitState()),
      inbound_gate_(hop_migrate_.InboundGate()),
      guest_(hop_migrate_.Guest()),
      publishers_(hop_migrate_.Publishers()),
      sfu_(hop_migrate_.Sfu()),
      sessions_(sessions),
      contacts_(contacts),
      media_(media),
      ranking_(contacts),
      planning_(sessions, ranking_) {
  redirectLogger("CallTopologyController");
  ranking_.SetMediaRelayDeps(&relay_deps_);
  planning_.SetMediaRelayDeps(&relay_deps_);
  BindHopMigratePortsAndOps();
}

void CallTopologyController::BindHopMigratePortsAndOps() {
  hop_migrate_.SetMediaRelayDeps(&relay_deps_);
  CallHopMigrateWorkflow::TopologyOps ops;
  ops.apply = [this](CallHopPlannerEvent ev, const std::string& call_id) { Apply(ev, call_id); };
  ops.sync_sfu_subscriptions = [this](const std::string& call_id) { SyncSfuSubscriptions(call_id); };
  ops.refresh_adaptation = [this](const std::string& call_id) { RefreshAdaptation(call_id); };
  ops.ranked_media_hop_candidates = [this]() { return ranking_.Ranked(); };
  ops.resolve_hop_multiaddr = [this](const std::string& hop_peer_id) {
    return ranking_.HopMultiaddr(hop_peer_id);
  };
  ops.resolve_local_advertise_ma = [this](const std::string& local_peer_id) {
    return ranking_.LocalAdvertiseMa(local_peer_id);
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
  hop_migrate_.SetTopologyOps(std::move(ops));
}

void CallTopologyController::SetHostPorts(HostPorts ports) {
  hop_migrate_.SetHostPorts(MakeMigrateHostPorts(ports));
  host_ = std::move(ports);
}

CallTopologyController::~CallTopologyController() {
  // No RemoveClientTransportLostObserver here: the relay may already be gone (CallStack::Shutdown
  // clears the media plane first; mesh stop unwatches through SetMediaRelayDeps({})). Invalidating
  // drops notices still queued for us.
  planning_.Invalidate();
}

void CallTopologyController::SetMediaRelayDeps(MediaRelayDeps deps) {
  UnwatchRelayLoss();
  relay_deps_ = std::move(deps);
  hop_migrate_.SetMediaRelayDeps(&relay_deps_);
  WatchRelayLoss();
}

void CallTopologyController::WatchRelayLoss() {
  if (!relay_deps_.relay) {
    return;
  }
  relay_loss_observer_ = relay_deps_.relay->AddClientTransportLostObserver(
      [outbox = outbox_, watch = ++relay_watch_](MediaRelayClientLoss loss) {
        // Only a dead transport means reattach: our own hop migrations and leaves replace / detach
        // the session too. (Another feature taking the client is the call + broadcast case — see
        // media-client-layers PHASES "Allow call + broadcast at once".)
        if (loss != MediaRelayClientLoss::TransportLost) {
          return;
        }
        outbox.Emit(topology_event::RelayTransportLost{watch});  // edge: the relay's thread only reports
      });
}

void CallTopologyController::UnwatchRelayLoss() {
  if (relay_deps_.relay && relay_loss_observer_ != 0) {
    relay_deps_.relay->RemoveClientTransportLostObserver(relay_loss_observer_);
  }
  relay_loss_observer_ = 0;
  ++relay_watch_;  // a loss the old relay reported is stale
}

void CallTopologyController::SetMediaKeyStore(CallMediaKeyStore* keys) {
  media_keys_ = keys;
  hop_migrate_.SetMediaKeyStore(keys);
}

void CallTopologyController::SetSeatPorts(CallTopologySeatPorts ports) {
  hop_migrate_.SetSeatPorts(MakeMigrateSeatPorts(ports));
  seat_.Set(std::move(ports));
}

void CallTopologyController::SetHopArmingPorts(CallHopArmingPorts ports) {
  hop_migrate_.SetArmingPorts(MakeMigrateArmingPorts(ports));
  arming_.Set(std::move(ports));
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
  out.call_media = ports.call_media;
  out.clear_media_peer_identity = ports.clear_media_peer_identity;
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
  return flight_.in_flight || !attach_wait_.call_id.empty() ||
         guest_.reattach_in_flight;
}

bool CallTopologyController::IsSfuAttached() const {
  return sfu_.attached;
}

bool CallTopologyController::IsOnSfuForCall(const std::string& call_id) const {
  return sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
}

bool CallTopologyController::IsSoftMigrateInFlight() const {
  return flight_.in_flight;
}

bool CallTopologyController::IsSfuAttachWaitActive() const {
  return !attach_wait_.call_id.empty();
}

void CallTopologyController::BeginSfuAttachWait(const std::string& call_id) {
  attach_wait_.call_id = call_id;
  attach_wait_.deadline_ms = util::NowUnixMs() + kSfuAttachWaitDefaultMs;
  ArmAttachWaitTimer(call_id, attach_wait_.deadline_ms);
}

void CallTopologyController::ClearSfuAttachWait() {
  CancelAttachWaitTimer();
  attach_wait_.call_id.clear();
  attach_wait_.deadline_ms = 0;
}

void CallTopologyController::CancelAttachWaitTimer() {
  ++attach_wait_.armed;  // a deadline already queued is stale
  outbox_.Cancel(attach_wait_.timer_id);
}

void CallTopologyController::ArmAttachWaitTimer(const std::string& call_id, int64_t deadline_ms) {
  CancelAttachWaitTimer();
  if (call_id.empty() || deadline_ms <= 0) {
    return;
  }
  const int64_t delay = std::max<int64_t>(0, deadline_ms - util::NowUnixMs());
  attach_wait_.timer_id = outbox_.After(std::chrono::milliseconds(delay),
                                        topology_event::AttachWaitDeadline{call_id, ++attach_wait_.armed});
}

void CallTopologyController::OnAttachWaitTimerFire(const std::string& call_id) {
  if (attach_wait_.call_id != call_id) {
    return;
  }
  if (sfu_.attached) {
    ClearSfuAttachWait();
    return;
  }
  Apply(CallHopPlannerEvent::AttachWaitExpired, call_id);
  // Attach-wait expiry runs PollPendingSfuAttach for eject / recovery (timer primary path).
  PollPendingSfuAttach();
}

CallHopPlannerApplyContext CallTopologyController::BuildHopPlannerContext(
    const std::string& call_id, size_t joined_count, bool has_sfu_hint) const {
  const auto arming_ports = arming_.Get();
  CallHopPlannerApplyContext ctx;
  ctx.allows_hop_path = !arming_ports->IsBound() || arming_ports->hop_ops_allowed();
  ctx.should_arm_hop = ShouldArmHopPlanner(joined_count);
  ctx.has_sfu_hint = has_sfu_hint;
  ctx.soft_migrate_in_flight = flight_.in_flight;
  ctx.sfu_attached = sfu_.attached && media_.IsSfuMode() &&
                     (call_id.empty() || media_.ActiveCallId() == call_id);
  return ctx;
}

void CallTopologyController::SetHopPlannerPhase(const CallHopPlannerPhase next,
                                                const CallHopPlannerEvent ev,
                                                const std::string& call_id) {
  const CallHopPlannerPhase prev = sfu_.hop_planner_phase;
  sfu_.hop_planner_phase = next;
  log().info << "planner=Hop phase=" << CallHopPlannerPhaseName(prev) << "->"
             << CallHopPlannerPhaseName(next) << " event=" << CallHopPlannerEventName(ev)
             << " call_id=" << call_id
             << " migrate_gen=" << flight_.migrate_generation.load(std::memory_order_acquire);
}

void CallTopologyController::ReportHopProgress(const CallHopPlannerPhase phase,
                                               const std::string& call_id) {
  const auto arming_ports = arming_.Get();
  if (arming_ports->report_progress) {
    arming_ports->report_progress(phase, call_id);
  }
}

void CallTopologyController::Apply(CallHopPlannerEvent ev, const std::string& call_id) {
  size_t n_joined = 0;
  if (!call_id.empty()) {
    if (auto j = sessions_.CountJoined(call_id)) {
      n_joined = *j;
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
      BuildHopPlannerContext(call_id, n_joined, has_hint);
  const CallHopPlannerPhaseOutcome out = DecideCallHopPlannerPhase(sfu_.hop_planner_phase, ev, ctx);
  if (out.decision == CallHopPlannerDecision::Ignore) {
    log().info << "planner=Hop ignore event=" << CallHopPlannerEventName(ev)
               << " phase=" << CallHopPlannerPhaseName(sfu_.hop_planner_phase) << " call_id=" << call_id
               << " allows_hop=" << (ctx.allows_hop_path ? 1 : 0);
    return;
  }
  if (out.decision == CallHopPlannerDecision::Transition && out.next != sfu_.hop_planner_phase) {
    SetHopPlannerPhase(out.next, ev, call_id);
  } else {
    log().info << "planner=Hop keep phase=" << CallHopPlannerPhaseName(sfu_.hop_planner_phase)
               << " event=" << CallHopPlannerEventName(ev) << " call_id=" << call_id;
  }
  if (ev == CallHopPlannerEvent::Stop) {
    CancelAttachWaitTimer();
  }
}

void CallTopologyController::OnMediaStopped(const std::string& call_id) {
  const auto seat_ports = seat_.Get();
  // Invalidate in-flight SoftMigrate / AttachLocalToSfu so they cannot StartSfu after Leave
  // (Linux quit dogfood: double-free from SDL reopen during teardown).
  Apply(CallHopPlannerEvent::Stop, call_id);
  flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  if (seat_ports->IsBound()) {
    seat_ports->cancel_attach_for_call(call_id);
  }
  if (sfu_.attached && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_.attached = false;
  ReleaseMigrateFlight();
  flight_.attaching_hop_peer_id.clear();
  flight_.attached_hop_peer_id.clear();
  flight_.pending_hop_prefer.clear();
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending_call_id.clear();
  inbound_gate_.last_fail_call_id.clear();
  inbound_gate_.last_fail_hop.clear();
  publishers_.local_stream_id = 0;
  {
    std::lock_guard lock(publishers_.mu);
    publishers_.remote_stream_ids.clear();
  }
  guest_.active_attach.reset();
  guest_.active_call_id.clear();
  guest_.reattach_attempts = 0;
  guest_.reattach_in_flight = false;
  host_.ClearMediaActivity();
  hop_hint_repicked_.erase(call_id);
  planning_.Forget(call_id);
  if (attach_wait_.call_id == call_id) {
    ClearSfuAttachWait();
  }
}

void CallTopologyController::PollPendingSfuAttach() {
  const std::string call_id = attach_wait_.call_id;
  SfuAttachWaitPollInput in;
  in.wait_active = !call_id.empty();
  in.now_ms = util::NowUnixMs();
  in.deadline_ms = attach_wait_.deadline_ms;
  in.soft_migrate_in_flight = flight_.in_flight;
  in.sfu_attached_for_call =
      sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
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
    return;
  case SfuAttachWaitPollResult::TimeoutLeave:
    ClearSfuAttachWait();
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
  in.per_user_up_bps = sfu_.last_quote_a_up_bps;
  in.allow_video_hi = false;
  double pressure = media_.PathPressure();
  if (relay_deps_.relay) {
    pressure = std::max(pressure, relay_deps_.relay->PathPressure());
  }
  in.path_pressure = pressure;
  media_.NoteUplinkBudget(sfu_.last_quote_a_up_bps);
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

CallHopHealth CallTopologyController::HopHealth() const {
  if (!relay_deps_.relay || !sfu_.attached) {
    return {};
  }
  return relay_deps_.relay->HealthSnapshot();
}

bool CallTopologyController::IsMigrateGenerationCurrent(uint64_t gen) const {
  return gen == 0 || gen == flight_.migrate_generation.load(std::memory_order_acquire);
}

std::vector<std::string> CallTopologyController::JoinedRemoteIdentities(const std::string& call_id,
                                                                     const std::string& local_identity) const {
  std::vector<std::string> out;
  if (auto participants = sessions_.ListParticipants(call_id)) {
    for (const CallParticipant& p : *participants) {
      if (p.state == CallParticipantState::Joined && !p.identity.empty() && p.identity != local_identity) {
        out.push_back(p.identity);
      }
    }
  }
  return out;
}

std::vector<std::string> CallTopologyController::JoinedMembersOtherThan(const std::string& call_id,
                                                                     const std::string& guest) const {
  auto local = host_.local_relay_identity();
  std::vector<std::string> out;
  for (std::string& identity : JoinedRemoteIdentities(call_id, local ? *local : std::string())) {
    if (identity != guest) {
      out.push_back(std::move(identity));
    }
  }
  return out;
}

bool CallTopologyController::ResolveGroupHopForJoin(const std::string& call_id, const std::string& joiner_identity) {
  auto local = host_.local_relay_identity();
  const auto outcome =
      planning_.ResolveAtJoin(call_id, joiner_identity, JoinedRemoteIdentities(call_id, local ? *local : std::string()));
  if (outcome == CallHopPlanning::JoinOutcome::RefuseJoiner) {
    RefuseGuestNoSharedHop(call_id, joiner_identity);
    return false;
  }
  return true;
}

bool CallTopologyController::AwaitsOwnInviteeAccept(const std::string& call_id,
                                                    const std::string& local_identity) const {
  for (const std::string& identity : JoinedRemoteIdentities(call_id, local_identity)) {
    auto invite = sessions_.LoadPendingInvite(call_id, identity);
    // Inbound CallAccept marks the row "accepted" before OnRemoteAcceptJoined runs.
    if (invite && invite->has_value() && (*invite)->inviter_identity == local_identity &&
        (*invite)->status == "pending") {
      return true;
    }
  }
  return false;
}

CallHopScope CallTopologyController::InferScopeForCall(const std::string& call_id,
                                                       const std::string& local_identity) const {
  return ranking_.ScopeForPeers(JoinedRemoteIdentities(call_id, local_identity));
}

bool CallTopologyController::LanReachabilityConfirmedForCall(
    const std::string& call_id, const std::string& local_identity) const {
  return ranking_.LanConfirmedForPeers(JoinedRemoteIdentities(call_id, local_identity));
}

bool CallTopologyController::IsActiveCallForTopology(const std::string& call_id) const {
  const auto seat_ports = seat_.Get();
  if (call_id.empty()) {
    return false;
  }

  // SoftMigrate / WaitForAttach are exclusive while in flight (zombie Active disk rows).
  if (!flight_.call_id.empty()) {
    return flight_.call_id == call_id;
  }
  if (!attach_wait_.call_id.empty()) {
    return attach_wait_.call_id == call_id;
  }

  // V036: seat bind is the media-active answer — never veto via leftover engine ActiveCallId.
  if (seat_ports->IsBound()) {
    if (seat_ports->is_bound(call_id)) {
      return true;
    }
    const std::string bound = seat_ports->bound_call_id();
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
    if (!guest_.active_call_id.empty()) {
      if (guest_.active_call_id == call_id) {
        return true;
      }
      if (session_still_active(guest_.active_call_id)) {
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
  attach.hop_multiaddr = ranking_.HopMultiaddr(hop_peer_id);
  attach.publisher_stream_id = PublisherStreamIdForLocal();
  CallSfuAttachDetail fanout = BuildSfuAttachFanout(attach);
  if (auto encoded = CallControlCodec::EncodeSfuAttach(fanout)) {
    (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                       "Call SFU attach", local_identity);
  }
}

void CallTopologyController::FlushPendingHopPrefer(const std::string& call_id) {
  if (flight_.pending_hop_prefer.empty() || call_id.empty()) {
    return;
  }
  const std::string prefer = flight_.pending_hop_prefer;
  flight_.pending_hop_prefer.clear();
  if (!flight_.attached_hop_peer_id.empty() && prefer == flight_.attached_hop_peer_id) {
    auto local = host_.local_relay_identity();
    if (local) {
      FanOutSfuAttachForHop(call_id, prefer, *local);
    }
    SyncSfuSubscriptions(call_id);
    return;
  }
  if (flight_.in_flight) {
    flight_.pending_hop_prefer = prefer;
    return;
  }
  log().info << "Flush pending hop prefer=" << prefer << " call_id=" << call_id;
  const uint64_t gen = flight_.migrate_generation.load(std::memory_order_acquire);
  flight_.flight_gen = gen;
  flight_.in_flight = true;
  flight_.call_id = call_id;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::IceRecover, prefer, gen,
                             [this, call_id, gen](Roe<void> /*mig*/) {
    CallsThread::Post([this, call_id, gen]() {
      if (flight_.flight_gen != gen && !IsMigrateGenerationCurrent(gen)) {
        return;
      }
      ReleaseMigrateFlight();
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
  const uint32_t local_stream = publishers_.local_stream_id.load();
  if (local_stream != 0 && stream_id == local_stream) {
    return;
  }
  (void)relay_deps_.relay->Subscribe(stream_id, 0);
  (void)relay_deps_.relay->Subscribe(stream_id, 1);
  MaybeRequestPublisherKeyframe(stream_id);
}

void CallTopologyController::MaybeRequestPublisherKeyframe(uint32_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  {
    std::lock_guard lock(publishers_.mu);
    if (!publishers_.video_refresh_sent.insert(stream_id).second) {
      return;
    }
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
  const uint32_t local_stream = publishers_.local_stream_id.load();
  if (local_stream != 0 && attach.publisher_stream_id == local_stream) {
    return;
  }
  {
    std::lock_guard lock(publishers_.mu);
    publishers_.remote_stream_ids.insert(attach.publisher_stream_id);
  }
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == attach.call_id) {
    log().info << "SFU subscribe announced stream=" << attach.publisher_stream_id
               << " call_id=" << attach.call_id;
    SubscribePublisherStream(attach.publisher_stream_id);
  }
}

void CallTopologyController::AnnounceLocalPublisher(const std::string& call_id,
                                                    const CallSfuAttachDetail& hop_attach) {
  if (!sfu_.attached || publishers_.local_stream_id == 0) {
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  CallSfuAttachDetail announce = hop_attach;
  announce.call_id = call_id;
  announce.publisher_stream_id = publishers_.local_stream_id;
  announce.quote_id.clear();
  const CallSfuAttachDetail fanout = BuildSfuAttachFanout(announce);
  auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
  if (!encoded) {
    return;
  }
  log().info << "AnnounceLocalPublisher stream=" << publishers_.local_stream_id
             << " call_id=" << call_id;
  (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                     "Call SFU attach", *local);
  outbox_.After(std::chrono::milliseconds(2000),
                 topology_event::ReannouncePublisher{call_id, *encoded, *local, announce.hop_peer_id,
                                                     announce.publisher_stream_id});
}

void CallTopologyController::SyncSfuSubscriptions(const std::string& call_id) {
  if (!relay_deps_.relay || !sfu_.attached || !media_.IsSfuMode() || media_.ActiveCallId() != call_id) {
    return;
  }
  // Runs on the calls owner: update the stream sets under the lock (read off the owner),
  // subscribe after releasing it (relay I/O takes the relay's own lock).
  std::vector<uint32_t> to_subscribe;
  {
    // Streams announced via CallSfuAttach (covers incomplete Joined roster).
    std::lock_guard lock(publishers_.mu);
    to_subscribe.assign(publishers_.remote_stream_ids.begin(), publishers_.remote_stream_ids.end());
  }
  auto participants = sessions_.ListParticipants(call_id);
  int subscribed = 0;
  size_t announced = 0;
  if (participants) {
    auto local = host_.local_relay_identity();
    for (const CallParticipant& p : *participants) {
      if (p.state != CallParticipantState::Joined) {
        continue;
      }
      if (local && p.identity == *local) {
        continue;
      }
      const uint32_t stream = PublisherStreamIdForIdentity(p.identity);
      {
        std::lock_guard lock(publishers_.mu);
        publishers_.remote_stream_ids.insert(stream);
      }
      to_subscribe.push_back(stream);
      ++subscribed;
      log().info << "SFU subscribe peer=" << p.identity << " stream=" << stream << " call_id=" << call_id;
    }
  }
  {
    std::lock_guard lock(publishers_.mu);
    announced = publishers_.remote_stream_ids.size();
  }
  for (const uint32_t stream : to_subscribe) {
    SubscribePublisherStream(stream);
  }
  if (!participants) {
    return;
  }
  log().info << "SyncSfuSubscriptions call_id=" << call_id << " peers=" << subscribed
             << " announced=" << announced;
}

void CallTopologyController::MaybeSoftMigrateToSfuAsync(const std::string& call_id,
                                                         SoftMigrateTrigger trigger,
                                                         const std::string& prefer_hop_peer_id,
                                                         uint64_t expected_gen,
                                                         std::function<void(Roe<void>)> on_done) {
  hop_migrate_.MaybeSoftMigrateToSfuAsync(call_id, trigger, prefer_hop_peer_id, expected_gen,
                                          std::move(on_done));
}

void CallTopologyController::AttachLocalToSfuAsync(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach,
                                                   std::function<void(Roe<void>)> on_done) {
  hop_migrate_.AttachLocalToSfuAsync(call_id, attach, std::move(on_done));
}

void CallTopologyController::OnGuestSfuTransportLost() {
  hop_migrate_.OnGuestSfuTransportLost();
}

uint64_t CallTopologyController::ClaimMigrateFlight(const std::string& call_id) {
  const uint64_t gen = flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  flight_.flight_gen = gen;
  flight_.in_flight = true;
  flight_.call_id = call_id;
  return gen;
}

void CallTopologyController::ReleaseMigrateFlight() {
  flight_.in_flight = false;
  flight_.call_id.clear();
}

bool CallTopologyController::MigrateFlightBusyFor(const std::string& call_id) const {
  return flight_.in_flight && (flight_.call_id.empty() || flight_.call_id == call_id);
}

bool CallTopologyController::ExpectsGroupMedia(const std::string& call_id) const {
  if (call_id.empty()) {
    return false;
  }
  CallExpectGroupSfuInput in;
  in.awaiting_sfu_recovery = IsAwaitingSfuRecovery();
  in.sfu_attached = IsSfuAttached();
  if (auto n = sessions_.CountJoined(call_id)) {
    in.joined_count = *n;
  }
  if (auto session = sessions_.LoadSession(call_id);
      session && *session && (*session)->sfu_hint && !(*session)->sfu_hint->empty()) {
    in.has_sfu_hint = true;
  }
  return ShouldExpectGroupSfuMigration(in);
}

bool CallTopologyController::OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                                 const std::optional<std::string>& sfu_hint) {
  Apply(CallHopPlannerEvent::LocalAcceptN3, call_id);
  if (n_joined >= 3 && sfu_hint && !sfu_hint->empty()) {
    AttachFromInviteHint(call_id, *sfu_hint);
    return true;
  }
  if (ShouldArmHopPlanner(n_joined)) {
    JoinGroupWithoutHint(call_id, n_joined);
    return true;
  }
  StayDirectAfterAccept(call_id, n_joined);
  return false;
}

void CallTopologyController::AttachFromInviteHint(const std::string& call_id, const std::string& hop_peer_id) {
  ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = hop_peer_id;
  attach.hop_multiaddr = ranking_.HopMultiaddr(hop_peer_id);
  attach.publisher_stream_id = PublisherStreamIdForLocal();
  host_.note_media_attempted(call_id);
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
  host_.NotifyRingChanged();
  if (MigrateFlightBusyFor(call_id)) {
    // Inbound CallSfuAttach already dialing — do not bump flight_.migrate_generation.
    log().info << "OnLocalAcceptJoined keep in-flight attach (invite hint) call_id=" << call_id;
    DeferInboundSfuAttach(call_id, attach);
    return;
  }
  const uint64_t gen = ClaimMigrateFlight(call_id);
  AttachLocalToSfuAsync(call_id, attach, [this, call_id, gen](Roe<void> ok) {
    CallsThread::Post([this, call_id, ok, gen]() { FinishInviteHintAttach(call_id, gen, ok); });
  });
}

void CallTopologyController::FinishInviteHintAttach(const std::string& call_id, uint64_t gen, const Roe<void>& ok) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (!ok) {
    if (sfu_.attached && media_.IsSfuMode()) {
      SyncSfuSubscriptions(call_id);
      host_.NotifyRingChanged();
      return;
    }
    log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
    host_.SetLastMediaError(ok.error().message);
    // V050: a joiner the group's hop cannot serve asks the owner (one hop change per joiner, or a
    // refusal) — ReportSfuAttachFailedToInitiator leaves when we are the owner ourselves.
    std::string hop;
    if (auto session = sessions_.LoadSession(call_id); session && session->has_value() && (*session)->sfu_hint) {
      hop = *(*session)->sfu_hint;
    }
    ReportSfuAttachFailedToInitiator(call_id, hop, ok.error().message);
  } else {
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending_call_id.clear();
  }
  FlushPendingInboundSfuAttach();
  host_.NotifyRingChanged();
}

void CallTopologyController::JoinGroupWithoutHint(const std::string& call_id, size_t n_joined) {
  host_.note_media_attempted(call_id);
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.setting_up_group"));
  host_.NotifyRingChanged();
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    ClearSfuAttachWait();
    SyncSfuSubscriptions(call_id);
    Apply(CallHopPlannerEvent::AttachSucceeded, call_id);
    ReportHopProgress(CallHopPlannerPhase::Live, call_id);
    return;
  }
  // Dogfood: CallSfuAttach often starts AcceptAndAttach before AcceptInvite finishes.
  // Bumping flight_.migrate_generation here aborts that worker before StartSfu — caller shows
  // Connected, guest stuck Connecting with streams=0.
  if (MigrateFlightBusyFor(call_id)) {
    log().info << "OnLocalAcceptJoined keep in-flight SoftMigrate/attach call_id=" << call_id;
    ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
    return;
  }
  // PreferLocal Node may PickHop on LocalJoinedWithoutHint; phones/guests WaitForAttach.
  // WaitForAttach must not bump gen or hold flight_.in_flight (defers inbound attach).
  const bool may_prefer_local = relay_deps_.prefer_local_as_hop && relay_deps_.relay && relay_deps_.relay->IsStarted();
  if (!may_prefer_local) {
    log().info << "OnLocalAcceptJoined WaitForAttach call_id=" << call_id << " n=" << n_joined;
    ReportHopProgress(CallHopPlannerPhase::WaitingAttach, call_id);
    FlushPendingInboundSfuAttach();
    return;
  }
  ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::LocalJoinedWithoutHint, {}, gen,
                             [this, call_id, gen](Roe<void> mig) {
                               const bool attached = sfu_.attached;
                               CallsThread::Post([this, call_id, mig, attached, gen]() {
                                 FinishJoinSoftMigrate(call_id, gen, mig, attached);
                               });
                             });
}

void CallTopologyController::FinishJoinSoftMigrate(const std::string& call_id, uint64_t gen, const Roe<void>& mig,
                                                   bool attached_when_done) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (sfu_.attached && media_.IsSfuMode()) {
    ClearSfuAttachWait();
    SyncSfuSubscriptions(call_id);
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending_call_id.clear();
    host_.NotifyRingChanged();
    return;
  }
  if (!mig) {
    log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
    host_.SetLastMediaError(mig.error().message);
    ClearSfuAttachWait();
    (void)host_.leave_call(call_id);
  } else if (!attached_when_done && !sfu_.attached) {
    // Wait for CallSfuAttach (LocalJoinedWithoutHint → WaitForAttach).
    FlushPendingInboundSfuAttach();
  } else {
    ClearSfuAttachWait();
  }
  host_.NotifyRingChanged();
}

void CallTopologyController::StayDirectAfterAccept(const std::string& call_id, size_t n_joined) {
  ClearSfuAttachWait();
  // Cancel any leftover SoftMigrate / inbound AcceptAndAttach so it cannot StartSfu on this 1:1
  // after ScheduleStartDirectMedia (dogfood: stale gen StartSfu → brief hop audio → "direct").
  flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  ReleaseMigrateFlight();
  flight_.attaching_hop_peer_id.clear();
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending_call_id.clear();
  host_.ClearMediaActivity();
  log().info << "OnLocalAcceptJoined → P2P ScheduleStart call_id=" << call_id << " n=" << n_joined;
}

bool CallTopologyController::OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                                  const std::string& joiner_identity) {
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "OnRemoteAcceptJoined ignored (not active call) call_id=" << call_id
               << " joiner=" << joiner_identity << " media_active=" << media_.ActiveCallId();
    // true → caller must not ScheduleStartDirectMedia for a stale/zombie call.
    return true;
  }
  if (!CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    log().info << "OnRemoteAcceptJoined stay P2P call_id=" << call_id << " n=" << n_joined
               << " joiner=" << joiner_identity;
    ClearSfuAttachWait();
    host_.ClearMediaActivity();
    return false;
  }
  log().info << "OnRemoteAcceptJoined call_id=" << call_id << " n=" << n_joined << " joiner=" << joiner_identity
             << " sfu=" << (sfu_.attached ? 1 : 0) << " inflight=" << (flight_.in_flight ? 1 : 0);
  // Already on SFU (2nd concurrent accept): refresh subscriptions and re-fan-out attach so the
  // late joiner (missed SoftMigrate fan-out while still Ringing) can WaitForAttach → attach.
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    SyncSfuSubscriptions(call_id);
    RefanOutLocalHopForJoiner(call_id, joiner_identity);
    ClearSfuAttachWait();
    host_.NotifyRingChanged();
    return true;
  }
  // V050: the first migrate goes to the planned hop — or the one adjustment, or the joiner is refused
  // and the call stays as it is.
  if (!flight_.in_flight && !ResolveGroupHopForJoin(call_id, joiner_identity)) {
    return true;  // joiner refused; nothing to schedule for it
  }
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.setting_up_group"));
  host_.NotifyRingChanged();
  // Overlapping Accept: keep the in-flight SoftMigrate; bumping gen would Detach mid-attach.
  if (flight_.in_flight) {
    log().info << "OnRemoteAcceptJoined defer SoftMigrate (already in flight) joiner=" << joiner_identity;
    return true;
  }
  host_.note_media_attempted(call_id);
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  ReportHopProgress(CallHopPlannerPhase::Migrating, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::RemoteAcceptObserved, {}, gen,
                             [this, call_id, joiner_identity, gen](Roe<void> mig) {
                               CallsThread::Post([this, call_id, joiner_identity, mig, gen]() {
                                 FinishRemoteAcceptMigrate(call_id, joiner_identity, gen, mig);
                               });
                             });
  return true;
}

void CallTopologyController::RefanOutLocalHopForJoiner(const std::string& call_id, const std::string& joiner_identity) {
  if (!relay_deps_.relay || !relay_deps_.relay->IsLocalHopAttached()) {
    return;
  }
  auto hop = relay_deps_.relay->LocalPeerIdBase58();
  auto local = host_.local_relay_identity();
  if (!hop || !local) {
    return;
  }
  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = *hop;
  attach.hop_multiaddr = ranking_.HopMultiaddr(*hop);
  attach.publisher_stream_id = PublisherStreamIdForLocal();
  if (auto encoded = CallControlCodec::EncodeSfuAttach(BuildSfuAttachFanout(attach))) {
    log().info << "OnRemoteAcceptJoined re-fan-out CallSfuAttach joiner=" << joiner_identity;
    (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", *local);
  }
}

void CallTopologyController::FinishRemoteAcceptMigrate(const std::string& call_id, const std::string& joiner_identity,
                                                       uint64_t gen, const Roe<void>& mig) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (sfu_.attached && media_.IsSfuMode()) {
    ClearSfuAttachWait();
    SyncSfuSubscriptions(call_id);
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending_call_id.clear();
    host_.NotifyRingChanged();
    return;
  }
  if (!mig) {
    log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
    EjectParticipantAfterMigrateFailure(
        call_id, joiner_identity,
        mig.error().message.empty() ? Tr("call.error.no_media_relay_hop") : mig.error().message);
  }
  // Non-initiator WaitForAttach: keep wait; apply deferred CallSfuAttach from owner.
  FlushPendingInboundSfuAttach();
  host_.NotifyRingChanged();
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
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = n_joined;
  in.sfu_attach_wait_active = IsSfuAttachWaitActive();
  in.sfu_attached = IsSfuAttached();
  in.already_on_sfu_for_call = IsOnSfuForCall(call_id);
  if (!ShouldNudgeSoftMigrateOnRelayCap(in)) {
    log().info << "SoftMigrate relay-cap nudge skipped (1:1 stay Direct) call_id=" << call_id
               << " n=" << in.joined_count << " peer=" << peer_id;
    return;
  }
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  MaybeSoftMigrateToSfuAsync(
      call_id, SoftMigrateTrigger::JoinedCountObserved, {}, 0,
      [this](Roe<void> mig) {
        if (!mig) {
          log().warning << "SoftMigrate (relay-cap nudge) failed: " << mig.error().message;
        }
        CallsThread::Post([this]() { host_.NotifyRingChanged(); });
      });
}

void CallTopologyController::OnJoinedCountObserved(const std::string& call_id, size_t n_joined) {
  if (!CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    return;
  }
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    // Roster grew while already on SFU — ensure we subscribe to any late Joined peers.
    SyncSfuSubscriptions(call_id);
    return;
  }
  if (flight_.in_flight) {
    log().info << "OnJoinedCountObserved skip SoftMigrate (in flight) n=" << n_joined
               << " call_id=" << call_id;
    return;
  }
  // Already waiting for owner CallSfuAttach — re-entry only bumps gen and thrashs inbox.
  if (!attach_wait_.call_id.empty() && attach_wait_.call_id == call_id) {
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
  // V050: with a planned hop the initiator forms the group from the joiner's CallAccept — it carries
  // the hop report. A CallRoster that shows the join first (transports reorder) must not migrate
  // without that report — but only a joiner this device invited sends its Accept here; one another
  // member added accepts to that member, so its roster is all this device will see.
  if (session && session->has_value() && (*session)->planned_hop) {
    if (auto local = host_.local_relay_identity();
        local && IsStickyInitiator(call_id, *local) && AwaitsOwnInviteeAccept(call_id, *local)) {
      log().info << "OnJoinedCountObserved: wait for the joiner's CallAccept (planned hop) n=" << n_joined
                 << " call_id=" << call_id;
      return;
    }
  }

  host_.note_media_attempted(call_id);
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.setting_up_group"));
  host_.NotifyRingChanged();
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  ReportHopProgress(CallHopPlannerPhase::Migrating, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::JoinedCountObserved, {}, gen,
                             [this, call_id, gen](Roe<void> mig) {
    CallsThread::Post([this, call_id, mig, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      ReleaseMigrateFlight();
      if (sfu_.attached && media_.IsSfuMode()) {
        ClearSfuAttachWait();
        SyncSfuSubscriptions(call_id);
        inbound_gate_.pending_attach.reset();
        inbound_gate_.pending_call_id.clear();
        host_.NotifyRingChanged();
        return;
      }
      if (!mig) {
        log().warning << "MaybeSoftMigrateToSfu (roster) failed: " << mig.error().message;
        host_.SetLastMediaError(mig.error().message);
        // Do not LeaveCall here — attach-wait / inviter eject paths handle failure.
      } else {
        FlushPendingInboundSfuAttach();
        if (!sfu_.attached) {
          host_.RequestInboxSync();
        }
      }
      host_.NotifyRingChanged();
    });
  });
}

Roe<void> CallTopologyController::OnInboundSfuAttach(const std::string& call_id,
                                                     const CallSfuAttachDetail& attach, const std::string& sender) {
  log().info << "OnInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " sfu=" << (sfu_.attached ? 1 : 0) << " inflight=" << (flight_.in_flight ? 1 : 0);
  if (!ExpectsInboundSfuAttach(call_id, attach)) {
    return {};
  }
  Apply(CallHopPlannerEvent::SfuAttachInbound, call_id);
  if (sfu_.hop_planner_phase == CallHopPlannerPhase::Attaching) {
    ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  }
  if (!LeaveHopForOwnerMove(call_id, attach, sender) && SettleInboundSfuAttachWithoutDial(call_id, attach)) {
    return {};
  }
  StartInboundSfuAttach(call_id, attach);
  return {};
}

bool CallTopologyController::LeaveHopForOwnerMove(const std::string& call_id, const CallSfuAttachDetail& attach,
                                                  const std::string& sender) {
  if (!sfu_.attached || attach.hop_peer_id.empty() || flight_.attached_hop_peer_id.empty() ||
      flight_.attached_hop_peer_id == attach.hop_peer_id || sender.empty() || !IsStickyInitiator(call_id, sender)) {
    return false;
  }
  log().info << "OnInboundSfuAttach owner moved the group " << flight_.attached_hop_peer_id << " → "
             << attach.hop_peer_id << " call_id=" << call_id;
  if (relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_.attached = false;
  flight_.attached_hop_peer_id.clear();
  flight_.attaching_hop_peer_id.clear();
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    CallSession moved = **session;
    moved.sfu_hint.reset();
    (void)sessions_.UpsertSession(moved);
  }
  return true;
}

bool CallTopologyController::ExpectsInboundSfuAttach(const std::string& call_id,
                                                     const CallSfuAttachDetail& attach) const {
  const auto arming_ports = arming_.Get();
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "OnInboundSfuAttach ignored (not active call) call_id=" << call_id
               << " media_active=" << media_.ActiveCallId();
    return false;
  }
  // 1:1 must stay on call-media duplex. Owner SoftMigrate / inflated roster can still fan
  // CallSfuAttach; accepting it races ScheduleStartDirectMedia (intermittent hop sound → "direct").
  // Allow only when Status arms Topology (V037) or we are waiting / SoftMigrating this call.
  size_t n_joined = 0;
  if (auto joined = sessions_.CountJoined(call_id)) {
    n_joined = *joined;
  }
  const bool status_allows_hop = !arming_ports->IsBound() || arming_ports->hop_ops_allowed();
  const bool expect_group_attach =
      status_allows_hop &&
      (CallMediaTopology::ShouldUseMediaRelay(n_joined) || attach_wait_.call_id == call_id ||
       (flight_.in_flight && flight_.call_id == call_id) || sfu_.attached);
  if (!expect_group_attach) {
    log().info << "OnInboundSfuAttach ignored (1:1 / hop not armed) call_id=" << call_id << " n_joined=" << n_joined
               << " hop=" << attach.hop_peer_id << " arming="
               << (arming_ports->IsBound() && arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "null");
  }
  return expect_group_attach;
}

void CallTopologyController::DeferInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) {
  inbound_gate_.pending_attach = attach;
  inbound_gate_.pending_call_id = call_id;
}

bool CallTopologyController::SettleInboundSfuAttachWithoutDial(const std::string& call_id,
                                                               const CallSfuAttachDetail& attach) {
  const auto seat_ports = seat_.Get();
  if (sfu_.attached && media_.ActiveCallId() == call_id &&
      (media_.IsSfuMode() || flight_.attached_hop_peer_id == attach.hop_peer_id)) {
    // Already attached (duplicate fan-out / late roster / peer publisher announce).
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    ClearSfuAttachWait();
    host_.ClearMediaActivity();
    host_.NotifyRingChanged();
    return true;
  }
  // An attach already dialing (seat flight, or attaching_hop even if flight_.in_flight briefly
  // cleared): same hop coalesces; a different hop waits — never a parallel Detach.
  const bool seat_in_flight = seat_ports->IsBound() && seat_ports->has_attach_in_flight();
  const std::string in_flight_hop = seat_in_flight ? seat_ports->attaching_hop() : flight_.attaching_hop_peer_id;
  if (seat_in_flight || !in_flight_hop.empty()) {
    if (in_flight_hop == attach.hop_peer_id) {
      log().info << "OnInboundSfuAttach coalesce (attaching same hop) call_id=" << call_id
                 << " hop=" << attach.hop_peer_id;
    } else {
      DeferInboundSfuAttach(call_id, attach);
      log().info << "OnInboundSfuAttach deferred (attach in flight) call_id=" << call_id
                 << " in_flight_hop=" << in_flight_hop << " requested=" << attach.hop_peer_id;
    }
    BeginSfuAttachWait(call_id);
    return true;
  }
  // SoftMigrate PickHop may be mid-AcceptAndAttach. Bumping gen Detach's that stream and races
  // libp2p asio (Moto SIGSEGV on pp-worker). Defer until SoftMigrate clears in-flight.
  if (flight_.in_flight) {
    if (!flight_.call_id.empty() && call_id != flight_.call_id) {
      log().info << "OnInboundSfuAttach ignored (SoftMigrate in flight for other call)"
                 << " pending_call=" << flight_.call_id << " call_id=" << call_id;
      return true;
    }
    DeferInboundSfuAttach(call_id, attach);
    log().info << "OnInboundSfuAttach deferred (SoftMigrate in flight) call_id=" << call_id;
    BeginSfuAttachWait(call_id);
    host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.NotifyRingChanged();
    return true;
  }
  return RefusePrivateHopMultiaddr(call_id, attach);
}

bool CallTopologyController::RefusePrivateHopMultiaddr(const std::string& call_id, const CallSfuAttachDetail& attach) {
  // Cross-net: PreferLocal often fans a private LAN MA. Fail fast and ask owner to re-pick a
  // shared public hop instead of waiting for media-relay timeout.
  if (attach.hop_multiaddr.empty() || !MultiaddrHasPrivateIpv4Host(attach.hop_multiaddr)) {
    return false;
  }
  const bool may_dial = GuestMayDialPrivateHopMa(attach.hop_multiaddr, ranking_.LocalAdvertiseMas());
  const bool lan_hop = relay_deps_.peer_lan_confirmed && relay_deps_.peer_lan_confirmed(attach.hop_peer_id);
  if (may_dial && lan_hop) {
    return false;
  }
  log().warning << "OnInboundSfuAttach skip private hop MA hop=" << attach.hop_peer_id << " ma=" << attach.hop_multiaddr
                << " may_dial=" << (may_dial ? 1 : 0) << " lan_hop=" << (lan_hop ? 1 : 0);
  BeginSfuAttachWait(call_id);
  host_.SetMediaActivity(Tr("call.status.looking_for_another_path"));
  host_.NotifyRingChanged();
  ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id, "hop multiaddr not reachable (private)");
  return true;
}

void CallTopologyController::StartInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) {
  const auto seat_ports = seat_.Get();
  BeginSfuAttachWait(call_id);
  if (seat_ports->IsBound()) {
    seat_ports->note_connecting(call_id);
  }
  host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
  host_.NotifyRingChanged();
  const uint64_t gen = ClaimMigrateFlight(call_id);
  AttachLocalToSfuAsync(call_id, attach, [this, call_id, attach, gen](Roe<void> ok) {
    const bool superseded = !IsMigrateGenerationCurrent(gen);
    if (superseded) {
      log().info << "OnInboundSfuAttach worker gen moved want=" << gen
                 << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
                 << " attached=" << (sfu_.attached ? 1 : 0) << " ok=" << (ok ? 1 : 0);
    }
    CallsThread::Post([this, call_id, attach, gen, superseded, ok]() {
      if (superseded) {
        FinishSupersededInboundSfuAttach(call_id, gen, ok);
      } else {
        FinishInboundSfuAttach(call_id, attach, gen, ok);
      }
    });
  });
}

void CallTopologyController::FinishInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach,
                                                    uint64_t gen, const Roe<void>& ok) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (ok) {
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending_call_id.clear();
    SyncSfuSubscriptions(call_id);
    host_.ClearMediaActivity();
    host_.NotifyRingChanged();
    return;
  }
  if (sfu_.attached && media_.IsSfuMode()) {
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
}

void CallTopologyController::FinishSupersededInboundSfuAttach(const std::string& call_id, uint64_t gen,
                                                              const Roe<void>& ok) {
  const bool duplex = sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
  const bool owns_flight = flight_.flight_gen == gen;
  if (owns_flight || duplex) {
    ReleaseMigrateFlight();
  }
  if (duplex) {
    // Attach finished (StartSfu may still be settling on another worker).
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending_call_id.clear();
    ClearSfuAttachWait();
    SyncSfuSubscriptions(call_id);
    host_.ClearMediaActivity();
  } else {
    if (owns_flight) {
      FlushPendingInboundSfuAttach();
    }
    // Never ReportSfuAttachFailed under a superseded gen — that makes the owner RefuseGuest →
    // CallHopRefuse → LeaveCall mid-call (dogfood: Connected then aborted). Wait for a fresh fan-out.
    if (!ok) {
      BeginSfuAttachWait(call_id);
    }
  }
  host_.NotifyRingChanged();
}

void CallTopologyController::FlushPendingInboundSfuAttach() {
  if (!inbound_gate_.pending_attach || inbound_gate_.pending_call_id.empty()) {
    return;
  }
  const std::string call_id = inbound_gate_.pending_call_id;
  const CallSfuAttachDetail attach = *inbound_gate_.pending_attach;
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending_call_id.clear();
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    return;
  }
  log().info << "FlushPendingInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id;
  (void)OnInboundSfuAttach(call_id, attach);
}

std::vector<std::string> CallTopologyController::DialableHopPeerIds() const {
  std::vector<std::string> out;
  for (const MeshHopCandidate& hop : ranking_.Ranked()) {
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
  if (!call_id.empty() && call_id == inbound_gate_.last_fail_call_id &&
      failed_hop == inbound_gate_.last_fail_hop) {
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
  inbound_gate_.last_fail_call_id = call_id;
  inbound_gate_.last_fail_hop = failed_hop;
  log().info << "ReportSfuAttachFailed to initiator=" << initiator << " prefs="
             << detail.preferred_hop_peer_ids.size();
  host_.SetMediaActivity(Tr("call.status.looking_for_another_path"));
  host_.NotifyRingChanged();
  (void)host_.send_direct(initiator, CallControlType::CallSfuAttachFailed, *encoded,
                                 "Call hop attach failed");
}

void CallTopologyController::RefuseGuestNoSharedHop(const std::string& call_id,
                                                    const std::string& guest_identity) {
  // As the next event, never on the path that found it (call-control once arrived on Browser IO —
  // SoftMigrate dogfood: malloc corruption / abort when refusing Samsung mid PreferLocal).
  outbox_.Emit(topology_event::RefuseGuest{call_id, guest_identity});
}

void CallTopologyController::RefuseGuest(const topology_event::RefuseGuest& refuse_event) {
  const std::string& call_id = refuse_event.call_id;
  const std::string& guest_identity = refuse_event.guest_identity;
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
}

void CallTopologyController::SetOutbox(CallsOutbox<TopologyEvent> outbox) {
  outbox_ = std::move(outbox);
  if (relay_loss_observer_ != 0) {
    UnwatchRelayLoss();  // its observer holds the outbox it was given: watch again with this one
    WatchRelayLoss();
  }
  hop_migrate_.SetOutbox(
      outbox_.For<HopMigrateEvent>([](HopMigrateEvent event) { return TopologyEvent{std::move(event)}; }));
}

void CallTopologyController::Handle(TopologyEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, topology_event::RelayTransportLost>) {
          if (e.watch == relay_watch_) {
            OnGuestSfuTransportLost();
          }
        } else if constexpr (std::is_same_v<E, topology_event::AttachWaitDeadline>) {
          if (e.armed == attach_wait_.armed) {
            attach_wait_.timer_id = 0;
            OnAttachWaitTimerFire(e.call_id);
          }
        } else if constexpr (std::is_same_v<E, topology_event::ReannouncePublisher>) {
          ReannouncePublisher(e);
        } else if constexpr (std::is_same_v<E, topology_event::RefuseGuest>) {
          RefuseGuest(e);
        } else if constexpr (std::is_same_v<E, HopMigrateEvent>) {
          hop_migrate_.Handle(e);
        } else {
          static_assert(!sizeof(E), "handle every TopologyEvent");
        }
      },
      event);
}

void CallTopologyController::ReannouncePublisher(const topology_event::ReannouncePublisher& again) {
  // V050: a later joiner's re-pick may have moved the group — never re-announce the old hop.
  if (!sfu_.attached || media_.ActiveCallId() != again.call_id || flight_.attached_hop_peer_id != again.hop_peer_id ||
      publishers_.local_stream_id != again.publisher_stream_id) {
    return;
  }
  log().info << "AnnounceLocalPublisher re-fan-out call_id=" << again.call_id;
  (void)host_.fan_out_joined(again.call_id, CallControlType::CallSfuAttach, again.encoded_attach, "Call SFU attach",
                             again.local_identity);
}

void CallTopologyController::OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail) {
  auto local = host_.local_relay_identity();
  if (!local || !IsStickyInitiator(detail.call_id, *local)) {
    return; // only sticky initiator handles hop hints
  }
  const std::string& guest = detail.identity;
  // V050: one hop change per joiner — failing again on the hop the group already moved to for this
  // guest keeps that hop and refuses (other attach failures keep today's recovery).
  if (auto repicked = hop_hint_repicked_.find(detail.call_id); !guest.empty() && repicked != hop_hint_repicked_.end()) {
    if (auto moved = repicked->second.find(guest);
        moved != repicked->second.end() && moved->second == detail.failed_hop_peer_id) {
      log().warning << "Hop hint refuse guest=" << guest << " (the group already moved to " << moved->second
                    << " for this joiner)";
      RefuseGuestNoSharedHop(detail.call_id, guest);
      return;
    }
  }
  // V050: everyone moves, so the new hop must be one the other members reached too (their accept
  // reports; members without a report do not constrain).
  const std::vector<std::string> guest_prefs =
      planning_.UsableForMembers(detail.call_id, JoinedMembersOtherThan(detail.call_id, guest), detail.preferred_hop_peer_ids);
  const auto decision = DecideHopHintOwnerAction(guest_prefs, DialableHopPeerIds(), detail.failed_hop_peer_id);
  if (decision.action == HopHintOwnerAction::RefuseGuest || guest.empty()) {
    log().warning << "Hop hint refuse guest=" << guest << " failed_hop=" << detail.failed_hop_peer_id;
    if (!guest.empty()) {
      RefuseGuestNoSharedHop(detail.call_id, guest);
    }
    return;
  }
  const std::string& prefer = decision.preferred_hop_peer_id;
  if (!HopHintMayLeavePreferLocal(prefer)) {
    log().warning << "Hop hint refuse (keep PreferLocal) guest=" << guest << " failed_hop=" << detail.failed_hop_peer_id
                  << " prefer=" << prefer;
    RefuseGuestNoSharedHop(detail.call_id, guest);
    return;
  }
  // Already SoftMigrated onto the guest prefer hop: re-fan-out only (do not Detach again).
  if (sfu_.attached && !prefer.empty() && IsOnOrHintedHop(detail.call_id, prefer)) {
    log().info << "Hop hint no-op: already on prefer hop=" << prefer << " guest=" << guest;
    FanOutSfuAttachForHop(detail.call_id, prefer, *local);
    SyncSfuSubscriptions(detail.call_id);
    host_.ClearMediaActivity();
    host_.NotifyRingChanged();
    return;
  }
  if (flight_.in_flight) {
    flight_.pending_hop_prefer = prefer;
    log().info << "Hop hint coalesced prefer=" << flight_.pending_hop_prefer << " guest=" << guest;
    return;
  }
  if (!guest.empty()) {
    if (hop_hint_repicked_.size() > 4 && hop_hint_repicked_.count(detail.call_id) == 0) {
      hop_hint_repicked_.clear();  // bounded like the report maps; OnMediaStopped erases per call
    }
    hop_hint_repicked_[detail.call_id][guest] = prefer;
  }
  StartHopHintRepick(detail.call_id, prefer, guest);
}

bool CallTopologyController::IsStickyInitiator(const std::string& call_id, const std::string& local_identity) const {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return false;
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
  return SelectCallInitiator(joined_peers) == local_identity;
}

bool CallTopologyController::HopHintMayLeavePreferLocal(const std::string& prefer_hop_peer_id) const {
  // PreferLocal LAN hop often unreachable for cross-net guests. If guest/owner share another
  // dialable hop (directory/org seed), SoftMigrate off PreferLocal onto that hop.
  if (!relay_deps_.relay || !relay_deps_.relay->IsLocalHopAttached()) {
    return true;
  }
  std::string local_pid;
  if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
    local_pid = *pid;
  }
  if (prefer_hop_peer_id.empty() || prefer_hop_peer_id == local_pid) {
    return false;
  }
  log().info << "Hop hint PreferLocal → shared public hop re-pick prefer=" << prefer_hop_peer_id;
  return true;
}

bool CallTopologyController::IsOnOrHintedHop(const std::string& call_id, const std::string& hop_peer_id) const {
  if (hop_peer_id == flight_.attached_hop_peer_id) {
    return true;
  }
  auto session = sessions_.LoadSession(call_id);
  return session && session->has_value() && (*session)->sfu_hint && *(*session)->sfu_hint == hop_peer_id;
}

void CallTopologyController::StartHopHintRepick(const std::string& call_id, const std::string& prefer,
                                                const std::string& guest) {
  log().info << "Hop hint re-pick prefer=" << prefer << " guest=" << guest;
  host_.SetMediaActivity(Tr("call.status.switching_media_path"));
  host_.NotifyRingChanged();
  BeginSfuAttachWait(call_id);
  // V035: do not bump flight_.migrate_generation on hop-hint (Leave/teardown only).
  const uint64_t gen = flight_.migrate_generation.load(std::memory_order_acquire);
  flight_.flight_gen = gen;
  flight_.in_flight = true;
  flight_.call_id = call_id;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::IceRecover, prefer, gen,
                             [this, call_id, guest, gen](Roe<void> mig) {
                               CallsThread::Post([this, call_id, mig, guest, gen]() {
                                 FinishHopHintRepick(call_id, guest, gen, mig);
                               });
                             });
}

void CallTopologyController::FinishHopHintRepick(const std::string& call_id, const std::string& guest, uint64_t gen,
                                                 const Roe<void>& mig) {
  if (flight_.flight_gen != gen) {
    return;
  }
  ReleaseMigrateFlight();
  if (!mig) {
    // keep_prefer_local: SoftMigrate chose to stay on our own hop — the guest cannot reach it.
    if (mig.error().message != "keep_prefer_local") {
      log().warning << "Hop hint re-pick failed: " << mig.error().message;
    }
    RefuseGuestNoSharedHop(call_id, guest);
    return;
  }
  SyncSfuSubscriptions(call_id);
  host_.ClearMediaActivity();
  host_.NotifyRingChanged();
  FlushPendingHopPrefer(call_id);
  FlushPendingInboundSfuAttach();
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
  (void)host_.leave_call(detail.call_id);
  host_.NotifyRingChanged();
}

} // namespace pbr
