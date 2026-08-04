#include "feature/messaging/CallTopologyController.h"

#include "base/media/CallMediaAdaptation.h"
#include "base/messaging/HopHintLogic.h"
#include "base/messaging/SfuAttachFanout.h"
#include "base/messaging/SfuAttachWaitLogic.h"
#include "base/i18n/LocalizationService.h"
#include "base/people/ContactTypes.h"
#include "base/people/MeshHopPolicy.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace pbr {
namespace {

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

} // namespace

CallTopologyController::CallTopologyController(CallTopologyHost& host, CallSessionStore& sessions,
                                               ContactsStore& contacts, CallMediaEngine& media)
    : host_(host), sessions_(sessions), contacts_(contacts), media_(media) {
  redirectLogger("CallTopologyController");
}

void CallTopologyController::SetMediaRelayDeps(MediaRelayDeps deps) {
  relay_deps_ = std::move(deps);
}

bool CallTopologyController::IsAwaitingSfuRecovery() const {
  return awaiting_sfu_recovery_ || soft_migrate_in_flight_ || !sfu_attach_wait_call_id_.empty();
}

bool CallTopologyController::IsSfuAttached() const {
  return sfu_attached_;
}

bool CallTopologyController::IsOnSfuForCall(const std::string& call_id) const {
  return sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
}

bool CallTopologyController::IsSoftMigrateInFlight() const {
  return soft_migrate_in_flight_;
}

bool CallTopologyController::IsSfuAttachWaitActive() const {
  return !sfu_attach_wait_call_id_.empty();
}

std::vector<MeshHopCandidate> CallTopologyController::RankedMediaHopCandidates() const {
  std::vector<Contact> contacts;
  if (auto listed = contacts_.List()) {
    contacts = std::move(*listed);
  }
  auto contact_hops = CollectContactHopCandidates(contacts);
  auto seed_hops = CollectSeedHopCandidates(relay_deps_.bootstrap_peers);
  auto merged = OrderCircuitHops(std::move(contact_hops), std::move(seed_hops), relay_deps_.prefer_contacts);
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
  sfu_attach_wait_call_id_ = call_id;
  sfu_attach_wait_deadline_ms_ = util::NowUnixMs() + kSfuAttachWaitDefaultMs;
}

void CallTopologyController::ClearSfuAttachWait() {
  sfu_attach_wait_call_id_.clear();
  sfu_attach_wait_deadline_ms_ = 0;
}

void CallTopologyController::ClearAwaitingSfuRecovery() {
  awaiting_sfu_recovery_ = false;
}

