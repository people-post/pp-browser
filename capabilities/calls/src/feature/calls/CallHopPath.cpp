#include "feature/calls/CallHopPath.h"

#include "lib/calls/CallHopJoinLogic.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/calls/CallSessionQueries.h"
#include "mesh/types/MediaChannel.h"
#include "lib/calls/CallMediaPlannerSelectLogic.h"
#include "lib/calls/CallHopPlannerLogic.h"

#include "lib/calls/CallMediaAdaptation.h"
#include "lib/calls/CallHopPlan.h"
#include "lib/calls/CallHopAttachLogic.h"
#include "common/Localized.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/PeerDisplayLabel.h"
#include "common/runtime/AppRuntime.h"
#include "common/media/MediaSessionsOwner.h"
#include "common/Utilities.h"
#include "mesh/types/MeshHopDial.h"
#include "mesh/media_plane/MediaRelayAttach.h"
#include "domain/call_media/CallMediaFrameCrypto.h"

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
#include "common/MeshAlias.h"
#include "feature/calls/SfuKeyRing.h"
#include "lib/calls/CallMediaSeat.h"
#include "lib/pricing/InitiationPricing.h"
#include "lib/calls/SoftMigrateLogic.h"
#include <functional>

namespace pbr {

CallHopPath::CallHopPath(std::string call_id, const ICallSessionReader& sessions, ICallSessionNotes& notes,
                         const IContactBook& contacts, std::shared_ptr<CallEngine> engine, CallHopService& hops)
    : call_id_(std::move(call_id)),
      sessions_(sessions),
      notes_(notes),
      contacts_(contacts),
      engine_(std::move(engine)),
      hops_(hops),
      relay_deps_(hops.RelayDeps()),
      ranking_(hops.Ranking()),
      planning_(hops.Planning()) {
  redirectLogger("CallHopPath");
  channel_ = std::make_shared<CallHopMediaChannel>(call_id_, engine_, relay_deps_);
}

void CallHopPath::SetHostPorts(HostPorts ports) {
  host_ = std::move(ports);
}

CallHopPath::~CallHopPath() {
  // No RemoveClientTransportLostObserver here: the relay may already be gone (CallStack::Shutdown
  // clears the media plane first; mesh stop unwatches through UnwatchRelay before the deps change).
  // Invalidating drops notices still queued for us.
}

void CallHopPath::OnRelayDepsChanged() {
  UnwatchRelayLoss(); // no-op after UnwatchRelay (the deps' owner unwatches before it swaps them)
  WatchRelayLoss();
}

void CallHopPath::UnwatchRelay() {
  UnwatchRelayLoss();
}

void CallHopPath::WatchRelayLoss() {
  if (!relay_deps_.relay) {
    return;
  }
  relay_loss_observer_ = relay_deps_.relay->AddClientTransportLostObserver(
      [outbox = outbox_, watch = ++relay_watch_](mesh::MediaRelayClientLoss loss) {
        // Only a dead transport means reattach: our own hop migrations and leaves replace / detach
        // the session too. (Another feature taking the client is the call + broadcast case — see
        // media-client-layers PHASES "Allow call + broadcast at once".)
        if (loss != mesh::MediaRelayClientLoss::TransportLost) {
          return;
        }
        outbox.Emit(hop_path_event::RelayTransportLost{watch}); // edge: the relay's thread only reports
      });
}

void CallHopPath::UnwatchRelayLoss() {
  if (relay_deps_.relay && relay_loss_observer_ != 0) {
    relay_deps_.relay->RemoveClientTransportLostObserver(relay_loss_observer_);
  }
  relay_loss_observer_ = 0;
  ++relay_watch_; // a loss the old relay reported is stale
}

void CallHopPath::SetMediaKeyStore(ICallMediaKeyStore* keys) {
  media_keys_ = keys;
}

void CallHopPath::SetSeatPorts(CallHopPathSeatPorts ports) {
  seat_.Set(std::move(ports));
}

void CallHopPath::SetHopArmingPorts(CallHopArmingPorts ports) {
  arming_.Set(std::move(ports));
}

bool CallHopPath::IsAwaitingSfuRecovery() const {
  return flight_.in_flight || attach_wait_.waiting || guest_.reattach_in_flight;
}

bool CallHopPath::IsSfuAttached() const {
  return sfu_.attached;
}

bool CallHopPath::IsOnSfuForCall(const std::string& call_id) const {
  return call_id == call_id_ && IsLiveOnHopFor(call_id);
}

bool CallHopPath::IsSoftMigrateInFlight() const {
  return flight_.in_flight;
}

CallRecovery CallHopPath::Recovery() const {
  if (flight_.in_flight) {
    return CallRecovery::Migrating;
  }
  if (attach_wait_.waiting) {
    return CallRecovery::WaitingForHop;
  }
  if (guest_.reattach_in_flight) {
    return CallRecovery::Reattaching;
  }
  // From the first reattach attempt until one succeeds (attempts reset): between attempts and after
  // they ran out too, nothing flows.
  if (guest_.reattach_attempts > 0) {
    return CallRecovery::HopLost;
  }
  return CallRecovery::None;
}

bool CallHopPath::IsSfuAttachWaitActive() const {
  return attach_wait_.waiting;
}

void CallHopPath::BeginSfuAttachWait(const std::string& call_id) {
  if (!ForThisCall(call_id, "BeginSfuAttachWait")) {
    return;
  }
  attach_wait_.waiting = true;
  attach_wait_.deadline_ms = util::NowUnixMs() + kSfuAttachWaitDefaultMs;
  ArmAttachWaitTimer(call_id, attach_wait_.deadline_ms);
}

void CallHopPath::ClearSfuAttachWait() {
  CancelAttachWaitTimer();
  attach_wait_.waiting = false;
  attach_wait_.deadline_ms = 0;
}

void CallHopPath::CancelAttachWaitTimer() {
  ++attach_wait_.armed; // a deadline already queued is stale
  outbox_.Cancel(attach_wait_.timer_id);
}

void CallHopPath::ArmAttachWaitTimer(const std::string& call_id, int64_t deadline_ms) {
  CancelAttachWaitTimer();
  if (call_id.empty() || deadline_ms <= 0) {
    return;
  }
  const int64_t delay = std::max<int64_t>(0, deadline_ms - util::NowUnixMs());
  attach_wait_.timer_id = outbox_.After(std::chrono::milliseconds(delay),
                                        hop_path_event::AttachWaitDeadline{call_id, ++attach_wait_.armed});
}

void CallHopPath::OnAttachWaitTimerFire(const std::string& call_id) {
  if (!(attach_wait_.waiting && call_id == call_id_)) {
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

CallHopPlannerApplyContext CallHopPath::BuildHopPlannerContext(const std::string& call_id, size_t joined_count,
                                                               bool has_sfu_hint) const {
  const auto arming_ports = arming_.Get();
  CallHopPlannerApplyContext ctx;
  ctx.allows_hop_path = !arming_ports->IsBound() || arming_ports->hop_ops_allowed();
  ctx.direct_failed = arming_ports->IsBound() && arming_ports->direct_failed && arming_ports->direct_failed();
  ctx.should_arm_hop = ShouldArmHopPlanner(joined_count);
  ctx.has_sfu_hint = has_sfu_hint;
  ctx.soft_migrate_in_flight = flight_.in_flight;
  ctx.sfu_attached = sfu_.attached && engine_->Runs();
  return ctx;
}

void CallHopPath::SetHopPlannerPhase(const CallHopPlannerPhase next, const CallHopPlannerEvent ev,
                                     const std::string& call_id) {
  const CallHopPlannerPhase prev = sfu_.hop_planner_phase;
  sfu_.hop_planner_phase = next;
  log().info << "planner=Hop phase=" << CallHopPlannerPhaseName(prev) << "->" << CallHopPlannerPhaseName(next)
             << " event=" << CallHopPlannerEventName(ev) << " call_id=" << call_id
             << " migrate_gen=" << flight_.migrate_generation.load(std::memory_order_acquire);
}

void CallHopPath::ReportHopProgress(const CallHopPlannerPhase phase, const std::string& call_id) {
  reports_.Emit(hop_report::Progress{call_id, phase});
}

void CallHopPath::SetReports(OwnerOutbox<HopReport> reports) {
  reports_ = std::move(reports);
}

void CallHopPath::Apply(CallHopPlannerEvent ev, const std::string& call_id) {
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
  const CallHopPlannerApplyContext ctx = BuildHopPlannerContext(call_id, n_joined, has_hint);
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
  // The call's media status follows every transition the planner decides, here and only here
  // (a status without a planner transition — a join decision, a guest's reattach — is reported
  // where it is decided).
  if (out.decision == CallHopPlannerDecision::Transition) {
    ReportHopProgress(out.next, call_id);
  }
  if (ev == CallHopPlannerEvent::Stop) {
    CancelAttachWaitTimer();
  }
}

void CallHopPath::OnMediaStopped(const std::string& call_id) {
  if (!ForThisCall(call_id, "OnMediaStopped")) {
    return;
  }
  const auto seat_ports = seat_.Get();
  // Invalidate in-flight SoftMigrate / AttachLocalToSfu so they cannot StartDuplex after Leave
  // (Linux quit dogfood: double-free from SDL reopen during teardown).
  Apply(CallHopPlannerEvent::Stop, call_id);
  flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  if (seat_ports->IsBound()) {
    seat_ports->cancel_attach_for_call(call_id);
  }
  if (sfu_.attached && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_.SetAttached(false);
  ReleaseMigrateFlight();
  flight_.attaching_hop_peer_id.clear();
  flight_.attached_hop_peer_id.clear();
  flight_.pending_hop_prefer.clear();
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending = false;
  inbound_gate_.last_failed = false;
  inbound_gate_.last_fail_hop.clear();
  channel_->ResetStreams();
  guest_.active_attach.reset();
  guest_.active = false;
  guest_.reattach_attempts = 0;
  guest_.reattach_in_flight = false;
  reports_.Emit(hop_report::Activity{call_id_, {}});
  hop_hint_repicked_.clear();
  planning_.Forget(call_id);
  if (attach_wait_.waiting && call_id == call_id_) {
    ClearSfuAttachWait();
  }
}

void CallHopPath::PollPendingSfuAttach() {
  const std::string call_id = attach_wait_.waiting ? call_id_ : std::string{};
  SfuAttachWaitPollInput in;
  in.wait_active = !call_id.empty();
  in.now_ms = util::NowUnixMs();
  in.deadline_ms = attach_wait_.deadline_ms;
  in.soft_migrate_in_flight = flight_.in_flight;
  in.sfu_attached_for_call = IsOnSfuForCall(call_id);
  if (auto joined = sessions_.CountJoined(call_id)) {
    in.joined_count = *joined;
  }
  in.media_active_mesh_for_call = engine_->RunsOn(CallMediaPath::Direct); // the 1:1 still carries this call

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
      reports_.Emit(hop_report::MediaError{call_id_, Localized("call.error.no_media_relay_hop")});
      log().warning << "SFU attach wait timed out call_id=" << call_id;
      reports_.Emit(hop_report::LeaveWanted{call_id});
      return;
  }
}

void CallHopPath::EjectParticipantAfterMigrateFailure(const std::string& call_id, const std::string& identity,
                                                      const std::string& reason) {
  if (identity.empty()) {
    return;
  }
  reports_.Emit(hop_report::MediaError{call_id_, reason});
  log().warning << "Ejecting " << identity << " after soft-migrate failure: " << reason;

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = identity;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (detail) {
    (void)host_.send_direct(identity, CallControlType::CallLeave, *detail, "Left the call");
  }

  notes_.NoteParticipantLeft(call_id, identity);

  if (detail) {
    auto local = host_.local_relay_identity();
    if (local) {
      (void)host_.fan_out_joined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);
    }
  }
  reports_.Emit(hop_report::Changed{call_id_});
}

uint32_t CallHopPath::PublisherStreamIdForLocal() const {
  auto local = host_.local_relay_identity();
  if (!local) {
    return 1;
  }
  return PublisherStreamIdForIdentity(*local);
}

void CallHopPath::RefreshAdaptation(const std::string& call_id) {
  // The user's switch: adaptation never turns the camera off (it pauses the video send), so this reads it as set.
  RefreshAdaptation(call_id, engine_->IsCameraEnabled());
}

void CallHopPath::RefreshAdaptation(const std::string& call_id, bool camera_user_wants) {
  if (!ForThisCall(call_id, "RefreshAdaptation")) {
    return;
  }
  bool video_allowed = false;
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    video_allowed = (*session)->video_allowed;
  }
  CallAdaptationInput in;
  in.camera_user_wants = camera_user_wants && video_allowed;
  in.muted = engine_->IsMuted();
  in.per_user_up_bps = sfu_.last_quote_a_up_bps;
  in.allow_video_hi = false;
  double pressure = engine_->PathPressure();
  if (relay_deps_.relay) {
    pressure = std::max(pressure, relay_deps_.relay->PathPressure());
  }
  in.path_pressure = pressure;
  engine_->NoteUplinkBudget(sfu_.last_quote_a_up_bps);
  engine_->ApplyTargets(CallMediaAdaptation::ToMediaTargets(CallMediaAdaptation::Evaluate(in)));
}

CallHopHealth CallHopPath::HopHealth() const {
  if (!relay_deps_.relay || !sfu_.attached) {
    return {};
  }
  // The relay's session health, in the call's terms (Call details, media health).
  const mesh::MediaRelayClientHealth relay = relay_deps_.relay->HealthSnapshot();
  CallHopHealth hop;
  hop.attached = relay.attached;
  hop.link = CallLinkCounters{.available = relay.link_available,
                              .link_id = relay.link_id,
                              .reliable_sent = relay.reliable_sent,
                              .retransmits = relay.retransmits,
                              .srtt_ms = relay.srtt_ms};
  return hop;
}

bool CallHopPath::SettleFinishedMigrate(const std::string& call_id) {
  ReleaseMigrateFlight();
  if (!(sfu_.attached && engine_->Runs())) {
    return false;
  }
  ClearSfuAttachWait();
  SyncSfuSubscriptions(call_id);
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending = false;
  reports_.Emit(hop_report::Changed{call_id_});
  return true;
}

std::vector<std::string> CallHopPath::JoinedRemoteIdentities(const std::string& call_id,
                                                             const std::string& local_identity) const {
  return call_queries::JoinedRemoteIdentities(sessions_, call_id, local_identity);
}

std::vector<std::string> CallHopPath::JoinedMembersOtherThan(const std::string& call_id,
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

bool CallHopPath::ResolveGroupHopForJoin(const std::string& call_id, const std::string& joiner_identity) {
  auto local = host_.local_relay_identity();
  const auto outcome = planning_.ResolveAtJoin(call_id, joiner_identity,
                                               JoinedRemoteIdentities(call_id, local ? *local : std::string()));
  if (outcome.adjusted_plan) {
    notes_.NotePlannedHop(call_id, *outcome.adjusted_plan);
  }
  if (outcome.action == CallHopPlanning::JoinOutcome::Action::RefuseJoiner) {
    RefuseGuestNoSharedHop(call_id, joiner_identity);
    return false;
  }
  return true;
}

bool CallHopPath::AwaitsOwnInviteeAccept(const std::string& call_id, const std::string& local_identity) const {
  return call_queries::AwaitsOwnInviteeAccept(sessions_, call_id, local_identity);
}

CallHopScope CallHopPath::InferScopeForCall(const std::string& call_id, const std::string& local_identity) const {
  return ranking_.ScopeForPeers(JoinedRemoteIdentities(call_id, local_identity));
}

bool CallHopPath::LanReachabilityConfirmedForCall(const std::string& call_id, const std::string& local_identity) const {
  return ranking_.LanConfirmedForPeers(JoinedRemoteIdentities(call_id, local_identity));
}

bool CallHopPath::IsActiveCallForTopology(const std::string& call_id) const {
  const auto seat_ports = seat_.Get();
  auto session_active = [this](const std::string& id) -> bool {
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
  HopActiveCallInput in;
  in.call_id = call_id;
  in.flight_call_id = flight_.for_call ? call_id_ : std::string{};
  in.attach_wait_call_id = attach_wait_.waiting ? call_id_ : std::string{};
  in.seat_bound = seat_ports->IsBound();
  if (in.seat_bound) {
    in.seat_bound_to_call = seat_ports->is_bound(call_id);
    in.seat_bound_call_id = seat_ports->bound_call_id();
  } else {
    in.guest_call_id = guest_.active ? call_id_ : std::string{};
    in.guest_session_active = session_active(guest_.active ? call_id_ : std::string{});
  }
  in.call_session_active = session_active(call_id);
  return IsHopActiveCall(in);
}

void CallHopPath::FanOutSfuAttachForHop(const std::string& call_id, const std::string& hop_peer_id,
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
    (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", local_identity);
  }
}

void CallHopPath::FlushPendingHopPrefer(const std::string& call_id) {
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
  flight_.for_call = true;
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::IceRecover, prefer, gen,
                             [this, call_id, gen](Roe<void> /*mig*/) {
                               Defer([this, call_id, gen]() {
                                 if (flight_.flight_gen != gen && !IsMigrateGenerationCurrent(gen)) {
                                   return;
                                 }
                                 ReleaseMigrateFlight();
                                 FlushPendingHopPrefer(call_id);
                                 FlushPendingInboundSfuAttach();
                                 reports_.Emit(hop_report::Changed{call_id_});
                               });
                             });
}

void CallHopPath::SubscribePublisherStream(uint32_t stream_id) {
  if (!relay_deps_.relay || stream_id == 0) {
    return;
  }
  const uint32_t local_stream = channel_->LocalStreamId();
  if (local_stream != 0 && stream_id == local_stream) {
    return;
  }
  // Calls send one video level (the default): audio + that video channel.
  (void)relay_deps_.relay->Subscribe(stream_id, mesh::kMediaChannelAudio);
  (void)relay_deps_.relay->Subscribe(stream_id, mesh::VideoChannel(mesh::kDefaultVideoLevel));
  MaybeRequestPublisherKeyframe(stream_id);
}

void CallHopPath::MaybeRequestPublisherKeyframe(uint32_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  if (!channel_->FirstVideoRefresh(stream_id)) {
    return;
  }
  if (!engine_->Runs()) {
    return;
  }
  const std::string& call_id = call_id_;
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

void CallHopPath::NoteRemotePublisherFromAttach(const CallSfuAttachDetail& attach) {
  if (attach.publisher_stream_id == 0) {
    return;
  }
  const uint32_t local_stream = channel_->LocalStreamId();
  if (local_stream != 0 && attach.publisher_stream_id == local_stream) {
    return;
  }
  channel_->AddRemoteStream(attach.publisher_stream_id);
  if (sfu_.attached && engine_->Runs()) {
    log().info << "SFU subscribe announced stream=" << attach.publisher_stream_id << " call_id=" << attach.call_id;
    SubscribePublisherStream(attach.publisher_stream_id);
  }
}

void CallHopPath::AnnounceLocalPublisher(const std::string& call_id, const CallSfuAttachDetail& hop_attach) {
  if (!sfu_.attached || channel_->LocalStreamId() == 0) {
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return;
  }
  CallSfuAttachDetail announce = hop_attach;
  announce.call_id = call_id;
  announce.publisher_stream_id = channel_->LocalStreamId();
  announce.quote_id.clear();
  const CallSfuAttachDetail fanout = BuildSfuAttachFanout(announce);
  auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
  if (!encoded) {
    return;
  }
  log().info << "AnnounceLocalPublisher stream=" << channel_->LocalStreamId() << " call_id=" << call_id;
  (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", *local);
  outbox_.After(std::chrono::milliseconds(2000),
                hop_path_event::ReannouncePublisher{call_id, *encoded, *local, announce.hop_peer_id,
                                                    announce.publisher_stream_id});
}

void CallHopPath::SyncSfuSubscriptions(const std::string& call_id) {
  if (!relay_deps_.relay || !sfu_.attached || !engine_->Runs()) {
    return;
  }
  // Streams announced via CallSfuAttach (covers incomplete Joined roster), then each Joined peer's;
  // subscribe after (relay I/O takes the relay's own lock).
  std::vector<uint32_t> to_subscribe = channel_->RemoteStreams();
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
      channel_->AddRemoteStream(stream);
      to_subscribe.push_back(stream);
      ++subscribed;
      log().info << "SFU subscribe peer=" << p.identity << " stream=" << stream << " call_id=" << call_id;
    }
  }
  announced = channel_->RemoteStreamCount();
  for (const uint32_t stream : to_subscribe) {
    SubscribePublisherStream(stream);
  }
  if (!participants) {
    return;
  }
  log().info << "SyncSfuSubscriptions call_id=" << call_id << " peers=" << subscribed << " announced=" << announced;
}

uint64_t CallHopPath::ClaimMigrateFlight(const std::string& call_id) {
  const uint64_t gen = flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  flight_.flight_gen = gen;
  flight_.in_flight = true;
  flight_.for_call = true;
  return gen;
}

void CallHopPath::ReleaseMigrateFlight() {
  flight_.in_flight = false;
  flight_.for_call = false;
}

bool CallHopPath::MigrateFlightBusyFor(const std::string& call_id) const {
  return flight_.in_flight && (!flight_.for_call || call_id == call_id_);
}

bool CallHopPath::ExpectsGroupMedia(const std::string& call_id) const {
  if (call_id.empty() || call_id != call_id_) {
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

bool CallHopPath::OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                      const std::optional<std::string>& sfu_hint) {
  if (!ForThisCall(call_id, "OnLocalAcceptJoined")) {
    return false;
  }
  Apply(CallHopPlannerEvent::LocalAcceptN3, call_id);
  bool group_call = false;
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    auto offered = sessions_.ListParticipants(call_id);
    group_call = CallSessionLogic::IsGroupCall(**session, offered ? offered->size() : 0);
  }
  switch (DecideLocalAcceptHop(n_joined, sfu_hint && !sfu_hint->empty(), group_call)) {
    case LocalAcceptHopAction::AttachToInviteHint:
      AttachFromInviteHint(call_id, *sfu_hint);
      return true;
    case LocalAcceptHopAction::JoinGroup:
      JoinGroupWithoutHint(call_id, n_joined);
      return true;
    case LocalAcceptHopAction::StayDirect:
      break;
  }
  StayDirectAfterAccept(call_id, n_joined);
  return false;
}

void CallHopPath::AttachFromInviteHint(const std::string& call_id, const std::string& hop_peer_id) {
  ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = hop_peer_id;
  attach.hop_multiaddr = ranking_.HopMultiaddr(hop_peer_id);
  attach.publisher_stream_id = PublisherStreamIdForLocal();
  reports_.Emit(hop_report::MediaAttempted{call_id});
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.connecting_media_relay")});
  reports_.Emit(hop_report::Changed{call_id_});
  if (MigrateFlightBusyFor(call_id)) {
    // Inbound CallSfuAttach already dialing — do not bump flight_.migrate_generation.
    log().info << "OnLocalAcceptJoined keep in-flight attach (invite hint) call_id=" << call_id;
    DeferInboundSfuAttach(call_id, attach);
    return;
  }
  const uint64_t gen = ClaimMigrateFlight(call_id);
  AttachLocalToSfuAsync(call_id, attach, [this, call_id, gen](Roe<void> ok) {
    Defer([this, call_id, ok, gen]() { FinishInviteHintAttach(call_id, gen, ok); });
  });
}

void CallHopPath::FinishInviteHintAttach(const std::string& call_id, uint64_t gen, const Roe<void>& ok) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (!ok) {
    if (sfu_.attached && engine_->Runs()) {
      SyncSfuSubscriptions(call_id);
      reports_.Emit(hop_report::Changed{call_id_});
      return;
    }
    log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
    reports_.Emit(hop_report::MediaError{call_id_, ok.error().message});
    // V050: a joiner the group's hop cannot serve asks the owner (one hop change per joiner, or a
    // refusal) — ReportSfuAttachFailedToInitiator leaves when we are the owner ourselves.
    std::string hop;
    if (auto session = sessions_.LoadSession(call_id); session && session->has_value() && (*session)->sfu_hint) {
      hop = *(*session)->sfu_hint;
    }
    ReportSfuAttachFailedToInitiator(call_id, hop, ok.error().message);
  } else {
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending = false;
  }
  FlushPendingInboundSfuAttach();
  reports_.Emit(hop_report::Changed{call_id_});
}

void CallHopPath::JoinGroupWithoutHint(const std::string& call_id, size_t n_joined) {
  reports_.Emit(hop_report::MediaAttempted{call_id});
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.setting_up_group")});
  reports_.Emit(hop_report::Changed{call_id_});
  JoinWithoutHintInput in;
  in.on_hop_for_call = IsOnSfuForCall(call_id);
  in.flight_busy_for_call = MigrateFlightBusyFor(call_id);
  // PreferLocal Node may PickHop on LocalJoinedWithoutHint; phones/guests WaitForAttach.
  in.may_prefer_local_hop = relay_deps_.prefer_local_as_hop && relay_deps_.relay && relay_deps_.relay->IsStarted();
  switch (DecideJoinWithoutHint(in)) {
    case JoinWithoutHintAction::AlreadyOnHop:
      ClearSfuAttachWait();
      SyncSfuSubscriptions(call_id);
      Apply(CallHopPlannerEvent::AttachSucceeded, call_id);
      return;
    case JoinWithoutHintAction::KeepFlight:
      // Dogfood: CallSfuAttach often starts AcceptAndAttach before AcceptInvite finishes; bumping
      // the migrate generation would abort that worker before StartDuplex (guest stuck Connecting).
      log().info << "OnLocalAcceptJoined keep in-flight SoftMigrate/attach call_id=" << call_id;
      ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
      return;
    case JoinWithoutHintAction::WaitForAttach:
      // Must not bump the generation or hold the flight (it defers the inbound attach).
      log().info << "OnLocalAcceptJoined WaitForAttach call_id=" << call_id << " n=" << n_joined;
      ReportHopProgress(CallHopPlannerPhase::WaitingAttach, call_id);
      FlushPendingInboundSfuAttach();
      return;
    case JoinWithoutHintAction::PickHop:
      break;
  }
  ReportHopProgress(CallHopPlannerPhase::Attaching, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(
      call_id, SoftMigrateTrigger::LocalJoinedWithoutHint, {}, gen, [this, call_id, gen](Roe<void> mig) {
        const bool attached = sfu_.attached;
        Defer([this, call_id, mig, attached, gen]() { FinishJoinSoftMigrate(call_id, gen, mig, attached); });
      });
}

void CallHopPath::FinishJoinSoftMigrate(const std::string& call_id, uint64_t gen, const Roe<void>& mig,
                                        bool attached_when_done) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  if (SettleFinishedMigrate(call_id)) {
    return;
  }
  if (!mig) {
    log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
    reports_.Emit(hop_report::MediaError{call_id_, mig.error().message});
    ClearSfuAttachWait();
    reports_.Emit(hop_report::LeaveWanted{call_id});
  } else if (!attached_when_done && !sfu_.attached) {
    // Wait for CallSfuAttach (LocalJoinedWithoutHint → WaitForAttach).
    FlushPendingInboundSfuAttach();
  } else {
    ClearSfuAttachWait();
  }
  reports_.Emit(hop_report::Changed{call_id_});
}

void CallHopPath::StayDirectAfterAccept(const std::string& call_id, size_t n_joined) {
  ClearSfuAttachWait();
  // Cancel any leftover SoftMigrate / inbound AcceptAndAttach so it cannot StartDuplex on this 1:1
  // after ScheduleStartDirectMedia (dogfood: stale gen StartDuplex → brief hop audio → "direct").
  flight_.migrate_generation.fetch_add(1, std::memory_order_acq_rel);
  ReleaseMigrateFlight();
  flight_.attaching_hop_peer_id.clear();
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending = false;
  reports_.Emit(hop_report::Activity{call_id_, {}});
  log().info << "OnLocalAcceptJoined → P2P ScheduleStart call_id=" << call_id << " n=" << n_joined;
}

bool CallHopPath::OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                       const std::string& joiner_identity) {
  if (!ForThisCall(call_id, "OnRemoteAcceptJoined")) {
    return true; // not this path's call: no 1:1 start either (as for a call no longer active)
  }
  RemoteAcceptHopInput in;
  in.active_call = IsActiveCallForTopology(call_id);
  in.n_joined = n_joined;
  in.on_hop_for_call = IsOnSfuForCall(call_id);
  in.migrate_in_flight = flight_.in_flight;
  const RemoteAcceptHopAction action = DecideRemoteAcceptHop(in);
  switch (action) {
    case RemoteAcceptHopAction::IgnoreNotActive:
      log().info << "OnRemoteAcceptJoined ignored (not active call) call_id=" << call_id
                 << " joiner=" << joiner_identity << " media_active=" << engine_->RunningSessionForLog();
      return true; // the caller must not start direct media for a stale / zombie call
    case RemoteAcceptHopAction::StayDirect:
      log().info << "OnRemoteAcceptJoined stay P2P call_id=" << call_id << " n=" << n_joined
                 << " joiner=" << joiner_identity;
      ClearSfuAttachWait();
      reports_.Emit(hop_report::Activity{call_id_, {}});
      return false;
    case RemoteAcceptHopAction::RefreshOnHop:
      // 2nd concurrent accept: re-fan-out the attach so a joiner that missed the SoftMigrate fan-out
      // while still Ringing can WaitForAttach → attach.
      SyncSfuSubscriptions(call_id);
      RefanOutHopForJoiner(call_id, joiner_identity);
      ClearSfuAttachWait();
      reports_.Emit(hop_report::Changed{call_id_});
      return true;
    case RemoteAcceptHopAction::JoinFlight:
    case RemoteAcceptHopAction::ResolveAndMigrate:
      break;
  }
  log().info << "OnRemoteAcceptJoined call_id=" << call_id << " n=" << n_joined << " joiner=" << joiner_identity
             << " inflight=" << (flight_.in_flight ? 1 : 0);
  // V050: the first migrate goes to the planned hop — or the one adjustment, or the joiner is
  // refused and the call stays as it is.
  if (action == RemoteAcceptHopAction::ResolveAndMigrate && !ResolveGroupHopForJoin(call_id, joiner_identity)) {
    return true; // joiner refused; nothing to schedule for it
  }
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.setting_up_group")});
  reports_.Emit(hop_report::Changed{call_id_});
  if (action == RemoteAcceptHopAction::JoinFlight) {
    // Overlapping Accept: keep the in-flight SoftMigrate; bumping gen would Detach mid-attach.
    log().info << "OnRemoteAcceptJoined defer SoftMigrate (already in flight) joiner=" << joiner_identity;
    return true;
  }
  reports_.Emit(hop_report::MediaAttempted{call_id});
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::RemoteAcceptObserved, {}, gen,
                             [this, call_id, joiner_identity, gen](Roe<void> mig) {
                               Defer([this, call_id, joiner_identity, mig, gen]() {
                                 FinishRemoteAcceptMigrate(call_id, joiner_identity, gen, mig);
                               });
                             });
  return true;
}

