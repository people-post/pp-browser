#include "feature/calls/CallHopMigrateWorkflow.h"
#include "feature/calls/SfuKeyRing.h"

#include "domain/calls/CallMediaSeat.h"

#include "domain/media/CallMediaAdaptation.h"
#include "domain/calls/CallHopPlan.h"
#include "domain/calls/CallMediaPlannerSelectLogic.h"
#include "domain/calls/CallSessionLogic.h"
#include "domain/calls/CallHopAttachLogic.h"
#include "domain/pricing/InitiationPricing.h"
#include "mesh/media_plane/MediaRelayAttach.h"
#include "domain/calls/SoftMigrateLogic.h"
#include "feature/call_media/CallMediaFrameCrypto.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/PlatformUserHints.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <type_traits>
#include <variant>
#include <memory>
#include <thread>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

std::string SoftMigrateNoHopMessage(const std::vector<std::string>& hop_failures) {
  const HopPickFailure failure = ClassifyHopPickFailures(hop_failures);
  if (failure == HopPickFailure::AllPayment) {
    return Tr("call.error.payment_unavailable_media");
  }
  std::string msg = Tr("call.error.no_media_relay_hop");
  if (failure != HopPickFailure::AllUndialable) {
    return msg;
  }
  const std::string tip = Tr(PlatformUserHints::P2pNetworkHintKey(), {{"product", kProductName}});
  if (!tip.empty()) {
    msg += " ";
    msg += tip;
  }
  return msg;
}

} // namespace

CallHopMigrateWorkflow::CallHopMigrateWorkflow(CallSessionStore& sessions, CallMediaEngine& media)
    : sessions_(sessions), media_(media) {
  redirectLogger("CallHopMigrateWorkflow");
}


void CallHopMigrateWorkflow::SetHostPorts(CallHopMigrateHostPorts ports) {
  host_ = std::move(ports);
}

void CallHopMigrateWorkflow::SetArmingPorts(CallHopMigrateArmingPorts ports) {
  arming_.Set(std::move(ports));
}

void CallHopMigrateWorkflow::SetSeatPorts(CallHopMigrateSeatPorts ports) {
  seat_.Set(std::move(ports));
}

void CallHopMigrateWorkflow::SetTopologyOps(TopologyOps ops) {
  ops_ = std::move(ops);
}

void CallHopMigrateWorkflow::SetMediaRelayDeps(CallTopologyMediaRelayDeps* deps) {
  relay_deps_ = deps;
}

void CallHopMigrateWorkflow::SetMediaKeyStore(CallMediaKeyStore* keys) {
  media_keys_ = keys;
}

bool CallHopMigrateWorkflow::IsMigrateGenerationCurrent(uint64_t gen) const {
  return gen == 0 || gen == flight_.migrate_generation.load(std::memory_order_acquire);
}

/** One SoftMigrate hop pick: walked by TryPickHop / AttachPickedHop, each step holding it by shared_ptr. */
struct CallHopMigrateWorkflow::HopPick {
  std::string call_id;
  uint64_t expected_gen = 0;
  std::string local_identity;
  std::string local_peer_id;
  std::optional<CallSession> session;
  std::vector<mesh::MeshHopCandidate> ranked;
  std::vector<std::string> failures;
  std::function<void(Roe<void>)> on_done;
};

bool CallHopMigrateWorkflow::IsLiveOnHopFor(const std::string& call_id) const {
  return sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
}

void CallHopMigrateWorkflow::MaybeSoftMigrateToSfuAsync(const std::string& call_id,
                                                         SoftMigrateTrigger trigger,
                                                         const std::string& prefer_hop_peer_id,
                                                         uint64_t expected_gen,
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

bool CallHopMigrateWorkflow::PassSoftMigrateArmingGate(const std::string& call_id, SoftMigrateTrigger trigger,
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
    in.migrate_ops_allowed = arming_ports->migrate_ops_allowed && arming_ports->migrate_ops_allowed();
  }
  const char* arming_name =
      in.arming_bound && arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "?";
  switch (DecideSoftMigrateArming(in)) {
  case SoftMigrateArming::Proceed:
    return true;
  case SoftMigrateArming::ArmAndProceed:
    ops_.apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
    if (arming_ports->report_progress) {
      arming_ports->report_progress(CallHopPlannerPhase::Migrating, call_id);
    }
    return true;
  case SoftMigrateArming::StayDirect:
    log().info << "MaybeSoftMigrateToSfuAsync skipped (1:1 stay Direct) call_id=" << call_id
               << " n_joined=" << in.n_joined << " arming=" << arming_name << " trigger=" << static_cast<int>(trigger);
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

void CallHopMigrateWorkflow::RunSoftMigrate(const std::string& call_id, SoftMigrateTrigger trigger,
                                            const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                            std::function<void(Roe<void>)> on_done) {
  if (!IsMigrateGenerationCurrent(expected_gen)) {
    log().info << "SoftMigrate skip stale gen want=" << expected_gen
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
               << " call_id=" << call_id;
    on_done(Roe<void>());
    return;
  }
  const bool repick = !prefer_hop_peer_id.empty();
  if (!repick && IsLiveOnHopFor(call_id)) {
    ops_.sync_sfu_subscriptions(call_id);
    on_done(Roe<void>());
    return;
  }
  if (!relay_deps_ || !relay_deps_->relay || !relay_deps_->dial) {
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
      ops_.sync_sfu_subscriptions(call_id);
      pick->on_done(Roe<void>());
      return;
    }
    if (action == SoftMigrateAction::WaitForAttach) {
      host_.SetMediaActivity(Tr("call.status.waiting_for_media_path"));
      // Owner may have already fan-out CallSfuAttach; do not sit behind TailSync-starved polls.
      host_.RequestInboxSync();
      host_.NotifyRingChanged();
      pick->on_done(Roe<void>());
      return;
    }
  } else if (sfu_.attached && SettleRepickOnCurrentHop(*pick, prefer_hop_peer_id)) {
    return;
  }

  pick->ranked = RankHopsForSoftMigrate(*pick, prefer_hop_peer_id);
  if (pick->ranked.empty()) {
    pick->on_done(Error(Tr("call.error.no_media_relay_hop")));
    return;
  }
  host_.SetMediaActivity(repick ? Tr("call.status.switching_media_path") : Tr("call.status.finding_media_path"));
  host_.NotifyRingChanged();
  pick->failures.reserve(pick->ranked.size());
  TryPickHop(std::move(pick), 0);
}

SoftMigrateAction CallHopMigrateWorkflow::DecideFirstSoftMigrate(const HopPick& pick, SoftMigrateTrigger trigger,
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

  const SoftMigrateAction who_picks = DecideSoftMigrate(decision_in);
  HopHostingFacts hosting;
  hosting.hosts_hops = relay_deps_->prefer_local_as_hop;
  if (who_picks == SoftMigrateAction::WaitForAttach) {
    hosting.prefer_local_allowed = PreferLocalHopAllowed(pick.call_id, pick.local_identity);
  } else if (who_picks == SoftMigrateAction::PickHop && !hosting.hosts_hops) {
    hosting.durable_hop_known = HasDurableMediaRelayHop();
  }
  const SoftMigrateAction action = AdjustForHopHosting(who_picks, hosting);
  log().info << "SoftMigrate decide action=" << static_cast<int>(action) << " who_picks=" << static_cast<int>(who_picks)
             << " trigger=" << static_cast<int>(trigger)
             << " joined=" << decision_in.joined_identities.size()
             << " initiator=" << decision_in.initiator_identity << " local=" << pick.local_identity
             << " call_id=" << pick.call_id;
  return action;
}

bool CallHopMigrateWorkflow::PreferLocalHopAllowed(const std::string& call_id,
                                                   const std::string& local_identity) const {
  const CallHopScope scope = ops_.infer_scope_for_call(call_id, local_identity);
  std::string local_peer_id;
  if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
    local_peer_id = *pid;
  }
  const std::string local_ma = ops_.resolve_local_advertise_ma(local_peer_id);
  const bool lan_ok = ops_.lan_reachability_confirmed_for_call(call_id, local_identity);
  return PreferLocalAllowedForScope(scope, relay_deps_->prefer_local_as_hop && relay_deps_->relay->IsStarted(),
                                    local_ma, lan_ok);
}