void CallTopologyController::OnMediaStopped(const std::string& call_id) {
  // Invalidate in-flight SoftMigrate / AttachLocalToSfu so they cannot StartSfu after Leave
  // (Linux quit dogfood: double-free from SDL reopen during teardown).
  migrate_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (sfu_attached_ && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_attached_ = false;
  awaiting_sfu_recovery_ = false;
  soft_migrate_in_flight_ = false;
  pending_inbound_sfu_attach_.reset();
  pending_inbound_sfu_attach_call_id_.clear();
  local_publisher_stream_id_ = 0;
  host_.TopologyClearMediaActivity();
  if (sfu_attach_wait_call_id_ == call_id) {
    ClearSfuAttachWait();
  }
}

void CallTopologyController::PollPendingSfuAttach() {
  const std::string call_id = sfu_attach_wait_call_id_;
  SfuAttachWaitPollInput in;
  in.wait_active = !call_id.empty();
  in.now_ms = util::NowUnixMs();
  in.deadline_ms = sfu_attach_wait_deadline_ms_;
  in.soft_migrate_in_flight = soft_migrate_in_flight_;
  in.sfu_attached_for_call =
      sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id;
  if (auto joined = sessions_.CountJoined(call_id)) {
    in.joined_count = *joined;
  }
  in.media_active_p2p_for_call =
      media_.IsActive() && media_.ActiveCallId() == call_id && !media_.IsSfuMode();

  switch (PollSfuAttachWait(in)) {
  case SfuAttachWaitPollResult::Idle:
  case SfuAttachWaitPollResult::Waiting:
    return;
  case SfuAttachWaitPollResult::ClearAttached:
  case SfuAttachWaitPollResult::ClearAsP2p:
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    return;
  case SfuAttachWaitPollResult::TimeoutLeave:
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    host_.TopologySetLastMediaError(Tr("call.error.no_media_relay_hop"));
    log().warning << "SFU attach wait timed out call_id=" << call_id;
    (void)host_.TopologyLeaveCall(call_id);
    return;
  }
}

void CallTopologyController::EjectParticipantAfterMigrateFailure(const std::string& call_id,
                                                                 const std::string& identity,
                                                                 const std::string& reason) {
  if (identity.empty()) {
    return;
  }
  host_.TopologySetLastMediaError(reason);
  log().warning << "Ejecting " << identity << " after soft-migrate failure: " << reason;

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = identity;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (detail) {
    (void)host_.TopologySendDirect(identity, CallControlType::CallLeave, *detail, "Left the call");
  }

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Left;
  participant.left_at = util::NowUnixMs();
  (void)sessions_.UpsertParticipant(participant);

  if (detail) {
    auto local = host_.TopologyLocalIdentity();
    if (local) {
      (void)host_.TopologyFanOutToJoined(call_id, CallControlType::CallLeave, *detail, "Left the call",
                                         *local);
    }
  }
  host_.TopologyNotifyRingChanged();
}

uint32_t CallTopologyController::PublisherStreamIdForLocal() const {
  auto local = host_.TopologyLocalIdentity();
  if (!local) {
    return 1;
  }
  return PublisherStreamIdForIdentity(*local);
}

void CallTopologyController::RefreshAdaptation(const std::string& /*call_id*/) {
  CallAdaptationInput in;
  in.camera_user_wants = true;
  in.muted = media_.IsMuted();
  in.per_user_up_bps = 0;
  in.allow_video_hi = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

bool CallTopologyController::IsMigrateGenerationCurrent(uint64_t gen) const {
  return gen == 0 || gen == migrate_generation_.load(std::memory_order_acquire);
}

void CallTopologyController::SyncSfuSubscriptions(const std::string& call_id) {
  if (!relay_deps_.relay || !sfu_attached_ || !media_.IsSfuMode() || media_.ActiveCallId() != call_id) {
    return;
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return;
  }
  auto local = host_.TopologyLocalIdentity();
  int subscribed = 0;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    if (local && p.identity == *local) {
      continue;
    }
    const uint32_t stream = PublisherStreamIdForIdentity(p.identity);
    (void)relay_deps_.relay->Subscribe(stream, 0);
    (void)relay_deps_.relay->Subscribe(stream, 1);
    ++subscribed;
    log().info << "SFU subscribe peer=" << p.identity << " stream=" << stream << " call_id=" << call_id;
  }
  log().info << "SyncSfuSubscriptions call_id=" << call_id << " peers=" << subscribed;
}

Roe<void> CallTopologyController::MaybeSoftMigrateToSfu(const std::string& call_id,
                                                        SoftMigrateTrigger trigger,
                                                        const std::string& prefer_hop_peer_id,
                                                        uint64_t expected_gen) {
  if (!IsMigrateGenerationCurrent(expected_gen)) {
    log().info << "SoftMigrate skip stale gen want=" << expected_gen
               << " have=" << migrate_generation_.load(std::memory_order_acquire)
               << " call_id=" << call_id;
    return {};
  }
  const bool repick = !prefer_hop_peer_id.empty();
  if (!repick && sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    SyncSfuSubscriptions(call_id);
    return {};
  }
  if (!relay_deps_.relay || !relay_deps_.dial) {
    return Error("media_relay not available");
  }

  auto local = host_.TopologyLocalIdentity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
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
  const bool first_attach =
      !session || !session->has_value() || !(*session)->sfu_hint || (*session)->sfu_hint->empty();

  if (!repick) {
    SoftMigrateDecisionInput decision_in;
    decision_in.local_identity = *local;
    decision_in.joined_identities = joined_ids;
    decision_in.initiator_identity = SelectCallInitiator(joined_peers);
    decision_in.sfu_hint_empty = first_attach;
    decision_in.trigger = trigger;
    decision_in.already_on_sfu = sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id;

    SoftMigrateAction action = DecideSoftMigrate(decision_in);
    // PreferLocal durable Node hosts media_relay for the call (V029). Sticky-initiator
    // WaitForAttach must not block PreferLocal SoftMigrate — dogfood: wrong earliest
    // joined_at made a phone the "initiator", Linux waited, nobody fan-out CallSfuAttach.
    if (action == SoftMigrateAction::WaitForAttach && relay_deps_.prefer_local_as_hop &&
        relay_deps_.relay->IsStarted()) {
      log().info << "SoftMigrate PreferLocal Node overrides WaitForAttach → PickHop call_id="
                 << call_id;
      action = SoftMigrateAction::PickHop;
    } else if (action == SoftMigrateAction::PickHop && !relay_deps_.prefer_local_as_hop) {
      // Phones must not PickHop when a durable media_relay Node is available — quote hits
      // prefer-contacts stranger refuse before PreferLocal session exists.
      bool durable_hop = false;
      if (relay_deps_.list_media_relay_peers) {
        for (const std::string& pid : relay_deps_.list_media_relay_peers()) {
          if (!pid.empty()) {
            durable_hop = true;
            break;
          }
        }
      }
      if (!durable_hop && relay_deps_.peer_has_media_relay) {
        for (const MeshHopCandidate& hop : RankedMediaHopCandidates()) {
          if (!hop.peer_id.empty() && relay_deps_.peer_has_media_relay(hop.peer_id)) {
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
      SyncSfuSubscriptions(call_id);
      return {};
    }
    if (action == SoftMigrateAction::WaitForAttach) {
      host_.TopologySetMediaActivity(Tr("call.status.waiting_for_media_path"));
      host_.TopologyNotifyRingChanged();
      return {};
    }
  } else if (sfu_attached_) {
    // Guest hint re-pick: never Detach a healthy PreferLocal session — that silenced already-
    // attached guests (Moto) while Samsung hop-hints looped onto a non-media_relay seed.
    std::string local_pid;
    if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
      local_pid = *pid;
    }
    const bool prefer_self =
        !prefer_hop_peer_id.empty() && !local_pid.empty() && prefer_hop_peer_id == local_pid;
    if (prefer_self || relay_deps_.relay->IsLocalHopAttached()) {
      log().info << "SoftMigrate re-pick no-op: keep PreferLocal call_id=" << call_id
                 << " prefer=" << prefer_hop_peer_id;
      SyncSfuSubscriptions(call_id);
      return Error("keep_prefer_local");
    }
    const bool prefer_dialable =
        relay_deps_.dial && !prefer_hop_peer_id.empty() &&
        relay_deps_.dial->IsDialable(prefer_hop_peer_id);
    if (!prefer_dialable) {
      log().warning << "SoftMigrate re-pick aborted: prefer hop not dialable, keep current SFU hop="
                    << prefer_hop_peer_id;
      SyncSfuSubscriptions(call_id);
      return {};
    }
    log().info << "SoftMigrate re-pick detach call_id=" << call_id << " prefer=" << prefer_hop_peer_id;
    host_.TopologySetMediaActivity(Tr("call.status.switching_media_path"));
    host_.TopologyNotifyRingChanged();
    relay_deps_.relay->Detach();
    sfu_attached_ = false;
    if (session && session->has_value()) {
      (*session)->sfu_hint.reset();
      (void)sessions_.UpsertSession(**session);
    }
  }

  auto ranked = RankedMediaHopCandidates();
  // Durable Node PreferLocal only — do not PreferInCall phones as SFU host (V029).
  std::string local_peer_id;
  if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
    local_peer_id = *pid;
  }
  if (relay_deps_.prefer_local_as_hop && relay_deps_.relay->IsStarted() && !local_peer_id.empty()) {
    std::string local_ma;
    if (relay_deps_.resolve_local_advertise) {
      const auto live = relay_deps_.resolve_local_advertise();
      if (!live.empty()) {
        local_ma = live.front();
      }
    }
    if (local_ma.empty() && !relay_deps_.local_advertise_multiaddrs.empty()) {
      local_ma = relay_deps_.local_advertise_multiaddrs.front();
    }
    if (local_ma.empty() && !relay_deps_.local_listen_multiaddr.empty()) {
      local_ma = relay_deps_.local_listen_multiaddr;
      if (local_ma.find("/p2p/") == std::string::npos) {
        local_ma += "/p2p/" + local_peer_id;
      }
    }
    if (!local_ma.empty()) {
      ranked = PreferLocalMediaHop(std::move(ranked), local_peer_id, local_ma);
    } else {
      log().warning << "PreferLocal skipped: no advertise multiaddr for local hop";
    }
  }
  if (!prefer_hop_peer_id.empty()) {
    ranked = PreferNamedHopFirst(std::move(ranked), prefer_hop_peer_id);
  }
  if (ranked.empty()) {
    return Error(Tr("call.error.no_media_relay_hop"));
  }

  host_.TopologySetMediaActivity(repick ? Tr("call.status.switching_media_path")
                                        : Tr("call.status.finding_media_path"));
  host_.TopologyNotifyRingChanged();

  std::vector<std::string> hop_failures;
  hop_failures.reserve(ranked.size());
  for (const MeshHopCandidate& hop : ranked) {
    if (!IsMigrateGenerationCurrent(expected_gen)) {
      log().info << "SoftMigrate abort mid-pick stale gen want=" << expected_gen
                 << " have=" << migrate_generation_.load(std::memory_order_acquire);
      return {};
    }
    const bool self_hop = !local_peer_id.empty() && hop.peer_id == local_peer_id;
    if (!self_hop && relay_deps_.circuit_reach && relay_deps_.dial &&
        !relay_deps_.dial->IsDialable(hop.peer_id)) {
      (void)relay_deps_.circuit_reach->TryEnsureHopReachable(hop.peer_id);
    }
    if (!self_hop && (!relay_deps_.dial || !relay_deps_.dial->IsDialable(hop.peer_id))) {
      const std::string detail = "hop not dialable (hop=" + hop.peer_id + ")";
      hop_failures.push_back(detail);
      log().warning << "SoftMigrate skip: " << detail;
      continue;
    }
    std::string hop_ma = hop.multiaddr;
    if (hop_ma.empty() && relay_deps_.dial) {
      if (auto ma = relay_deps_.dial->PreferredMultiaddr(hop.peer_id)) {
        hop_ma = *ma;
      }
    }
    log().info << "SoftMigrate try hop=" << hop.peer_id
               << " affinity=" << static_cast<int>(hop.affinity)
               << " ma=" << (hop_ma.empty() ? "(circuit)" : hop_ma);
    host_.TopologySetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.TopologyNotifyRingChanged();
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = hop.peer_id;
    attach.hop_multiaddr = hop_ma;
    attach.publisher_stream_id = PublisherStreamIdForLocal();

    if (auto attached = AttachLocalToSfu(call_id, attach); !attached) {
      std::string detail = attached.error().message;
      if (detail.find(hop.peer_id) == std::string::npos) {
        detail += " (hop=" + hop.peer_id + ")";
      }
      hop_failures.push_back(detail);
      log().warning << "SoftMigrate hop failed: " << detail;
      continue;
    }

    if (session && session->has_value()) {
      (*session)->sfu_hint = hop.peer_id;
      (void)sessions_.UpsertSession(**session);
    }

    CallSfuAttachDetail fanout = BuildSfuAttachFanout(attach);
    auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
    if (encoded) {
      log().info << "SoftMigrate fan-out CallSfuAttach hop=" << hop.peer_id
                 << " ma=" << (hop_ma.empty() ? "(empty)" : hop_ma) << " call_id=" << call_id;
      (void)host_.TopologyFanOutToJoined(call_id, CallControlType::CallSfuAttach, *encoded,
                                         "Call SFU attach", *local);
    }
    return {};
  }
  if (hop_failures.empty()) {
    return Error(Tr("call.error.no_media_relay_hop"));
  }
  std::string summary = "media_relay SoftMigrate failed (" + std::to_string(hop_failures.size()) +
                        " hops): ";
  for (size_t i = 0; i < hop_failures.size(); ++i) {
    if (i > 0) {
      summary += " | ";
    }
    summary += hop_failures[i];
  }
  log().warning << summary;
  return Error(Tr("call.error.no_media_relay_hop"));
}

Roe<void> CallTopologyController::AttachLocalToSfu(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach_in) {
  std::lock_guard<std::mutex> attach_lock(sfu_attach_mu_);
  const uint64_t gen_at_start = migrate_generation_.load(std::memory_order_acquire);
  if (!relay_deps_.relay || !relay_deps_.dial) {
    return Error("media_relay not available");
  }
  CallSfuAttachDetail attach = attach_in;
  if (attach.hop_peer_id.empty()) {
    return Error("missing hop_peer_id");
  }

  log().info << "AttachLocalToSfu begin call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " already_sfu=" << (sfu_attached_ ? 1 : 0);

  // AcceptAndAttach keeps the control stream open; frame reader starts after StartSfu.
  auto sfu_frames_ready = std::make_shared<std::atomic<bool>>(false);
  auto on_sfu_frame = [this, sfu_frames_ready](MediaDataFrame frame) {
    if (!sfu_frames_ready->load(std::memory_order_acquire)) {
      return;
    }
    CallMediaEngine::SfuPacket pkt;
    pkt.channel_id = frame.channel_id;
    pkt.seq = frame.seq;
    pkt.mark = frame.mark;
    pkt.payload = std::move(frame.payload);
    media_.OnSfuPacket(pkt);
  };

  const bool self_hop = [&]() {
    if (auto local_pid = relay_deps_.relay->LocalPeerIdBase58()) {
      return *local_pid == attach.hop_peer_id;
    }
    return false;
  }();

  int64_t a_up_bps = CallMediaAdaptation::kDefaultAudioBps + CallMediaAdaptation::kDefaultVideoLoBps;
  if (self_hop) {
    // Durable Node PreferLocal only (V029/V030) — never host as ephemeral listen-only.
    if (!relay_deps_.prefer_local_as_hop || !relay_deps_.relay->IsStarted()) {
      log().warning << "AttachLocalToSfu refused local hop (prefer_local="
                    << (relay_deps_.prefer_local_as_hop ? 1 : 0)
                    << " started=" << (relay_deps_.relay->IsStarted() ? 1 : 0) << ")";
      return Error(Tr("call.error.local_media_relay_unavailable"));
    }
    log().info << "AttachLocalToSfu as local media_relay hop call_id=" << call_id;
    auto attach_res = relay_deps_.relay->AttachAsLocalHop(call_id, on_sfu_frame);
    if (!attach_res || !attach_res->ok) {
      return Error(attach_res ? attach_res->error : attach_res.error().message);
    }
  } else {
    if (attach.hop_multiaddr.empty()) {
      attach.hop_multiaddr = ResolveHopMultiaddr(attach.hop_peer_id);
    }
    if (!attach.hop_multiaddr.empty()) {
      (void)relay_deps_.dial->RegisterEndpoint(attach.hop_peer_id, attach.hop_multiaddr);
      relay_deps_.dial->ClearDialBackoff(attach.hop_peer_id);
    }
    if (!relay_deps_.dial->IsDialable(attach.hop_peer_id) && relay_deps_.circuit_reach) {
      (void)relay_deps_.circuit_reach->TryEnsureHopReachable(attach.hop_peer_id);
    }
    if (!relay_deps_.dial->IsDialable(attach.hop_peer_id)) {
      return Error("hop not dialable");
    }

    MediaRelayQuoteRequest qreq;
    qreq.call_id = call_id;
    auto joined = sessions_.CountJoined(call_id);
    qreq.participants = joined ? static_cast<int>(*joined) : 2;
    qreq.want_up_bps = CallMediaAdaptation::kDefaultAudioBps + CallMediaAdaptation::kDefaultVideoLoBps;
    qreq.want_down_bps = qreq.want_up_bps * std::max(1, qreq.participants - 1);

    // Always RequestQuote locally. Shared quote_ids from fan-out are already consumed.
    // Soft-migrate tries multiple hops; keep per-hop budget tight so seed/contact failover fits
    // inside attach-wait.
    auto quote = relay_deps_.relay->RequestQuote(attach.hop_peer_id, qreq, 5000);
    if (!quote || !quote->ok) {
      return Error(quote ? quote->error : quote.error().message);
    }
    a_up_bps = quote->a_up_bps;

    auto attach_res = relay_deps_.relay->AcceptAndAttach(
        attach.hop_peer_id, quote->quote_id, call_id, call_id, on_sfu_frame, 8000);
    if (!attach_res || !attach_res->ok) {
      return Error(attach_res ? attach_res->error : attach_res.error().message);
    }
    log().info << "AttachLocalToSfu AcceptAndAttach ok hop=" << attach.hop_peer_id
               << " quote=" << quote->quote_id;
  }

  CallAdaptationInput in;
  in.per_user_up_bps = a_up_bps;
  in.camera_user_wants = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));

  local_publisher_stream_id_ = PublisherStreamIdForLocal();

  const uint32_t pub = local_publisher_stream_id_;
  host_.TopologyNoteMediaAttempted(call_id);
  host_.TopologyBindMediaCallId(call_id);
  if (!IsMigrateGenerationCurrent(gen_at_start)) {
    log().info << "AttachLocalToSfu aborted before StartSfu (media stopped) call_id=" << call_id;
    relay_deps_.relay->Detach();
    return Error("attach aborted");
  }
  log().info << "AttachLocalToSfu StartSfu call_id=" << call_id << " pub_stream=" << pub;
  auto started = media_.StartSfu(call_id, [this, pub](const CallMediaEngine::SfuPacket& pkt) {
    if (!relay_deps_.relay) {
      return;
    }
    MediaDataFrame frame;
    frame.stream_id = pub;
    frame.channel_id = pkt.channel_id;
    frame.channel_type =
        pkt.channel_id == 0 ? MediaChannelType::ReliableOrdered : MediaChannelType::LatestLossy;
    frame.seq = pkt.seq;
    frame.mark = pkt.mark;
    frame.payload = pkt.payload;
    (void)relay_deps_.relay->SendFrame(frame);
  });
  if (!started) {
    relay_deps_.relay->Detach();
    return started.error();
  }
  if (!IsMigrateGenerationCurrent(gen_at_start)) {
    log().info << "AttachLocalToSfu aborted after StartSfu (media stopped) call_id=" << call_id;
    media_.Stop();
    relay_deps_.relay->Detach();
    sfu_attached_ = false;
    return Error("attach aborted");
  }

  sfu_frames_ready->store(true, std::memory_order_release);
  if (!self_hop) {
    relay_deps_.relay->StartClientFrameReader();
    log().info << "AttachLocalToSfu StartClientFrameReader call_id=" << call_id;
  }

  sfu_attached_ = true;
  awaiting_sfu_recovery_ = false;
  SyncSfuSubscriptions(call_id);
  host_.TopologyClearMediaPeerIdentity();
  // Drop leftover 1:1 call-media so peer teardown cannot ConnectFailed over live SFU audio.
  host_.TopologyReleaseDirectMedia();
  host_.TopologyClearMediaActivity();
  ClearSfuAttachWait();
  RefreshAdaptation(call_id);
  log().info << "AttachLocalToSfu done call_id=" << call_id << " hop=" << attach.hop_peer_id;
  return {};
}