void CallHopPath::RefanOutHopForJoiner(const std::string& call_id, const std::string& joiner_identity) {
  if (!relay_deps_.relay) {
    return;
  }
  if (!relay_deps_.relay->IsLocalHopAttached()) {
    // On a remote hop the call's owner tells the joiner where the group is. Without it someone who
    // answered after the SoftMigrate fan-out was told nothing, waited out the attach wait and left with
    // no_media_relay_hop while the others saw one fewer (lab: group_fourth_answers_late). One sender.
    auto local = host_.local_relay_identity();
    if (!local || !sfu_.attached || !guest_.active_attach || !(guest_.active && call_id == call_id_) ||
        !IsStickyInitiator(call_id, *local)) {
      return;
    }
    log().info << "OnRemoteAcceptJoined re-fan-out CallSfuAttach (remote hop) joiner=" << joiner_identity;
    AnnounceLocalPublisher(call_id, *guest_.active_attach);
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

void CallHopPath::FinishRemoteAcceptMigrate(const std::string& call_id, const std::string& joiner_identity,
                                            uint64_t gen, const Roe<void>& mig) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  if (SettleFinishedMigrate(call_id)) {
    return;
  }
  if (!mig) {
    log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
    EjectParticipantAfterMigrateFailure(
        call_id, joiner_identity,
        mig.error().message.empty() ? Localized("call.error.no_media_relay_hop") : mig.error().message);
  }
  // Non-initiator WaitForAttach: keep wait; apply deferred CallSfuAttach from owner.
  FlushPendingInboundSfuAttach();
  reports_.Emit(hop_report::Changed{call_id_});
}

void CallHopPath::OnPeerMediaRelayCapLearned(const std::string& call_id, const std::string& peer_id) {
  if (call_id.empty() || !ForThisCall(call_id, "OnPeerMediaRelayCapLearned")) {
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
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::JoinedCountObserved, {}, 0, [this](Roe<void> mig) {
    if (!mig) {
      log().warning << "SoftMigrate (relay-cap nudge) failed: " << mig.error().message;
    }
    Defer([this]() { reports_.Emit(hop_report::Changed{call_id_}); });
  });
}

void CallHopPath::OnJoinedCountObserved(const std::string& call_id, size_t n_joined) {
  if (!ForThisCall(call_id, "OnJoinedCountObserved")) {
    return;
  }
  JoinedCountHopInput in;
  in.n_joined = n_joined;
  in.on_hop_for_call = IsOnSfuForCall(call_id);
  in.migrate_in_flight = flight_.in_flight;
  in.attach_wait_for_call = attach_wait_.waiting && call_id == call_id_;
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    in.has_sfu_hint = (*session)->sfu_hint && !(*session)->sfu_hint->empty();
    in.planned_hop = (*session)->planned_hop.has_value();
  }
  if (in.planned_hop) {
    auto local = host_.local_relay_identity();
    in.initiator_awaits_own_invitee =
        local && IsStickyInitiator(call_id, *local) && AwaitsOwnInviteeAccept(call_id, *local);
  }
  switch (DecideJoinedCountHop(in)) {
    case JoinedCountHopAction::None:
      return;
    case JoinedCountHopAction::SyncSubscriptions:
      SyncSfuSubscriptions(call_id); // the roster grew while on the hop: subscribe to late joiners
      return;
    case JoinedCountHopAction::SkipInFlight:
      log().info << "OnJoinedCountObserved skip SoftMigrate (in flight) n=" << n_joined << " call_id=" << call_id;
      return;
    case JoinedCountHopAction::SkipAttachWait:
      // Re-entry would only bump the generation and thrash the inbox.
      log().info << "OnJoinedCountObserved skip SoftMigrate (attach wait) n=" << n_joined << " call_id=" << call_id;
      reports_.Emit(hop_report::InboxSyncWanted{call_id_});
      return;
    case JoinedCountHopAction::AwaitJoinerAccept:
      log().info << "OnJoinedCountObserved: wait for the joiner's CallAccept (planned hop) n=" << n_joined
                 << " call_id=" << call_id;
      return;
    case JoinedCountHopAction::Migrate:
      break;
  }

  reports_.Emit(hop_report::MediaAttempted{call_id});
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.setting_up_group")});
  reports_.Emit(hop_report::Changed{call_id_});
  Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
  const uint64_t gen = ClaimMigrateFlight(call_id);
  MaybeSoftMigrateToSfuAsync(call_id, SoftMigrateTrigger::JoinedCountObserved, {}, gen,
                             [this, call_id, gen](Roe<void> mig) {
                               Defer([this, call_id, mig, gen]() {
                                 if (!IsMigrateGenerationCurrent(gen)) {
                                   return;
                                 }
                                 if (SettleFinishedMigrate(call_id)) {
                                   return;
                                 }
                                 if (!mig) {
                                   log().warning << "MaybeSoftMigrateToSfu (roster) failed: " << mig.error().message;
                                   reports_.Emit(hop_report::MediaError{call_id_, mig.error().message});
                                   // Do not LeaveCall here — attach-wait / inviter eject paths handle failure.
                                 } else {
                                   FlushPendingInboundSfuAttach();
                                   if (!sfu_.attached) {
                                     reports_.Emit(hop_report::InboxSyncWanted{call_id_});
                                   }
                                 }
                                 reports_.Emit(hop_report::Changed{call_id_});
                               });
                             });
}