bool CallHopMigrateWorkflow::HasDurableMediaRelayHop() const {
  if (relay_deps_->list_media_relay_peers) {
    for (const std::string& pid : relay_deps_->list_media_relay_peers()) {
      if (!pid.empty()) {
        return true;
      }
    }
  }
  if (relay_deps_->peer_has_media_relay) {
    for (const mesh::MeshHopCandidate& hop : ops_.ranked_media_hop_candidates()) {
      if (!hop.peer_id.empty() && relay_deps_->peer_has_media_relay(hop.peer_id)) {
        return true;
      }
    }
  }
  return false;
}

bool CallHopMigrateWorkflow::SettleRepickOnCurrentHop(HopPick& pick, const std::string& prefer_hop_peer_id) {
  const std::string& call_id = pick.call_id;
  RepickOnHopInput in;
  in.current_hop = !flight_.attached_hop_peer_id.empty()            ? flight_.attached_hop_peer_id
                   : pick.session && pick.session->sfu_hint ? *pick.session->sfu_hint
                                                            : std::string();
  in.prefer_hop = prefer_hop_peer_id;
  if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
    in.local_peer_id = *pid;
  }
  in.local_hop_attached = relay_deps_->relay->IsLocalHopAttached();
  in.prefer_dialable = relay_deps_->dial && !prefer_hop_peer_id.empty() &&
                       (relay_deps_->dial->IsDialable(prefer_hop_peer_id) ||
                        !ops_.resolve_hop_multiaddr(prefer_hop_peer_id).empty());
  const RepickOnHopAction action = DecideRepickOnHop(in);
  switch (action) {
  case RepickOnHopAction::StayOnNamedHop:
  case RepickOnHopAction::KeepCurrentHop:
    log().info << "SoftMigrate re-pick no-op: "
               << (action == RepickOnHopAction::StayOnNamedHop ? "already on prefer" : "keep attached")
               << " hop=" << in.current_hop << " call_id=" << call_id;
    ops_.fan_out_sfu_attach_for_hop(call_id, in.current_hop, pick.local_identity);
    ops_.sync_sfu_subscriptions(call_id);
    if (action == RepickOnHopAction::StayOnNamedHop) {
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
    }
    pick.on_done(Roe<void>());
    return true;
  case RepickOnHopAction::KeepPreferLocal:
    log().info << "SoftMigrate re-pick no-op: keep PreferLocal call_id=" << call_id << " prefer=" << prefer_hop_peer_id;
    ops_.sync_sfu_subscriptions(call_id);
    pick.on_done(Error("keep_prefer_local"));
    return true;
  case RepickOnHopAction::KeepNotDialable:
    log().warning << "SoftMigrate re-pick aborted: prefer hop not dialable, keep current SFU hop="
                  << prefer_hop_peer_id;
    ops_.sync_sfu_subscriptions(call_id);
    pick.on_done(Roe<void>());
    return true;
  case RepickOnHopAction::Detach:
    break;
  }
  log().info << "SoftMigrate re-pick detach call_id=" << call_id << " prefer=" << prefer_hop_peer_id
             << " leaving_prefer_local=" << (in.local_hop_attached ? 1 : 0);
  host_.SetMediaActivity(Tr("call.status.switching_media_path"));
  host_.NotifyRingChanged();
  relay_deps_->relay->Detach();
  sfu_.attached = false;
  flight_.attached_hop_peer_id.clear();
  flight_.attaching_hop_peer_id.clear();
  if (pick.session) {
    pick.session->sfu_hint.reset();
    (void)sessions_.UpsertSession(*pick.session);
  }
  return false;
}