void CallTopologyController::TryRecoverViaSfu(const std::string& call_id) {
  if (sfu_attached_ && media_.IsSfuMode()) {
    return;
  }
  if (soft_migrate_in_flight_ || (!sfu_attach_wait_call_id_.empty() && sfu_attach_wait_call_id_ == call_id)) {
    return;
  }
  awaiting_sfu_recovery_ = true;
  BeginSfuAttachWait(call_id);
  host_.TopologySetMediaActivity(Tr("call.status.finding_media_path"));
  host_.TopologyNotifyRingChanged();
  const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  soft_migrate_in_flight_ = true;
  AppRuntime::PostWorkerNormal([this, call_id, gen]() {
    Roe<void> migrated = MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::IceRecover, {}, gen);
    const bool attached = sfu_attached_ && media_.IsSfuMode();
    AppRuntime::PostUI([this, call_id, migrated, attached, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      soft_migrate_in_flight_ = false;
      if (attached || (sfu_attached_ && media_.IsSfuMode())) {
        awaiting_sfu_recovery_ = false;
        ClearSfuAttachWait();
        SyncSfuSubscriptions(call_id);
        host_.TopologyNotifyRingChanged();
        return;
      }
      if (!migrated) {
        awaiting_sfu_recovery_ = false;
        const std::string msg =
            migrated.error().message.empty()
                ? Tr("call.error.no_media_relay_hop")
                : migrated.error().message;
        host_.TopologySetLastMediaError(msg);
        log().warning << "ICE-fail SFU recovery failed: " << msg;
        (void)host_.TopologyLeaveCall(call_id);
        host_.TopologyNotifyRingChanged();
        return;
      }
      host_.TopologyNotifyRingChanged();
    });
  });
}

