#include "feature/calls/CallHopMigrateWorkflow.h"

#include "feature/calls/CallMediaSeat.h"

#include "domain/media/CallMediaAdaptation.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/HopHintLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/messaging/SfuAttachFanout.h"
#include "domain/messaging/SfuAttachWaitLogic.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/PlatformUserHints.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/SettledWait.h"
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

using Clock = std::chrono::steady_clock;

void PostControlOrRun(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (MeshControlDispatch::IsInstalled()) {
    MeshControlDispatch::Post(std::move(task));
  } else {
    task();
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

void CallHopMigrateWorkflow::SetHostPorts(CallHopMigrateHostPorts ports) {
  host_ = std::move(ports);
}

void CallHopMigrateWorkflow::SetArmingPorts(CallHopMigrateArmingPorts ports) {
  arming_ = std::move(ports);
}

void CallHopMigrateWorkflow::SetSeatPorts(CallHopMigrateSeatPorts ports) {
  seat_ = std::move(ports);
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

Roe<void> CallHopMigrateWorkflow::MaybeSoftMigrateToSfu(const std::string& call_id,
                                                        SoftMigrateTrigger trigger,
                                                        const std::string& prefer_hop_peer_id,
                                                        uint64_t expected_gen) {
  if (AppRuntime::IsShuttingDown()) {
    log().debug << "MaybeSoftMigrateToSfu rejected: shutting down call_id=" << call_id;
    return Error("shutdown in progress");
  }
  SettledWait<void> wait;
  MaybeSoftMigrateToSfuAsync(call_id, trigger, prefer_hop_peer_id, expected_gen,
                             [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(60000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, {});
  return wait.Wait(std::chrono::milliseconds(1), Error("SoftMigrate timed out"));
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
  // V037/V048: SoftMigrate from Direct* enters Migrating only when N≥3 (or IceRecover / prefer).
  // Relay-cap nudge used expected_gen=0 and promoted 1:1 DirectConnecting → Migrating PreferLocal
  // while the peer stayed on circuit — dogfood "Connecting group media…" vs Connecting.
  if (arming_.IsBound()) {
    size_t n_joined = 0;
    if (auto joined = sessions_.CountJoined(call_id)) {
      n_joined = *joined;
    }
    const bool n_requires_hop = CallMediaTopology::ShouldUseMediaRelay(n_joined);
    const bool ice_or_prefer =
        trigger == SoftMigrateTrigger::IceRecover || !prefer_hop_peer_id.empty();
    const bool may_arm = arming_.soft_migrate_may_arm && arming_.soft_migrate_may_arm();
    const bool hop_armed = arming_.migrate_ops_allowed && arming_.migrate_ops_allowed();
    if (!n_requires_hop && !ice_or_prefer && may_arm) {
      log().info << "MaybeSoftMigrateToSfuAsync skipped (1:1 stay Direct) call_id=" << call_id
                 << " n_joined=" << n_joined
                 << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?")
                 << " trigger=" << static_cast<int>(trigger);
      on_done(Roe<void>());
      return;
    }
    if (may_arm) {
      ops_.apply(CallHopPlannerEvent::SoftMigrateRequested, call_id);
      if (arming_.report_progress) {
        arming_.report_progress(CallHopPlannerPhase::Migrating, call_id);
      }
    } else if (!hop_armed) {
      log().info << "MaybeSoftMigrateToSfuAsync skipped (hop not armed) call_id=" << call_id
                 << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
      on_done(Error("hop path not armed"));
      return;
    }
  }
  PostControlOrRun([this, call_id, trigger, prefer_hop_peer_id, expected_gen,
                    on_done = std::move(on_done)]() mutable {
  if (!IsMigrateGenerationCurrent(expected_gen)) {
    log().info << "SoftMigrate skip stale gen want=" << expected_gen
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
               << " call_id=" << call_id;
    on_done(Roe<void>());
    return;
  }
  const bool repick = !prefer_hop_peer_id.empty();
  if (!repick && sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
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
  std::vector<std::string> joined_ids;
  std::vector<SoftMigrateJoinedPeer> joined_peers;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    joined_ids.push_back(p.identity);
    SoftMigrateJoinedPeer peer;
    peer.identity = p.identity;
    peer.joined_at = p.joined_at;
    joined_peers.push_back(std::move(peer));
  }

  auto session = sessions_.LoadSession(call_id);
  if (session && session->has_value() && IsBroadcastSession((*session)->session_kind)) {
    // Belt-and-suspenders: broadcast audience must never SoftMigrate (B001 / is_broadcast NoOp).
    log().info << "SoftMigrate skip broadcast session call_id=" << call_id
               << " trigger=" << static_cast<int>(trigger);
    on_done(Roe<void>());
    return;
  }
  const bool first_attach =
      !session || !session->has_value() || !(*session)->sfu_hint || (*session)->sfu_hint->empty();

  if (!repick) {
    SoftMigrateDecisionInput decision_in;
    decision_in.local_identity = *local;
    decision_in.joined_identities = joined_ids;
    decision_in.initiator_identity = SelectCallInitiator(joined_peers);
    decision_in.sfu_hint_empty = first_attach;
        decision_in.trigger = trigger;
    decision_in.already_on_sfu = sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
    if (session && session->has_value()) {
      decision_in.is_broadcast = IsBroadcastSession((*session)->session_kind);
    }

    SoftMigrateAction action = DecideSoftMigrate(decision_in);
    // PreferLocal durable Node hosts media_relay when Link + LAN confirmed (V035).
    // Sticky-initiator WaitForAttach must not block PreferLocal SoftMigrate on LAN.
    const CallHopScope scope = ops_.infer_scope_for_call(call_id, *local);
    std::string local_peer_for_scope;
    if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
      local_peer_for_scope = *pid;
    }
    const std::string local_ma_for_scope = ops_.resolve_local_advertise_ma(local_peer_for_scope);
    const bool lan_ok = ops_.lan_reachability_confirmed_for_call(call_id, *local);
    const bool prefer_local_ok =
        PreferLocalAllowedForScope(scope, relay_deps_->prefer_local_as_hop && relay_deps_->relay->IsStarted(),
                                   local_ma_for_scope, lan_ok);
    if (action == SoftMigrateAction::WaitForAttach && prefer_local_ok) {
      log().info << "SoftMigrate PreferLocal Node overrides WaitForAttach → PickHop call_id="
                 << call_id << " scope=" << static_cast<int>(scope) << " lan_ok=" << (lan_ok ? 1 : 0);
      action = SoftMigrateAction::PickHop;
    } else if (action == SoftMigrateAction::PickHop && !relay_deps_->prefer_local_as_hop) {
      // Phones must not PickHop when a durable media_relay Node is available — quote hits
      // prefer-contacts stranger refuse before PreferLocal session exists.
      bool durable_hop = false;
      if (relay_deps_->list_media_relay_peers) {
        for (const std::string& pid : relay_deps_->list_media_relay_peers()) {
          if (!pid.empty()) {
            durable_hop = true;
            break;
          }
        }
      }
      if (!durable_hop && relay_deps_->peer_has_media_relay) {
        for (const MeshHopCandidate& hop : ops_.ranked_media_hop_candidates()) {
          if (!hop.peer_id.empty() && relay_deps_->peer_has_media_relay(hop.peer_id)) {
            durable_hop = true;
            break;
          }
        }
      }
      if (durable_hop) {
        log().info << "SoftMigrate defer PickHop to durable media_relay Node call_id=" << call_id;
        action = SoftMigrateAction::WaitForAttach;
      }
    }
    log().info << "SoftMigrate decide action=" << static_cast<int>(action)
               << " trigger=" << static_cast<int>(trigger) << " joined=" << joined_ids.size()
               << " initiator=" << decision_in.initiator_identity << " local=" << *local
               << " call_id=" << call_id;
    if (action == SoftMigrateAction::NoOp) {
      ops_.sync_sfu_subscriptions(call_id);
      on_done(Roe<void>());
    return;
    }
    if (action == SoftMigrateAction::WaitForAttach) {
      host_.SetMediaActivity(Tr("call.status.waiting_for_media_path"));
      // Owner may have already fan-out CallSfuAttach; do not sit behind TailSync-starved polls.
      host_.RequestInboxSync();
      host_.NotifyRingChanged();
      on_done(Roe<void>());
    return;
    }
  } else if (sfu_.attached) {
    // V035 FSM: same hop → re-fan-out only — never Detach/reattach (hop-hint storms).
    const std::string current_hop =
        !flight_.attached_hop_peer_id.empty()
            ? flight_.attached_hop_peer_id
            : (session && session->has_value() && (*session)->sfu_hint ? *(*session)->sfu_hint : "");
    if (!prefer_hop_peer_id.empty() && !current_hop.empty() && prefer_hop_peer_id == current_hop) {
      log().info << "SoftMigrate re-pick no-op: already on prefer hop=" << prefer_hop_peer_id
                 << " call_id=" << call_id;
      ops_.fan_out_sfu_attach_for_hop(call_id, prefer_hop_peer_id, *local);
      ops_.sync_sfu_subscriptions(call_id);
      host_.ClearMediaActivity();
      host_.NotifyRingChanged();
      on_done(Roe<void>());
      return;
    }
    if (prefer_hop_peer_id.empty() && !current_hop.empty()) {
      log().info << "SoftMigrate re-pick no-op: keep attached hop=" << current_hop
                 << " call_id=" << call_id;
      ops_.fan_out_sfu_attach_for_hop(call_id, current_hop, *local);
      ops_.sync_sfu_subscriptions(call_id);
      on_done(Roe<void>());
      return;
    }
    // Guest hint re-pick: leave PreferLocal only when prefer is a different shared hop.
    std::string local_pid;
    if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
      local_pid = *pid;
    }
    const bool prefer_self =
        !prefer_hop_peer_id.empty() && !local_pid.empty() && prefer_hop_peer_id == local_pid;
    const bool prefer_other =
        !prefer_hop_peer_id.empty() && !local_pid.empty() && prefer_hop_peer_id != local_pid;
    if ((prefer_self || relay_deps_->relay->IsLocalHopAttached()) && !prefer_other) {
      log().info << "SoftMigrate re-pick no-op: keep PreferLocal call_id=" << call_id
                 << " prefer=" << prefer_hop_peer_id;
      ops_.sync_sfu_subscriptions(call_id);
      on_done(Error("keep_prefer_local"));
      return;
    }
    if (prefer_other && relay_deps_->relay->IsLocalHopAttached()) {
      log().info << "SoftMigrate re-pick leave PreferLocal for shared hop=" << prefer_hop_peer_id
                 << " call_id=" << call_id;
    }
    const bool prefer_dialable =
        relay_deps_->dial && !prefer_hop_peer_id.empty() &&
        (relay_deps_->dial->IsDialable(prefer_hop_peer_id) ||
         !ops_.resolve_hop_multiaddr(prefer_hop_peer_id).empty());
    if (!prefer_dialable) {
      log().warning << "SoftMigrate re-pick aborted: prefer hop not dialable, keep current SFU hop="
                    << prefer_hop_peer_id;
      ops_.sync_sfu_subscriptions(call_id);
      on_done(Roe<void>());
      return;
    }
    log().info << "SoftMigrate re-pick detach call_id=" << call_id << " prefer=" << prefer_hop_peer_id;
    host_.SetMediaActivity(Tr("call.status.switching_media_path"));
    host_.NotifyRingChanged();
    relay_deps_->relay->Detach();
    sfu_.attached = false;
    flight_.attached_hop_peer_id.clear();
    flight_.attaching_hop_peer_id.clear();
    if (session && session->has_value()) {
      (*session)->sfu_hint.reset();
      (void)sessions_.UpsertSession(**session);
    }
  }

  auto ranked = ops_.ranked_media_hop_candidates();
  std::string local_peer_id;
  if (auto pid = relay_deps_->relay->LocalPeerIdBase58()) {
    local_peer_id = *pid;
  }
  const std::string local_ma = ops_.resolve_local_advertise_ma(local_peer_id);
  const CallHopScope hop_scope = ops_.infer_scope_for_call(call_id, *local);
  const bool lan_ok = ops_.lan_reachability_confirmed_for_call(call_id, *local);
  const bool prefer_local_flag =
      relay_deps_->prefer_local_as_hop && relay_deps_->relay->IsStarted() && !local_peer_id.empty();
  if (prefer_local_flag && local_ma.empty()) {
    log().warning << "PreferLocal skipped: no advertise multiaddr for local hop";
  }
  ranked = SelectCallMediaHop(std::move(ranked), hop_scope, local_peer_id, prefer_local_flag, local_ma,
                              lan_ok);
  log().info << "SoftMigrate PickHop scope=" << static_cast<int>(hop_scope)
             << " lan_ok=" << (lan_ok ? 1 : 0) << " prefer_local=" << (prefer_local_flag ? 1 : 0)
             << " first=" << (ranked.empty() ? "" : ranked.front().peer_id)
             << " call_id=" << call_id;
  if (!prefer_hop_peer_id.empty()) {
    ranked = PreferNamedHopFirst(std::move(ranked), prefer_hop_peer_id);
  }
  if (ranked.empty()) {
    on_done(Error(Tr("call.error.no_media_relay_hop")));
    return;
  }

  host_.SetMediaActivity(repick ? Tr("call.status.switching_media_path")
                                        : Tr("call.status.finding_media_path"));
  host_.NotifyRingChanged();

  auto hop_failures = std::make_shared<std::vector<std::string>>();
  hop_failures->reserve(ranked.size());
  auto ranked_ptr = std::make_shared<std::vector<MeshHopCandidate>>(std::move(ranked));
  auto try_hop = std::make_shared<std::function<void(size_t)>>();
  *try_hop = [this, call_id, expected_gen, local_peer_id, local, session, ranked_ptr, hop_failures,
              try_hop, on_done](size_t index) mutable {
    if (!IsMigrateGenerationCurrent(expected_gen)) {
      log().info << "SoftMigrate abort mid-pick stale gen want=" << expected_gen
                 << " have=" << flight_.migrate_generation.load(std::memory_order_acquire);
      on_done(Roe<void>());
      return;
    }
    if (index >= ranked_ptr->size()) {
      if (hop_failures->empty()) {
        on_done(Error(Tr("call.error.no_media_relay_hop")));
        return;
      }
      std::string summary = "media_relay SoftMigrate failed (" + std::to_string(hop_failures->size()) +
                            " hops): ";
      for (size_t i = 0; i < hop_failures->size(); ++i) {
        if (i > 0) {
          summary += " | ";
        }
        summary += (*hop_failures)[i];
      }
      log().warning << summary;
      on_done(Error(SoftMigrateNoHopMessage(*hop_failures)));
      return;
    }
    const MeshHopCandidate& hop = (*ranked_ptr)[index];
    const bool self_hop = !local_peer_id.empty() && hop.peer_id == local_peer_id;

    auto continue_hop = [this, call_id, local_peer_id, local, session, ranked_ptr, hop_failures, try_hop,
                         index, on_done, self_hop]() mutable {
    const MeshHopCandidate& hop = (*ranked_ptr)[index];
    // Directory/org seeds publish MAs before Amp address-book learn — register then dial.
    if (!self_hop && relay_deps_->dial && !hop.multiaddr.empty() &&
        !relay_deps_->dial->IsDialable(hop.peer_id)) {
      (void)relay_deps_->dial->RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
    if (!self_hop && (!relay_deps_->dial || !relay_deps_->dial->IsDialable(hop.peer_id))) {
      const std::string detail = "hop not dialable (hop=" + hop.peer_id + ")";
      hop_failures->push_back(detail);
      log().warning << "SoftMigrate skip: " << detail;
      (*try_hop)(index + 1);
      return;
    }
    std::string hop_ma = hop.multiaddr;
    if (hop_ma.empty() && relay_deps_->dial) {
      if (auto ma = relay_deps_->dial->PreferredMultiaddr(hop.peer_id)) {
        hop_ma = *ma;
      }
    }
    log().info << "SoftMigrate try hop=" << hop.peer_id
               << " affinity=" << static_cast<int>(hop.affinity)
               << " ma=" << (hop_ma.empty() ? "(circuit)" : hop_ma);
    // Seat BeginAttach runs inside AttachLocalToSfuAsync (single owner). If another hop is
    // already attaching, skip this candidate so SoftMigrate does not stall on coalesce no-op.
    if (seat_.IsBound() && seat_.has_attach_in_flight() &&
        seat_.attaching_hop() != hop.peer_id) {
      log().info << "SoftMigrate defer hop (seat attach in flight) call_id=" << call_id
                 << " hop=" << hop.peer_id
                 << " in_flight=" << seat_.attaching_hop();
      (*try_hop)(index + 1);
      return;
    }
    flight_.attaching_hop_peer_id = hop.peer_id;
    if (seat_.IsBound()) {
      seat_.note_connecting(call_id);
    }
    host_.SetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.NotifyRingChanged();
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = hop.peer_id;
    attach.hop_multiaddr = hop_ma;
    attach.publisher_stream_id = ops_.publisher_stream_id_for_local();

    auto fanout_attach = [this, call_id, hop, hop_ma, attach, local]() {
      CallSfuAttachDetail fanout = BuildSfuAttachFanout(attach);
      auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
      if (!encoded) {
        return;
      }
      log().info << "SoftMigrate fan-out CallSfuAttach hop=" << hop.peer_id
                 << " ma=" << (hop_ma.empty() ? "(empty)" : hop_ma) << " call_id=" << call_id;
      (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                         "Call SFU attach", *local);
      const std::string encoded_copy = *encoded;
      const std::string local_copy = *local;
      AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(2000), [this, call_id, encoded_copy,
                                                                       local_copy]() {
        if (!sfu_.attached || media_.ActiveCallId() != call_id) {
          return;
        }
        log().info << "SoftMigrate re-fan-out CallSfuAttach call_id=" << call_id;
        (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, encoded_copy,
                                           "Call SFU attach", local_copy);
      });
    };

    if (self_hop) {
      if (session && session->has_value()) {
        (*session)->sfu_hint = hop.peer_id;
        (void)sessions_.UpsertSession(**session);
      }
      fanout_attach();
    }

    AttachLocalToSfuAsync(call_id, attach, [this, call_id, hop, self_hop, session, fanout_attach, hop_failures,
                                            try_hop, index, on_done](Roe<void> attached) mutable {
      if (!attached) {
        std::string detail = attached.error().message;
        if (detail.find(hop.peer_id) == std::string::npos) {
          detail += " (hop=" + hop.peer_id + ")";
        }
        hop_failures->push_back(detail);
        log().warning << "SoftMigrate hop failed: " << detail;
        PostControlOrRun([try_hop, index]() { (*try_hop)(index + 1); });
        return;
      }
      if (session && session->has_value()) {
        (*session)->sfu_hint = hop.peer_id;
        (void)sessions_.UpsertSession(**session);
      }
      if (!self_hop) {
        fanout_attach();
      }
      on_done(Roe<void>());
    });
    };

    if (!self_hop && relay_deps_->dial && !hop.multiaddr.empty() &&
        !relay_deps_->dial->IsDialable(hop.peer_id)) {
      (void)relay_deps_->dial->RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
    if (!self_hop && relay_deps_->circuit_reach && relay_deps_->dial &&
        !relay_deps_->dial->IsDialable(hop.peer_id)) {
      const std::string hop_peer_id = hop.peer_id;
      relay_deps_->circuit_reach->TryEnsureHopReachableAsync(
          hop_peer_id, [continue_hop = std::move(continue_hop)](Roe<void>) mutable {
            PostControlOrRun(std::move(continue_hop));
          });
      return;
    }
    continue_hop();
  };
  (*try_hop)(0);
  });
}

Roe<void> CallHopMigrateWorkflow::CompleteAttachLocalToSfu(
    const std::string& call_id, CallSfuAttachDetail attach, const bool self_hop, const int64_t a_up_bps,
    const uint64_t gen_at_start, const uint64_t cancel_gen_at_start,
    const std::shared_ptr<std::atomic<bool>>& sfu_frames_ready,
    const std::vector<uint8_t>& media_key, const uint32_t media_epoch) {
  sfu_.last_quote_a_up_bps = a_up_bps;
  CallAdaptationInput in;
  in.per_user_up_bps = a_up_bps;
  in.camera_user_wants = media_.IsCameraEnabled();
  in.path_pressure = media_.PathPressure();
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));

  publishers_.local_stream_id = ops_.publisher_stream_id_for_local();

  const uint32_t pub = publishers_.local_stream_id;
  const std::string captured_call = call_id;
  host_.note_media_attempted(call_id);
  host_.bind_media_call_id(call_id);
  CallMediaSeat::Token hop_token;
  if (seat_.IsBound()) {
    hop_token = seat_.acquire(call_id);
    if (!seat_.allows_path_op(hop_token)) {
      log().info << "AttachLocalToSfu aborted (seat token rejected) call_id=" << call_id;
      relay_deps_->relay->Detach();
      return Error("media seat token rejected for hop path");
    }
  }
  // V048: hop must be armed; cancel gen must still match Deciding/Leave bumps.
  if (arming_.IsBound() && arming_.migrate_ops_allowed && !arming_.migrate_ops_allowed()) {
    log().info << "AttachLocalToSfu aborted (hop not armed) call_id=" << call_id
               << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
    relay_deps_->relay->Detach();
    return Error("attach aborted");
  }
  if (arming_.IsBound() && arming_.media_cancel_gen &&
      arming_.media_cancel_gen() != cancel_gen_at_start) {
    log().info << "AttachLocalToSfu aborted (media_cancel_gen moved) call_id=" << call_id
               << " want=" << cancel_gen_at_start << " have=" << arming_.media_cancel_gen();
    relay_deps_->relay->Detach();
    return Error("attach aborted");
  }
  // After AcceptAndAttach succeeded, finish StartSfu whenever this call is still the active
  // topology call. flight_.migrate_generation stampede (duplicate CallSfuAttach / SoftMigrate) must not
  // abort duplex — dogfood: caller Connected, guest stuck "looking for another media path".
  if (!ops_.is_active_call_for_topology(call_id)) {
    log().info << "AttachLocalToSfu aborted before StartSfu (call inactive) call_id=" << call_id
               << " gen_want=" << gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    relay_deps_->relay->Detach();
    return Error("attach aborted");
  }
  const bool gen_current = IsMigrateGenerationCurrent(gen_at_start);
  if (!gen_current) {
    log().info << "AttachLocalToSfu stale migrate gen want=" << gen_at_start
               << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
               << " call_id=" << call_id << " sfu=" << (sfu_.attached ? 1 : 0)
               << " media=" << media_.ActiveCallId();
  }
  // Dogfood: parallel CallSfuAttach AcceptAndAttach storms re-enter StartSfu → send-swap /
  // Detach clears RX (quality flips, brief audio, reconnecting flash). Once this call already
  // owns SFU duplex, skip StartSfu — even when hop differs or migrate gen is stale.
  const bool duplex_live =
      media_.IsSfuMode() && media_.ActiveCallId() == call_id && (sfu_.attached || media_.IsActive());
  const bool same_hop =
      !flight_.attached_hop_peer_id.empty() && flight_.attached_hop_peer_id == attach.hop_peer_id;
  // Skip StartSfu when duplex already live on this hop, or when this worker is stale
  // (newer SoftMigrate/attach owns the generation). Intentional hop switch (current gen,
  // different hop) still StartSfu send-swap so TX follows the new AcceptAndAttach.
  const bool already_live = duplex_live && (same_hop || !gen_current);
  if (already_live) {
    log().info << "AttachLocalToSfu skip StartSfu (already live) call_id=" << call_id
               << " hop=" << attach.hop_peer_id
               << " attached_hop=" << flight_.attached_hop_peer_id << " same_hop=" << (same_hop ? 1 : 0)
               << " gen_current=" << (gen_current ? 1 : 0);
    sfu_frames_ready->store(true, std::memory_order_release);
    flight_.attaching_hop_peer_id.clear();
    sfu_.awaiting_recovery = false;
    if (!self_hop) {
      guest_.active_attach = attach;
      guest_.active_call_id = call_id;
      guest_.reattach_attempts = 0;
    }
    if (!attach.hop_peer_id.empty()) {
      flight_.attached_hop_peer_id = attach.hop_peer_id;
    }
    sfu_.attached = true;
    ops_.note_remote_publisher_from_attach(attach);
    ops_.sync_sfu_subscriptions(call_id);
    ops_.announce_local_publisher(call_id, attach);
    host_.ClearMediaPeerIdentity();
    ops_.clear_sfu_attach_wait();
    ops_.refresh_adaptation(call_id);
    // Do not ReleaseDirect / DirectConnected again — duplicate completes flash chrome.
    host_.ClearMediaActivity();
    // Also NoteLive in already_live branch
  if (seat_.IsBound()) {
      seat_.note_live(call_id);
      seat_.end_attach_if_matching(call_id, attach.hop_peer_id);
    }
    ops_.apply(CallHopPlannerEvent::AttachSucceeded, call_id);
    if (arming_.report_progress) {
      arming_.report_progress(CallHopPlannerPhase::Live, call_id);
    }
    log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << attach.hop_peer_id;
    return {};
  }
  // Superseded worker: a newer SoftMigrate/attach owns the flight — do not StartSfu (and do not
  // Detach; that would kill the newer AcceptAndAttach).
  if (!gen_current && !flight_.attaching_hop_peer_id.empty() &&
      flight_.attaching_hop_peer_id != attach.hop_peer_id) {
    log().info << "AttachLocalToSfu skip StartSfu (superseded hop) call_id=" << call_id
               << " want_hop=" << attach.hop_peer_id << " in_flight=" << flight_.attaching_hop_peer_id;
    return {};
  }
  // Dogfood 1cee3df4: zombie AcceptAndAttach (gen 85→169 across Leave cycles) still StartSfu'd
  // onto a fresh 1:1 — brief media_relay audio then chrome flipped to "direct" / silence.
  // Stampede (duplicate CallSfuAttach) still owns attaching_hop or flight_.flight_gen.
  // V048: hop arming is authority — never StartSfu when Direct* even if migrate gen "owns flight".
  if (arming_.IsBound() && arming_.migrate_ops_allowed && !arming_.migrate_ops_allowed()) {
    log().info << "AttachLocalToSfu abort StartSfu (hop not armed before StartSfu) call_id="
               << call_id
               << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
    relay_deps_->relay->Detach();
    if (seat_.IsBound()) {
      seat_.end_attach_if_matching(call_id, attach.hop_peer_id);
    }
    return Error("attach aborted");
  }
  if (arming_.IsBound() && arming_.media_cancel_gen &&
      arming_.media_cancel_gen() != cancel_gen_at_start) {
    log().info << "AttachLocalToSfu abort StartSfu (media_cancel_gen moved before StartSfu) call_id="
               << call_id << " want=" << cancel_gen_at_start
               << " have=" << arming_.media_cancel_gen();
    relay_deps_->relay->Detach();
    if (seat_.IsBound()) {
      seat_.end_attach_if_matching(call_id, attach.hop_peer_id);
    }
    return Error("attach aborted");
  }
  if (!gen_current && !duplex_live) {
    const bool owns_flight =
        flight_.flight_gen == gen_at_start ||
        (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id == attach.hop_peer_id) ||
        // Stampede may Leave-bump gen while guest WaitForAttach is still armed for this call.
        (attach_wait_.call_id == call_id);
    if (!owns_flight) {
      log().info << "AttachLocalToSfu abort StartSfu (stale gen, no flight ownership) call_id="
                 << call_id << " want=" << gen_at_start
                 << " have=" << flight_.migrate_generation.load(std::memory_order_acquire)
                 << " flight_gen=" << flight_.flight_gen
                 << " attaching=" << flight_.attaching_hop_peer_id;
      relay_deps_->relay->Detach();
      if (seat_.IsBound()) {
        seat_.end_attach_if_matching(call_id, attach.hop_peer_id);
      }
      return Error("attach aborted");
    }
  }
  if (!self_hop) {
    relay_deps_->relay->StartClientFrameReader();
    log().info << "AttachLocalToSfu StartClientFrameReader call_id=" << call_id;
  }
  log().info << "AttachLocalToSfu StartSfu call_id=" << call_id << " pub_stream=" << pub;
  auto started = media_.StartSfu(
      call_id, [this, pub, captured_call, media_epoch, media_key](const CallMediaEngine::SfuPacket& pkt) {
        if (!relay_deps_ || !relay_deps_->relay) {
          return;
        }
        MediaDataFrame frame;
        frame.stream_id = pub;
        frame.channel_id = pkt.channel_id;
        frame.channel_type =
            pkt.channel_id == 0 ? MediaChannelType::ReliableOrdered : MediaChannelType::LatestLossy;
        frame.seq = pkt.seq;
        frame.mark = pkt.mark;
        if (!media_key.empty()) {
          auto sealed = EncryptCallMediaSfuFrame(media_key, captured_call, media_epoch, pub, pkt.seq, pkt.mark,
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
      });
  if (!started) {
    relay_deps_->relay->Detach();
    return started.error();
  }
  if (seat_.IsBound()) {
    seat_.note_start(call_id);
    seat_.note_path(CallMediaSeat::PathKind::Hop);
    // NoteStart bumps epoch — AllowsPathOp (call_id bind) still holds; MatchesToken would not.
    if (!seat_.is_bound(call_id)) {
      log().info << "AttachLocalToSfu aborted after StartSfu (seat unbound) call_id=" << call_id;
      relay_deps_->relay->Detach();
      media_.Stop();
      sfu_.attached = false;
      return Error("attach aborted");
    }
  }
  if (!ops_.is_active_call_for_topology(call_id)) {
    log().info << "AttachLocalToSfu aborted after StartSfu (call inactive) call_id=" << call_id
               << " gen_want=" << gen_at_start
               << " gen_have=" << flight_.migrate_generation.load(std::memory_order_acquire);
    relay_deps_->relay->Detach();
    media_.Stop();
    sfu_.attached = false;
    return Error("attach aborted");
  }

  sfu_frames_ready->store(true, std::memory_order_release);

  sfu_.attached = true;
  flight_.attached_hop_peer_id = attach.hop_peer_id;
  flight_.attaching_hop_peer_id.clear();
  sfu_.awaiting_recovery = false;
  if (!self_hop) {
    guest_.active_attach = attach;
    guest_.active_call_id = call_id;
    guest_.reattach_attempts = 0;
  } else {
    guest_.active_attach.reset();
    guest_.active_call_id.clear();
  }
  ops_.note_remote_publisher_from_attach(attach);
  ops_.sync_sfu_subscriptions(call_id);
  ops_.announce_local_publisher(call_id, attach);
  host_.ClearMediaPeerIdentity();
  ops_.clear_sfu_attach_wait();
  ops_.refresh_adaptation(call_id);
  const uint64_t release_gen = gen_at_start;
  CallSfuAttachDetail release_fanout = BuildSfuAttachFanout(attach);
  // Advance lifecycle (DirectConnected via ReleaseDirect) + clear Connecting immediately.
  // Do not gate on migrate gen — stampede leaves gen_at_start permanently stale (dogfood UI).
  // V036 Phase 2: NoteLive before ReleaseDirect so chrome Connected is not ReleaseDirect alone.
  if (seat_.IsBound()) {
    seat_.note_live(call_id);
    seat_.end_attach_if_matching(call_id, attach.hop_peer_id);
  }
  ops_.apply(CallHopPlannerEvent::AttachSucceeded, call_id);
  if (arming_.report_progress) {
    arming_.report_progress(CallHopPlannerPhase::Live, call_id);
  }
  host_.ReleaseDirectMedia();
  host_.ClearMediaActivity();
  auto do_release = [this, call_id, release_gen, release_fanout, self_hop]() {
    if (!sfu_.attached || media_.ActiveCallId() != call_id) {
      return;
    }
    if (self_hop && IsMigrateGenerationCurrent(release_gen)) {
      if (auto local = host_.local_relay_identity()) {
        if (auto encoded = CallControlCodec::EncodeSfuAttach(release_fanout)) {
          log().info << "AttachLocalToSfu delayed fan-out CallSfuAttach call_id=" << call_id;
          (void)host_.fan_out_joined(call_id, CallControlType::CallSfuAttach, *encoded,
                                             "Call SFU attach", *local);
        }
      }
    }
    host_.ReleaseDirectMedia();
    host_.ClearMediaActivity();
  };
  const uint64_t timer = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(3500), [do_release]() { AppRuntime::PostUI(do_release); });
  if (timer == 0) {
    do_release();
  }
  log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << attach.hop_peer_id;
  return {};
}

void CallHopMigrateWorkflow::AttachLocalToSfuAsync(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach_in,
                                                   std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  const uint64_t gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
  const uint64_t cancel_gen_at_start = arming_.IsBound() && arming_.media_cancel_gen
                                           ? arming_.media_cancel_gen()
                                           : 0;
  if (!relay_deps_ || !relay_deps_->relay || !relay_deps_->dial) {
    on_done(Error("media_relay not available"));
    return;
  }
  CallSfuAttachDetail attach = attach_in;
  if (attach.hop_peer_id.empty()) {
    on_done(Error("missing hop_peer_id"));
    return;
  }

  log().info << "AttachLocalToSfu begin call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " already_sfu=" << (sfu_.attached ? 1 : 0);
  // Same call already owns SFU duplex — never open a parallel AcceptAndAttach (Detach kills RX).
  if (sfu_.attached && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    log().info << "AttachLocalToSfu no-op already duplex call_id=" << call_id
               << " hop=" << attach.hop_peer_id << " attached_hop=" << flight_.attached_hop_peer_id;
    ops_.note_remote_publisher_from_attach(attach);
    ops_.sync_sfu_subscriptions(call_id);
    if (!attach.hop_peer_id.empty()) {
      flight_.attached_hop_peer_id = attach.hop_peer_id;
    }
    host_.ClearMediaActivity();
    on_done(Roe<void>());
    return;
  }
  // SoftMigrate sets attaching_hop / seat BeginAttach before calling us — same hop means we
  // own this attempt. A different in-flight hop must not start a parallel AcceptAndAttach.
  if (seat_.IsBound()) {
    CallMediaSeat::AttachTicket ticket;
    const auto begin = seat_.begin_attach(call_id, attach.hop_peer_id, &ticket);
    if (begin == CallMediaSeat::AttachBeginResult::DeferredOtherHop) {
      log().info << "AttachLocalToSfu coalesce (other hop in flight) call_id=" << call_id
                 << " in_flight_hop=" << seat_.attaching_hop()
                 << " requested=" << attach.hop_peer_id;
      inbound_gate_.pending_attach = attach;
      inbound_gate_.pending_call_id = call_id;
      ops_.note_remote_publisher_from_attach(attach);
      ops_.begin_sfu_attach_wait(call_id);
      on_done(Roe<void>());
      return;
    }
    if (begin == CallMediaSeat::AttachBeginResult::CoalescedSameHop) {
      // SoftMigrate may have set attaching_hop before calling us; only skip when a *foreign*
      // AcceptAndAttach already owns the hop (attaching set by a prior AttachLocalToSfuAsync).
      // First claim for this hop always BeginAttach→Started; Coalesced means parallel entry.
      log().info << "AttachLocalToSfu coalesce (same hop in flight) call_id=" << call_id
                 << " hop=" << attach.hop_peer_id;
      flight_.attaching_hop_peer_id = attach.hop_peer_id;
      ops_.note_remote_publisher_from_attach(attach);
      ops_.begin_sfu_attach_wait(call_id);
      on_done(Roe<void>());
      return;
    }
    if (begin == CallMediaSeat::AttachBeginResult::Rejected) {
      on_done(Error("attach rejected"));
      return;
    }
  } else if (!flight_.attaching_hop_peer_id.empty() && flight_.attaching_hop_peer_id != attach.hop_peer_id) {
    log().info << "AttachLocalToSfu coalesce (other hop in flight) call_id=" << call_id
               << " in_flight_hop=" << flight_.attaching_hop_peer_id << " requested=" << attach.hop_peer_id;
    ops_.note_remote_publisher_from_attach(attach);
    ops_.begin_sfu_attach_wait(call_id);
    on_done(Roe<void>());
    return;
  }
  flight_.attaching_hop_peer_id = attach.hop_peer_id;
  if (seat_.IsBound()) {
    seat_.note_connecting(call_id);
  }
  // Clear seat attach flight on any failure so SoftMigrate / inbound can retry.
  {
    const std::string hop = attach.hop_peer_id;
    auto user_done = std::move(on_done);
    on_done = [this, call_id, hop, user_done = std::move(user_done)](Roe<void> r) mutable {
      if (!r) {
        if (flight_.attaching_hop_peer_id == hop) {
          flight_.attaching_hop_peer_id.clear();
        }
        if (seat_.IsBound()) {
          seat_.end_attach_if_matching(call_id, hop);
        }
      }
      user_done(std::move(r));
    };
  }

  auto sfu_frames_ready = std::make_shared<std::atomic<bool>>(false);
  const std::string captured_call = call_id;
  uint32_t media_epoch = 1;
  ByteVector media_key;
  if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
    media_epoch = session->value().media_epoch;
  }
  if (media_keys_) {
    if (auto key = media_keys_->LoadEpochKey(call_id, media_epoch); key && key->has_value()) {
      media_key = **key;
    }
    if (media_key.empty()) {
      log().warning << "AttachLocalToSfu missing media key call_id=" << call_id
                    << " epoch=" << media_epoch;
      on_done(Error("call media key required for SFU"));
      return;
    }
  } else {
    log().warning << "AttachLocalToSfu without media key store — plaintext SFU (test/incomplete wiring)";
  }
  auto on_sfu_frame = [this, sfu_frames_ready, captured_call, media_epoch,
                       media_key](MediaDataFrame frame) {
    if (!sfu_frames_ready->load(std::memory_order_acquire)) {
      return;
    }
    CallMediaEngine::SfuPacket pkt;
    pkt.stream_id = frame.stream_id;
    pkt.channel_id = frame.channel_id;
    pkt.seq = frame.seq;
    pkt.mark = frame.mark;
    if (!media_key.empty()) {
      auto plain = DecryptCallMediaSfuFrame(media_key, captured_call, media_epoch, frame.stream_id,
                                            static_cast<uint8_t>(frame.channel_id), frame.payload);
      if (!plain) {
        static std::atomic<int> decrypt_fail_log{0};
        const int n = decrypt_fail_log.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n % 100) == 0) {
          log().warning << "SFU decrypt failed stream=" << frame.stream_id << " ch=" << frame.channel_id
                        << " bytes=" << frame.payload.size() << " n=" << n
                        << " err=" << plain.error().message << " call_id=" << captured_call;
        }
        return;
      }
      pkt.payload = std::move(plain->payload);
    } else {
      pkt.payload = std::move(frame.payload);
    }
    media_.OnSfuPacket(pkt);
  };

  const bool self_hop = [&]() {
    if (auto local_pid = relay_deps_->relay->LocalPeerIdBase58()) {
      return *local_pid == attach.hop_peer_id;
    }
    return false;
  }();

  int64_t a_up_bps = CallMediaAdaptation::QuoteWantUpBps(
      [&]() {
        if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
          return (*session)->video_allowed;
        }
        return false;
      }());

  auto finish_complete = [this, call_id, attach, self_hop, gen_at_start, cancel_gen_at_start,
                          sfu_frames_ready, media_key, media_epoch, on_done](int64_t bps) mutable {
    PostControlOrRun([this, call_id, attach = std::move(attach), self_hop, bps, gen_at_start,
                      cancel_gen_at_start, sfu_frames_ready, media_key, media_epoch,
                      on_done = std::move(on_done)]() mutable {
      std::lock_guard<std::mutex> attach_lock(inbound_gate_.mu);
      on_done(CompleteAttachLocalToSfu(call_id, std::move(attach), self_hop, bps, gen_at_start,
                                       cancel_gen_at_start, sfu_frames_ready, media_key, media_epoch));
    });
  };

  if (self_hop) {
    if (!relay_deps_->prefer_local_as_hop || !relay_deps_->relay->IsStarted()) {
      log().warning << "AttachLocalToSfu refused local hop (prefer_local="
                    << (relay_deps_->prefer_local_as_hop ? 1 : 0)
                    << " started=" << (relay_deps_->relay->IsStarted() ? 1 : 0) << ")";
      on_done(Error(Tr("call.error.local_media_relay_unavailable")));
      return;
    }
    log().info << "AttachLocalToSfu as local media_relay hop call_id=" << call_id;
    std::lock_guard<std::mutex> attach_lock(inbound_gate_.mu);
    auto attach_res = relay_deps_->relay->AttachAsLocalHop(call_id, on_sfu_frame);
    if (!attach_res || !attach_res->ok) {
      on_done(Error(attach_res ? attach_res->error : attach_res.error().message));
      return;
    }
    on_done(CompleteAttachLocalToSfu(call_id, std::move(attach), self_hop, a_up_bps, gen_at_start,
                                     cancel_gen_at_start, sfu_frames_ready, media_key, media_epoch));
    return;
  }

  if (attach.hop_multiaddr.empty()) {
    attach.hop_multiaddr = ops_.resolve_hop_multiaddr(attach.hop_peer_id);
  }
  if (!attach.hop_multiaddr.empty()) {
    (void)relay_deps_->dial->RegisterEndpoint(attach.hop_peer_id, attach.hop_multiaddr);
    relay_deps_->dial->ClearDialBackoff(attach.hop_peer_id);
  }

  const std::string hop_for_ensure = attach.hop_peer_id;
  const bool need_circuit =
      !relay_deps_->dial->IsDialable(hop_for_ensure) && relay_deps_->circuit_reach != nullptr;
  auto continue_quote = [this, call_id, attach = std::move(attach), on_sfu_frame = std::move(on_sfu_frame),
                         finish_complete = std::move(finish_complete),
                         on_done]() mutable {
  if (!relay_deps_->dial->IsDialable(attach.hop_peer_id)) {
    on_done(Error("hop not dialable"));
    return;
  }

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  auto joined = sessions_.CountJoined(call_id);
  qreq.participants = joined ? static_cast<int>(*joined) : 2;
  const bool video_allowed = [&]() {
    if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
      return (*session)->video_allowed;
    }
    return false;
  }();
  qreq.want_up_bps = CallMediaAdaptation::QuoteWantUpBps(video_allowed);
  qreq.want_down_bps = qreq.want_up_bps * std::max(1, qreq.participants - 1);

  const std::string hop_peer_id = attach.hop_peer_id;
  relay_deps_->relay->RequestQuoteAsync(
      hop_peer_id, qreq,
      [this, call_id, attach = std::move(attach), hop_peer_id, on_sfu_frame = std::move(on_sfu_frame),
       finish_complete = std::move(finish_complete),
       on_done](Roe<MediaRelayQuote> quote) mutable {
        if (!quote || !quote->ok) {
          on_done(Error(quote ? quote->error : quote.error().message));
          return;
        }
        if (auto payable = InitiationPricing::CheckRelayQuotePayable(quote->rate); !payable) {
          log().info << "AttachLocalToSfu skip paid hop=" << hop_peer_id << " rate=" << quote->rate;
          on_done(payable.error());
          return;
        }
        const int64_t quote_a_up = quote->a_up_bps;
        const std::string quote_id = quote->quote_id;
        relay_deps_->relay->AcceptAndAttachAsync(
            hop_peer_id, quote_id, call_id, call_id, std::move(on_sfu_frame),
            [this, hop_peer_id, quote_id, quote_a_up, finish_complete = std::move(finish_complete),
             on_done](Roe<MediaRelayAttachResult> attach_res) mutable {
              if (!attach_res || !attach_res->ok) {
                on_done(Error(attach_res ? attach_res->error : attach_res.error().message));
                return;
              }
              log().info << "AttachLocalToSfu AcceptAndAttach ok hop=" << hop_peer_id
                         << " quote=" << quote_id;
              finish_complete(quote_a_up);
            },
            8000);
      },
      5000);
  };

  if (need_circuit) {
    relay_deps_->circuit_reach->TryEnsureHopReachableAsync(
        hop_for_ensure, [continue_quote = std::move(continue_quote)](Roe<void>) mutable {
          PostControlOrRun(std::move(continue_quote));
        });
    return;
  }
  continue_quote();
}