Roe<void> CallHopPath::OnInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach,
                                          const std::string& sender) {
  if (!ForThisCall(call_id, "OnInboundSfuAttach")) {
    return {};
  }
  log().info << "OnInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " sfu=" << (sfu_.attached ? 1 : 0) << " inflight=" << (flight_.in_flight ? 1 : 0);
  if (!ExpectsInboundSfuAttach(call_id, attach)) {
    return {};
  }
  Apply(CallHopPlannerEvent::SfuAttachInbound, call_id);
  if (!LeaveHopForOwnerMove(call_id, attach, sender) && SettleInboundSfuAttachWithoutDial(call_id, attach)) {
    return {};
  }
  StartInboundSfuAttach(call_id, attach);
  return {};
}

bool CallHopPath::LeaveHopForOwnerMove(const std::string& call_id, const CallSfuAttachDetail& attach,
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
  sfu_.SetAttached(false);
  flight_.attached_hop_peer_id.clear();
  flight_.attaching_hop_peer_id.clear();
  notes_.NoteCallHop(call_id, {});
  return true;
}

bool CallHopPath::ExpectsInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) const {
  const auto arming_ports = arming_.Get();
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "OnInboundSfuAttach ignored (not active call) call_id=" << call_id
               << " media_active=" << engine_->RunningSessionForLog();
    return false;
  }
  // 1:1 must stay on call-media duplex. Owner SoftMigrate / inflated roster can still fan
  // CallSfuAttach; accepting it races ScheduleStartDirectMedia (intermittent hop sound → "direct").
  // Allow only when Status arms Topology (V037) or we are waiting / SoftMigrating this call.
  InboundAttachInput in;
  in.active_call = true;
  if (auto joined = sessions_.CountJoined(call_id)) {
    in.n_joined = *joined;
  }
  in.status_allows_hop = !arming_ports->IsBound() || arming_ports->hop_ops_allowed();
  in.direct_failed = arming_ports->IsBound() && arming_ports->direct_failed && arming_ports->direct_failed();
  in.attach_wait_for_call = attach_wait_.waiting && call_id == call_id_;
  in.flight_for_call = flight_.in_flight && flight_.for_call && call_id == call_id_;
  in.on_hop = sfu_.attached;
  const bool expect_group_attach = ExpectsGroupAttach(in);
  if (!expect_group_attach) {
    log().info << "OnInboundSfuAttach ignored (1:1 / hop not armed) call_id=" << call_id << " n_joined=" << in.n_joined
               << " hop=" << attach.hop_peer_id << " arming="
               << (arming_ports->IsBound() && arming_ports->arming_debug_name ? arming_ports->arming_debug_name()
                                                                              : "null");
  }
  return expect_group_attach;
}

void CallHopPath::DeferInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) {
  inbound_gate_.pending_attach = attach;
  inbound_gate_.pending = true;
}

bool CallHopPath::SettleInboundSfuAttachWithoutDial(const std::string& call_id, const CallSfuAttachDetail& attach) {
  const auto seat_ports = seat_.Get();
  if (sfu_.attached && engine_->Runs()) {
    // Already attached (duplicate fan-out / late roster / peer publisher announce).
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    ClearSfuAttachWait();
    reports_.Emit(hop_report::Activity{call_id_, {}});
    reports_.Emit(hop_report::Changed{call_id_});
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
    if (flight_.for_call && call_id != call_id_) {
      log().info << "OnInboundSfuAttach ignored (SoftMigrate in flight for other call)"
                 << " pending_call=" << call_id_ << " call_id=" << call_id;
      return true;
    }
    DeferInboundSfuAttach(call_id, attach);
    log().info << "OnInboundSfuAttach deferred (SoftMigrate in flight) call_id=" << call_id;
    BeginSfuAttachWait(call_id);
    reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.connecting_media_relay")});
    reports_.Emit(hop_report::Changed{call_id_});
    return true;
  }
  return RefusePrivateHopMultiaddr(call_id, attach);
}

bool CallHopPath::RefusePrivateHopMultiaddr(const std::string& call_id, const CallSfuAttachDetail& attach) {
  // Cross-net: PreferLocal often fans a private LAN MA. Fail fast and ask owner to re-pick a
  // shared public hop instead of waiting for media-relay timeout.
  if (attach.hop_multiaddr.empty() || !mesh::MultiaddrHasPrivateIpv4Host(attach.hop_multiaddr)) {
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
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.looking_for_another_path")});
  reports_.Emit(hop_report::Changed{call_id_});
  ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id, "hop multiaddr not reachable (private)");
  return true;
}