bool CallTopologyController::OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                                 const std::optional<std::string>& sfu_hint) {
  if (n_joined >= 3 && sfu_hint && !sfu_hint->empty()) {
    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = *sfu_hint;
    attach.hop_multiaddr = ResolveHopMultiaddr(*sfu_hint);
    attach.publisher_stream_id = PublisherStreamIdForLocal();
    host_.TopologyNoteMediaAttempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.TopologySetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.TopologyNotifyRingChanged();
    const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    soft_migrate_in_flight_ = true;
    AppRuntime::PostWorkerNormal([this, call_id, attach, gen]() {
      Roe<void> ok = AttachLocalToSfu(call_id, attach);
      AppRuntime::PostUI([this, call_id, ok, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (!ok) {
          if (sfu_attached_ && media_.IsSfuMode()) {
            SyncSfuSubscriptions(call_id);
            host_.TopologyNotifyRingChanged();
            return;
          }
          log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
          host_.TopologySetLastMediaError(ok.error().message);
          ClearSfuAttachWait();
          (void)host_.TopologyLeaveCall(call_id);
        } else {
          pending_inbound_sfu_attach_.reset();
          pending_inbound_sfu_attach_call_id_.clear();
        }
        FlushPendingInboundSfuAttach();
        host_.TopologyNotifyRingChanged();
      });
    });
    return true;
  }
  if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    host_.TopologyNoteMediaAttempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.TopologySetMediaActivity(Tr("call.status.setting_up_group"));
    host_.TopologyNotifyRingChanged();
    const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    soft_migrate_in_flight_ = true;
    AppRuntime::PostWorkerNormal([this, call_id, gen]() {
      Roe<void> mig =
          MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::LocalJoinedWithoutHint, {}, gen);
      const bool attached = sfu_attached_;
      AppRuntime::PostUI([this, call_id, mig, attached, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (sfu_attached_ && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          SyncSfuSubscriptions(call_id);
          pending_inbound_sfu_attach_.reset();
          pending_inbound_sfu_attach_call_id_.clear();
          host_.TopologyNotifyRingChanged();
          return;
        }
        if (!mig) {
          log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
          host_.TopologySetLastMediaError(mig.error().message);
          ClearSfuAttachWait();
          (void)host_.TopologyLeaveCall(call_id);
        } else if (!attached && !sfu_attached_) {
          // Wait for CallSfuAttach (LocalJoinedWithoutHint → WaitForAttach).
          FlushPendingInboundSfuAttach();
        } else {
          ClearSfuAttachWait();
        }
        host_.TopologyNotifyRingChanged();
      });
    });
    return true;
  }
  ClearSfuAttachWait();
  awaiting_sfu_recovery_ = false;
  return false;
}