Roe<void> CallHopMigrateWorkflow::AttachLocalToSfu(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach_in) {
  SettledWait<void> wait;
  AttachLocalToSfuAsync(call_id, attach_in, [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, {});
  return wait.Wait(std::chrono::milliseconds(1), Error("AttachLocalToSfu timed out"));
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
      AppRuntime::PostUI([this]() { guest_.reattach_in_flight = false; });
      return;
    }
    AppRuntime::PostUI([this, call_id, ok, gen, attempt]() {
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
      (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(backoff_ms), [this]() {
        AppRuntime::PostUI([this]() { OnGuestSfuTransportLost(); });
      });
    });
  });
}

Roe<void> CallHopMigrateWorkflow::ReattachGuestSfuTransport(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach_in) {
  SettledWait<void> wait;
  ReattachGuestSfuTransportAsync(call_id, attach_in,
                                 [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, {});
  return wait.Wait(std::chrono::milliseconds(1), Error("ReattachGuestSfuTransport timed out"));
}

void CallHopMigrateWorkflow::ReattachGuestSfuTransportAsync(const std::string& call_id,
                                                            const CallSfuAttachDetail& attach_in,
                                                            std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  PostControlOrRun([this, call_id, attach_in, on_done = std::move(on_done)]() mutable {
    const uint64_t gen_at_start = flight_.migrate_generation.load(std::memory_order_acquire);
    if (!relay_deps_ || !relay_deps_->relay || !relay_deps_->dial) {
      on_done(Error("media_relay not available"));
      return;
    }
    if (!sfu_.attached || !media_.IsSfuMode() || media_.ActiveCallId() != call_id) {
      on_done(Error("sfu not active"));
      return;
    }
    CallSfuAttachDetail attach = attach_in;
    if (attach.hop_peer_id.empty()) {
      on_done(Error("missing hop_peer_id"));
      return;
    }
    if (auto local_pid = relay_deps_->relay->LocalPeerIdBase58();
        local_pid && *local_pid == attach.hop_peer_id) {
      on_done(Error("guest reattach is remote-hop only"));
      return;
    }

    log().info << "ReattachGuestSfuTransport begin call_id=" << call_id << " hop=" << attach.hop_peer_id;

    const std::string captured_call = call_id;
    uint32_t media_epoch = 1;
    ByteVector media_key;
    if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
      media_epoch = session->value().media_epoch;
    }
    if (media_keys_) {
      if (auto key = media_keys_->LoadEpochKey(call_id, media_epoch); key && key->has_value()) {
        media_key = **key;
      }
      if (media_key.empty()) {
        on_done(Error("call media key required for SFU"));
        return;
      }
    }

    auto on_sfu_frame = [this, captured_call, media_epoch, media_key](MediaDataFrame frame) {
      CallMediaEngine::SfuPacket pkt;
      pkt.stream_id = frame.stream_id;
      pkt.channel_id = frame.channel_id;
      pkt.seq = frame.seq;
      pkt.mark = frame.mark;
      if (!media_key.empty()) {
        auto plain = DecryptCallMediaSfuFrame(media_key, captured_call, media_epoch, frame.stream_id,
                                              static_cast<uint8_t>(frame.channel_id), frame.payload);
        if (!plain) {
          return;
        }
        pkt.payload = std::move(plain->payload);
      } else {
        pkt.payload = std::move(frame.payload);
      }
      media_.OnSfuPacket(pkt);
    };

    if (attach.hop_multiaddr.empty()) {
      attach.hop_multiaddr = ops_.resolve_hop_multiaddr(attach.hop_peer_id);
    }
    if (!attach.hop_multiaddr.empty()) {
      (void)relay_deps_->dial->RegisterEndpoint(attach.hop_peer_id, attach.hop_multiaddr);
      relay_deps_->dial->ClearDialBackoff(attach.hop_peer_id);
    }

    const std::string hop_for_ensure = attach.hop_peer_id;
    const bool need_circuit =
        !relay_deps_->dial->IsDialable(hop_for_ensure) && relay_deps_->circuit_reach != nullptr;
    auto continue_quote = [this, call_id, attach = std::move(attach), gen_at_start,
                           on_sfu_frame = std::move(on_sfu_frame), on_done]() mutable {
    if (!relay_deps_->dial->IsDialable(attach.hop_peer_id)) {
      on_done(Error("hop not dialable"));
      return;
    }

    MediaRelayQuoteRequest qreq;
    qreq.call_id = call_id;
    auto joined = sessions_.CountJoined(call_id);
    qreq.participants = joined ? static_cast<int>(*joined) : 2;
    const bool video_allowed = [&]() {
      if (auto session = sessions_.LoadSession(call_id); session && session->has_value()) {
        return (*session)->video_allowed;
      }
      return false;
    }();
    qreq.want_up_bps = CallMediaAdaptation::QuoteWantUpBps(video_allowed);
    qreq.want_down_bps = qreq.want_up_bps * std::max(1, qreq.participants - 1);

    const std::string hop_peer_id = attach.hop_peer_id;
    relay_deps_->relay->RequestQuoteAsync(
        hop_peer_id, qreq,
        [this, call_id, attach = std::move(attach), hop_peer_id, gen_at_start,
         on_sfu_frame = std::move(on_sfu_frame), on_done](Roe<MediaRelayQuote> quote) mutable {
          if (!quote || !quote->ok) {
            on_done(Error(quote ? quote->error : quote.error().message));
            return;
          }
          if (auto payable = InitiationPricing::CheckRelayQuotePayable(quote->rate); !payable) {
            log().info << "ReattachGuestSfu skip paid hop=" << hop_peer_id << " rate=" << quote->rate;
            on_done(payable.error());
            return;
          }
          if (!IsMigrateGenerationCurrent(gen_at_start)) {
            on_done(Error("reattach aborted"));
            return;
          }
          const int64_t quote_a_up = quote->a_up_bps;
          const std::string quote_id = quote->quote_id;
          relay_deps_->relay->AcceptAndAttachAsync(
              hop_peer_id, quote_id, call_id, call_id, std::move(on_sfu_frame),
              [this, call_id, attach = std::move(attach), gen_at_start, quote_a_up,
               on_done](Roe<MediaRelayAttachResult> attach_res) mutable {
                if (!attach_res || !attach_res->ok) {
                  on_done(Error(attach_res ? attach_res->error : attach_res.error().message));
                  return;
                }
                PostControlOrRun([this, call_id, attach = std::move(attach), gen_at_start, quote_a_up,
                                  on_done = std::move(on_done)]() mutable {
                  std::lock_guard<std::mutex> attach_lock(inbound_gate_.mu);
                  if (!IsMigrateGenerationCurrent(gen_at_start)) {
                    relay_deps_->relay->Detach();
                    on_done(Error("reattach aborted"));
                    return;
                  }
                  if (publishers_.local_stream_id == 0) {
                    publishers_.local_stream_id = ops_.publisher_stream_id_for_local();
                  }
                  relay_deps_->relay->StartClientFrameReader();
                  sfu_.last_quote_a_up_bps = quote_a_up;
                  guest_.active_attach = attach;
                  guest_.active_call_id = call_id;
                  ops_.note_remote_publisher_from_attach(attach);
                  ops_.sync_sfu_subscriptions(call_id);
                  ops_.announce_local_publisher(call_id, attach);
                  ops_.refresh_adaptation(call_id);
                  log().info << "ReattachGuestSfuTransport done call_id=" << call_id
                             << " hop=" << attach.hop_peer_id;
                  on_done(Roe<void>());
                });
              },
              8000);
        },
        5000);
    };

    if (need_circuit) {
      relay_deps_->circuit_reach->TryEnsureHopReachableAsync(
          hop_for_ensure, [continue_quote = std::move(continue_quote)](Roe<void>) mutable {
            PostControlOrRun(std::move(continue_quote));
          });
      return;
    }
    continue_quote();
  });
}


} // namespace pbr
