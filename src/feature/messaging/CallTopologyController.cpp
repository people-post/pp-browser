#include "feature/messaging/CallTopologyController.h"

#include "base/media/CallMediaAdaptation.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/people/ContactTypes.h"
#include "base/people/MeshHopPolicy.h"
#include "base/platform/BrowserThread.h"
#include "common/Utilities.h"

#include <algorithm>

namespace pbr {
namespace {

/** Soft-migrate quote/attach can take ~20–30s; wait must outlast initiator IO work. */
constexpr int64_t kSfuAttachWaitMs = 45000;

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
  auto ordered =
      OrderCircuitHops(std::move(contact_hops), std::move(seed_hops), relay_deps_.prefer_contacts);
  return RankMediaHops(std::move(ordered), relay_deps_.prefer_contacts);
}

bool CallTopologyController::HasMediaRelayHopCandidates() const {
  if (!relay_deps_.relay || !relay_deps_.sessions) {
    return false;
  }
  return !RankedMediaHopCandidates().empty();
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
  return {};
}

void CallTopologyController::BeginSfuAttachWait(const std::string& call_id) {
  sfu_attach_wait_call_id_ = call_id;
  sfu_attach_wait_deadline_ms_ = util::NowUnixMs() + kSfuAttachWaitMs;
}

void CallTopologyController::ClearSfuAttachWait() {
  sfu_attach_wait_call_id_.clear();
  sfu_attach_wait_deadline_ms_ = 0;
}

void CallTopologyController::ClearAwaitingSfuRecovery() {
  awaiting_sfu_recovery_ = false;
}

void CallTopologyController::OnMediaStopped(const std::string& call_id) {
  if (sfu_attached_ && relay_deps_.relay) {
    relay_deps_.relay->Detach();
  }
  sfu_attached_ = false;
  awaiting_sfu_recovery_ = false;
  soft_migrate_in_flight_ = false;
  local_publisher_stream_id_ = 0;
  if (sfu_attach_wait_call_id_ == call_id) {
    ClearSfuAttachWait();
  }
}

void CallTopologyController::PollPendingSfuAttach() {
  if (sfu_attach_wait_call_id_.empty()) {
    return;
  }
  const std::string call_id = sfu_attach_wait_call_id_;
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    return;
  }
  // 1:1 P2P may still be connecting — never convert that into a group-relay timeout leave.
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined < 3 && media_.IsActive() && media_.ActiveCallId() == call_id &&
      !media_.IsSfuMode()) {
    ClearSfuAttachWait();
    awaiting_sfu_recovery_ = false;
    return;
  }
  if (util::NowUnixMs() < sfu_attach_wait_deadline_ms_) {
    return;
  }
  ClearSfuAttachWait();
  awaiting_sfu_recovery_ = false;
  host_.TopologySetLastMediaError("No media relay available — group call needs a media_relay hop");
  log().warning << "SFU attach wait timed out call_id=" << call_id;
  (void)host_.TopologyLeaveCall(call_id);
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
  uint32_t h = 2166136261u;
  for (unsigned char c : *local) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

void CallTopologyController::RefreshAdaptation(const std::string& /*call_id*/) {
  CallAdaptationInput in;
  in.camera_user_wants = true;
  in.muted = media_.IsMuted();
  in.per_user_up_bps = 0;
  in.allow_video_hi = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));
}