bool CallTopologyController::OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                                  const std::string& joiner_identity) {
  if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    log().info << "OnRemoteAcceptJoined call_id=" << call_id << " n=" << n_joined
               << " joiner=" << joiner_identity << " sfu=" << (sfu_attached_ ? 1 : 0)
               << " inflight=" << (soft_migrate_in_flight_ ? 1 : 0);
    // Already on SFU (2nd concurrent accept): only refresh subscriptions — do not re-SoftMigrate.
    if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
      SyncSfuSubscriptions(call_id);
      ClearSfuAttachWait();
      host_.TopologyNotifyRingChanged();
      return true;
    }
    // Overlapping Accept: keep the in-flight SoftMigrate; bumping gen would Detach mid-attach.
    if (soft_migrate_in_flight_) {
      log().info << "OnRemoteAcceptJoined defer SoftMigrate (already in flight) joiner="
                 << joiner_identity;
      BeginSfuAttachWait(call_id);
      host_.TopologySetMediaActivity(Tr("call.status.setting_up_group"));
      host_.TopologyNotifyRingChanged();
      return true;
    }
    host_.TopologyNoteMediaAttempted(call_id);
    BeginSfuAttachWait(call_id);
    host_.TopologySetMediaActivity(Tr("call.status.setting_up_group"));
    host_.TopologyNotifyRingChanged();
    const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    soft_migrate_in_flight_ = true;
    AppRuntime::PostWorkerNormal([this, call_id, joiner_identity, gen]() {
      Roe<void> mig =
          MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved, {}, gen);
      AppRuntime::PostUI([this, call_id, joiner_identity, mig, gen]() {
        if (!IsMigrateGenerationCurrent(gen)) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (sfu_attached_ && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          SyncSfuSubscriptions(call_id);
          pending_inbound_sfu_attach_.reset();
          pending_inbound_sfu_attach_call_id_.clear();
          host_.TopologyNotifyRingChanged();
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
        host_.TopologyNotifyRingChanged();
      });
    });
    return true;
  }
  ClearSfuAttachWait();
  awaiting_sfu_recovery_ = false;
  return false;
}

