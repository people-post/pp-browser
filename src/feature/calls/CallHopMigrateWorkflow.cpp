#include "feature/calls/CallHopMigrateWorkflow.h"
#include "feature/calls/CallsThread.h"

#include "feature/calls/CallMediaSeat.h"

#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
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
#include <memory>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Next step of a migrate / attach flow, on the calls owner (was MeshControl before thread-ownership t2a). */
void PostOnCallsOwner(std::function<void()> task) {
  if (task) {
    CallsThread::Post(std::move(task));
  }
}

std::vector<MeshHopCandidate> PreferNamedHopFirst(std::vector<MeshHopCandidate> ranked,
                                                  const std::string& hop_peer_id) {
  if (hop_peer_id.empty() || ranked.empty()) {
    return ranked;
  }
  const auto it = std::find_if(ranked.begin(), ranked.end(), [&](const MeshHopCandidate& c) {
    return c.peer_id == hop_peer_id;
  });
  if (it == ranked.end() || it == ranked.begin()) {
    return ranked;
  }
  MeshHopCandidate chosen = std::move(*it);
  ranked.erase(it);
  ranked.insert(ranked.begin(), std::move(chosen));
  return ranked;
}

std::string SoftMigrateNoHopMessage(const std::vector<std::string>& hop_failures) {
  if (hop_failures.empty()) {
    return Tr("call.error.no_media_relay_hop");
  }
  const bool all_payment =
      std::all_of(hop_failures.begin(), hop_failures.end(), [](const std::string& f) {
        return f.find("payment_unavailable") != std::string::npos;
      });
  if (all_payment) {
    return Tr("call.error.payment_unavailable_media");
  }
  std::string msg = Tr("call.error.no_media_relay_hop");
  const bool all_undialable =
      std::all_of(hop_failures.begin(), hop_failures.end(), [](const std::string& f) {
        return f.find("hop not dialable") != std::string::npos;
      });
  if (!all_undialable) {
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

CallHopMigrateWorkflow::~CallHopMigrateWorkflow() {
  timers_self_.Invalidate();
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
  std::vector<MeshHopCandidate> ranked;
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
  PostOnCallsOwner([this, call_id, trigger, prefer_hop_peer_id, expected_gen,
                    on_done = std::move(on_done)]() mutable {
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
  if (!arming_ports->IsBound()) {
    return true;
  }
  size_t n_joined = 0;
  if (auto joined = sessions_.CountJoined(call_id)) {
    n_joined = *joined;
  }
  const bool n_requires_hop = CallMediaTopology::ShouldUseMediaRelay(n_joined);
  const bool ice_or_prefer = trigger == SoftMigrateTrigger::IceRecover || !prefer_hop_peer_id.empty();
  const bool may_arm = arming_ports->soft_migrate_may_arm && arming_ports->soft_migrate_may_arm();
  const bool hop_armed = arming_ports->migrate_ops_allowed && arming_ports->migrate_ops_allowed();
  const char* arming_name = arming_ports->arming_debug_name ? arming_ports->arming_debug_name() : "?";
  if (!n_requires_hop && !ice_or_prefer && may_arm) {
    log().info << "MaybeSoftMigrateToSfuAsync skipped (1:1 stay Direct) call_id=" << call_id
               << " n_joined=" << n_joined << " arming=" << arming_name
               << " trigger=" << static_cast<int>(trigger);
    on_done(Roe<void>());
    return false;
  }
  if (may_arm) {
    ops_.apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
    if (arming_ports->report_progress) {
      arming_ports->report_progress(CallHopPlannerPhase::Migrating, call_id);
    }
    return true;
  }
  if (!hop_armed) {
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

  SoftMigrateAction action = DecideSoftMigrate(decision_in);
  if (action == SoftMigrateAction::WaitForAttach && PreferLocalHopAllowed(pick.call_id, pick.local_identity)) {
    // PreferLocal durable Node hosts media_relay when Link + LAN confirmed (V035).
    // Sticky-initiator WaitForAttach must not block PreferLocal SoftMigrate on LAN.
    log().info << "SoftMigrate PreferLocal Node overrides WaitForAttach → PickHop call_id=" << pick.call_id;
    action = SoftMigrateAction::PickHop;
  } else if (action == SoftMigrateAction::PickHop && !relay_deps_->prefer_local_as_hop &&
             HasDurableMediaRelayHop()) {
    // Phones must not PickHop when a durable media_relay Node is available — quote hits
    // prefer-contacts stranger refuse before PreferLocal session exists.
    log().info << "SoftMigrate defer PickHop to durable media_relay Node call_id=" << pick.call_id;
    action = SoftMigrateAction::WaitForAttach;
  }
  log().info << "SoftMigrate decide action=" << static_cast<int>(action) << " trigger=" << static_cast<int>(trigger)
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
    for (const MeshHopCandidate& hop : ops_.ranked_media_hop_candidates()) {
      if (!hop.peer_id.empty() && relay_deps_->peer_has_media_relay(hop.peer_id)) {
        return true;
      }
    }
  }
  return false;
}

bool CallHopMigrateWorkflow::SettleRepickOnCurrentHop(HopPick& pick, const std::string& prefer_hop_peer_id) {
  const std::string& call_id = pick.call_id;
  // V035 FSM: same hop → re-fan-out only — never Detach/reattach (hop-hint storms).
  const std::string current_hop = !flight_.attached_hop_peer_id.empty() ? flight_.attached_hop_peer_id
                                  : pick.session && pick.session->sfu_hint ? *pick.session->sfu_hint
                                                                           : std::string();
  if (!current_hop.empty() && (prefer_hop_peer_id.empty() || prefer_hop_peer_id == current_hop)) {
    const bool same = prefer_hop_peer_id == current_hop;
    log().info << "SoftMigrate re-pick no-op: " << (same ? "already on prefer" : "keep attached")
               << " hop=" << current_hop << " call_id=" << call_id;
    ops_.fan_out_sfu_attach_for_hop(call_id, current_hop, pick.local_identity);
    ops_.sync_sfu_subscriptions(call_id);
    if (same) {
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
    }
    pick.on_done(Roe<void>());
    return true;
  }
  // Guest hint re-pick: leave PreferLocal only when prefer is a different shared hop.
  std::string local_pid;
  if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
    local_pid = *pid;
  }
  const bool prefer_self = !prefer_hop_peer_id.empty() && !local_pid.empty() && prefer_hop_peer_id == local_pid;
  const bool prefer_other = !prefer_hop_peer_id.empty() && !local_pid.empty() && prefer_hop_peer_id != local_pid;
  if ((prefer_self || relay_deps_->relay->IsLocalHopAttached()) && !prefer_other) {
    log().info << "SoftMigrate re-pick no-op: keep PreferLocal call_id=" << call_id << " prefer=" << prefer_hop_peer_id;
    ops_.sync_sfu_subscriptions(call_id);
    pick.on_done(Error("keep_prefer_local"));
    return true;
  }
  if (prefer_other && relay_deps_->relay->IsLocalHopAttached()) {
    log().info << "SoftMigrate re-pick leave PreferLocal for shared hop=" << prefer_hop_peer_id
               << " call_id=" << call_id;
  }
  const bool prefer_dialable = relay_deps_->dial && !prefer_hop_peer_id.empty() &&
                               (relay_deps_->dial->IsDialable(prefer_hop_peer_id) ||
                                !ops_.resolve_hop_multiaddr(prefer_hop_peer_id).empty());
  if (!prefer_dialable) {
    log().warning << "SoftMigrate re-pick aborted: prefer hop not dialable, keep current SFU hop="
                  << prefer_hop_peer_id;
    ops_.sync_sfu_subscriptions(call_id);
    pick.on_done(Roe<void>());
    return true;
  }
  log().info << "SoftMigrate re-pick detach call_id=" << call_id << " prefer=" << prefer_hop_peer_id;
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

std::vector<MeshHopCandidate> CallHopMigrateWorkflow::RankHopsForSoftMigrate(HopPick& pick,
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
  // V050: the hop planned from the invite list at StartCall goes first — invitees were told about
  // it, so it stays a candidate even if the listing changed since.
  if (pick.session && pick.session->planned_hop && !pick.session->planned_hop->peer_id.empty()) {
    const CallPlannedHop& planned = *pick.session->planned_hop;
    const bool listed = std::any_of(ranked.begin(), ranked.end(),
                                    [&](const MeshHopCandidate& c) { return c.peer_id == planned.peer_id; });
    if (!listed && !planned.multiaddr.empty()) {
      MeshHopCandidate hop;
      hop.peer_id = planned.peer_id;
      hop.multiaddr = planned.multiaddr;
      hop.dialable = true;
      ranked.insert(ranked.begin(), std::move(hop));
    }
    ranked = PreferNamedHopFirst(std::move(ranked), planned.peer_id);
    log().info << "SoftMigrate planned hop first=" << planned.peer_id << " call_id=" << pick.call_id;
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
  const MeshHopCandidate& hop = pick->ranked[index];
  const bool self_hop = !pick->local_peer_id.empty() && hop.peer_id == pick->local_peer_id;
  // Directory/org seeds publish MAs before Amp address-book learn — register then dial.
  if (!self_hop && relay_deps_->dial && !hop.multiaddr.empty() && !relay_deps_->dial->IsDialable(hop.peer_id)) {
    (void)relay_deps_->dial->RegisterEndpoint(hop.peer_id, hop.multiaddr);
  }
  if (!self_hop && relay_deps_->circuit_reach && relay_deps_->dial && !relay_deps_->dial->IsDialable(hop.peer_id)) {
    relay_deps_->circuit_reach->TryEnsureHopReachableAsync(hop.peer_id, [this, pick, index](Roe<void>) {
      PostOnCallsOwner([this, pick, index]() { AttachPickedHop(pick, index); });
    });
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
  const MeshHopCandidate& hop = pick->ranked[index];
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
    PostOnCallsOwner([this, pick, index]() { TryPickHop(pick, index + 1); });
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
  AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(2000), [this, token = timers_self_.token(),
                                                                            snap = timers_self_.Snapshot(), call_id,
                                                                            encoded = *encoded, local_identity,
                                                                            hop = attach.hop_peer_id]() {
    CallsThread::Post([this, token, snap, call_id, encoded, local_identity, hop]() {
      // V050: the group may have moved since (a later joiner's re-pick) — re-announcing this hop
      // would send everyone back to it.
      if (!DeferredSelf::Alive(token, snap) || !sfu_.attached || media_.ActiveCallId() != call_id ||
          flight_.attached_hop_peer_id != hop) {
        return;
      }
      log().info << "SoftMigrate re-fan-out CallSfuAttach call_id=" << call_id;
      (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, encoded, "Call SFU attach",
                                 local_identity);
    });
  });
}

MediaRelayAttachPorts CallHopMigrateWorkflow::RelayAttachPorts() const {
  return relay_deps_ ? relay_deps_->AttachPorts() : MediaRelayAttachPorts{};
}

MediaRelayAttachRequest CallHopMigrateWorkflow::MakeRelayAttachRequest(const std::string& call_id,
                                                                       const CallSfuAttachDetail& attach) const {
  MediaRelayAttachRequest request;
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

std::function<Roe<void>(const MediaRelayQuote&)> CallHopMigrateWorkflow::RelayQuotePricingGate() {
  // Calls only take volunteer / free hops today (pricing project gate).
  return [](const MediaRelayQuote& quote) -> Roe<void> { return InitiationPricing::CheckRelayQuotePayable(quote.rate); };
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
    log().warning << "AttachLocalToSfu without media key store — plaintext SFU (test/incomplete wiring)";
    return {};
  }
  if (auto key = media_keys_->LoadEpochKey(at.call_id, at.media_epoch); key && key->has_value()) {
    at.media_key = **key;
  }
  if (at.media_key.empty()) {
    log().warning << "AttachLocalToSfu missing media key call_id=" << at.call_id << " epoch=" << at.media_epoch;
    return Error("call media key required for SFU");
  }
  return {};
}

std::function<void(MediaDataFrame)> CallHopMigrateWorkflow::MakeHopFrameSink(const HopAttach& at) {
  return [this, ready = at.frames_ready, call_id = at.call_id, media_epoch = at.media_epoch,
          media_key = at.media_key](MediaDataFrame frame) {
    if (!ready->load(std::memory_order_acquire)) {
      return;
    }
    CallMediaEngine::SfuPacket pkt;
    pkt.stream_id = frame.stream_id;
    pkt.channel_id = frame.channel_id;
    pkt.seq = frame.seq;
    pkt.mark = frame.mark;
    if (media_key.empty()) {
      pkt.payload = std::move(frame.payload);
      media_.OnSfuPacket(pkt);
      return;
    }
    auto plain = DecryptCallMediaSfuFrame(media_key, call_id, media_epoch, frame.stream_id,
                                          static_cast<uint8_t>(frame.channel_id), frame.payload);
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
  MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.on_frame = MakeHopFrameSink(at);
  const MediaRelayAttachRequest request = MakeRelayAttachRequest(at.call_id, at.attach);
  AttachToMediaRelayAsync(
      RelayAttachPorts(), request, std::move(hooks),
      [this, at = std::move(at), on_done = std::move(on_done)](Roe<MediaRelayAttached> attached) mutable {
        if (!attached) {
          on_done(attached.error());
          return;
        }
        // Calls owner: completion calls CallMediaEngine::StartSfu / ApplyAdaptation and mutates seat and
        // topology planner state — all owner state (CALLS.md). On MeshControl it raced OnLocalAcceptJoined
        // (TSan: hop planner phase; heap corruption in CallTopologyControllerTest). The attach
        // network work already ran; only the local commit hops.
        CallsThread::Post([this, at = std::move(at), bps = attached->a_up_bps, on_done = std::move(on_done)]() {
          PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
          on_done(CompleteHopAttach(at, bps));
        });
      });
}

// --- attach (completion, UI) ----------------------------------------------------------------------

Roe<void> CallHopMigrateWorkflow::CompleteHopAttach(const HopAttach& at, int64_t a_up_bps) {
  const std::string& call_id = at.call_id;
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
  return [this, pub = publishers_.local_stream_id.load(), call_id = at.call_id, media_epoch = at.media_epoch,
          media_key = at.media_key](const CallMediaEngine::SfuPacket& pkt) {
    if (!relay_deps_ || !relay_deps_->relay) {
      return;
    }
    MediaDataFrame frame;
    frame.stream_id = pub;
    frame.channel_id = pkt.channel_id;
    frame.channel_type = pkt.channel_id == 0 ? MediaChannelType::ReliableOrdered : MediaChannelType::LatestLossy;
    frame.seq = pkt.seq;
    frame.mark = pkt.mark;
    if (!media_key.empty()) {
      auto sealed = EncryptCallMediaSfuFrame(media_key, call_id, media_epoch, pub, pkt.seq, pkt.mark,
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

void CallHopMigrateWorkflow::ReleaseDirectAfterHopAttach(const HopAttach& at) {
  // Advance lifecycle (DirectConnected via ReleaseDirect) + clear Connecting immediately, and again
  // after a settle delay. Do not gate on migrate gen — stampede leaves gen_at_start permanently
  // stale (dogfood UI).
  ReleaseDirectFor(at.call_id);
  host_.ClearMediaActivity();
  auto do_release = [this, token = timers_self_.token(), snap = timers_self_.Snapshot(), call_id = at.call_id,
                     release_gen = at.gen_at_start, release_fanout = BuildSfuAttachFanout(at.attach),
                     self_hop = at.self_hop]() {
    if (!DeferredSelf::Alive(token, snap) || !sfu_.attached || media_.ActiveCallId() != call_id) {
      return;
    }
    if (self_hop && IsMigrateGenerationCurrent(release_gen)) {
      if (auto local = host_.local_relay_identity()) {
        if (auto encoded = CallControlCodec::EncodeSfuAttach(release_fanout)) {
          log().info << "AttachLocalToSfu delayed fan-out CallSfuAttach call_id=" << call_id;
          (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded, "Call SFU attach", *local);
        }
      }
    }
    ReleaseDirectFor(call_id);
    host_.ClearMediaActivity();
  };
  const uint64_t timer = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(3500), [do_release]() { CallsThread::Post(do_release); });
  if (timer == 0) {
    do_release();
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
    host_.SetLastMediaError(Tr("call.error.hop_unreachable_guest"));
    host_.ClearMediaActivity();
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

  ReattachGuestSfuTransportAsync(call_id, attach, [this, call_id, gen, attempt](Roe<void> ok) {
    if (!IsMigrateGenerationCurrent(gen)) {
      CallsThread::Post([this]() { guest_.reattach_in_flight = false; });
      return;
    }
    CallsThread::Post([this, call_id, ok, gen, attempt]() {
      guest_.reattach_in_flight = false;
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      if (ok) {
        log().info << "Guest SFU reattach ok call_id=" << call_id;
        guest_.reattach_attempts = 0;
        host_.ClearMediaActivity();
        return;
      }
      log().warning << "Guest SFU reattach failed attempt=" << attempt
                    << " err=" << ok.error().message << " call_id=" << call_id;
      const int backoff_ms = 400 * attempt;
      (void)AppRuntime::ScheduleCoordinatorOneShot(
          std::chrono::milliseconds(backoff_ms), [this, token = timers_self_.token(), snap = timers_self_.Snapshot()]() {
            CallsThread::Post([this, token, snap]() {
              if (DeferredSelf::Alive(token, snap)) {
                OnGuestSfuTransportLost();
              }
            });
          });
    });
  });
}

void CallHopMigrateWorkflow::ReattachGuestSfuTransportAsync(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach_in,
                                                            std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  PostOnCallsOwner([this, call_id, attach_in, on_done = std::move(on_done)]() mutable {
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
  log().info << "ReattachGuestSfuTransport begin call_id=" << call_id << " hop=" << at.attach.hop_peer_id;
  if (at.attach.hop_multiaddr.empty()) {
    at.attach.hop_multiaddr = ops_.resolve_hop_multiaddr(at.attach.hop_peer_id);
  }
  MediaRelayAttachHooks hooks;
  hooks.accept_quote = RelayQuotePricingGate();
  hooks.still_wanted = [this, gen = at.gen_at_start]() { return IsMigrateGenerationCurrent(gen); };
  hooks.on_frame = MakeHopFrameSink(at);
  const MediaRelayAttachRequest request = MakeRelayAttachRequest(call_id, at.attach);
  AttachToMediaRelayAsync(
      RelayAttachPorts(), request, std::move(hooks),
      [this, at = std::move(at), on_done = std::move(on_done)](Roe<MediaRelayAttached> attached) mutable {
        if (!attached) {
          on_done(attached.error());
          return;
        }
        PostOnCallsOwner([this, at = std::move(at), bps = attached->a_up_bps, on_done = std::move(on_done)]() {
          on_done(CompleteGuestReattach(at, bps));
        });
      });
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