void CallHopPath::StartInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) {
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.connecting_media_relay")});
  reports_.Emit(hop_report::Changed{call_id_});
  const uint64_t gen = ClaimMigrateFlight(call_id);
  AttachLocalToSfuAsync(call_id, attach, [this, call_id, attach, gen](Roe<void> ok) {
    const bool superseded = !IsMigrateGenerationCurrent(gen);
    if (superseded) {
      log().info << "OnInboundSfuAttach worker gen moved want=" << gen
                 << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
                 << " attached=" << (sfu_.attached ? 1 : 0) << " ok=" << (ok ? 1 : 0);
    }
    Defer([this, call_id, attach, gen, superseded, ok]() {
      if (superseded) {
        FinishSupersededInboundSfuAttach(call_id, gen, ok);
      } else {
        FinishInboundSfuAttach(call_id, attach, gen, ok);
      }
    });
  });
}

void CallHopPath::FinishInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach, uint64_t gen,
                                         const Roe<void>& ok) {
  if (!IsMigrateGenerationCurrent(gen)) {
    return;
  }
  ReleaseMigrateFlight();
  if (ok) {
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending = false;
    SyncSfuSubscriptions(call_id);
    reports_.Emit(hop_report::Activity{call_id_, {}});
    reports_.Emit(hop_report::Changed{call_id_});
    return;
  }
  if (sfu_.attached && engine_->Runs()) {
    SyncSfuSubscriptions(call_id);
    reports_.Emit(hop_report::Changed{call_id_});
    return;
  }
  reports_.Emit(hop_report::MediaError{call_id_, ok.error().message});
  log().warning << "AttachLocalToSfu (inbound) failed: " << ok.error().message;
  // V029: ask owner to re-pick or refuse — keep attach-wait for a re-fan-out.
  ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id, ok.error().message);
  BeginSfuAttachWait(call_id);
  reports_.Emit(hop_report::Changed{call_id_});
}

void CallHopPath::FinishSupersededInboundSfuAttach(const std::string& call_id, uint64_t gen, const Roe<void>& ok) {
  const bool duplex = IsOnSfuForCall(call_id);
  const bool owns_flight = flight_.flight_gen == gen;
  if (owns_flight || duplex) {
    ReleaseMigrateFlight();
  }
  if (duplex) {
    // Attach finished (StartDuplex may still be settling on another worker).
    inbound_gate_.pending_attach.reset();
    inbound_gate_.pending = false;
    ClearSfuAttachWait();
    SyncSfuSubscriptions(call_id);
    reports_.Emit(hop_report::Activity{call_id_, {}});
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
  reports_.Emit(hop_report::Changed{call_id_});
}

void CallHopPath::FlushPendingInboundSfuAttach() {
  if (!inbound_gate_.pending_attach || !inbound_gate_.pending) {
    return;
  }
  const std::string call_id = inbound_gate_.pending ? call_id_ : std::string{};
  const CallSfuAttachDetail attach = *inbound_gate_.pending_attach;
  inbound_gate_.pending_attach.reset();
  inbound_gate_.pending = false;
  if (IsOnSfuForCall(call_id)) {
    NoteRemotePublisherFromAttach(attach);
    SyncSfuSubscriptions(call_id);
    return;
  }
  log().info << "FlushPendingInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id;
  (void)OnInboundSfuAttach(call_id, attach);
}

std::vector<std::string> CallHopPath::DialableHopPeerIds() const {
  std::vector<std::string> out;
  for (const mesh::MeshHopCandidate& hop : ranking_.Ranked()) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (hop.dialable || (relay_deps_.dial && relay_deps_.dial->IsDialable(hop.peer_id))) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

void CallHopPath::ReportSfuAttachFailedToInitiator(const std::string& call_id, const std::string& failed_hop,
                                                   const std::string& error) {
  if (!call_id.empty() && (inbound_gate_.last_failed && call_id == call_id_) &&
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
    reports_.Emit(hop_report::MediaError{call_id_, Localized("call.error.hop_unreachable_guest")});
    ClearSfuAttachWait();
    reports_.Emit(hop_report::LeaveWanted{call_id});
    return;
  }

  CallSfuAttachFailedDetail detail;
  detail.call_id = call_id;
  detail.identity = *local;
  detail.failed_hop_peer_id = failed_hop;
  detail.error = error;
  detail.preferred_hop_peer_ids = CapGuestHopPreferences(DialableHopPeerIds(), failed_hop, kMaxGuestHopPreferences);
  auto encoded = CallControlCodec::EncodeSfuAttachFailed(detail);
  if (!encoded) {
    return;
  }
  inbound_gate_.last_failed = true;
  inbound_gate_.last_fail_hop = failed_hop;
  log().info << "ReportSfuAttachFailed to initiator=" << initiator << " prefs=" << detail.preferred_hop_peer_ids.size();
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.looking_for_another_path")});
  reports_.Emit(hop_report::Changed{call_id_});
  (void)host_.send_direct(initiator, CallControlType::CallSfuAttachFailed, *encoded, "Call hop attach failed");
}

void CallHopPath::RefuseGuestNoSharedHop(const std::string& call_id, const std::string& guest_identity) {
  // As the next event, never on the path that found it (call-control once arrived on Browser IO —
  // SoftMigrate dogfood: malloc corruption / abort when refusing Samsung mid PreferLocal).
  outbox_.Emit(hop_path_event::RefuseGuest{call_id, guest_identity});
}

void CallHopPath::RefuseGuest(const hop_path_event::RefuseGuest& refuse_event) {
  const std::string& call_id = refuse_event.call_id;
  const std::string& guest_identity = refuse_event.guest_identity;
  const std::string message = Localized("call.error.hop_unreachable_guest");
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
  const std::string owner_toast = Localized("call.error.hop_unreachable_owner", {{"name", display}});
  EjectParticipantAfterMigrateFailure(call_id, guest_identity, owner_toast);
}

bool CallHopPath::ForThisCall(const std::string& call_id, const char* what) const {
  if (call_id == call_id_) {
    return true;
  }
  log().warning << what << " for call_id=" << call_id << " on the group path of call_id=" << call_id_ << " (ignored)";
  return false;
}

void CallHopPath::SetOutbox(OwnerOutbox<HopPathEvent> outbox) {
  outbox_ = std::move(outbox);
  if (relay_loss_observer_ != 0) {
    UnwatchRelayLoss(); // its observer holds the outbox it was given: watch again with this one
    WatchRelayLoss();
  }
  migrate_outbox_ = outbox_.For<HopMigrateEvent>([](HopMigrateEvent event) { return HopPathEvent{std::move(event)}; });
}

void CallHopPath::Handle(HopPathEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, hop_path_event::RelayTransportLost>) {
          if (e.watch == relay_watch_) {
            OnGuestSfuTransportLost();
          }
        } else if constexpr (std::is_same_v<E, hop_path_event::AttachWaitDeadline>) {
          if (e.armed == attach_wait_.armed) {
            attach_wait_.timer_id = 0;
            OnAttachWaitTimerFire(e.call_id);
          }
        } else if constexpr (std::is_same_v<E, hop_path_event::ReannouncePublisher>) {
          ReannouncePublisher(e);
        } else if constexpr (std::is_same_v<E, hop_path_event::RefuseGuest>) {
          RefuseGuest(e);
        } else if constexpr (std::is_same_v<E, hop_path_event::Continue>) {
          steps_.Run(e.id);
        } else if constexpr (std::is_same_v<E, HopMigrateEvent>) {
          HandleMigrateEvent(e);
        } else {
          static_assert(!sizeof(E), "handle every HopPathEvent");
        }
      },
      event);
}

void CallHopPath::Defer(std::function<void()> step) {
  // A hop flow's finish step runs as the next event, never inside the flow that reported it.
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  outbox_.Emit(hop_path_event::Continue{steps_.Store(std::move(step))});
}

void CallHopPath::ReannouncePublisher(const hop_path_event::ReannouncePublisher& again) {
  // V050: a later joiner's re-pick may have moved the group — never re-announce the old hop.
  if (!sfu_.attached || !engine_->Runs() || flight_.attached_hop_peer_id != again.hop_peer_id ||
      channel_->LocalStreamId() != again.publisher_stream_id) {
    return;
  }
  log().info << "AnnounceLocalPublisher re-fan-out call_id=" << again.call_id;
  (void)host_.fan_out_joined(again.call_id, CallControlType::CallSfuAttach, again.encoded_attach, "Call SFU attach",
                             again.local_identity);
}

void CallHopPath::OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail) {
  if (!ForThisCall(detail.call_id, "OnInboundSfuAttachFailed")) {
    return;
  }
  auto local = host_.local_relay_identity();
  HopFailureInput in;
  in.sticky_initiator = local && IsStickyInitiator(detail.call_id, *local);
  if (!in.sticky_initiator) {
    return;
  }
  in.guest = detail.identity;
  const std::string& guest = in.guest;
  if (auto moved = hop_hint_repicked_.find(guest); moved != hop_hint_repicked_.end()) {
    in.already_moved_guest_to_failed_hop = moved->second == detail.failed_hop_peer_id;
  }
  if (!in.already_moved_guest_to_failed_hop) {
    // V050: everyone moves, so the new hop must be one the other members reached too (their accept
    // reports; members without a report do not constrain).
    const std::vector<std::string> guest_prefs = planning_.UsableForMembers(
        detail.call_id, JoinedMembersOtherThan(detail.call_id, guest), detail.preferred_hop_peer_ids);
    in.owner = DecideHopHintOwnerAction(guest_prefs, DialableHopPeerIds(), detail.failed_hop_peer_id);
    if (in.owner.action == HopHintOwnerAction::RePick) {
      in.may_leave_prefer_local = HopHintMayLeavePreferLocal(in.owner.preferred_hop_peer_id);
      in.on_pick = sfu_.attached && IsOnOrHintedHop(detail.call_id, in.owner.preferred_hop_peer_id);
    }
  }
  in.migrate_in_flight = flight_.in_flight;
  const std::string& prefer = in.owner.preferred_hop_peer_id;
  switch (DecideHopFailure(in)) {
    case HopFailureAction::Ignore:
      log().warning << "Hop hint ignored (no guest) failed_hop=" << detail.failed_hop_peer_id;
      return;
    case HopFailureAction::RefuseGuest:
      log().warning << "Hop hint refuse guest=" << guest << " failed_hop=" << detail.failed_hop_peer_id
                    << " prefer=" << prefer << " moved_before=" << (in.already_moved_guest_to_failed_hop ? 1 : 0)
                    << " keep_prefer_local=" << (in.may_leave_prefer_local ? 0 : 1);
      RefuseGuestNoSharedHop(detail.call_id, guest);
      return;
    case HopFailureAction::RefanOut:
      // Already SoftMigrated onto the pick: re-fan-out only (do not Detach again).
      log().info << "Hop hint no-op: already on prefer hop=" << prefer << " guest=" << guest;
      FanOutSfuAttachForHop(detail.call_id, prefer, *local);
      SyncSfuSubscriptions(detail.call_id);
      reports_.Emit(hop_report::Activity{call_id_, {}});
      reports_.Emit(hop_report::Changed{call_id_});
      return;
    case HopFailureAction::Coalesce:
      flight_.pending_hop_prefer = prefer;
      log().info << "Hop hint coalesced prefer=" << prefer << " guest=" << guest;
      return;
    case HopFailureAction::Repick:
      break;
  }
  hop_hint_repicked_[guest] = prefer;
  StartHopHintRepick(detail.call_id, prefer, guest);
}

bool CallHopPath::IsStickyInitiator(const std::string& call_id, const std::string& local_identity) const {
  return call_queries::IsStickyInitiator(sessions_, call_id, local_identity);
}