void CallTopologyController::OnJoinedCountObserved(const std::string& call_id, size_t n_joined) {
  if (!CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    return;
  }
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    // Roster grew while already on SFU — ensure we subscribe to any late Joined peers.
    SyncSfuSubscriptions(call_id);
    return;
  }
  if (soft_migrate_in_flight_) {
    log().info << "OnJoinedCountObserved skip SoftMigrate (in flight) n=" << n_joined
               << " call_id=" << call_id;
    return;
  }
  auto session = sessions_.LoadSession(call_id);
  const bool first_attach =
      !session || !session->has_value() || !(*session)->sfu_hint || (*session)->sfu_hint->empty();
  if (!first_attach) {
    return;
  }

  host_.TopologyNoteMediaAttempted(call_id);
  BeginSfuAttachWait(call_id);
  host_.TopologySetMediaActivity(Tr("call.status.setting_up_group"));
  host_.TopologyNotifyRingChanged();
  const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  soft_migrate_in_flight_ = true;
  AppRuntime::PostWorkerNormal([this, call_id, gen]() {
    Roe<void> mig =
        MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved, {}, gen);
    AppRuntime::PostUI([this, call_id, mig, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      soft_migrate_in_flight_ = false;
      if (sfu_attached_ && media_.IsSfuMode()) {
        ClearSfuAttachWait();
        SyncSfuSubscriptions(call_id);
        pending_inbound_sfu_attach_.reset();
        pending_inbound_sfu_attach_call_id_.clear();
        host_.TopologyNotifyRingChanged();
        return;
      }
      if (!mig) {
        log().warning << "MaybeSoftMigrateToSfu (roster) failed: " << mig.error().message;
        host_.TopologySetLastMediaError(mig.error().message);
        // Do not LeaveCall here — attach-wait / inviter eject paths handle failure.
      } else {
        FlushPendingInboundSfuAttach();
      }
      host_.TopologyNotifyRingChanged();
    });
  });
}