Roe<void> CallTopologyController::MaybeSoftMigrateToSfu(const std::string& call_id) {
  if (sfu_attached_ && media_.IsSfuMode() && media_.ActiveCallId() == call_id) {
    return {};
  }
  if (!relay_deps_.relay || !relay_deps_.sessions) {
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
  std::string initiator_identity;
  int64_t earliest_joined = 0;
  bool have_earliest = false;
  for (const CallParticipant& p : *participants) {
    if (p.state != CallParticipantState::Joined) {
      continue;
    }
    joined_ids.push_back(p.identity);
    if (p.joined_at) {
      if (!have_earliest || *p.joined_at < earliest_joined) {
        earliest_joined = *p.joined_at;
        initiator_identity = p.identity;
        have_earliest = true;
      }
    } else if (!have_earliest && initiator_identity.empty()) {
      initiator_identity = p.identity;
    }
  }
  if (initiator_identity.empty() && !joined_ids.empty()) {
    initiator_identity = joined_ids.front();
  }

  auto session = sessions_.LoadSession(call_id);
  const bool first_attach =
      !session || !session->has_value() || !(*session)->sfu_hint || (*session)->sfu_hint->empty();
  const auto coordinator = CallSessionLogic::SelectEpochCoordinator(joined_ids);
  const bool is_coordinator = coordinator && *coordinator == *local;
  const bool is_initiator = !initiator_identity.empty() && initiator_identity == *local;
  // V021: only the initiator picks the hop on first soft-migrate; epoch coordinator handles re-pick.
  // Allowing coordinator||initiator here caused a second SoftMigrate (e.g. Windows as 3rd joiner)
  // that dialed Client contacts and failed with opaque stream-open errors.
  if (first_attach) {
    if (!is_initiator) {
      return {};
    }
  } else if (!is_coordinator) {
    return {};
  }

  auto ranked = RankedMediaHopCandidates();
  if (ranked.empty()) {
    return Error("no media_relay hop candidates");
  }

  std::string local_peer_id;
  if (auto pid = relay_deps_.relay->LocalPeerIdBase58()) {
    local_peer_id = *pid;
  }

  std::string last_err = "all hops failed";
  for (const MeshHopCandidate& hop : ranked) {
    if (!local_peer_id.empty() && hop.peer_id == local_peer_id) {
      last_err = "skip self hop: " + hop.peer_id;
      continue;
    }

    CallSfuAttachDetail attach;
    attach.call_id = call_id;
    attach.hop_peer_id = hop.peer_id;
    attach.hop_multiaddr = hop.multiaddr;
    attach.publisher_stream_id = PublisherStreamIdForLocal();

    if (auto attached = AttachLocalToSfu(call_id, attach); !attached) {
      last_err = attached.error().message;
      continue;
    }

    if (session && session->has_value()) {
      (*session)->sfu_hint = hop.peer_id;
      (void)sessions_.UpsertSession(**session);
    }

    // Peers RequestQuote themselves; do not fan out a consumed quote_id.
    CallSfuAttachDetail fanout = attach;
    fanout.quote_id.clear();
    auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
    if (encoded) {
      (void)host_.TopologyFanOutToJoined(call_id, CallControlType::CallSfuAttach, *encoded,
                                         "Call SFU attach", *local);
    }
    return {};
  }
  return Error(last_err);
}

Roe<void> CallTopologyController::AttachLocalToSfu(const std::string& call_id,
                                                   const CallSfuAttachDetail& attach_in) {
  if (!relay_deps_.relay || !relay_deps_.sessions) {
    return Error("media_relay not available");
  }
  CallSfuAttachDetail attach = attach_in;
  if (attach.hop_peer_id.empty()) {
    return Error("missing hop_peer_id");
  }
  if (auto local_pid = relay_deps_.relay->LocalPeerIdBase58()) {
    if (*local_pid == attach.hop_peer_id) {
      return Error("cannot attach to self as media_relay hop");
    }
  }
  if (attach.hop_multiaddr.empty()) {
    attach.hop_multiaddr = ResolveHopMultiaddr(attach.hop_peer_id);
  }
  if (!attach.hop_multiaddr.empty()) {
    (void)relay_deps_.sessions->RegisterEndpoint(attach.hop_peer_id, attach.hop_multiaddr);
    relay_deps_.sessions->ClearDialBackoff(attach.hop_peer_id);
  }
  if (!relay_deps_.sessions->IsDialable(attach.hop_peer_id)) {
    return Error("hop not dialable");
  }

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  auto joined = sessions_.CountJoined(call_id);
  qreq.participants = joined ? static_cast<int>(*joined) : 2;
  qreq.want_up_bps = CallMediaAdaptation::kDefaultAudioBps + CallMediaAdaptation::kDefaultVideoLoBps;
  qreq.want_down_bps = qreq.want_up_bps * std::max(1, qreq.participants - 1);

  // Always RequestQuote locally. Shared quote_ids from fan-out are already consumed by the
  // soft-migrate picker's AcceptAndAttach.
  auto quote = relay_deps_.relay->RequestQuote(attach.hop_peer_id, qreq, 8000);
  if (!quote || !quote->ok) {
    return Error(quote ? quote->error : quote.error().message);
  }
  const std::string quote_id = quote->quote_id;
  const int64_t a_up_bps = quote->a_up_bps;

  CallAdaptationInput in;
  in.per_user_up_bps = a_up_bps;
  in.camera_user_wants = false;
  media_.ApplyAdaptation(CallMediaAdaptation::Evaluate(in));

  local_publisher_stream_id_ = PublisherStreamIdForLocal();

  auto attach_res = relay_deps_.relay->AcceptAndAttach(
      attach.hop_peer_id, quote_id, call_id, call_id,
      [this](MediaDataFrame frame) {
        CallMediaEngine::SfuPacket pkt;
        pkt.channel_id = frame.channel_id;
        pkt.seq = frame.seq;
        pkt.mark = frame.mark;
        pkt.payload = std::move(frame.payload);
        media_.OnSfuPacket(pkt);
      },
      8000);
  if (!attach_res || !attach_res->ok) {
    return Error(attach_res ? attach_res->error : attach_res.error().message);
  }

  auto participants = sessions_.ListParticipants(call_id);
  if (participants) {
    auto local = host_.TopologyLocalIdentity();
    for (const CallParticipant& p : *participants) {
      if (p.state != CallParticipantState::Joined) {
        continue;
      }
      if (local && p.identity == *local) {
        continue;
      }
      uint32_t h = 2166136261u;
      for (unsigned char c : p.identity) {
        h ^= c;
        h *= 16777619u;
      }
      const uint32_t stream = h == 0 ? 1u : h;
      (void)relay_deps_.relay->Subscribe(stream, 0);
      (void)relay_deps_.relay->Subscribe(stream, 1);
    }
  }

  const uint32_t pub = local_publisher_stream_id_;
  host_.TopologyNoteMediaAttempted(call_id);
  host_.TopologyBindMediaCallId(call_id);
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

  sfu_attached_ = true;
  awaiting_sfu_recovery_ = false;
  host_.TopologyClearMediaPeerIdentity();
  ClearSfuAttachWait();
  RefreshAdaptation(call_id);
  return {};
}

void CallTopologyController::TryRecoverViaSfu(const std::string& call_id) {
  if (sfu_attached_ && media_.IsSfuMode()) {
    return;
  }
  if (soft_migrate_in_flight_ || (!sfu_attach_wait_call_id_.empty() && sfu_attach_wait_call_id_ == call_id)) {
    // Already migrating / waiting — do not stack another SoftMigrate (PC teardown ICE race).
    return;
  }
  awaiting_sfu_recovery_ = true;
  BeginSfuAttachWait(call_id);
  const uint64_t gen = ++migrate_generation_;
  soft_migrate_in_flight_ = true;
  BrowserThread::PostTask(BrowserThreadId::IO, [this, call_id, gen]() {
    Roe<void> migrated = MaybeSoftMigrateToSfu(call_id);
    const bool attached = sfu_attached_ && media_.IsSfuMode();
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, migrated, attached, gen]() {
      if (gen != migrate_generation_) {
        return; // superseded
      }
      soft_migrate_in_flight_ = false;
      if (attached || (sfu_attached_ && media_.IsSfuMode())) {
        awaiting_sfu_recovery_ = false;
        ClearSfuAttachWait();
        host_.TopologyNotifyRingChanged();
        return;
      }
      if (!migrated) {
        awaiting_sfu_recovery_ = false;
        const std::string msg =
            migrated.error().message.empty()
                ? "No media relay available — group call needs a media_relay hop"
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
    const uint64_t gen = ++migrate_generation_;
    soft_migrate_in_flight_ = true;
    BrowserThread::PostTask(BrowserThreadId::IO, [this, call_id, attach, gen]() {
      Roe<void> ok = AttachLocalToSfu(call_id, attach);
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, ok, gen]() {
        if (gen != migrate_generation_) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (!ok) {
          if (sfu_attached_ && media_.IsSfuMode()) {
            host_.TopologyNotifyRingChanged();
            return;
          }
          log().warning << "AttachLocalToSfu (invite hint) failed: " << ok.error().message;
          host_.TopologySetLastMediaError(ok.error().message);
          ClearSfuAttachWait();
          (void)host_.TopologyLeaveCall(call_id);
        }
        host_.TopologyNotifyRingChanged();
      });
    });
    return true;
  }
  if (CallMediaTopology::ShouldUseMediaRelay(n_joined)) {
    host_.TopologyNoteMediaAttempted(call_id);
    BeginSfuAttachWait(call_id);
    const uint64_t gen = ++migrate_generation_;
    soft_migrate_in_flight_ = true;
    BrowserThread::PostTask(BrowserThreadId::IO, [this, call_id, gen]() {
      Roe<void> mig = MaybeSoftMigrateToSfu(call_id);
      const bool attached = sfu_attached_;
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, mig, attached, gen]() {
        if (gen != migrate_generation_) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (sfu_attached_ && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          host_.TopologyNotifyRingChanged();
          return;
        }
        if (!mig) {
          log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
          host_.TopologySetLastMediaError(mig.error().message);
          ClearSfuAttachWait();
          (void)host_.TopologyLeaveCall(call_id);
        } else if (!attached && !sfu_attached_) {
          // Non-initiator: wait for CallSfuAttach.
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
    host_.TopologyNoteMediaAttempted(call_id);
    BeginSfuAttachWait(call_id);
    const uint64_t gen = ++migrate_generation_;
    soft_migrate_in_flight_ = true;
    BrowserThread::PostTask(BrowserThreadId::IO, [this, call_id, joiner_identity, gen]() {
      Roe<void> mig = MaybeSoftMigrateToSfu(call_id);
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, joiner_identity, mig, gen]() {
        if (gen != migrate_generation_) {
          return;
        }
        soft_migrate_in_flight_ = false;
        if (sfu_attached_ && media_.IsSfuMode()) {
          ClearSfuAttachWait();
          host_.TopologyNotifyRingChanged();
          return;
        }
        if (!mig) {
          log().warning << "MaybeSoftMigrateToSfu failed: " << mig.error().message;
          EjectParticipantAfterMigrateFailure(
              call_id, joiner_identity,
              mig.error().message.empty()
                  ? "No media relay available — group call needs a media_relay hop"
                  : mig.error().message);
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

Roe<void> CallTopologyController::OnInboundSfuAttach(const std::string& call_id,
                                                     const CallSfuAttachDetail& attach) {
  BeginSfuAttachWait(call_id);
  const uint64_t gen = ++migrate_generation_;
  soft_migrate_in_flight_ = true;
  BrowserThread::PostTask(BrowserThreadId::IO, [this, call_id, attach, gen]() {
    Roe<void> ok = AttachLocalToSfu(call_id, attach);
    BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, ok, gen]() {
      if (gen != migrate_generation_) {
        return;
      }
      soft_migrate_in_flight_ = false;
      if (!ok) {
        if (sfu_attached_ && media_.IsSfuMode()) {
          host_.TopologyNotifyRingChanged();
          return;
        }
        host_.TopologySetLastMediaError(ok.error().message);
        log().warning << "AttachLocalToSfu (inbound) failed: " << ok.error().message;
        ClearSfuAttachWait();
        if (!media_.IsActive() || media_.ActiveCallId() != call_id) {
          (void)host_.TopologyLeaveCall(call_id);
        }
      }
      host_.TopologyNotifyRingChanged();
    });
  });
  return {};
}

} // namespace pbr