bool CallHopPath::HopHintMayLeavePreferLocal(const std::string& prefer_hop_peer_id) const {
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

bool CallHopPath::IsOnOrHintedHop(const std::string& call_id, const std::string& hop_peer_id) const {
  if (hop_peer_id == flight_.attached_hop_peer_id) {
    return true;
  }
  auto session = sessions_.LoadSession(call_id);
  return session && session->has_value() && (*session)->sfu_hint && *(*session)->sfu_hint == hop_peer_id;
}

void CallHopPath::StartHopHintRepick(const std::string& call_id, const std::string& prefer, const std::string& guest) {
  log().info << "Hop hint re-pick prefer=" << prefer << " guest=" << guest;
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.switching_media_path")});
  reports_.Emit(hop_report::Changed{call_id_});
  BeginSfuAttachWait(call_id);
  // V035: do not bump flight_.migrate_generation on hop-hint (Leave/teardown only).
  const uint64_t gen = flight_.migrate_generation.load(std::memory_order_acquire);
  flight_.flight_gen = gen;
  flight_.in_flight = true;
  flight_.for_call = true;
  MaybeSoftMigrateToSfuAsync(
      call_id, SoftMigrateTrigger::IceRecover, prefer, gen, [this, call_id, guest, gen](Roe<void> mig) {
        Defer([this, call_id, mig, guest, gen]() { FinishHopHintRepick(call_id, guest, gen, mig); });
      });
}

void CallHopPath::FinishHopHintRepick(const std::string& call_id, const std::string& guest, uint64_t gen,
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
  reports_.Emit(hop_report::Activity{call_id_, {}});
  reports_.Emit(hop_report::Changed{call_id_});
  FlushPendingHopPrefer(call_id);
  FlushPendingInboundSfuAttach();
}

void CallHopPath::OnInboundHopRefuse(const CallHopRefuseDetail& detail) {
  if (!ForThisCall(detail.call_id, "OnInboundHopRefuse")) {
    return;
  }
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
  const std::string message = detail.message.empty() ? Localized("call.error.hop_unreachable_guest") : detail.message;
  reports_.Emit(hop_report::MediaError{call_id_, message});
  log().warning << "CallHopRefuse call_id=" << detail.call_id << " reason=" << detail.reason;
  ClearSfuAttachWait();
  reports_.Emit(hop_report::LeaveWanted{detail.call_id});
  reports_.Emit(hop_report::Changed{call_id_});
}

// --- The move to the hop and the attach (was CallHopMigrateWorkflow, V046/V047; merged by V057 6) ---

namespace {

std::string SoftMigrateNoHopMessage(const std::vector<std::string>& hop_failures) {
  const HopPickFailure failure = ClassifyHopPickFailures(hop_failures);
  if (failure == HopPickFailure::AllPayment) {
    return Localized("call.error.payment_unavailable_media");
  }
  std::string msg = Localized("call.error.no_media_relay_hop");
  if (failure != HopPickFailure::AllUndialable) {
    return msg;
  }
  const std::string tip = Localized(kPlatformNetworkHintKey);
  if (!tip.empty()) {
    msg += " ";
    msg += tip;
  }
  return msg;
}

} // namespace

bool CallHopPath::IsMigrateGenerationCurrent(uint64_t gen) const {
  return gen == 0 || gen == flight_.migrate_generation.load(std::memory_order_acquire);
}

/** One SoftMigrate hop pick: walked by TryPickHop / AttachPickedHop, each step holding it by shared_ptr. */
struct CallHopPath::HopPick {
  std::string call_id;
  uint64_t expected_gen = 0;
  std::string local_identity;
  std::string local_peer_id;
  std::optional<CallSession> session;
  std::vector<mesh::MeshHopCandidate> ranked;
  std::vector<std::string> failures;
  std::function<void(Roe<void>)> on_done;
};

bool CallHopPath::IsLiveOnHopFor(const std::string& call_id) const {
  return sfu_.attached && engine_->Runs();
}

void CallHopPath::MaybeSoftMigrateToSfuAsync(const std::string& call_id, SoftMigrateTrigger trigger,
                                             const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                             std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (AppRuntime::IsShuttingDown()) {
    log().debug << "MaybeSoftMigrateToSfuAsync rejected: shutting down call_id=" << call_id;
    on_done(Error("shutdown in progress"));
    return;
  }
  if (!PassSoftMigrateArmingGate(call_id, trigger, prefer_hop_peer_id, on_done)) {
    return;
  }
  Defer([this, call_id, trigger, prefer_hop_peer_id, expected_gen, on_done = std::move(on_done)]() mutable {
    RunSoftMigrate(call_id, trigger, prefer_hop_peer_id, expected_gen, std::move(on_done));
  });
}

bool CallHopPath::PassSoftMigrateArmingGate(const std::string& call_id, SoftMigrateTrigger trigger,
                                            const std::string& prefer_hop_peer_id,
                                            const std::function<void(Roe<void>)>& on_done) {
  const auto arming_ports = arming_.Get();
  // V037/V048: SoftMigrate from Direct* enters Migrating only when N≥3 (or IceRecover / prefer).
  // Relay-cap nudge used expected_gen=0 and promoted 1:1 DirectConnecting → Migrating PreferLocal
  // while the peer stayed on circuit — dogfood "Connecting group media…" vs Connecting.
  SoftMigrateArmingInput in;
  in.arming_bound = arming_ports->IsBound();
  if (in.arming_bound) {
    if (auto joined = sessions_.CountJoined(call_id)) {
      in.n_joined = *joined;
    }
    in.trigger = trigger;
    in.prefer_named = !prefer_hop_peer_id.empty();
    in.soft_migrate_may_arm = arming_ports->soft_migrate_may_arm && arming_ports->soft_migrate_may_arm();
    in.migrate_ops_allowed = arming_ports->hop_ops_allowed && arming_ports->hop_ops_allowed();
  }
  const char* arming_name =
      in.arming_bound && arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "?";
  switch (DecideSoftMigrateArming(in)) {
    case SoftMigrateArming::Proceed:
      return true;
    case SoftMigrateArming::ArmAndProceed:
      Apply(CallHopPlannerEvent::SoftMigrateRequested, call_id); // reports Migrating
      return true;
    case SoftMigrateArming::StayDirect:
      log().info << "MaybeSoftMigrateToSfuAsync skipped (1:1 stay Direct) call_id=" << call_id
                 << " n_joined=" << in.n_joined << " arming=" << arming_name
                 << " trigger=" << static_cast<int>(trigger);
      on_done(Roe<void>());
      return false;
    case SoftMigrateArming::NotArmed:
      log().info << "MaybeSoftMigrateToSfuAsync skipped (hop not armed) call_id=" << call_id
                 << " arming=" << arming_name;
      on_done(Error("hop path not armed"));
      return false;
  }
  return true;
}

void CallHopPath::RunSoftMigrate(const std::string& call_id, SoftMigrateTrigger trigger,
                                 const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                 std::function<void(Roe<void>)> on_done) {
  if (!IsMigrateGenerationCurrent(expected_gen)) {
    log().info << "SoftMigrate skip stale gen want=" << expected_gen
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire) << " call_id=" << call_id;
    on_done(Roe<void>());
    return;
  }
  const bool repick = !prefer_hop_peer_id.empty();
  if (!repick && IsLiveOnHopFor(call_id)) {
    SyncSfuSubscriptions(call_id);
    on_done(Roe<void>());
    return;
  }
  if (!relay_deps_.relay || !relay_deps_.dial) {
    on_done(Error("media_relay not available"));
    return;
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    on_done(local.error());
    return;
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    on_done(participants.error());
    return;
  }

  auto pick = std::make_shared<HopPick>();
  pick->call_id = call_id;
  pick->expected_gen = expected_gen;
  pick->local_identity = *local;
  pick->on_done = std::move(on_done);
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    pick->session = **session;
  }

  if (!repick) {
    const SoftMigrateAction action = DecideFirstSoftMigrate(*pick, trigger, *participants);
    if (action == SoftMigrateAction::NoOp) {
      SyncSfuSubscriptions(call_id);
      pick->on_done(Roe<void>());
      return;
    }
    if (action == SoftMigrateAction::WaitForAttach) {
      reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.waiting_for_media_path")});
      // Owner may have already fan-out CallSfuAttach; do not sit behind TailSync-starved polls.
      reports_.Emit(hop_report::InboxSyncWanted{call_id_});
      reports_.Emit(hop_report::Changed{call_id_});
      pick->on_done(Roe<void>());
      return;
    }
  } else if (sfu_.attached && SettleRepickOnCurrentHop(*pick, prefer_hop_peer_id)) {
    return;
  }

  pick->ranked = RankHopsForSoftMigrate(*pick, prefer_hop_peer_id);
  if (pick->ranked.empty()) {
    pick->on_done(Error(Localized("call.error.no_media_relay_hop")));
    return;
  }
  reports_.Emit(hop_report::Activity{
      call_id_, repick ? Localized("call.status.switching_media_path") : Localized("call.status.finding_media_path")});
  reports_.Emit(hop_report::Changed{call_id_});
  pick->failures.reserve(pick->ranked.size());
  TryPickHop(std::move(pick), 0);
}

SoftMigrateAction CallHopPath::DecideFirstSoftMigrate(const HopPick& pick, SoftMigrateTrigger trigger,
                                                      const std::vector<CallParticipant>& participants) {
  SoftMigrateDecisionInput decision_in;
  decision_in.local_identity = pick.local_identity;
  std::vector<SoftMigrateJoinedPeer> joined_peers;
  for (const CallParticipant& p : participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    decision_in.joined_identities.push_back(p.identity);
    SoftMigrateJoinedPeer peer;
    peer.identity = p.identity;
    peer.joined_at = p.joined_at;
    joined_peers.push_back(std::move(peer));
  }
  decision_in.initiator_identity = SelectCallInitiator(joined_peers);
  decision_in.sfu_hint_empty = !pick.session || !pick.session->sfu_hint || pick.session->sfu_hint->empty();
  decision_in.trigger = trigger;
  decision_in.already_on_sfu = IsLiveOnHopFor(pick.call_id);

  // The owner picks; everyone else waits for its CallSfuAttach (V050). Who hosts is the owner's ranking
  // (durable relays first, its own Node on a confirmed LAN) — never a reason for a second picker or for
  // the owner to wait (#385).
  const SoftMigrateAction action = DecideSoftMigrate(decision_in);
  log().info << "SoftMigrate decide action=" << static_cast<int>(action) << " trigger=" << static_cast<int>(trigger)
             << " joined=" << decision_in.joined_identities.size() << " initiator=" << decision_in.initiator_identity
             << " local=" << pick.local_identity << " call_id=" << pick.call_id;
  return action;
}

bool CallHopPath::SettleRepickOnCurrentHop(HopPick& pick, const std::string& prefer_hop_peer_id) {
  const std::string& call_id = pick.call_id;
  RepickOnHopInput in;
  in.current_hop = !flight_.attached_hop_peer_id.empty()    ? flight_.attached_hop_peer_id
                   : pick.session && pick.session->sfu_hint ? *pick.session->sfu_hint
                                                            : std::string();
  in.prefer_hop = prefer_hop_peer_id;
  if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
    in.local_peer_id = *pid;
  }
  in.local_hop_attached = relay_deps_.relay->IsLocalHopAttached();
  in.prefer_dialable =
      relay_deps_.dial && !prefer_hop_peer_id.empty() &&
      (relay_deps_.dial->IsDialable(prefer_hop_peer_id) || !ranking_.HopMultiaddr(prefer_hop_peer_id).empty());
  const RepickOnHopAction action = DecideRepickOnHop(in);
  switch (action) {
    case RepickOnHopAction::StayOnNamedHop:
    case RepickOnHopAction::KeepCurrentHop:
      log().info << "SoftMigrate re-pick no-op: "
                 << (action == RepickOnHopAction::StayOnNamedHop ? "already on prefer" : "keep attached")
                 << " hop=" << in.current_hop << " call_id=" << call_id;
      FanOutSfuAttachForHop(call_id, in.current_hop, pick.local_identity);
      SyncSfuSubscriptions(call_id);
      if (action == RepickOnHopAction::StayOnNamedHop) {
        reports_.Emit(hop_report::Activity{call_id_, {}});
        reports_.Emit(hop_report::Changed{call_id_});
      }
      pick.on_done(Roe<void>());
      return true;
    case RepickOnHopAction::KeepPreferLocal:
      log().info << "SoftMigrate re-pick no-op: keep PreferLocal call_id=" << call_id
                 << " prefer=" << prefer_hop_peer_id;
      SyncSfuSubscriptions(call_id);
      pick.on_done(Error("keep_prefer_local"));
      return true;
    case RepickOnHopAction::KeepNotDialable:
      log().warning << "SoftMigrate re-pick aborted: prefer hop not dialable, keep current SFU hop="
                    << prefer_hop_peer_id;
      SyncSfuSubscriptions(call_id);
      pick.on_done(Roe<void>());
      return true;
    case RepickOnHopAction::Detach:
      break;
  }
  log().info << "SoftMigrate re-pick detach call_id=" << call_id << " prefer=" << prefer_hop_peer_id
             << " leaving_prefer_local=" << (in.local_hop_attached ? 1 : 0);
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.switching_media_path")});
  reports_.Emit(hop_report::Changed{call_id_});
  relay_deps_.relay->Detach();
  sfu_.SetAttached(false);
  flight_.attached_hop_peer_id.clear();
  flight_.attaching_hop_peer_id.clear();
  if (pick.session) {
    pick.session->sfu_hint.reset();
  }
  notes_.NoteCallHop(pick.call_id, {});
  return false;
}

std::vector<mesh::MeshHopCandidate> CallHopPath::RankHopsForSoftMigrate(HopPick& pick,
                                                                        const std::string& prefer_hop_peer_id) {
  if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
    pick.local_peer_id = *pid;
  }
  const std::string local_ma = ranking_.LocalAdvertiseMa(pick.local_peer_id);
  const CallHopScope hop_scope = InferScopeForCall(pick.call_id, pick.local_identity);
  const bool lan_ok = LanReachabilityConfirmedForCall(pick.call_id, pick.local_identity);
  const bool prefer_local_flag =
      relay_deps_.prefer_local_as_hop && relay_deps_.relay->IsStarted() && !pick.local_peer_id.empty();
  if (prefer_local_flag && local_ma.empty()) {
    log().warning << "PreferLocal skipped: no advertise multiaddr for local hop";
  }
  auto ranked =
      SelectCallMediaHop(ranking_.Ranked(), hop_scope, pick.local_peer_id, prefer_local_flag, local_ma, lan_ok);
  log().info << "SoftMigrate PickHop scope=" << static_cast<int>(hop_scope) << " lan_ok=" << (lan_ok ? 1 : 0)
             << " prefer_local=" << (prefer_local_flag ? 1 : 0)
             << " first=" << (ranked.empty() ? "" : ranked.front().peer_id) << " call_id=" << pick.call_id;
  if (!prefer_hop_peer_id.empty()) {
    return PreferNamedHopFirst(std::move(ranked), prefer_hop_peer_id);
  }
  if (pick.session && pick.session->planned_hop && !pick.session->planned_hop->peer_id.empty()) {
    ranked = PlannedHopFirst(std::move(ranked), *pick.session->planned_hop);
    log().info << "SoftMigrate planned hop first=" << pick.session->planned_hop->peer_id << " call_id=" << pick.call_id;
  }
  return ranked;
}