Roe<void> CallTopologyController::OnInboundSfuAttach(const std::string& call_id,
                                                     const CallSfuAttachDetail& attach) {
  log().info << "OnInboundSfuAttach call_id=" << call_id << " hop=" << attach.hop_peer_id
             << " ma=" << (attach.hop_multiaddr.empty() ? "(empty)" : attach.hop_multiaddr)
             << " sfu=" << (sfu_attached_ ? 1 : 0) << " inflight=" << (soft_migrate_in_flight_ ? 1 : 0);
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    // Already attached (duplicate fan-out / late roster) — refresh subscriptions only.
    SyncSfuSubscriptions(call_id);
    ClearSfuAttachWait();
    host_.TopologyClearMediaActivity();
    host_.TopologyNotifyRingChanged();
    return {};
  }
  // SoftMigrate PickHop may be mid-AcceptAndAttach. Bumping gen Detach's that stream and races
  // libp2p asio (Moto SIGSEGV on pp-worker). Defer until SoftMigrate clears in-flight.
  if (soft_migrate_in_flight_) {
    pending_inbound_sfu_attach_ = attach;
    pending_inbound_sfu_attach_call_id_ = call_id;
    log().info << "OnInboundSfuAttach deferred (SoftMigrate in flight) call_id=" << call_id;
    BeginSfuAttachWait(call_id);
    host_.TopologySetMediaActivity(Tr("call.status.connecting_media_relay"));
    host_.TopologyNotifyRingChanged();
    return {};
  }
  BeginSfuAttachWait(call_id);
  host_.TopologySetMediaActivity(Tr("call.status.connecting_media_relay"));
  host_.TopologyNotifyRingChanged();
  const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  soft_migrate_in_flight_ = true;
  AppRuntime::PostWorkerNormal([this, call_id, attach, gen]() {
    if (!IsMigrateGenerationCurrent(gen)) {
      log().info << "OnInboundSfuAttach worker skip stale gen=" << gen;
      return;
    }
    Roe<void> ok = AttachLocalToSfu(call_id, attach);
    AppRuntime::PostUI([this, call_id, attach, ok, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      soft_migrate_in_flight_ = false;
      if (!ok) {
        if (sfu_attached_ && media_.IsSfuMode()) {
          SyncSfuSubscriptions(call_id);
          host_.TopologyNotifyRingChanged();
          return;
        }
        host_.TopologySetLastMediaError(ok.error().message);
        log().warning << "AttachLocalToSfu (inbound) failed: " << ok.error().message;
        // V029: ask owner to re-pick or refuse — keep attach-wait for a re-fan-out.
        ReportSfuAttachFailedToInitiator(call_id, attach.hop_peer_id, ok.error().message);
        BeginSfuAttachWait(call_id);
        host_.TopologyNotifyRingChanged();
        return;
      }
      pending_inbound_sfu_attach_.reset();
      pending_inbound_sfu_attach_call_id_.clear();
      SyncSfuSubscriptions(call_id);
      host_.TopologyNotifyRingChanged();
    });
  });
  return {};
}