std::vector<mesh::MeshHopCandidate> CallHopMigrateWorkflow::RankHopsForSoftMigrate(HopPick& pick,
                                                                             const std::string& prefer_hop_peer_id) {
  if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
    pick.local_peer_id = *pid;
  }
  const std::string local_ma = ops_.resolve_local_advertise_ma(pick.local_peer_id);
  const CallHopScope hop_scope = ops_.infer_scope_for_call(pick.call_id, pick.local_identity);
  const bool lan_ok = ops_.lan_reachability_confirmed_for_call(pick.call_id, pick.local_identity);
  const bool prefer_local_flag =
      relay_deps_->prefer_local_as_hop && relay_deps_->relay->IsStarted() && !pick.local_peer_id.empty();
  if (prefer_local_flag && local_ma.empty()) {
    log().warning << "PreferLocal skipped: no advertise multiaddr for local hop";
  }
  auto ranked = SelectCallMediaHop(ops_.ranked_media_hop_candidates(), hop_scope, pick.local_peer_id,
                                   prefer_local_flag, local_ma, lan_ok);
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

void CallHopMigrateWorkflow::TryPickHop(std::shared_ptr<HopPick> pick, size_t index) {
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
  if (!self_hop && relay_deps_->dial && !hop.multiaddr.empty() && !relay_deps_->dial->IsDialable(hop.peer_id)) {
    (void)relay_deps_->dial->RegisterEndpoint(hop.peer_id, hop.multiaddr);
  }
  if (!self_hop && relay_deps_->circuit_reach && relay_deps_->dial && !relay_deps_->dial->IsDialable(hop.peer_id)) {
    // Edge: the reach answers on another thread — it only reports; the pick goes on as the next event.
    const uint64_t id = steps_.Store([this, pick, index]() { AttachPickedHop(pick, index); });
    relay_deps_->circuit_reach->TryEnsureHopReachableAsync(
        hop.peer_id, [outbox = outbox_, id](mesh::CircuitReachRoe) { outbox.Emit(hop_migrate_event::Continue{id}); });
    return;
  }
  AttachPickedHop(std::move(pick), index);
}