void CallHopPath::TryPickHop(std::shared_ptr<HopPick> pick, size_t index) {
  if (!IsMigrateGenerationCurrent(pick->expected_gen)) {
    log().info << "SoftMigrate abort mid-pick stale gen want=" << pick->expected_gen
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    pick->on_done(Roe<void>());
    return;
  }
  if (index >= pick->ranked.size()) {
    FailHopPick(*pick);
    return;
  }
  const mesh::MeshHopCandidate& hop = pick->ranked[index];
  const bool self_hop = !pick->local_peer_id.empty() && hop.peer_id == pick->local_peer_id;
  // Directory/org seeds publish MAs before Amp address-book learn — register then dial.
  if (!self_hop && relay_deps_.dial && !hop.multiaddr.empty() && !relay_deps_.dial->IsDialable(hop.peer_id)) {
    (void)relay_deps_.dial->RegisterEndpoint(hop.peer_id, hop.multiaddr);
  }
  if (!self_hop && relay_deps_.circuit_reach && relay_deps_.dial && !relay_deps_.dial->IsDialable(hop.peer_id)) {
    // Edge: the reach answers on another thread — it only reports; the pick goes on as the next event.
    const uint64_t id = steps_.Store([this, pick, index]() { AttachPickedHop(pick, index); });
    relay_deps_.circuit_reach->TryEnsureHopReachableAsync(
        hop.peer_id,
        [outbox = migrate_outbox_, id](mesh::CircuitReachRoe) { outbox.Emit(hop_migrate_event::Continue{id}); });
    return;
  }
  AttachPickedHop(std::move(pick), index);
}

void CallHopPath::FailHopPick(HopPick& pick) {
  if (pick.failures.empty()) {
    pick.on_done(Error(Localized("call.error.no_media_relay_hop")));
    return;
  }
  std::string summary = "media_relay SoftMigrate failed (" + std::to_string(pick.failures.size()) + " hops): ";
  for (size_t i = 0; i < pick.failures.size(); ++i) {
    if (i > 0) {
      summary += " | ";
    }
    summary += pick.failures[i];
  }
  log().warning << summary;
  pick.on_done(Error(SoftMigrateNoHopMessage(pick.failures)));
}

void CallHopPath::AttachPickedHop(std::shared_ptr<HopPick> pick, size_t index) {
  const auto seat_ports = seat_.Get();
  const mesh::MeshHopCandidate& hop = pick->ranked[index];
  const bool self_hop = !pick->local_peer_id.empty() && hop.peer_id == pick->local_peer_id;
  if (!self_hop && (!relay_deps_.dial || !relay_deps_.dial->IsDialable(hop.peer_id))) {
    const std::string detail = "hop not dialable (hop=" + hop.peer_id + ")";
    pick->failures.push_back(detail);
    log().warning << "SoftMigrate skip: " << detail;
    TryPickHop(std::move(pick), index + 1);
    return;
  }
  std::string hop_ma = hop.multiaddr;
  if (hop_ma.empty() && relay_deps_.dial) {
    if (auto ma = relay_deps_.dial->PreferredMultiaddr(hop.peer_id)) {
      hop_ma = *ma;
    }
  }
  log().info << "SoftMigrate try hop=" << hop.peer_id << " affinity=" << static_cast<int>(hop.affinity)
             << " ma=" << (hop_ma.empty() ? "(circuit)" : hop_ma);
  // Seat BeginAttach runs inside AttachLocalToSfuAsync (single owner). If another hop is
  // already attaching, skip this candidate so SoftMigrate does not stall on coalesce no-op.
  if (seat_ports->IsBound() && seat_ports->has_attach_in_flight() && seat_ports->attaching_hop() != hop.peer_id) {
    log().info << "SoftMigrate defer hop (seat attach in flight) call_id=" << pick->call_id << " hop=" << hop.peer_id
               << " in_flight=" << seat_ports->attaching_hop();
    TryPickHop(std::move(pick), index + 1);
    return;
  }
  flight_.attaching_hop_peer_id = hop.peer_id;
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.connecting_media_relay")});
  reports_.Emit(hop_report::Changed{call_id_});
  CallSfuAttachDetail attach;
  attach.call_id = pick->call_id;
  attach.hop_peer_id = hop.peer_id;
  attach.hop_multiaddr = hop_ma;
  attach.publisher_stream_id = PublisherStreamIdForLocal();

  if (self_hop) {
    RecordPickedHop(*pick, hop.peer_id);
    FanOutPickedHop(pick->call_id, attach, pick->local_identity);
  }
  const std::string call_id = pick->call_id;
  AttachLocalToSfuAsync(call_id, attach, [this, pick, index, self_hop, attach](Roe<void> attached) {
    OnPickedHopAttached(pick, index, self_hop, attach, std::move(attached));
  });
}

void CallHopPath::OnPickedHopAttached(const std::shared_ptr<HopPick>& pick, size_t index, bool self_hop,
                                      const CallSfuAttachDetail& attach, Roe<void> attached) {
  if (!attached) {
    std::string detail = attached.error().message;
    if (detail.find(attach.hop_peer_id) == std::string::npos) {
      detail += " (hop=" + attach.hop_peer_id + ")";
    }
    pick->failures.push_back(detail);
    log().warning << "SoftMigrate hop failed: " << detail;
    Defer([this, pick, index]() { TryPickHop(pick, index + 1); });
    return;
  }
  RecordPickedHop(*pick, attach.hop_peer_id);
  if (!self_hop) {
    FanOutPickedHop(pick->call_id, attach, pick->local_identity);
  }
  pick->on_done(Roe<void>());
}

void CallHopPath::RecordPickedHop(HopPick& pick, const std::string& hop_peer_id) {
  if (pick.session) {
    pick.session->sfu_hint = hop_peer_id;
  }
  notes_.NoteCallHop(pick.call_id, hop_peer_id);
}

void CallHopPath::FanOutPickedHop(const std::string& call_id, const CallSfuAttachDetail& attach,
                                  const std::string& local_identity) {
  auto encoded = CallControlCodec::EncodeSfuAttach(BuildSfuAttachFanout(attach));
  if (!encoded) {
    return;
  }
  log().info << "SoftMigrate fan-out CallSfuAttach hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr) << " call_id=" << call_id;
  (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", local_identity);
  migrate_outbox_.After(std::chrono::milliseconds(2000),
                        hop_migrate_event::RefanOutPickedHop{call_id, *encoded, local_identity, attach.hop_peer_id});
}

mesh::MediaRelayAttachPorts CallHopPath::RelayAttachPorts() const {
  return relay_deps_.AttachPorts();
}

mesh::MediaRelayAttachRequest CallHopPath::MakeRelayAttachRequest(const std::string& call_id,
                                                                  const CallSfuAttachDetail& attach) const {
  mesh::MediaRelayAttachRequest request;
  request.hop_peer_id = attach.hop_peer_id;
  request.hop_multiaddr = attach.hop_multiaddr;
  request.session_id = call_id;
  request.auth = call_id;
  request.quote.session_id = call_id;
  auto joined = sessions_.CountJoined(call_id);
  request.quote.participants = joined ? static_cast<int>(*joined) : 2;
  const bool video_allowed = [&]() {
    if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
      return (*session)->video_allowed;
    }
    return false;
  }();
  request.quote.want_up_bps = CallMediaAdaptation::QuoteWantUpBps(video_allowed);
  request.quote.want_down_bps = request.quote.want_up_bps * std::max(1, request.quote.participants - 1);
  return request;
}

std::function<Roe<void>(const mesh::MediaRelayQuote&)> CallHopPath::RelayQuotePricingGate() {
  // Calls only take volunteer / free hops today (pricing project gate).
  return [](const mesh::MediaRelayQuote& quote) -> Roe<void> {
    return InitiationPricing::CheckRelayQuotePayable(quote.rate);
  };
}

/** One attach of this call to a media_relay hop (remote or our own): what the completion checks against. */
struct CallHopPath::HopAttach {
  std::string call_id;
  CallSfuAttachDetail attach;
  bool self_hop = false;
  /** Migrate generation / media cancel generation when the attach began. */
  uint64_t gen_at_start = 0;
  uint64_t cancel_gen_at_start = 0;
  ByteVector media_key;
  uint32_t media_epoch = 1;
  /** The attach's keys: the epoch it starts on, then each rotation (V003) — see SfuKeyRing. */
  std::shared_ptr<SfuKeyRing> keys = std::make_shared<SfuKeyRing>();
  /** Inbound frames are dropped until the attach is committed. */
  std::shared_ptr<std::atomic<bool>> frames_ready = std::make_shared<std::atomic<bool>>(false);
};

// --- attach (start) -------------------------------------------------------------------------------

void CallHopPath::AttachLocalToSfuAsync(const std::string& call_id, const CallSfuAttachDetail& attach_in,
                                        std::function<void(Roe<void>)> on_done) {
  const auto arming_ports = arming_.Get();
  if (!on_done) {
    return;
  }
  HopAttach at;
  at.call_id = call_id;
  at.attach = attach_in;
  at.gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
  at.cancel_gen_at_start =
      arming_ports->IsBound() && arming_ports->media_cancel_gen ? arming_ports->media_cancel_gen() : 0;
  if (!relay_deps_.relay || !relay_deps_.dial) {
    on_done(Error("media_relay not available"));
    return;
  }
  if (at.attach.hop_peer_id.empty()) {
    on_done(Error("missing hop_peer_id"));
    return;
  }
  log().info << "AttachLocalToSfu begin call_id=" << call_id << " hop=" << at.attach.hop_peer_id
             << " ma=" << (at.attach.hop_multiaddr.empty() ? "(empty)" : at.attach.hop_multiaddr)
             << " already_sfu=" << (sfu_.attached ? 1 : 0);
  // Same call already owns SFU duplex — never open a parallel AcceptAndAttach (Detach kills RX).
  if (IsLiveOnHopFor(call_id)) {
    log().info << "AttachLocalToSfu no-op already duplex call_id=" << call_id << " hop=" << at.attach.hop_peer_id
               << " attached_hop=" << flight_.attached_hop_peer_id;
    NoteRemotePublisherFromAttach(at.attach);
    SyncSfuSubscriptions(call_id);
    flight_.attached_hop_peer_id = at.attach.hop_peer_id;
    reports_.Emit(hop_report::Activity{call_id_, {}});
    on_done(Roe<void>());
    return;
  }
  if (auto claimed = ClaimHopAttachFlight(call_id, at.attach); !claimed || !*claimed) {
    on_done(claimed ? Roe<void>() : Roe<void>(claimed.error()));
    return;
  }
  on_done = ReleaseHopAttachFlightOnError(call_id, at.attach.hop_peer_id, std::move(on_done));
  if (auto keyed = LoadHopMediaKey(at); !keyed) {
    on_done(keyed.error());
    return;
  }
  if (auto local_pid = relay_deps_.relay->LocalPeerIdBase58()) {
    at.self_hop = *local_pid == at.attach.hop_peer_id;
  }
  if (at.self_hop) {
    AttachAsLocalHop(std::move(at), std::move(on_done));
  } else {
    AttachThroughRelay(std::move(at), std::move(on_done));
  }
}

Roe<bool> CallHopPath::ClaimHopAttachFlight(const std::string& call_id, const CallSfuAttachDetail& attach) {
  const auto seat_ports = seat_.Get();
  // SoftMigrate sets attaching_hop / seat BeginAttach before calling us — same hop means we
  // own this attempt. A different in-flight hop must not start a parallel AcceptAndAttach.
  if (seat_ports->IsBound()) {
    CallMediaSeat::AttachTicket ticket;
    switch (seat_ports->begin_attach(call_id, attach.hop_peer_id, &ticket)) {
      case CallMediaSeat::AttachBeginResult::DeferredOtherHop:
        log().info << "AttachLocalToSfu coalesce (other hop in flight) call_id=" << call_id
                   << " in_flight_hop=" << seat_ports->attaching_hop() << " requested=" << attach.hop_peer_id;
        inbound_gate_.pending_attach = attach;
        inbound_gate_.pending = true;
        NoteRemotePublisherFromAttach(attach);
        BeginSfuAttachWait(call_id);
        return false;
      case CallMediaSeat::AttachBeginResult::CoalescedSameHop:
        // SoftMigrate may have set attaching_hop before calling us; only skip when a *foreign*
        // AcceptAndAttach already owns the hop (attaching set by a prior AttachLocalToSfuAsync).
        // First claim for this hop always BeginAttach→Started; Coalesced means parallel entry.
        log().info << "AttachLocalToSfu coalesce (same hop in flight) call_id=" << call_id
                   << " hop=" << attach.hop_peer_id;
        flight_.attaching_hop_peer_id = attach.hop_peer_id;
        NoteRemotePublisherFromAttach(attach);
        BeginSfuAttachWait(call_id);
        return false;
      case CallMediaSeat::AttachBeginResult::Rejected:
        return Error("attach rejected");
      default:
        break;
    }
  } else if (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id != attach.hop_peer_id) {
    log().info << "AttachLocalToSfu coalesce (other hop in flight) call_id=" << call_id
               << " in_flight_hop=" << flight_.attaching_hop_peer_id << " requested=" << attach.hop_peer_id;
    NoteRemotePublisherFromAttach(attach);
    BeginSfuAttachWait(call_id);
    return false;
  }
  flight_.attaching_hop_peer_id = attach.hop_peer_id;
  return true;
}

std::function<void(Roe<void>)> CallHopPath::ReleaseHopAttachFlightOnError(const std::string& call_id,
                                                                          const std::string& hop,
                                                                          std::function<void(Roe<void>)> on_done) {
  // Any failure clears the attach flight so SoftMigrate / inbound can retry.
  return [this, call_id, hop, on_done = std::move(on_done)](Roe<void> r) {
    const auto seat_ports = seat_.Get();
    if (!r) {
      if (flight_.attaching_hop_peer_id == hop) {
        flight_.attaching_hop_peer_id.clear();
      }
      if (seat_ports->IsBound()) {
        seat_ports->end_attach_if_matching(call_id, hop);
      }
    }
    on_done(std::move(r));
  };
}