void CallTopologyController::FlushPendingInboundSfuAttach() {
  if (!pending_inbound_sfu_attach_ || pending_inbound_sfu_attach_call_id_.empty()) {
    return;
  }
  const std::string call_id = pending_inbound_sfu_attach_call_id_;
  const CallSfuAttachDetail attach = *pending_inbound_sfu_attach_;
  pending_inbound_sfu_attach_.reset();
  pending_inbound_sfu_attach_call_id_.clear();
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
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
  auto local = host_.TopologyLocalIdentity();
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
    host_.TopologySetLastMediaError(Tr("call.error.hop_unreachable_guest"));
    ClearSfuAttachWait();
    (void)host_.TopologyLeaveCall(call_id);
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
  log().info << "ReportSfuAttachFailed to initiator=" << initiator << " prefs="
             << detail.preferred_hop_peer_ids.size();
  host_.TopologySetMediaActivity(Tr("call.status.looking_for_another_path"));
  host_.TopologyNotifyRingChanged();
  (void)host_.TopologySendDirect(initiator, CallControlType::CallSfuAttachFailed, *encoded,
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
      (void)host_.TopologySendDirect(guest_identity, CallControlType::CallHopRefuse, *encoded, message);
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
  auto local = host_.TopologyLocalIdentity();
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

  // PreferLocal already hosting: eject the stranded guest; do not Detach (kills healthy peers).
  if (relay_deps_.relay && relay_deps_.relay->IsLocalHopAttached()) {
    log().warning << "Hop hint refuse (keep PreferLocal) guest=" << guest
                  << " failed_hop=" << detail.failed_hop_peer_id
                  << " prefer=" << decision.preferred_hop_peer_id;
    if (!guest.empty()) {
      RefuseGuestNoSharedHop(detail.call_id, guest);
    }
    return;
  }
  if (soft_migrate_in_flight_) {
    log().info << "Hop hint re-pick skipped (SoftMigrate in flight) guest=" << guest;
    return;
  }

  log().info << "Hop hint re-pick prefer=" << decision.preferred_hop_peer_id << " guest=" << guest;
  host_.TopologySetMediaActivity(Tr("call.status.switching_media_path"));
  host_.TopologyNotifyRingChanged();
  BeginSfuAttachWait(detail.call_id);
  const uint64_t gen = migrate_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  soft_migrate_in_flight_ = true;
  const std::string prefer = decision.preferred_hop_peer_id;
  AppRuntime::PostWorkerNormal([this, call_id = detail.call_id, prefer, guest, gen]() {
    Roe<void> mig =
        MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::IceRecover, prefer, gen);
    AppRuntime::PostUI([this, call_id, mig, guest, gen]() {
      if (!IsMigrateGenerationCurrent(gen)) {
        return;
      }
      soft_migrate_in_flight_ = false;
      if (!mig) {
        log().warning << "Hop hint re-pick failed: " << mig.error().message;
        RefuseGuestNoSharedHop(call_id, guest);
        return;
      }
      SyncSfuSubscriptions(call_id);
      host_.TopologyNotifyRingChanged();
    });
  });
}

void CallTopologyController::OnInboundHopRefuse(const CallHopRefuseDetail& detail) {
  auto local = host_.TopologyLocalIdentity();
  if (!local) {
    return;
  }
  if (!detail.identity.empty() && detail.identity != *local) {
    return;
  }
  const std::string message =
      detail.message.empty() ? Tr("call.error.hop_unreachable_guest") : detail.message;
  host_.TopologySetLastMediaError(message);
  log().warning << "CallHopRefuse call_id=" << detail.call_id << " reason=" << detail.reason;
  ClearSfuAttachWait();
  awaiting_sfu_recovery_ = false;
  (void)host_.TopologyLeaveCall(detail.call_id);
  host_.TopologyNotifyRingChanged();
}

} // namespace pbr