void CallHopMigrateWorkflow::FailHopPick(HopPick& pick) {
  if (pick.failures.empty()) {
    pick.on_done(Error(Tr("call.error.no_media_relay_hop")));
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

void CallHopMigrateWorkflow::AttachPickedHop(std::shared_ptr<HopPick> pick, size_t index) {
  const auto seat_ports = seat_.Get();
  const mesh::MeshHopCandidate& hop = pick->ranked[index];
  const bool self_hop = !pick->local_peer_id.empty() && hop.peer_id == pick->local_peer_id;
  if (!self_hop && (!relay_deps_->dial || !relay_deps_->dial->IsDialable(hop.peer_id))) {
    const std::string detail = "hop not dialable (hop=" + hop.peer_id + ")";
    pick->failures.push_back(detail);
    log().warning << "SoftMigrate skip: " << detail;
    TryPickHop(std::move(pick), index + 1);
    return;
  }
  std::string hop_ma = hop.multiaddr;
  if (hop_ma.empty() && relay_deps_->dial) {
    if (auto ma = relay_deps_->dial->PreferredMultiaddr(hop.peer_id)) {
      hop_ma = *ma;
    }
  }
  log().info << "SoftMigrate try hop=" << hop.peer_id << " affinity=" << static_cast<int>(hop.affinity)
             << " ma=" << (hop_ma.empty() ? "(circuit)" : hop_ma);
  // Seat BeginAttach runs inside AttachLocalToSfuAsync (single owner). If another hop is
  // already attaching, skip this candidate so SoftMigrate does not stall on coalesce no-op.
  if (seat_ports->IsBound() && seat_ports->has_attach_in_flight() && seat_ports->attaching_hop() != hop.peer_id) {
    log().info << "SoftMigrate defer hop (seat attach in flight) call_id=" << pick->call_id
               << " hop=" << hop.peer_id << " in_flight=" << seat_ports->attaching_hop();
    TryPickHop(std::move(pick), index + 1);
    return;
  }
  flight_.attaching_hop_peer_id = hop.peer_id;
  if (seat_ports->IsBound()) {
    seat_ports->note_connecting(pick->call_id);
  }
  host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
  host_.NotifyRingChanged();
  CallSfuAttachDetail attach;
  attach.call_id = pick->call_id;
  attach.hop_peer_id = hop.peer_id;
  attach.hop_multiaddr = hop_ma;
  attach.publisher_stream_id = ops_.publisher_stream_id_for_local();

  if (self_hop) {
    RecordPickedHop(*pick, hop.peer_id);
    FanOutPickedHop(pick->call_id, attach, pick->local_identity);
  }
  const std::string call_id = pick->call_id;
  AttachLocalToSfuAsync(call_id, attach, [this, pick, index, self_hop, attach](Roe<void> attached) {
    OnPickedHopAttached(pick, index, self_hop, attach, std::move(attached));
  });
}

void CallHopMigrateWorkflow::OnPickedHopAttached(const std::shared_ptr<HopPick>& pick, size_t index, bool self_hop,
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

void CallHopMigrateWorkflow::RecordPickedHop(HopPick& pick, const std::string& hop_peer_id) {
  if (pick.session) {
    pick.session->sfu_hint = hop_peer_id;
    (void)sessions_.UpsertSession(*pick.session);
  }
}

void CallHopMigrateWorkflow::FanOutPickedHop(const std::string& call_id, const CallSfuAttachDetail& attach,
                                             const std::string& local_identity) {
  auto encoded = CallControlCodec::EncodeSfuAttach(BuildSfuAttachFanout(attach));
  if (!encoded) {
    return;
  }
  log().info << "SoftMigrate fan-out CallSfuAttach hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr) << " call_id=" << call_id;
  (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", local_identity);
  outbox_.After(std::chrono::milliseconds(2000),
                 hop_migrate_event::RefanOutPickedHop{call_id, *encoded, local_identity, attach.hop_peer_id});
}

mesh::MediaRelayAttachPorts CallHopMigrateWorkflow::RelayAttachPorts() const {
  return relay_deps_ ? relay_deps_->AttachPorts() : mesh::MediaRelayAttachPorts{};
}

mesh::MediaRelayAttachRequest CallHopMigrateWorkflow::MakeRelayAttachRequest(const std::string& call_id,
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

std::function<Roe<void>(const mesh::MediaRelayQuote&)> CallHopMigrateWorkflow::RelayQuotePricingGate() {
  // Calls only take volunteer / free hops today (pricing project gate).
  return [](const mesh::MediaRelayQuote& quote) -> Roe<void> { return InitiationPricing::CheckRelayQuotePayable(quote.rate); };
}

/** One attach of this call to a media_relay hop (remote or our own): what the completion checks against. */
struct CallHopMigrateWorkflow::HopAttach {
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

void CallHopMigrateWorkflow::AttachLocalToSfuAsync(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach_in,
                                                   std::function<void(Roe<void>)> on_done) {
  const auto arming_ports = arming_.Get();
  if (!on_done) {
    return;
  }
  HopAttach at;
  at.call_id = call_id;
  at.attach = attach_in;
  at.gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
  at.cancel_gen_at_start = arming_ports->IsBound() && arming_ports->media_cancel_gen ? arming_ports->media_cancel_gen() : 0;
  if (!relay_deps_ || !relay_deps_->relay || !relay_deps_->dial) {
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
    ops_.note_remote_publisher_from_attach(at.attach);
    ops_.sync_sfu_subscriptions(call_id);
    flight_.attached_hop_peer_id = at.attach.hop_peer_id;
    host_.ClearMediaActivity();
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
  if (auto local_pid = relay_deps_->relay->LocalPeerIdBase58()) {
    at.self_hop = *local_pid == at.attach.hop_peer_id;
  }
  if (at.self_hop) {
    AttachAsLocalHop(std::move(at), std::move(on_done));
  } else {
    AttachThroughRelay(std::move(at), std::move(on_done));
  }
}

Roe<bool> CallHopMigrateWorkflow::ClaimHopAttachFlight(const std::string& call_id, const CallSfuAttachDetail& attach) {
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
        inbound_gate_.pending_call_id = call_id;
        ops_.note_remote_publisher_from_attach(attach);
        ops_.begin_sfu_attach_wait(call_id);
        return false;
      case CallMediaSeat::AttachBeginResult::CoalescedSameHop:
        // SoftMigrate may have set attaching_hop before calling us; only skip when a *foreign*
        // AcceptAndAttach already owns the hop (attaching set by a prior AttachLocalToSfuAsync).
        // First claim for this hop always BeginAttach→Started; Coalesced means parallel entry.
        log().info << "AttachLocalToSfu coalesce (same hop in flight) call_id=" << call_id
                   << " hop=" << attach.hop_peer_id;
        flight_.attaching_hop_peer_id = attach.hop_peer_id;
        ops_.note_remote_publisher_from_attach(attach);
        ops_.begin_sfu_attach_wait(call_id);
        return false;
      case CallMediaSeat::AttachBeginResult::Rejected:
        return Error("attach rejected");
      default:
        break;
    }
  } else if (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id != attach.hop_peer_id) {
    log().info << "AttachLocalToSfu coalesce (other hop in flight) call_id=" << call_id
               << " in_flight_hop=" << flight_.attaching_hop_peer_id << " requested=" << attach.hop_peer_id;
    ops_.note_remote_publisher_from_attach(attach);
    ops_.begin_sfu_attach_wait(call_id);
    return false;
  }
  flight_.attaching_hop_peer_id = attach.hop_peer_id;
  if (seat_ports->IsBound()) {
    seat_ports->note_connecting(call_id);
  }
  return true;
}

std::function<void(Roe<void>)> CallHopMigrateWorkflow::ReleaseHopAttachFlightOnError(
    const std::string& call_id, const std::string& hop, std::function<void(Roe<void>)> on_done) {
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

Roe<void> CallHopMigrateWorkflow::LoadHopMediaKey(HopAttach& at) const {
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

void CallHopMigrateWorkflow::InstallMediaKey(const std::string& call_id) {
  if (!live_keys_ || live_keys_call_id_ != call_id || !media_keys_) {
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return;
  }
  const uint32_t epoch = (*session)->media_epoch;
  if (auto key = media_keys_->LoadEpochKey(call_id, epoch); key && key->has_value()) {
    live_keys_->Install(epoch, **key, util::NowUnixMs());
    log().info << "SFU media key now epoch=" << epoch << " call_id=" << call_id;
  }
}

std::function<void(mesh::MediaDataFrame)> CallHopMigrateWorkflow::MakeHopFrameSink(const HopAttach& at) {
  return [this, ready = at.frames_ready, call_id = at.call_id, keys = at.keys](mesh::MediaDataFrame frame) {
    if (!ready->load(std::memory_order_acquire)) {
      return;
    }
    CallMediaEngine::SfuPacket pkt;
    pkt.stream_id = frame.stream_id;
    pkt.channel_id = frame.channel_id;
    pkt.seq = frame.seq;
    pkt.mark = frame.mark;
    const std::vector<SfuKeyRing::Key> open_with = keys->ForOpen(util::NowUnixMs());
    if (open_with.empty()) {
      pkt.payload = std::move(frame.payload);
      media_.OnSfuPacket(pkt);
      return;
    }
    // The newest epoch first; the previous one while the others switch to it.
    Roe<CallMediaDecodedFrame> plain = Error("no key opened the frame");
    for (const SfuKeyRing::Key& key : open_with) {
      plain = DecryptCallMediaSfuFrame(key.key, call_id, key.epoch, frame.stream_id,
                                       static_cast<uint8_t>(frame.channel_id), frame.payload);
      if (plain) {
        break;
      }
    }
    if (!plain) {
      static std::atomic<int> decrypt_fail_log{0};
      const int n = decrypt_fail_log.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n % 100) == 0) {
        log().warning << "SFU decrypt failed stream=" << frame.stream_id << " ch=" << frame.channel_id
                      << " bytes=" << frame.payload.size() << " n=" << n << " err=" << plain.error().message
                      << " call_id=" << call_id;
      }
      return;
    }
    pkt.payload = std::move(plain->payload);
    media_.OnSfuPacket(pkt);
  };
}

void CallHopMigrateWorkflow::AttachAsLocalHop(HopAttach at, std::function<void(Roe<void>)> on_done) {
  if (!relay_deps_->prefer_local_as_hop || !relay_deps_->relay->IsStarted()) {
    log().warning << "AttachLocalToSfu refused local hop (prefer_local=" << (relay_deps_->prefer_local_as_hop ? 1 : 0)
                  << " started=" << (relay_deps_->relay->IsStarted() ? 1 : 0) << ")";
    on_done(Error(Tr("call.error.local_media_relay_unavailable")));
    return;
  }
  log().info << "AttachLocalToSfu as local media_relay hop call_id=" << at.call_id;
  auto attached = relay_deps_->relay->AttachAsLocalHop(at.call_id, MakeHopFrameSink(at));
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

void CallHopMigrateWorkflow::AttachThroughRelay(HopAttach at, std::function<void(Roe<void>)> on_done) {
  if (at.attach.hop_multiaddr.empty()) {
    at.attach.hop_multiaddr = ops_.resolve_hop_multiaddr(at.attach.hop_peer_id);
  }
  mesh::MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.on_frame = MakeHopFrameSink(at);
  const mesh::MediaRelayAttachRequest request = MakeRelayAttachRequest(at.call_id, at.attach);
  // The commit (StartSfu, adaptation, seat, planner) is owner state: the relay's answer — success
  // or failure — comes back as an event, never run on its I/O thread.
  const uint64_t id = StoreAttach(std::move(at), std::move(on_done), /*guest=*/false);
  mesh::AttachToMediaRelayAsync(RelayAttachPorts(), request, std::move(hooks), RelayAttachReporter(id));
}

// --- attach (completion, UI) ----------------------------------------------------------------------

Roe<void> CallHopMigrateWorkflow::CompleteHopAttach(const HopAttach& at, int64_t a_up_bps) {
  const std::string& call_id = at.call_id;
  live_keys_ = at.keys;  // a rotation from now on reaches this attach's frames
  live_keys_call_id_ = call_id;
  // Whoever is on the hop can say so: an invite this device sends (a mid-call add, an answer to a
  // join request — V053) names the hop the call is on, not only the picker's.
  if (auto session = sessions_.LoadSession(call_id);
      session && session->has_value() && !at.attach.hop_peer_id.empty() &&
      (*session)->sfu_hint.value_or("") != at.attach.hop_peer_id) {
    (*session)->sfu_hint = at.attach.hop_peer_id;
    (void)sessions_.UpsertSession(**session);
  }
  ApplyQuoteAdaptation(a_up_bps);
  publishers_.local_stream_id = ops_.publisher_stream_id_for_local();
  host_.note_media_attempted(call_id);
  if (CallMediaCoordinator* call_media = host_.call_media ? host_.call_media(call_id) : nullptr) {
    call_media->HoldSeatForHop();
  }
  if (auto wanted = CheckHopAttachStillWanted(at); !wanted) {
    return wanted;
  }
  const bool gen_current = IsMigrateGenerationCurrent(at.gen_at_start);
  if (!gen_current) {
    log().info << "AttachLocalToSfu stale migrate gen want=" << at.gen_at_start
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire) << " call_id=" << call_id
               << " sfu=" << (sfu_.attached ? 1 : 0) << " media=" << media_.ActiveCallId();
  }
  // Dogfood: parallel CallSfuAttach AcceptAndAttach storms re-enter StartSfu → send-swap /
  // Detach clears RX (quality flips, brief audio, reconnecting flash). Once this call already
  // owns SFU duplex, skip StartSfu when duplex is live on this hop, or when this worker is stale
  // (newer SoftMigrate/attach owns the generation). Intentional hop switch (current gen,
  // different hop) still StartSfu send-swap so TX follows the new AcceptAndAttach.
  const bool duplex_live =
      media_.IsSfuMode() && media_.ActiveCallId() == call_id && (sfu_.attached || media_.IsActive());
  const bool same_hop =
      !flight_.attached_hop_peer_id.empty() && flight_.attached_hop_peer_id == at.attach.hop_peer_id;
  if (duplex_live && (same_hop || !gen_current)) {
    log().info << "AttachLocalToSfu skip StartSfu (already live) call_id=" << call_id
               << " hop=" << at.attach.hop_peer_id << " attached_hop=" << flight_.attached_hop_peer_id
               << " same_hop=" << (same_hop ? 1 : 0) << " gen_current=" << (gen_current ? 1 : 0);
    // Do not ReleaseDirect / DirectConnected again — duplicate completes flash chrome.
    MarkHopAttachLive(at, /*fresh_start=*/false);
    host_.ClearMediaActivity();
    log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
    return {};
  }
  // Superseded worker: a newer SoftMigrate/attach owns the flight — do not StartSfu (and do not
  // Detach; that would kill the newer AcceptAndAttach).
  if (!gen_current && !flight_.attaching_hop_peer_id.empty() &&
      flight_.attaching_hop_peer_id != at.attach.hop_peer_id) {
    log().info << "AttachLocalToSfu skip StartSfu (superseded hop) call_id=" << call_id
               << " want_hop=" << at.attach.hop_peer_id << " in_flight=" << flight_.attaching_hop_peer_id;
    return {};
  }
  // Dogfood 1cee3df4: zombie AcceptAndAttach (gen 85→169 across Leave cycles) still StartSfu'd
  // onto a fresh 1:1. A stale worker only proceeds while it still owns the flight.
  if (!gen_current && !duplex_live && !OwnsHopAttachFlight(at)) {
    log().info << "AttachLocalToSfu abort StartSfu (stale gen, no flight ownership) call_id=" << call_id
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

void CallHopMigrateWorkflow::ApplyQuoteAdaptation(int64_t a_up_bps) {
  sfu_.last_quote_a_up_bps = a_up_bps;
  CallAdaptationInput in;
  in.per_user_up_bps = a_up_bps;
  in.camera_user_wants = media_.IsCameraEnabled();
  in.path_pressure = media_.PathPressure();
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

Roe<void> CallHopMigrateWorkflow::AbortHopAttach() {
  relay_deps_->relay->Detach();
  return Error("attach aborted");
}

Roe<void> CallHopMigrateWorkflow::CheckHopAttachStillWanted(const HopAttach& at) {
  const auto seat_ports = seat_.Get();
  const auto arming_ports = arming_.Get();
  const std::string& call_id = at.call_id;
  if (seat_ports->IsBound() && !seat_ports->allows_path_op(seat_ports->acquire(call_id))) {
    log().info << "AttachLocalToSfu aborted (seat token rejected) call_id=" << call_id;
    relay_deps_->relay->Detach();
    return Error("media seat token rejected for hop path");
  }
  // V048: hop arming is authority — never StartSfu when Direct*; cancel gen must still match
  // Deciding/Leave bumps.
  if (arming_ports->IsBound() && arming_ports->migrate_ops_allowed && !arming_ports->migrate_ops_allowed()) {
    log().info << "AttachLocalToSfu aborted (hop not armed) call_id=" << call_id
               << " arming=" << (arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "?");
    return AbortHopAttach();
  }
  if (arming_ports->IsBound() && arming_ports->media_cancel_gen && arming_ports->media_cancel_gen() != at.cancel_gen_at_start) {
    log().info << "AttachLocalToSfu aborted (media_cancel_gen moved) call_id=" << call_id
               << " want=" << at.cancel_gen_at_start << " have=" << arming_ports->media_cancel_gen();
    return AbortHopAttach();
  }
  // After AcceptAndAttach succeeded, finish StartSfu whenever this call is still the active
  // topology call. flight_.migrate_generation stampede (duplicate CallSfuAttach / SoftMigrate) must not
  // abort duplex — dogfood: caller Connected, guest stuck "looking for another media path".
  if (!ops_.is_active_call_for_topology(call_id)) {
    log().info << "AttachLocalToSfu aborted before StartSfu (call inactive) call_id=" << call_id
               << " gen_want=" << at.gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    return AbortHopAttach();
  }
  return {};
}

bool CallHopMigrateWorkflow::OwnsHopAttachFlight(const HopAttach& at) const {
  return flight_.flight_gen == at.gen_at_start ||
         (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id == at.attach.hop_peer_id) ||
         // Stampede may Leave-bump gen while guest WaitForAttach is still armed for this call.
         attach_wait_.call_id == at.call_id;
}

CallMediaEngine::SfuSendFn CallHopMigrateWorkflow::MakeHopSendFn(const HopAttach& at) {
  return [this, pub = publishers_.local_stream_id.load(), call_id = at.call_id,
          keys = at.keys](const CallMediaEngine::SfuPacket& pkt) {
    if (!relay_deps_ || !relay_deps_->relay) {
      return;
    }
    mesh::MediaDataFrame frame;
    frame.stream_id = pub;
    frame.channel_id = pkt.channel_id;
    frame.channel_type = mesh::MediaChannelTypeFor(pkt.channel_id);
    frame.seq = pkt.seq;
    frame.mark = pkt.mark;
    const SfuKeyRing::Key key = keys->Current();  // the newest epoch (V003)
    if (!key.key.empty()) {
      auto sealed = EncryptCallMediaSfuFrame(key.key, call_id, key.epoch, pub, pkt.seq, pkt.mark,
                                             static_cast<uint8_t>(pkt.channel_id), pkt.payload);
      if (!sealed) {
        media_.NoteOutboundDrop();
        return;
      }
      frame.payload = std::move(*sealed);
    } else {
      frame.payload = pkt.payload;
    }
    if (!relay_deps_->relay->SendFrame(frame)) {
      media_.NoteOutboundDrop();
    }
  };
}

Roe<void> CallHopMigrateWorkflow::StartHopMedia(const HopAttach& at) {
  const std::string& call_id = at.call_id;
  if (!at.self_hop) {
    relay_deps_->relay->StartClientFrameReader();
    log().info << "AttachLocalToSfu StartClientFrameReader call_id=" << call_id;
  }
  log().info << "AttachLocalToSfu StartSfu call_id=" << call_id << " pub_stream=" << publishers_.local_stream_id.load();
  CallMediaCoordinator* call_media = host_.call_media ? host_.call_media(call_id) : nullptr;
  if (!call_media) {
    relay_deps_->relay->Detach();
    return Error("no live call for media " + call_id);
  }
  // Takes the seat's hold for this call, starts (or send-swaps) the engine, marks the seat on Hop.
  if (auto started = call_media->StartEngine(CallMediaSeat::PathKind::Hop, MakeHopSendFn(at)); !started) {
    log().info << "AttachLocalToSfu aborted at StartSfu (" << started.error().message << ") call_id=" << call_id;
    relay_deps_->relay->Detach();
    sfu_.attached = false;
    return started.error();
  }
  if (!ops_.is_active_call_for_topology(call_id)) {
    log().info << "AttachLocalToSfu aborted after StartSfu (call inactive) call_id=" << call_id
               << " gen_want=" << at.gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    relay_deps_->relay->Detach();
    call_media->StopEngine("hop attach aborted: call inactive");
    sfu_.attached = false;
    return Error("attach aborted");
  }
  return {};
}

void CallHopMigrateWorkflow::MarkHopAttachLive(const HopAttach& at, bool fresh_start) {
  const auto seat_ports = seat_.Get();
  const auto arming_ports = arming_.Get();
  const std::string& call_id = at.call_id;
  at.frames_ready->store(true, std::memory_order_release);
  sfu_.attached = true;
  flight_.attached_hop_peer_id = at.attach.hop_peer_id;
  flight_.attaching_hop_peer_id.clear();
  if (!at.self_hop) {
    guest_.active_attach = at.attach;
    guest_.active_call_id = call_id;
    guest_.reattach_attempts = 0;
  } else if (fresh_start) {
    guest_.active_attach.reset();
    guest_.active_call_id.clear();
  }
  ops_.note_remote_publisher_from_attach(at.attach);
  ops_.sync_sfu_subscriptions(call_id);
  ops_.announce_local_publisher(call_id, at.attach);
  host_.ClearMediaPeerIdentity();
  ops_.clear_sfu_attach_wait();
  ops_.refresh_adaptation(call_id);
  // V036 Phase 2: NoteLive before ReleaseDirect so chrome Connected is not ReleaseDirect alone.
  if (seat_ports->IsBound()) {
    seat_ports->note_live(call_id);
    seat_ports->end_attach_if_matching(call_id, at.attach.hop_peer_id);
  }
  ops_.apply(CallHopPlannerEvent::AttachSucceeded, call_id);
  if (arming_ports->report_progress) {
    arming_ports->report_progress(CallHopPlannerPhase::Live, call_id);
  }
}

void CallHopMigrateWorkflow::ReleaseDirectFor(const std::string& call_id) {
  // The call runs on the hop now: its coordinator drops the 1:1 transport (seat-checked).
  if (CallMediaCoordinator* call_media = host_.call_media ? host_.call_media(call_id) : nullptr) {
    call_media->ReleaseDirect();
  }
}

void CallHopMigrateWorkflow::Handle(HopMigrateEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, hop_migrate_event::RefanOutPickedHop>) {
          RefanOutPickedHop(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::ReleaseDirectAfterAttach>) {
          ReleaseDirectSettled(e);
        } else if constexpr (std::is_same_v<E, hop_migrate_event::GuestReattachRetry>) {
          OnGuestSfuTransportLost();
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

void CallHopMigrateWorkflow::Defer(std::function<void()> step) {
  outbox_.Emit(hop_migrate_event::Continue{steps_.Store(std::move(step))});
}

uint64_t CallHopMigrateWorkflow::StoreAttach(HopAttach at, std::function<void(Roe<void>)> on_done, const bool guest) {
  const uint64_t id = ++next_attach_;
  attaches_.emplace(id, PendingAttach{std::make_shared<HopAttach>(std::move(at)), std::move(on_done), guest});
  return id;
}

std::function<void(mesh::MediaRelayRoe<mesh::MediaRelayAttached>)> CallHopMigrateWorkflow::RelayAttachReporter(const uint64_t id) const {
  // Edge: the relay answers on its I/O thread — it only reports.
  return [outbox = outbox_, id](mesh::MediaRelayRoe<mesh::MediaRelayAttached> attached) {
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

void CallHopMigrateWorkflow::OnRelayAttached(const hop_migrate_event::RelayAttached& answer) {
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
  // Calls owner: the commit runs StartSfu / adaptation and moves seat and planner state (CALLS.md).
  pending.on_done(pending.guest ? CompleteGuestReattach(*pending.at, answer.a_up_bps)
                                : CompleteHopAttach(*pending.at, answer.a_up_bps));
}

void CallHopMigrateWorkflow::RefanOutPickedHop(const hop_migrate_event::RefanOutPickedHop& again) {
  // V050: the group may have moved since (a later joiner's re-pick) — re-announcing this hop would
  // send everyone back to it.
  if (!sfu_.attached || media_.ActiveCallId() != again.call_id || flight_.attached_hop_peer_id != again.hop_peer_id) {
    return;
  }
  log().info << "SoftMigrate re-fan-out CallSfuAttach call_id=" << again.call_id;
  (void)host_.fan_out_joined(again.call_id, CallControlType::CallSfuAttach, again.encoded_attach, "Call SFU attach",
                             again.local_identity);
}

void CallHopMigrateWorkflow::ReleaseDirectSettled(const hop_migrate_event::ReleaseDirectAfterAttach& release) {
  if (!sfu_.attached || media_.ActiveCallId() != release.call_id) {
    return;
  }
  // The guest's hop transport went down since this attach (a reattach is under way, or gave up):
  // the settle of the attach that was lost must not read the call as connected again. The direct
  // transport was released when the attach completed; a reattach that succeeds reports Live.
  if (guest_.active_call_id == release.call_id && (guest_.reattach_in_flight || guest_.reattach_attempts > 0)) {
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
  ReleaseDirectFor(release.call_id);
  host_.ClearMediaActivity();
}

void CallHopMigrateWorkflow::ReleaseDirectAfterHopAttach(const HopAttach& at) {
  // Advance lifecycle (DirectConnected via ReleaseDirect) + clear Connecting immediately, and again
  // after a settle delay. Do not gate on migrate gen — stampede leaves gen_at_start permanently
  // stale (dogfood UI).
  ReleaseDirectFor(at.call_id);
  host_.ClearMediaActivity();
  hop_migrate_event::ReleaseDirectAfterAttach release{at.call_id, at.gen_at_start, BuildSfuAttachFanout(at.attach),
                                                     at.self_hop};
  if (outbox_.After(std::chrono::milliseconds(3500), release) == 0) {
    ReleaseDirectSettled(release);  // no timer (unbound): settle now
  }
}

void CallHopMigrateWorkflow::OnGuestSfuTransportLost() {
  if (!sfu_.attached || flight_.in_flight || guest_.reattach_in_flight) {
    return;
  }
  if (!relay_deps_->relay || relay_deps_->relay->IsLocalHopAttached()) {
    return;
  }
  if (!guest_.active_attach || guest_.active_call_id.empty()) {
    return;
  }
  const std::string call_id = guest_.active_call_id;
  if (!media_.IsSfuMode() || media_.ActiveCallId() != call_id) {
    return;
  }
  if (guest_.reattach_attempts >= kMaxGuestSfuReattachAttempts) {
    log().warning << "Guest SFU reattach exhausted attempts=" << guest_.reattach_attempts
                  << " call_id=" << call_id;
    // Prefer guest-path copy over "no hop available" (reattach lost duplex, hop may still exist).
    // The call still reads Reconnecting (not Connected: nothing flows) until the device's network
    // comes back or moves (OnLocalNetworkMoved tries again) or the user leaves.
    host_.SetLastMediaError(Tr("call.error.hop_unreachable_guest"));
    return;
  }
  ++guest_.reattach_attempts;
  guest_.reattach_in_flight = true;
  const CallSfuAttachDetail attach = *guest_.active_attach;
  const uint64_t gen = flight_.migrate_generation.load(std::memory_order_acquire);
  const int attempt = guest_.reattach_attempts;
  log().warning << "Guest SFU duplex lost — reattach attempt=" << attempt
                << " hop=" << attach.hop_peer_id << " call_id=" << call_id;
  host_.SetMediaActivity(Tr("call.status.reconnecting"));
  if (attempt == 1) {
    // The hop path is down: the call is no longer live (it read Connected while nothing flowed).
    // The seat stays Live by design (a reattach is not a demotion); IsGuestHopRecovering keeps
    // the Reconnecting line up until a reattach succeeds.
    const auto arming_ports = arming_.Get();
    if (arming_ports->report_progress) {
      arming_ports->report_progress(CallHopPlannerPhase::Attaching, call_id);
    }
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
      host_.ClearMediaActivity();
      const auto arming_ports = arming_.Get();
      if (arming_ports->report_progress) {
        arming_ports->report_progress(CallHopPlannerPhase::Live, call_id);
      }
      return;
    }
    log().warning << "Guest SFU reattach failed attempt=" << attempt << " err=" << ok.error().message
                  << " call_id=" << call_id;
    const int backoff_ms = 400 * attempt;
    outbox_.After(std::chrono::milliseconds(backoff_ms), hop_migrate_event::GuestReattachRetry{});
  });
}

void CallHopMigrateWorkflow::OnLocalNetworkMoved() {
  if (!guest_.active_attach || guest_.active_call_id.empty()) {
    return;
  }
  // Attempts spent while the network was down say nothing about the network now: a fresh budget,
  // and a guest that had given up tries again (a retry still pending picks the budget up).
  const bool gave_up = guest_.reattach_attempts >= kMaxGuestSfuReattachAttempts && !guest_.reattach_in_flight;
  guest_.reattach_attempts = 0;
  if (gave_up) {
    log().info << "Guest SFU reattach again: the local network moved call_id=" << guest_.active_call_id;
    OnGuestSfuTransportLost();
  }
}

void CallHopMigrateWorkflow::ReattachGuestSfuTransportAsync(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach_in,
                                                            std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  Defer([this, call_id, attach_in, on_done = std::move(on_done)]() mutable {
    StartGuestReattach(call_id, attach_in, std::move(on_done));
  });
}

void CallHopMigrateWorkflow::StartGuestReattach(const std::string& call_id, const CallSfuAttachDetail& attach_in,
                                                std::function<void(Roe<void>)> on_done) {
  HopAttach at;
  at.call_id = call_id;
  at.attach = attach_in;
  at.gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
  if (!relay_deps_ || !relay_deps_->relay || !relay_deps_->dial) {
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
  if (auto local_pid = relay_deps_->relay->LocalPeerIdBase58(); local_pid && *local_pid == at.attach.hop_peer_id) {
    on_done(Error("guest reattach is remote-hop only"));
    return;
  }
  if (auto keyed = LoadHopMediaKey(at); !keyed) {
    on_done(keyed.error());
    return;
  }
  // The engine is already live: frames flow as soon as the relay session is back.
  at.frames_ready->store(true, std::memory_order_release);
  live_keys_ = at.keys;
  live_keys_call_id_ = call_id;
  log().info << "ReattachGuestSfuTransport begin call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
  if (at.attach.hop_multiaddr.empty()) {
    at.attach.hop_multiaddr = ops_.resolve_hop_multiaddr(at.attach.hop_peer_id);
  }
  mesh::MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.still_wanted = [this, gen = at.gen_at_start]() { return IsMigrateGenerationCurrent(gen); };
  hooks.on_frame = MakeHopFrameSink(at);
  const mesh::MediaRelayAttachRequest request = MakeRelayAttachRequest(call_id, at.attach);
  const uint64_t id = StoreAttach(std::move(at), std::move(on_done), /*guest=*/true);
  mesh::AttachToMediaRelayAsync(RelayAttachPorts(), request, std::move(hooks), RelayAttachReporter(id));
}

Roe<void> CallHopMigrateWorkflow::CompleteGuestReattach(const HopAttach& at, int64_t a_up_bps) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
  if (!IsMigrateGenerationCurrent(at.gen_at_start)) {
    relay_deps_->relay->Detach();
    return Error("reattach aborted");
  }
  if (publishers_.local_stream_id == 0) {
    publishers_.local_stream_id = ops_.publisher_stream_id_for_local();
  }
  relay_deps_->relay->StartClientFrameReader();
  sfu_.last_quote_a_up_bps = a_up_bps;
  guest_.active_attach = at.attach;
  guest_.active_call_id = at.call_id;
  ops_.note_remote_publisher_from_attach(at.attach);
  ops_.sync_sfu_subscriptions(at.call_id);
  ops_.announce_local_publisher(at.call_id, at.attach);
  ops_.refresh_adaptation(at.call_id);
  log().info << "ReattachGuestSfuTransport done call_id=" << at.call_id << " hop=" << at.attach.hop_peer_id;
  return {};
}


} // namespace pbr