Roe<void> CallHopPath::LoadHopMediaKey(HopAttach& at) const {
  if (auto session = sessions_.LoadSession(at.call_id); session && session->has_value()) {
    at.media_epoch = session->value().media_epoch;
  }
  if (!media_keys_) {
    // Fail closed: no key store means no media key — never a plaintext SFU attach.
    log().warning << "AttachLocalToSfu without media key store call_id=" << at.call_id;
    return Error("call media key required for SFU");
  }
  if (auto key = media_keys_->LoadEpochKey(at.call_id, at.media_epoch); key && key->has_value()) {
    at.media_key = **key;
  }
  if (at.media_key.empty()) {
    log().warning << "AttachLocalToSfu missing media key call_id=" << at.call_id << " epoch=" << at.media_epoch;
    return Error("call media key required for SFU");
  }
  at.keys->Install(at.media_epoch, at.media_key, util::NowUnixMs());
  return {};
}

void CallHopPath::InstallMediaKey(const std::string& call_id) {
  if (call_id != call_id_ || !channel_->HasLiveKeys() || !media_keys_) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return;
  }
  const uint32_t epoch = (*session)->media_epoch;
  if (auto key = media_keys_->LoadEpochKey(call_id, epoch); key && key->has_value()) {
    channel_->InstallKey(epoch, **key);
    log().info << "SFU media key now epoch=" << epoch << " call_id=" << call_id;
  }
}

std::function<void(mesh::MediaDataFrame)> CallHopPath::MakeHopFrameSink(const HopAttach& at) {
  return channel_->MakeFrameSink(at.keys, at.frames_ready);
}

void CallHopPath::AttachAsLocalHop(HopAttach at, std::function<void(Roe<void>)> on_done) {
  if (!relay_deps_.prefer_local_as_hop || !relay_deps_.relay->IsStarted()) {
    log().warning << "AttachLocalToSfu refused local hop (prefer_local=" << (relay_deps_.prefer_local_as_hop ? 1 : 0)
                  << " started=" << (relay_deps_.relay->IsStarted() ? 1 : 0) << ")";
    on_done(Error(Localized("call.error.local_media_relay_unavailable")));
    return;
  }
  log().info << "AttachLocalToSfu as local media_relay hop call_id=" << at.call_id;
  auto attached = relay_deps_.relay->AttachAsLocalHop(at.call_id, MakeHopFrameSink(at));
  if (!attached || !attached->ok) {
    on_done(Error(attached ? attached->error : attached.error().message));
    return;
  }
  bool video_allowed = false;
  if (auto session = sessions_.LoadSession(at.call_id); session && session->has_value()) {
    video_allowed = (*session)->video_allowed;
  }
  on_done(CompleteHopAttach(at, CallMediaAdaptation::QuoteWantUpBps(video_allowed)));
}

void CallHopPath::AttachThroughRelay(HopAttach at, std::function<void(Roe<void>)> on_done) {
  if (at.attach.hop_multiaddr.empty()) {
    at.attach.hop_multiaddr = ranking_.HopMultiaddr(at.attach.hop_peer_id);
  }
  mesh::MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.on_frame = MakeHopFrameSink(at);
  const mesh::MediaRelayAttachRequest request = MakeRelayAttachRequest(at.call_id, at.attach);
  // The commit (StartDuplex, adaptation, seat, planner) is owner state: the relay's answer — success
  // or failure — comes back as an event, never run on its I/O thread.
  const uint64_t id = StoreAttach(std::move(at), std::move(on_done), /*guest=*/false);
  mesh::AttachToMediaRelayAsync(RelayAttachPorts(), request, std::move(hooks), RelayAttachReporter(id));
}

// --- attach (completion, UI) ----------------------------------------------------------------------

Roe<void> CallHopPath::CompleteHopAttach(const HopAttach& at, int64_t a_up_bps) {
  const std::string& call_id = at.call_id;
  channel_->MakeLive(at.keys); // a rotation from now on reaches this attach's frames
  InstallMediaKey(call_id);    // and one that arrived while attaching (the ring holds the epoch it started with)
  // Whoever is on the hop can say so: an invite this device sends (a mid-call add, an answer to a
  // join request — V053) names the hop the call is on, not only the picker's.
  if (!at.attach.hop_peer_id.empty()) {
    notes_.NoteCallHop(call_id, at.attach.hop_peer_id);
  }
  ApplyQuoteAdaptation(a_up_bps);
  channel_->SetLocalStreamId(PublisherStreamIdForLocal());
  reports_.Emit(hop_report::MediaAttempted{call_id});
  if (CallMedia* call_media = host_.call_media ? host_.call_media(call_id) : nullptr) {
    call_media->HoldSeatForHop();
  }
  if (auto wanted = CheckHopAttachStillWanted(at); !wanted) {
    return wanted;
  }
  const bool gen_current = IsMigrateGenerationCurrent(at.gen_at_start);
  if (!gen_current) {
    log().info << "AttachLocalToSfu stale migrate gen want=" << at.gen_at_start
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire) << " call_id=" << call_id
               << " sfu=" << (sfu_.attached ? 1 : 0) << " media=" << engine_->RunningSessionForLog();
  }
  // Dogfood: parallel CallSfuAttach AcceptAndAttach storms re-enter StartDuplex → send-swap /
  // Detach clears RX (quality flips, brief audio, reconnecting flash). Once this call already
  // owns SFU duplex, skip StartDuplex when duplex is live on this hop, or when this worker is stale
  // (newer SoftMigrate/attach owns the generation). Intentional hop switch (current gen,
  // different hop) still StartDuplex send-swap so TX follows the new AcceptAndAttach.
  const bool duplex_live = engine_->Runs();
  const bool same_hop = !flight_.attached_hop_peer_id.empty() && flight_.attached_hop_peer_id == at.attach.hop_peer_id;
  if (duplex_live && (same_hop || !gen_current)) {
    log().info << "AttachLocalToSfu skip StartDuplex (already live) call_id=" << call_id
               << " hop=" << at.attach.hop_peer_id << " attached_hop=" << flight_.attached_hop_peer_id
               << " same_hop=" << (same_hop ? 1 : 0) << " gen_current=" << (gen_current ? 1 : 0);
    // Do not ReleaseDirect / DirectConnected again — duplicate completes flash chrome.
    MarkHopAttachLive(at, /*fresh_start=*/false);
    reports_.Emit(hop_report::Activity{call_id_, {}});
    log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
    return {};
  }
  // Superseded worker: a newer SoftMigrate/attach owns the flight — do not StartDuplex (and do not
  // Detach; that would kill the newer AcceptAndAttach).
  if (!gen_current && !flight_.attaching_hop_peer_id.empty() &&
      flight_.attaching_hop_peer_id != at.attach.hop_peer_id) {
    log().info << "AttachLocalToSfu skip StartDuplex (superseded hop) call_id=" << call_id
               << " want_hop=" << at.attach.hop_peer_id << " in_flight=" << flight_.attaching_hop_peer_id;
    return {};
  }
  // Dogfood 1cee3df4: zombie AcceptAndAttach (gen 85→169 across Leave cycles) still StartDuplex'd
  // onto a fresh 1:1. A stale worker only proceeds while it still owns the flight.
  if (!gen_current && !duplex_live && !OwnsHopAttachFlight(at)) {
    log().info << "AttachLocalToSfu abort StartDuplex (stale gen, no flight ownership) call_id=" << call_id
               << " want=" << at.gen_at_start << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
               << " flight_gen=" << flight_.flight_gen << " attaching=" << flight_.attaching_hop_peer_id;
    return AbortHopAttach();
  }
  if (auto started = StartHopMedia(at); !started) {
    return started;
  }
  MarkHopAttachLive(at, /*fresh_start=*/true);
  ReleaseDirectAfterHopAttach(at);
  log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
  return {};
}

void CallHopPath::ApplyQuoteAdaptation(int64_t a_up_bps) {
  sfu_.last_quote_a_up_bps = a_up_bps;
  // One input for every adaptation (#474): the call's video permission, the mute and the relay's pressure too.
  RefreshAdaptation(call_id_);
}

Roe<void> CallHopPath::AbortHopAttach() {
  relay_deps_.relay->Detach();
  return Error("attach aborted");
}

Roe<void> CallHopPath::CheckHopAttachStillWanted(const HopAttach& at) {
  const auto seat_ports = seat_.Get();
  const auto arming_ports = arming_.Get();
  const std::string& call_id = at.call_id;
  if (seat_ports->IsBound() && !seat_ports->allows_path_op(seat_ports->acquire(call_id))) {
    log().info << "AttachLocalToSfu aborted (seat token rejected) call_id=" << call_id;
    relay_deps_.relay->Detach();
    return Error("media seat token rejected for hop path");
  }
  // V048: hop arming is authority — never StartDuplex when Direct*; cancel gen must still match
  // Deciding/Leave bumps.
  if (arming_ports->IsBound() && arming_ports->hop_ops_allowed && !arming_ports->hop_ops_allowed()) {
    log().info << "AttachLocalToSfu aborted (hop not armed) call_id=" << call_id
               << " arming=" << (arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "?");
    return AbortHopAttach();
  }
  if (arming_ports->IsBound() && arming_ports->media_cancel_gen &&
      arming_ports->media_cancel_gen() != at.cancel_gen_at_start) {
    log().info << "AttachLocalToSfu aborted (media_cancel_gen moved) call_id=" << call_id
               << " want=" << at.cancel_gen_at_start << " have=" << arming_ports->media_cancel_gen();
    return AbortHopAttach();
  }
  // After AcceptAndAttach succeeded, finish StartDuplex whenever this call is still the active
  // topology call. flight_.migrate_generation stampede (duplicate CallSfuAttach / SoftMigrate) must not
  // abort duplex — dogfood: caller Connected, guest stuck "looking for another media path".
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "AttachLocalToSfu aborted before StartDuplex (call inactive) call_id=" << call_id
               << " gen_want=" << at.gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    return AbortHopAttach();
  }
  return {};
}

bool CallHopPath::OwnsHopAttachFlight(const HopAttach& at) const {
  return flight_.flight_gen == at.gen_at_start ||
         (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id == at.attach.hop_peer_id) ||
         // Stampede may Leave-bump gen while guest WaitForAttach is still armed for this call.
         (attach_wait_.waiting && at.call_id == call_id_);
}

MediaEngine::PacketSendFn CallHopPath::MakeHopSendFn(const HopAttach& at) {
  return channel_->MakeSendFn(at.keys);
}

Roe<void> CallHopPath::StartHopMedia(const HopAttach& at) {
  const std::string& call_id = at.call_id;
  if (!at.self_hop) {
    relay_deps_.relay->StartClientFrameReader();
    log().info << "AttachLocalToSfu StartClientFrameReader call_id=" << call_id;
  }
  log().info << "AttachLocalToSfu StartDuplex call_id=" << call_id << " pub_stream=" << channel_->LocalStreamId();
  CallMedia* call_media = host_.call_media ? host_.call_media(call_id) : nullptr;
  if (!call_media) {
    relay_deps_.relay->Detach();
    return Error("no live call for media " + call_id);
  }
  // Takes the seat's hold for this call, starts (or send-swaps) the engine, marks the seat on Hop.
  if (auto started = call_media->StartEngine(CallMediaPath::Hop, MakeHopSendFn(at)); !started) {
    log().info << "AttachLocalToSfu aborted at StartDuplex (" << started.error().message << ") call_id=" << call_id;
    relay_deps_.relay->Detach();
    sfu_.SetAttached(false);
    return started.error();
  }
  if (!IsActiveCallForTopology(call_id)) {
    log().info << "AttachLocalToSfu aborted after StartDuplex (call inactive) call_id=" << call_id
               << " gen_want=" << at.gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    relay_deps_.relay->Detach();
    call_media->StopEngine("hop attach aborted: call inactive");
    sfu_.SetAttached(false);
    return Error("attach aborted");
  }
  return {};
}

void CallHopPath::MarkHopAttachLive(const HopAttach& at, bool fresh_start) {
  const auto seat_ports = seat_.Get();
  const auto arming_ports = arming_.Get();
  const std::string& call_id = at.call_id;
  at.frames_ready->store(true, std::memory_order_release);
  sfu_.SetAttached(true);
  flight_.attached_hop_peer_id = at.attach.hop_peer_id;
  flight_.attaching_hop_peer_id.clear();
  if (!at.self_hop) {
    guest_.active_attach = at.attach;
    guest_.active = true;
    guest_.reattach_attempts = 0;
  } else if (fresh_start) {
    guest_.active_attach.reset();
    guest_.active = false;
  }
  NoteRemotePublisherFromAttach(at.attach);
  SyncSfuSubscriptions(call_id);
  AnnounceLocalPublisher(call_id, at.attach);
  ClearSfuAttachWait();
  RefreshAdaptation(call_id);
  if (seat_ports->IsBound()) {
    seat_ports->end_attach_if_matching(call_id, at.attach.hop_peer_id);
  }
  Apply(CallHopPlannerEvent::AttachSucceeded, call_id); // reports Live
}

void CallHopPath::ReleaseDirectFor(const std::string& call_id) {
  // The call runs on the hop now: its coordinator drops the 1:1 transport (seat-checked).
  if (CallMedia* call_media = host_.call_media ? host_.call_media(call_id) : nullptr) {
    call_media->ReleaseDirect();
  }
}

void CallHopPath::HandleMigrateEvent(HopMigrateEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, hop_migrate_event::RefanOutPickedHop>) {
          RefanOutPickedHop(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::ReleaseDirectAfterAttach>) {
          ReleaseDirectSettled(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::ReleaseDirectWhenHopHeard>) {
          ReleaseDirectWhenHopHeard(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::GuestReattachRetry>) {
          OnGuestReattachRetry(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::Continue>) {
          steps_.Run(e.id);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::RelayAttached>) {
          OnRelayAttached(e);
        } else {
          static_assert(!sizeof(E), "handle every HopMigrateEvent");
        }
      },
      event);
}

uint64_t CallHopPath::StoreAttach(HopAttach at, std::function<void(Roe<void>)> on_done, const bool guest) {
  const uint64_t id = ++next_attach_;
  attaches_.emplace(id, PendingAttach{std::make_shared<HopAttach>(std::move(at)), std::move(on_done), guest});
  return id;
}

std::function<void(mesh::MediaRelayRoe<mesh::MediaRelayAttached>)> CallHopPath::RelayAttachReporter(
    const uint64_t id) const {
  // Edge: the relay answers on its I/O thread — it only reports.
  return [outbox = migrate_outbox_, id](mesh::MediaRelayRoe<mesh::MediaRelayAttached> attached) {
    hop_migrate_event::RelayAttached answer;
    answer.id = id;
    answer.ok = static_cast<bool>(attached);
    if (attached) {
      answer.a_up_bps = attached->a_up_bps;
    } else {
      answer.error = attached.error().message;
    }
    outbox.Emit(std::move(answer));
  };
}

void CallHopPath::OnRelayAttached(const hop_migrate_event::RelayAttached& answer) {
  auto it = attaches_.find(answer.id);
  if (it == attaches_.end()) {
    return;
  }
  PendingAttach pending = std::move(it->second);
  attaches_.erase(it);
  if (!answer.ok) {
    pending.on_done(Error(answer.error));
    return;
  }
  // Calls owner: the commit runs StartDuplex / adaptation and moves seat and planner state (CALLS.md).
  pending.on_done(pending.guest ? CompleteGuestReattach(*pending.at, answer.a_up_bps)
                                : CompleteHopAttach(*pending.at, answer.a_up_bps));
}

void CallHopPath::RefanOutPickedHop(const hop_migrate_event::RefanOutPickedHop& again) {
  // V050: the group may have moved since (a later joiner's re-pick) — re-announcing this hop would
  // send everyone back to it.
  if (!sfu_.attached || !engine_->Runs() || flight_.attached_hop_peer_id != again.hop_peer_id) {
    return;
  }
  log().info << "SoftMigrate re-fan-out CallSfuAttach call_id=" << again.call_id;
  (void)host_.fan_out_joined(again.call_id, CallControlType::CallSfuAttach, again.encoded_attach, "Call SFU attach",
                             again.local_identity);
}

void CallHopPath::ReleaseDirectSettled(const hop_migrate_event::ReleaseDirectAfterAttach& release) {
  if (!sfu_.attached || !engine_->Runs()) {
    return;
  }
  // The guest's hop transport went down since this attach (a reattach is under way, or gave up):
  // the settle of the attach that was lost must not read the call as connected again. The direct
  // transport was released when the attach completed; a reattach that succeeds reports Live.
  if ((guest_.active && release.call_id == call_id_) && (guest_.reattach_in_flight || guest_.reattach_attempts > 0)) {
    log().info << "ReleaseDirect settle skipped (hop transport lost) call_id=" << release.call_id;
    return;
  }
  if (release.self_hop && IsMigrateGenerationCurrent(release.migrate_generation)) {
    if (auto local = host_.local_relay_identity()) {
      if (auto encoded = CallControlCodec::EncodeSfuAttach(release.fanout)) {
        log().info << "AttachLocalToSfu delayed fan-out CallSfuAttach call_id=" << release.call_id;
        (void)host_.fan_out_joined(release.call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach",
                                   *local);
      }
    }
  }
  reports_.Emit(hop_report::Activity{call_id_, {}});
}

void CallHopPath::ReleaseDirectWhenHopHeard(const hop_migrate_event::ReleaseDirectWhenHopHeard& wait) {
  const bool still_on_hop = sfu_.attached && engine_->Runs();
  std::vector<int64_t> heard_ms;
  if (still_on_hop) {
    const MediaEngineHealth health = engine_->HealthSnapshot();
    if (health.packet_mode) {
      for (const MediaStreamHealth& stream : health.streams) {
        heard_ms.push_back(stream.last_rx_ms);
      }
    }
  }
  const int64_t now_ms = util::NowUnixMs();
  if (!ReleaseDirectAfterHopAttachNow(still_on_hop, heard_ms, wait.attached_ms, now_ms, wait.deadline_ms)) {
    if (migrate_outbox_.After(std::chrono::milliseconds(kKeepDirectRecheckMs), wait) != 0) {
      return; // looked at again shortly
    }
  }
  log().info << "ReleaseDirect after hop attach call_id=" << wait.call_id
             << " waited_ms=" << (now_ms - wait.attached_ms) << " heard=" << heard_ms.size();
  ReleaseDirectFor(wait.call_id);
}

void CallHopPath::ReleaseDirectAfterHopAttach(const HopAttach& at) {
  // Clear Connecting immediately, and again after a settle delay. Do not gate on migrate gen —
  // stampede leaves gen_at_start permanently stale (dogfood UI). #431 (b): the 1:1 leg goes once the
  // others are heard on the hop (at most kKeepDirectUntilHopHeardMs): until then its live link is how
  // the peer gets our CallSfuAttach at once, control going direct-first.
  reports_.Emit(hop_report::Activity{call_id_, {}});
  const int64_t attached_ms = util::NowUnixMs();
  ReleaseDirectWhenHopHeard(hop_migrate_event::ReleaseDirectWhenHopHeard{
      .call_id = at.call_id, .attached_ms = attached_ms, .deadline_ms = attached_ms + kKeepDirectUntilHopHeardMs});
  hop_migrate_event::ReleaseDirectAfterAttach release{at.call_id, at.gen_at_start, BuildSfuAttachFanout(at.attach),
                                                      at.self_hop};
  if (migrate_outbox_.After(std::chrono::milliseconds(3500), release) == 0) {
    ReleaseDirectSettled(release); // no timer (unbound): settle now
  }
}

std::optional<std::string> CallHopPath::GuestReattachCallId() const {
  if (!sfu_.attached || flight_.in_flight || guest_.reattach_in_flight) {
    return std::nullopt;
  }
  if (!relay_deps_.relay || relay_deps_.relay->IsLocalHopAttached()) {
    return std::nullopt;
  }
  if (!guest_.active_attach || !guest_.active) {
    return std::nullopt;
  }
  if (!guest_.active || !engine_->Runs()) {
    return std::nullopt;
  }
  return guest_.active ? call_id_ : std::string{};
}

void CallHopPath::OnGuestSfuTransportLost() {
  const std::optional<std::string> call_id = GuestReattachCallId();
  if (!call_id) {
    return;
  }
  if (guest_.reattach_attempts >= kMaxGuestSfuReattachAttempts) {
    ScheduleSlowGuestReattach(*call_id);
    return;
  }
  StartGuestReattachAttempt(*call_id);
}

void CallHopPath::ScheduleSlowGuestReattach(const std::string& call_id) {
  if (guest_.slow_retry_pending) {
    return;
  }
  log().warning << "Guest SFU reattach quick attempts spent=" << guest_.reattach_attempts << ", again every "
                << kGuestSfuSlowRetry.count() << " ms call_id=" << call_id;
  // Prefer guest-path copy over "no hop available" (reattach lost duplex, hop may still exist).
  // The call reads Reconnecting (not Connected: nothing flows) while the guest keeps trying at a
  // steady pace: a phone out of coverage comes back without its network moving, and waiting for
  // a move (OnLocalNetworkMoved) left it out of the call for good (lab: group_call_on_the_road).
  reports_.Emit(hop_report::MediaError{call_id_, Localized("call.error.hop_unreachable_guest")});
  guest_.slow_retry_pending =
      migrate_outbox_.After(kGuestSfuSlowRetry, hop_migrate_event::GuestReattachRetry{.slow = true}) != 0;
}

void CallHopPath::OnGuestReattachRetry(const hop_migrate_event::GuestReattachRetry& retry) {
  if (retry.slow) {
    guest_.slow_retry_pending = false;
  }
  // Another attach brought the transport back meanwhile: nothing to retry.
  if (relay_deps_.relay && relay_deps_.relay->IsAttached()) {
    return;
  }
  if (!retry.slow) {
    OnGuestSfuTransportLost();
    return;
  }
  if (const std::optional<std::string> call_id = GuestReattachCallId()) {
    StartGuestReattachAttempt(*call_id);
  }
}

void CallHopPath::StartGuestReattachAttempt(const std::string& call_id) {
  ++guest_.reattach_attempts;
  guest_.reattach_in_flight = true;
  const CallSfuAttachDetail attach = *guest_.active_attach;
  const uint64_t gen = flight_.migrate_generation.load(std::memory_order_acquire);
  const int attempt = guest_.reattach_attempts;
  log().warning << "Guest SFU duplex lost — reattach attempt=" << attempt << " hop=" << attach.hop_peer_id
                << " call_id=" << call_id;
  reports_.Emit(hop_report::Activity{call_id_, Localized("call.status.reconnecting")});
  if (attempt == 1) {
    // The hop path is down: the call is no longer live (it read Connected while nothing flowed).
    // Recovery() (Reattaching, then HopLost) keeps the Reconnecting line up until a reattach succeeds.
    reports_.Emit(hop_report::Progress{call_id, CallHopPlannerPhase::Attaching});
  }

  // The completion runs on the owner (from the attach's event, or a deferred early failure).
  ReattachGuestSfuTransportAsync(call_id, attach, [this, call_id, gen, attempt](Roe<void> ok) {
    guest_.reattach_in_flight = false;
    if (!IsMigrateGenerationCurrent(gen)) {
      return;
    }
    if (ok) {
      log().info << "Guest SFU reattach ok call_id=" << call_id;
      guest_.reattach_attempts = 0;
      reports_.Emit(hop_report::Activity{call_id_, {}});
      reports_.Emit(hop_report::Progress{call_id, CallHopPlannerPhase::Live});
      return;
    }
    log().warning << "Guest SFU reattach failed attempt=" << attempt << " err=" << ok.error().message
                  << " call_id=" << call_id;
    const int backoff_ms = 400 * std::min(attempt, kMaxGuestSfuReattachAttempts);
    migrate_outbox_.After(std::chrono::milliseconds(backoff_ms), hop_migrate_event::GuestReattachRetry{});
  });
}

void CallHopPath::OnLocalNetworkMoved() {
  if (!guest_.active_attach || !guest_.active) {
    return;
  }
  // Attempts spent while the network was down say nothing about the network now: a fresh budget,
  // and a guest that had given up tries again (a retry still pending picks the budget up).
  const bool gave_up = guest_.reattach_attempts >= kMaxGuestSfuReattachAttempts && !guest_.reattach_in_flight;
  guest_.reattach_attempts = 0;
  if (gave_up) {
    log().info << "Guest SFU reattach again: the local network moved call_id=" << call_id_;
    OnGuestSfuTransportLost();
  }
}

void CallHopPath::ReattachGuestSfuTransportAsync(const std::string& call_id, const CallSfuAttachDetail& attach_in,
                                                 std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  Defer([this, call_id, attach_in, on_done = std::move(on_done)]() mutable {
    StartGuestReattach(call_id, attach_in, std::move(on_done));
  });
}

void CallHopPath::StartGuestReattach(const std::string& call_id, const CallSfuAttachDetail& attach_in,
                                     std::function<void(Roe<void>)> on_done) {
  HopAttach at;
  at.call_id = call_id;
  at.attach = attach_in;
  at.gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
  if (!relay_deps_.relay || !relay_deps_.dial) {
    on_done(Error("media_relay not available"));
    return;
  }
  if (!IsLiveOnHopFor(call_id)) {
    on_done(Error("sfu not active"));
    return;
  }
  if (at.attach.hop_peer_id.empty()) {
    on_done(Error("missing hop_peer_id"));
    return;
  }
  if (auto local_pid = relay_deps_.relay->LocalPeerIdBase58(); local_pid && *local_pid == at.attach.hop_peer_id) {
    on_done(Error("guest reattach is remote-hop only"));
    return;
  }
  if (auto keyed = LoadHopMediaKey(at); !keyed) {
    on_done(keyed.error());
    return;
  }
  // The engine is already live: frames flow as soon as the relay session is back.
  at.frames_ready->store(true, std::memory_order_release);
  channel_->MakeLive(at.keys);
  InstallMediaKey(call_id); // a rotation that arrived while the transport was down
  log().info << "ReattachGuestSfuTransport begin call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
  if (at.attach.hop_multiaddr.empty()) {
    at.attach.hop_multiaddr = ranking_.HopMultiaddr(at.attach.hop_peer_id);
  }
  mesh::MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.still_wanted = [this, gen = at.gen_at_start]() { return IsMigrateGenerationCurrent(gen); };
  hooks.on_frame = MakeHopFrameSink(at);
  const mesh::MediaRelayAttachRequest request = MakeRelayAttachRequest(call_id, at.attach);
  const uint64_t id = StoreAttach(std::move(at), std::move(on_done), /*guest=*/true);
  mesh::AttachToMediaRelayAsync(RelayAttachPorts(), request, std::move(hooks), RelayAttachReporter(id));
}

Roe<void> CallHopPath::CompleteGuestReattach(const HopAttach& at, int64_t a_up_bps) {
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  if (!IsMigrateGenerationCurrent(at.gen_at_start)) {
    relay_deps_.relay->Detach();
    return Error("reattach aborted");
  }
  if (channel_->LocalStreamId() == 0) {
    channel_->SetLocalStreamId(PublisherStreamIdForLocal());
  }
  relay_deps_.relay->StartClientFrameReader();
  sfu_.last_quote_a_up_bps = a_up_bps;
  guest_.active_attach = at.attach;
  guest_.active = true;
  NoteRemotePublisherFromAttach(at.attach);
  SyncSfuSubscriptions(at.call_id);
  AnnounceLocalPublisher(at.call_id, at.attach);
  RefreshAdaptation(at.call_id);
  log().info << "ReattachGuestSfuTransport done call_id=" << at.call_id << " hop=" << at.attach.hop_peer_id;
  return {};
}

} // namespace pbr
