#include "feature/calls/CallMediaBridge.h"
#include "domain/messaging/CallTxOnlyEscalateLogic.h"

#include "foundation/i18n/LocalizationService.h"
#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Answerer waits for offerer dial; offerer retries can take ~60s — keep chrome aligned. */
constexpr int64_t kMeshConnectTimeoutMs = 75000;
/** Cover long PollInbox HTTP + offerer MediaKey send/resend window. */
constexpr int kMediaKeyInboxPollRounds = 90;
/** Rate-limit PeerId→relay unknown drops (PreferLocal / non-contact dogfood). */
std::atomic<uint32_t> g_inbound_unmapped_audio_drops{0};

} // namespace

CallMediaBridge::CallMediaBridge(CallMediaHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             ICallMediaTransport& direct, IDialRegistry* dial,
                                             ICircuitHopReach* circuit_reach)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct),
      reach_(dial, circuit_reach), connect_(direct, reach_),
      media_key_inbox_poll_rounds_(kMediaKeyInboxPollRounds) {
  redirectLogger("CallMediaBridge");
  alive_ = std::make_shared<std::atomic<bool>>(true);

  connect_.SetInboundPorts(MakeInboundPorts());
}

CallMediaInboundPorts CallMediaBridge::MakeInboundPorts() {
  // Worker hop (transport inbound handler): stores are thread-safe; no bridge UI state here.
  CallMediaInboundPorts ports;
  ports.session_open = [this](const std::string& call_id) {
    auto session = sessions_.LoadSession(call_id);
    return session && session->has_value() && (*session)->state != CallSessionState::Ended;
  };
  ports.load_key = [this](const std::string& call_id, const uint32_t epoch) -> std::optional<ByteVector> {
    if (auto key = media_keys_.LoadEpochKey(call_id, epoch); key && key->has_value()) {
      return **key;
    }
    return std::nullopt;
  };
  // Offerer often dials before the relay delivers CallMediaKey — keep inbox sync running.
  ports.request_key = [this](const std::string& /*call_id*/) { host_.P2pRequestInboxSync(); };
  ports.on_accepted = [this](const CallMediaInboundHello& hello) {
    // Identity binding reads / writes bridge state — UI thread. Posted ahead of any media or
    // connected callback of this bundle (FIFO), so frames never see a stale stream id.
    AppRuntime::PostUI([this, call_id = hello.call_id, peer_id = hello.peer_id]() {
      BindInboundPeer(call_id, peer_id);
    });
    return MakeBundleCallbacks(hello.call_id, /*fixed_stream=*/0, "Inbound call-media");
  };
  return ports;
}

void CallMediaBridge::BindInboundPeer(const std::string& call_id, const std::string& inbound_peer_id) {
  // Prefer call-roster Account ID for PublisherStreamIdForIdentity. The hello's peer is the mesh
  // PeerId; hashing that yields a different stream_id than SoftMigrate (account:…). Never use
  // P2pPeerIdentityForCall as the mapping — with N≥2 remotes it returns an arbitrary peer
  // (dogfood: Moto PeerId → wrong person stream).
  std::string identity = inbound_peer_id;
  if (inbound_peer_id.rfind("account:", 0) != 0) {
    if (auto mapped = host_.RelayIdentityForMeshPeerId(call_id, inbound_peer_id);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      identity = mapped->value();
    } else if (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0) {
      // Last resort for 1:1 before contacts hydrate — only when dialed peer is the sole remote.
      if (auto sole = host_.P2pPeerIdentityForCall(call_id);
          sole && sole->has_value() && sole->value() == media_peer_identity_) {
        identity = media_peer_identity_;
      }
    } else if (!pending_answerer_peer_.empty() && pending_answerer_peer_.rfind("account:", 0) == 0) {
      identity = pending_answerer_peer_;
    }
  }
  if (!identity.empty() && identity.rfind("account:", 0) == 0) {
    media_peer_identity_ = identity;
    inbound_remote_stream_.store(PublisherStreamIdForIdentity(identity), std::memory_order_release);
    if (!inbound_peer_id.empty() && inbound_peer_id != identity) {
      log().info << "Inbound call-media mapped PeerId→account stream identity peer_id=" << inbound_peer_id
                 << " account=" << identity;
    }
  } else {
    // Do not hash PeerId into a mixer track — SoftMigrate uses Account stream ids. Defer until
    // BeginSession / CallAccept teaches PeerId→Account (moto contact often lacks peer_id).
    inbound_remote_stream_.store(0, std::memory_order_release);
    log().warning << "Inbound call-media stream identity not account: peer_id="
                  << (inbound_peer_id.empty() ? "(empty)" : inbound_peer_id)
                  << " — deferring on_audio stream_id until Account identity known";
  }
  inbound_deferred_peer_id_ = (inbound_peer_id.rfind("account:", 0) == 0) ? std::string{} : inbound_peer_id;
}

CallMediaDirectCallbacks CallMediaBridge::MakeBundleCallbacks(const std::string& call_id,
                                                              const uint32_t fixed_stream,
                                                              const char* label) {
  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [this, call_id, label]() {
    AppRuntime::PostUI([this, call_id, label]() {
      log().info << label << " connected call_id=" << call_id;
      CommitDirectConnected(call_id);
    });
  };
  cbs.on_media = [this, call_id, fixed_stream](uint8_t channel, uint32_t seq, uint8_t mark,
                                               const std::vector<uint8_t>& payload) {
    DeliverDirectMedia(call_id, fixed_stream, channel, seq, mark, payload);
  };
  cbs.on_failed = [this, call_id](const std::string& reason) {
    AppRuntime::PostUI([this, call_id, reason]() { OnBundleFailed(call_id, reason); });
  };
  return cbs;
}

void CallMediaBridge::OnBundleFailed(const std::string& call_id, const std::string& reason) {
  if (media_.ActiveCallId() != call_id) {
    return;
  }
  // Ignore a late fail when another bundle already carries media.
  if (DirectMediaReady() && media_.IsConnected()) {
    return;
  }
  // SoftMigrate StartSfu swaps send to media_relay but leaves the 1:1 stream until
  // ReleaseDirectTransport; peer teardown must not flip ConnectFailed over live SFU.
  // IsSfuMode() is also true for 1:1 capture — require media_relay attach.
  if (host_.P2pIsSfuAttached() && media_.IsConnected()) {
    log().info << "Ignoring call-media fail after SoftMigrate/SFU call_id=" << call_id << " reason=" << reason;
    direct_.Detach();
    ClearMeshConnectFailed();
    if (arming_.on_connected) {
      arming_.on_connected(call_id);
    }
    host_.P2pNotifyRingChanged();
    return;
  }
  // PreferLocal ReleaseDirect closes 1:1 while capture stays up; CallSfuAttach may still be in
  // flight (dogfood: Moto ConnectFailed when attach lagged ReleaseDirect).
  const bool soft_direct_close =
      reason.find("read_eof") != std::string::npos || reason.find("stream closed") != std::string::npos;
  if (host_.P2pIsAwaitingSfuRecovery() || host_.P2pExpectGroupSfuMigration(call_id) ||
      (soft_direct_close && media_.IsActive() && media_.IsConnected())) {
    log().info << "Ignoring call-media fail while awaiting SFU attach call_id=" << call_id
               << " reason=" << reason;
    host_.P2pNoteExpectSfuAttach(call_id);
    direct_.Detach();
    ClearMeshConnectFailed();
    host_.P2pRequestInboxSync();
    if (arming_.on_connected) {
      arming_.on_connected(call_id);
    }
    host_.P2pNotifyRingChanged();
    return;
  }
  log().warning << "Call-media failed call_id=" << call_id << " reason=" << reason;
  SurfaceConnectFailed(call_id, reason, /*stop_media=*/true);
}

CallMediaBridge::~CallMediaBridge() {
  alive_->store(false, std::memory_order_release);
}

void CallMediaBridge::SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach) {
  reach_.SetDeps(dial, circuit_reach);
}

void CallMediaBridge::SetSeedWarm(std::function<void()> warm) {
  seed_warm_ = std::move(warm);
}

void CallMediaBridge::SetSeedReserve(std::function<void()> reserve) {
  seed_reserve_ = std::move(reserve);
}

void CallMediaBridge::SetSeedParkAwait(PeerReachCoordinator::SeedParkAwait park) {
  reach_.SetSeedParkAwait(std::move(park));
}

std::string CallMediaBridge::MediaPathKind() const {
  // The bound link is the truth: an answerer's inbound leg can ride a relay carrier while the
  // reach loop (and the dialer-only hop registry) think "punched" (dogfood 2026-09-24).
  if (direct_.IsActive() && direct_.ActiveLinkKind() == CallMediaLinkKind::Relayed) {
    return "circuit";
  }
  if (reach_.HasRelayHop(media_peer_identity_)) {
    return "circuit";
  }
  switch (reach_kind_) {
  case PeerLinkKind::Direct:
    return "direct";
  case PeerLinkKind::Punched:
    return "punched";
  case PeerLinkKind::Relayed:
    return "circuit";
  case PeerLinkKind::Unknown:
    break;
  }
  return {};
}

bool CallMediaBridge::DirectMediaReady() const {
  return direct_.Phase() == CallMediaSessionPhase::MediaReady;
}

bool CallMediaBridge::HasActiveDirectStream() const {
  return direct_.IsActive();
}

void CallMediaBridge::SetDirectArmingPorts(CallDirectArmingPorts ports) {
  arming_ = std::move(ports);
}

void CallMediaBridge::SetMediaKeyInboxPollRoundsForTest(const int rounds) {
  media_key_inbox_poll_rounds_ = rounds < 0 ? 0 : rounds;
}

void CallMediaBridge::SetDialWaitBudgetMsForTest(const int budget_ms) {
  reach_.SetDialBudgetMsForTest(budget_ms);
}

void CallMediaBridge::SetConnectAttemptTimeoutMsForTest(const int timeout_ms) {
  connect_.SetAttemptTimeoutMsForTest(timeout_ms);
}

void CallMediaBridge::SetSeatPorts(CallDirectSeatPorts ports) {
  seat_ = std::move(ports);
}

CallDirectPlannerApplyContext CallMediaBridge::BuildDirectPlannerContext(
    const std::string& call_id, const std::string& peer_identity) const {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = !arming_.IsBound() || (arming_.direct_ops_allowed && arming_.direct_ops_allowed());
  ctx.stopping = stopping_.load(std::memory_order_acquire);
  ctx.peer_nonempty = !peer_identity.empty();
  if (!call_id.empty() && media_.IsActive() && media_.ActiveCallId() == call_id &&
      (DirectMediaReady() || media_.IsConnected())) {
    ctx.media_live_same_call = true;
  }
  if (!call_id.empty()) {
    if (auto key = LoadActiveMediaKey(call_id); key) {
      ctx.has_media_key = true;
    }
  }
  return ctx;
}

void CallMediaBridge::SetDirectPlannerPhase(const CallDirectPlannerPhase next,
                                            const CallDirectPlannerEvent ev,
                                            const std::string& call_id) {
  const CallDirectPlannerPhase prev = direct_planner_phase_;
  direct_planner_phase_ = next;
  log().info << "planner=Direct phase=" << CallDirectPlannerPhaseName(prev) << "->"
             << CallDirectPlannerPhaseName(next) << " event=" << CallDirectPlannerEventName(ev)
             << " call_id=" << call_id
             << " connect_gen=" << connect_generation_.load(std::memory_order_acquire);
  if (next == CallDirectPlannerPhase::Connecting || next == CallDirectPlannerPhase::Live ||
      next == CallDirectPlannerPhase::DegradedTxOnly) {
    ArmDirectHealthTimer();
  } else if (next == CallDirectPlannerPhase::Idle || next == CallDirectPlannerPhase::KeyWait) {
    if (next == CallDirectPlannerPhase::Idle) {
      CancelDirectHealthTimer();
    }
  }
}

void CallMediaBridge::Apply(CallDirectPlannerEvent ev, const std::string& call_id,
                            const std::string& peer_identity) {
  const CallDirectPlannerApplyContext ctx = BuildDirectPlannerContext(call_id, peer_identity);
  const CallDirectPlannerPhaseOutcome out =
      DecideCallDirectPlannerPhase(direct_planner_phase_, ev, ctx);
  if (out.decision == CallDirectPlannerDecision::Ignore) {
    log().info << "planner=Direct ignore event=" << CallDirectPlannerEventName(ev)
               << " phase=" << CallDirectPlannerPhaseName(direct_planner_phase_)
               << " call_id=" << call_id << " allows_direct=" << (ctx.allows_direct_path ? 1 : 0);
    return;
  }
  if (out.decision == CallDirectPlannerDecision::Transition && out.next != direct_planner_phase_) {
    SetDirectPlannerPhase(out.next, ev, call_id);
  } else {
    log().info << "planner=Direct keep phase=" << CallDirectPlannerPhaseName(direct_planner_phase_)
               << " event=" << CallDirectPlannerEventName(ev) << " call_id=" << call_id;
  }

  switch (ev) {
  case CallDirectPlannerEvent::TxOnlyGraceExpired:
    if (direct_planner_phase_ == CallDirectPlannerPhase::DegradedTxOnly) {
      std::string peer = peer_identity.empty() ? media_peer_identity_ : peer_identity;
      const std::string cid = call_id.empty() ? media_.ActiveCallId() : call_id;
      if (peer.empty() && !cid.empty()) {
        if (auto resolved = host_.P2pPeerIdentityForCall(cid); resolved && resolved->has_value()) {
          peer = **resolved;
        }
      }
      if (!cid.empty() && !peer.empty()) {
        if (arming_.report_progress) {
          arming_.report_progress(CallDirectPlannerPhase::DegradedTxOnly, cid);
        }
        EscalateTxOnlyViaCircuit(cid, peer);
      }
    }
    break;
  case CallDirectPlannerEvent::CircuitEscalated:
    break;
  case CallDirectPlannerEvent::KeyTimeout:
  case CallDirectPlannerEvent::ConnectFailed:
  case CallDirectPlannerEvent::ConnectSucceeded:
  case CallDirectPlannerEvent::ScheduleOfferer:
  case CallDirectPlannerEvent::ScheduleAnswerer:
  case CallDirectPlannerEvent::KeyReady:
  case CallDirectPlannerEvent::ReleaseTransport:
  case CallDirectPlannerEvent::Stop:
    break;
  }
}

void CallMediaBridge::CancelDirectHealthTimer() {
  if (direct_health_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(direct_health_timer_id_);
    direct_health_timer_id_ = 0;
  }
}

void CallMediaBridge::ArmDirectHealthTimer() {
  CancelDirectHealthTimer();
  // ~1s tick while Connecting/Live/Degraded — primary connect health / TX-only path (no UI poll).
  direct_health_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(
      std::chrono::milliseconds(1000), [this]() {
        AppRuntime::PostUI([this]() { OnDirectHealthTimerFire(); });
      });
}

void CallMediaBridge::CancelReserveRenewal() {
  if (reserve_renew_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(reserve_renew_timer_id_);
    reserve_renew_timer_id_ = 0;
  }
}

void CallMediaBridge::ArmReserveRenewal() {
  CancelReserveRenewal();
  if (!seed_reserve_) {
    return;
  }
  // Reservations are a 15 s lease (StartReserve TTL) and nothing else renews them; a caller that
  // dials after the lease lapses finds no park (dogfood 2026-09-24 "Couldn't connect"). 10 s keeps
  // one lease overlapping the next, so the relay link also stays hot throughout.
  reserve_renew_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(
      std::chrono::milliseconds(reserve_renew_interval_ms_), [this]() {
        AppRuntime::PostUI([this]() { OnReserveRenewFire(); });
      });
}

void CallMediaBridge::OnReserveRenewFire() {
  if (stopping_.load(std::memory_order_acquire) || media_call_id_.empty()) {
    CancelReserveRenewal();
    return;
  }
  log().info << "circuit reserve renew call_id=" << media_call_id_;
  if (seed_reserve_) {
    seed_reserve_();
  }
}

void CallMediaBridge::OnDirectHealthTimerFire() {
  if (direct_planner_phase_ == CallDirectPlannerPhase::Idle ||
      direct_planner_phase_ == CallDirectPlannerPhase::KeyWait ||
      direct_planner_phase_ == CallDirectPlannerPhase::Stopping) {
    CancelDirectHealthTimer();
    return;
  }
  PollMeshConnectHealth();
}

void CallMediaBridge::CommitDirectConnected(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  Apply(CallDirectPlannerEvent::ConnectSucceeded, call_id, media_peer_identity_);
  if (direct_planner_phase_ != CallDirectPlannerPhase::Live &&
      direct_planner_phase_ != CallDirectPlannerPhase::DegradedTxOnly) {
    // Late Connect after Leave / wrong Status — do not advance chrome.
    return;
  }
  // Capture may lag the stream (inbound before BeginSession). Still advance phase so chrome
  // can leave Calling/Connecting once StartSfu runs; keep_inbound / on_connected re-enter.
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.SetConnectionState("connected");
  }
  ClearMeshConnectFailed();
  if (direct_connected_at_ms_ <= 0) {
    direct_connected_at_ms_ = util::NowUnixMs();
  }
  // V036 Phase 2: seat Live is the chrome Connected gate — DirectConnected alone is signaling.
  // Prefer Live only when the direct stream is actually up (not StartSfu alone).
  if (seat_.IsBound() && media_.IsActive() && media_.ActiveCallId() == call_id &&
      (DirectMediaReady() || host_.P2pIsSfuAttached())) {
    seat_.note_live(call_id);
  }
  if (arming_.on_connected) {
    arming_.on_connected(call_id);
  }
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::DeliverDirectMedia(const std::string& call_id, const uint32_t fixed_stream,
                                         uint8_t channel, uint32_t seq, uint8_t mark,
                                         const std::vector<uint8_t>& payload) {
  AppRuntime::PostUI([this, call_id, fixed_stream, channel, seq, mark, payload]() {
    if (!media_.IsActive() || media_.ActiveCallId() != call_id) {
      return;
    }
    if (host_.P2pIsSfuAttached()) {
      return;
    }
    // Outbound bundles know the dialed identity; inbound ones use the bound (or deferred) stream.
    uint32_t remote_stream =
        fixed_stream != 0 ? fixed_stream : inbound_remote_stream_.load(std::memory_order_acquire);
    if (remote_stream == 0) {
      std::string account = media_peer_identity_;
      const std::string deferred = inbound_deferred_peer_id_;
      if (account.rfind("account:", 0) != 0 && !deferred.empty()) {
        if (auto mapped = host_.RelayIdentityForMeshPeerId(call_id, deferred);
            mapped && mapped->has_value() && !mapped->value().empty()) {
          account = mapped->value();
          media_peer_identity_ = account;
        }
      }
      if (account.rfind("account:", 0) == 0) {
        remote_stream = PublisherStreamIdForIdentity(account);
        inbound_remote_stream_.store(remote_stream, std::memory_order_release);
        log().info << "Inbound call-media rebound stream_id=" << remote_stream
                   << " account=" << account << " call_id=" << call_id;
      } else {
        const uint32_t n = g_inbound_unmapped_audio_drops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 50) == 0) {
          log().warning << "Inbound call-media drop: PeerId→relay unknown"
                        << " peer_id=" << (deferred.empty() ? "(empty)" : deferred)
                        << " media_peer=" << (media_peer_identity_.empty() ? "(empty)" : media_peer_identity_)
                        << " call_id=" << call_id << " drops=" << n;
        }
        return;
      }
    }
    CallMediaEngine::SfuPacket pkt;
    pkt.stream_id = remote_stream;
    pkt.channel_id = channel;
    pkt.seq = seq;
    pkt.mark = mark;
    pkt.payload = payload;
    media_.OnSfuPacket(pkt);
  });
}

bool CallMediaBridge::IsMeshConnectFailed() const {
  return mesh_connect_failed_;
}

bool CallMediaBridge::MeshConnectMissingMic() const {
  return mesh_connect_missing_mic_;
}

void CallMediaBridge::ClearMeshConnectFailed() {
  mesh_connect_failed_ = false;
  mesh_connect_missing_mic_ = false;
}

void CallMediaBridge::PollMeshConnectHealth() {
  if (mesh_connect_failed_ || host_.P2pIsAwaitingSfuRecovery() || !media_.IsActive() || !media_.IsSfuMode()) {
    return;
  }
  // ConnectOffererWithRetry runs on a worker thread — do not UI-timeout while it is dialing.
  if (connect_.InFlight()) {
    return;
  }
  if (media_.IsConnected() && DirectMediaReady()) {
    ClearMeshConnectFailed();
    MaybeEscalateTxOnlyDirect();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  // Stream can be up while StartSfu→"connecting" left IsConnected false (chrome stuck).
  if (DirectMediaReady()) {
    ClearMeshConnectFailed();
    if (media_.IsActive() && !media_.IsConnected()) {
      log().info << "Heal call-media connected from direct stream call_id=" << call_id;
      CommitDirectConnected(call_id);
    }
    MaybeEscalateTxOnlyDirect();
    return;
  }
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined >= 3) {
    return;
  }
  const int64_t started = media_.StartedAtMs();
  if (started <= 0) {
    return;
  }
  if (util::NowUnixMs() - started < kMeshConnectTimeoutMs) {
    return;
  }
  log().warning << "Mesh connect timeout call_id=" << call_id;
  mesh_connect_missing_mic_ = !media_.HasLocalCapture();
  SurfaceConnectFailed(call_id, "mesh connect timeout", /*stop_media=*/true);
}

void CallMediaBridge::MaybeEscalateTxOnlyDirect() {
  std::string peer = media_peer_identity_;
  const std::string call_id = media_.ActiveCallId();
  if (peer.empty() && !call_id.empty()) {
    if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  const auto snap = media_.HealthSnapshot();
  CallTxOnlyEscalateDecisionInput in;
  in.already_done = tx_only_escalation_done_;
  in.sfu_attached = host_.P2pIsSfuAttached();
  in.stopping = stopping_.load();
  // Bound-link truth: never "escalate via circuit" when media already rides a relay carrier.
  in.media_path_kind = MediaPathKind();
  in.has_circuit_reach = reach_.HasCircuitReach();
  in.direct_active = direct_.IsActive();
  in.active_call_id = call_id;
  in.media_call_id = media_call_id_;
  in.rx_audio_frames = snap.rx_audio_frames;
  in.tx_audio_frames = snap.tx_audio_frames;
  in.direct_connected_at_ms = direct_connected_at_ms_;
  in.now_ms = util::NowUnixMs();
  in.peer_nonempty = !peer.empty();
  if (!ShouldEscalateTxOnlyDirect(in)) {
    return;
  }
  tx_only_escalation_done_ = true;
  log().warning << "Call-media TX-only on path=" << (in.media_path_kind.empty() ? "unknown" : in.media_path_kind)
                << " — escalate via circuit call_id=" << call_id << " peer=" << peer
                << " tx_frames=" << snap.tx_audio_frames;
  Apply(CallDirectPlannerEvent::TxOnlyGraceExpired, call_id, peer);
}

void CallMediaBridge::EscalateTxOnlyViaCircuit(const std::string& call_id, const std::string& peer) {
  Apply(CallDirectPlannerEvent::CircuitEscalated, call_id, peer);
  if (seat_.IsBound()) {
    seat_.note_connecting(call_id);
  }
  media_.SetConnectionState("connecting");
  reach_kind_ = PeerLinkKind::Unknown;
  direct_connected_at_ms_ = 0;
  reach_.ForgetPath(peer);
  // Re-open transport under circuit without full engine Stop (keep capture).
  AbortConnectSequence();
  force_circuit_ensure_ = true;  // consumed by the next reach (BuildReachRequest)
  direct_.Detach();
  AppRuntime::PostUI([this, call_id, peer]() {
    if (stopping_.load() || media_.ActiveCallId() != call_id) {
      return;
    }
    log().info << "TX-only escalate BeginSession role=" << (session_offerer_ ? "offerer" : "answerer")
               << " call_id=" << call_id;
    if (auto started = BeginSession(call_id, peer, session_offerer_); !started) {
      log().warning << "TX-only escalate BeginSession failed: " << started.error().message;
      SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
    }
  });
}

bool CallMediaBridge::ShouldUseMeshForPeer(const std::string& /*peer_identity*/) const {
  return reach_.Available();
}

void CallMediaBridge::AbortConnectSequence() {
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  connect_.Abort();
}

void CallMediaBridge::SurfaceConnectFailed(const std::string& call_id, const std::string& err,
                                           const bool stop_media) {
  if (!err.empty()) {
    host_.P2pSetLastMediaError(err);
  }
  // Stop late EnsureViaCircuit / StartBridge before chrome refresh (dogfood SIGSEGV after give-up).
  reach_.AbortCircuitAttempts();
  if (stop_media && (media_.IsActive() || media_.IsSfuMode())) {
    // StopMeshMedia clears mesh_connect_failed_ for Leave hygiene — re-assert below.
    StopMeshMedia(call_id);
  } else if (!media_peer_identity_.empty()) {
    reach_.AbandonDial(media_peer_identity_);
  }
  if (seat_.note_failed) {
    seat_.note_failed(call_id);
  }
  Apply(CallDirectPlannerEvent::ConnectFailed, call_id, media_peer_identity_);
  if (arming_.on_connect_failed) {
    arming_.on_connect_failed(call_id);
  }
  mesh_connect_failed_ = true;
  host_.P2pNotifyRingChanged();
}

PeerReachRequest CallMediaBridge::BuildReachRequest(const CallMediaDirectConnectParams& params) {
  PeerReachRequest request;
  // Circuit / punch / OpenChannel keys are Amp PeerIds. Invite/Accept may pass account: —
  // resolve here (call roster knowledge); keep the account as an alias the mesh may know too
  // (hard-lab / dogfood NAT: "endpoint not registered").
  std::string reach_key = params.peer_key;
  if (params.peer_key.rfind("account:", 0) == 0) {
    if (auto mapped = host_.MeshPeerIdForAccount(params.peer_key);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      reach_key = mapped->value();
      log().info << "CallMedia reach account→PeerId account=" << params.peer_key
                 << " peer_id=" << reach_key;
    }
  }
  request.keys.push_back(reach_key);
  if (reach_key != params.peer_key) {
    request.keys.push_back(params.peer_key);
  }
  // The offerer reaches; the answerer awaits the offerer's link (invite/accept is the agreement).
  request.mode = params.offerer ? PeerReachMode::Reach : PeerReachMode::Await;
  request.exclude_direct = std::exchange(force_circuit_ensure_, false);
  return request;
}

CallMediaConnectHooks CallMediaBridge::MakeConnectHooks(const std::string& call_id) {
  CallMediaConnectHooks hooks;
  hooks.before_attempt = [this](const CallMediaDirectConnectParams& params) {
    if (!params.offerer) {
      return;
    }
    // Answerer often defers waiting on the relay — resend the epoch key each attempt. MediaKey is
    // addressed by roster account:, not the mesh dial PeerId.
    const std::string key_peer =
        (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0)
            ? media_peer_identity_
            : params.peer_key;
    host_.P2pResendMediaKey(params.call_id, key_peer);
  };
  hooks.on_link_ready = [this](const PeerLinkKind kind) { reach_kind_ = kind; };
  hooks.on_finished = [this, call_id](Roe<void> result) {
    if (DirectMediaReady()) {
      CommitDirectConnected(call_id);
      return;
    }
    if (!result) {
      SurfaceConnectFailed(call_id, result.error().message, /*stop_media=*/true);
    }
  };
  return hooks;
}

Roe<ByteVector> CallMediaBridge::LoadActiveMediaKey(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }
  auto key = media_keys_.LoadEpochKey(call_id, (*session)->media_epoch);
  if (!key) {
    return key.error();
  }
  if (!key->has_value()) {
    return Error("call media key not ready");
  }
  return **key;
}

Roe<void> CallMediaBridge::BeginSession(const std::string& call_id, const std::string& peer_identity,
                                              bool offerer) {
  if (arming_.IsBound() && arming_.direct_ops_allowed && !arming_.direct_ops_allowed()) {
    log().info << "BeginSession skipped (direct not armed) call_id=" << call_id
               << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
    return Error("direct path not armed");
  }
  auto key = LoadActiveMediaKey(call_id);
  if (!key) {
    log().warning << "BeginSession LoadActiveMediaKey failed call_id=" << call_id
                  << " role=" << (offerer ? "offerer" : "answerer")
                  << " err=" << key.error().message;
    return key.error();
  }

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }

  if (media_call_id_ != call_id) {
    tx_only_escalation_done_ = false;
  }
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  media_peer_identity_ = peer_identity;
  session_offerer_ = offerer;
  direct_connected_at_ms_ = 0;
  if (peer_identity.rfind("account:", 0) == 0) {
    const uint32_t stream = PublisherStreamIdForIdentity(peer_identity);
    inbound_remote_stream_.store(stream, std::memory_order_release);
  }
  audio_seq_.store(0);
  ClearMeshConnectFailed();
  if (!offerer) {
    pending_answerer_call_id_.clear();
    key_wait_gen_.fetch_add(1, std::memory_order_acq_rel);
    pending_answerer_peer_.clear();
  }

  // Answerer may already have accepted inbound hello (key landed first). Offerer must NOT Detach:
  // answerer-only dial often negotiates the stream before BeginSession runs on the offerer
  // (dogfood: Detach raced inbound → phone never read hello → "Failed to read call-media frame header").
  const bool keep_inbound = direct_.IsActive();
  const bool restarting =
      media_.IsActive() || connect_.InFlight();
  if (media_.IsActive()) {
    media_.Stop();
  }
  if (!offerer && !keep_inbound) {
    direct_.Detach();
  }
  // Abort in-flight Connect from a prior BeginSession (Kick thrash used to Stop without bumping).
  if (restarting && !keep_inbound) {
    AbortConnectSequence();
  }

  const uint32_t media_epoch = (*session)->media_epoch;
  const ByteVector media_key = *key;

  log().info << "CallMedia BeginSession role=" << (offerer ? "offerer" : "answerer")
             << " call_id=" << call_id << " peer=" << peer_identity << " epoch=" << media_epoch
             << " keep_inbound=" << (keep_inbound ? 1 : 0);

  // Both roles park on org seed: answerer reverse-dial makes the *offerer* the circuit target
  // (dogfood 997c1c6f). Reserve keeps a Connected PeerLink so peer-id-only StartBridge can
  // EnsureAssociation without dialing into NAT / requiring a dial-book MA. Single entry —
  // ReserveOnBootstrapSeeds registers + associates (do not also Warm in parallel).
  if (seed_reserve_) {
    seed_reserve_();
    ArmReserveRenewal();
  } else if (seed_warm_) {
    seed_warm_();
  }

  media_.SetOnStateChanged([this](const std::string& state) {
    if (state == "connected") {
      ClearMeshConnectFailed();
      host_.P2pNotifyRingChanged();
      return;
    }
    host_.P2pNotifyRingChanged();
  });

  const uint64_t send_gen = connect_generation_.load(std::memory_order_acquire);
  CallMediaSeat::Token seat_token;
  if (seat_.IsBound()) {
    seat_token = seat_.acquire(call_id);
    if (!seat_.allows_path_op(seat_token)) {
      return Error("media seat token rejected for direct path");
    }
  }
  if (auto started = media_.StartSfu(call_id, [this, send_gen](const CallMediaEngine::SfuPacket& pkt) {
        if (pkt.channel_id > kCallMediaChannelVideoLo) {
          return;
        }
        // SoftMigrate ReleaseDirectTransport bumps connect_generation_ before Detach.
        if (connect_generation_.load(std::memory_order_acquire) != send_gen) {
          return;
        }
        const uint32_t seq =
            pkt.channel_id == 0 ? (audio_seq_.fetch_add(1) + 1) : pkt.seq;
        (void)direct_.SendMedia(static_cast<uint8_t>(pkt.channel_id), pkt.payload, seq, pkt.mark);
      });
      !started) {
    return started;
  }
  if (seat_.IsBound()) {
    seat_.note_start(call_id);
    seat_.note_path(CallMediaSeat::PathKind::Direct);
    // Duplex Live only after direct stream (CommitDirectConnected) — not StartSfu alone.
    if (!DirectMediaReady()) {
      seat_.note_connecting(call_id);
    }
  }

  // StartSfu marks connected immediately for SFU capture; 1:1 chrome waits on the direct stream.
  if (!DirectMediaReady()) {
    media_.SetConnectionState("connecting");
  }

  if (keep_inbound && DirectMediaReady()) {
    log().info << "Media started with existing inbound stream call_id=" << call_id
                  << " role=" << (offerer ? "offerer" : "answerer");
    CommitDirectConnected(call_id);
    return {};
  }
  if (keep_inbound) {
    // Inbound bundle still in hello / AwaitingMedia: keep it (no Detach) and let the connect
    // sequence join it — ConnectAsync → StartLeg adopts and succeeds only at MediaReady.
    log().info << "Media start joins inbound stream still in handshake call_id=" << call_id
               << " role=" << (offerer ? "offerer" : "answerer");
  }

  CallMediaDirectConnectParams params;
  params.peer_key = peer_identity;
  params.call_id = call_id;
  params.media_epoch = media_epoch;
  params.media_key = media_key;
  params.offerer = offerer;
  // Prefer mesh PeerId for OpenChannel. Account: may be "dialable" via a stale alias while the
  // Connected PeerLink lives under PeerId (dogfood 7bd62: AssociationNotReady forever).
  if (peer_identity.rfind("account:", 0) == 0) {
    // Account → PeerId is roster knowledge (here); which key the mesh can dial is link knowledge.
    if (auto mapped = host_.MeshPeerIdForAccount(peer_identity);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      params.peer_key = reach_.PreferDialKey(peer_identity, **mapped);
    } else {
      log().info << "CallMedia dial key account (no PeerId map) account=" << peer_identity;
    }
  }

  CallMediaDirectCallbacks cbs =
      MakeBundleCallbacks(call_id, PublisherStreamIdForIdentity(peer_identity), "Call-media");

  // V049 / B31: both roles dial immediately (simultaneous open). CallMediaDirect claims one
  // stream and elects under A026; inbound still wins if it lands first (keep_inbound).
  CallMediaConnectRequest request;
  request.reach = BuildReachRequest(params);
  request.params = std::move(params);
  request.callbacks = std::move(cbs);
  connect_.Start(std::move(request), MakeConnectHooks(call_id));

  mesh_connect_missing_mic_ = false;
  host_.P2pNotifyRingChanged();
  return {};
}

Roe<void> CallMediaBridge::StartMediaAsOfferer(const std::string& call_id,
                                                     const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, true);
}

Roe<void> CallMediaBridge::StartMediaAsAnswerer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, false);
}

void CallMediaBridge::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                        const std::string& peer_identity) {
  // Mark before UI hop so CallController orphan auto-Leave cannot race CallAccept→Active.
  media_attempted_calls_.insert(call_id);
  AppRuntime::PostUI([this, call_id, peer_identity]() {
    Apply(CallDirectPlannerEvent::ScheduleOfferer, call_id, peer_identity);
    if (direct_planner_phase_ != CallDirectPlannerPhase::Arming &&
        direct_planner_phase_ != CallDirectPlannerPhase::Connecting &&
        direct_planner_phase_ != CallDirectPlannerPhase::KeyWait) {
      return;
    }
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value()) {
      log().info << "StartMediaAsOfferer skip: no session call_id=" << call_id;
      Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
      return;
    }
    if ((*session)->state == CallSessionState::Ended) {
      log().info << "StartMediaAsOfferer skip: session ended call_id=" << call_id;
      Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      log().info << "StartMediaAsOfferer skip: media already active call_id=" << call_id;
      return;
    }
    log().info << "StartMediaAsOfferer UI enter call_id=" << call_id << " peer=" << peer_identity;
    SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleOfferer,
                          call_id);
    if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsOfferer failed: " << started.error().message;
      SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
    }
  });
}

void CallMediaBridge::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                         const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  auto run = [this, call_id, peer_identity]() {
    log().info << "ScheduleStartMediaAsAnswerer UI enter call_id=" << call_id
               << " peer=" << peer_identity
               << " on_ui=" << (AppRuntime::CurrentlyOnUI() ? 1 : 0);
    // Re-arm Direct before Apply so product AllowsDirectPath for Schedule.
    if (arming_.IsBound() && arming_.direct_ops_allowed && !arming_.direct_ops_allowed()) {
      log().info << "ScheduleStartMediaAsAnswerer request_direct_arming call_id=" << call_id
                 << " arming=" << (arming_.arming_debug_name ? arming_.arming_debug_name() : "?");
      if (arming_.request_direct_arming) {
        arming_.request_direct_arming(call_id);
      }
    }
    Apply(CallDirectPlannerEvent::ScheduleAnswerer, call_id, peer_identity);
    if (direct_planner_phase_ != CallDirectPlannerPhase::Arming &&
        direct_planner_phase_ != CallDirectPlannerPhase::Connecting &&
        direct_planner_phase_ != CallDirectPlannerPhase::KeyWait) {
      return;
    }
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      log().info << "ScheduleStartMediaAsAnswerer skip (no active session) call_id=" << call_id
                 << " has_session=" << (session && session->has_value() ? 1 : 0)
                 << " state="
                 << (session && session->has_value() ? static_cast<int>((*session)->state) : -1);
      Apply(CallDirectPlannerEvent::Stop, call_id, peer_identity);
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      log().info << "ScheduleStartMediaAsAnswerer skip (already active) call_id=" << call_id
                 << " sfu_mode=" << (media_.IsSfuMode() ? 1 : 0)
                 << " direct=" << (direct_.IsActive() ? 1 : 0);
      return;
    }
    auto key = LoadActiveMediaKey(call_id);
    if (!key) {
      // V015: epoch-1 key is sent by offerer on CallAccept — defer until it lands.
      SetDirectPlannerPhase(CallDirectPlannerPhase::KeyWait, CallDirectPlannerEvent::ScheduleAnswerer,
                            call_id);
      log().info << "Defer answerer media until CallMediaKey call_id=" << call_id
                    << " reason=" << key.error().message;
      pending_answerer_call_id_ = call_id;
      pending_answerer_peer_ = peer_identity;
      media_attempted_calls_.insert(call_id);
      if (arming_.on_media_deferred) {
        arming_.on_media_deferred(call_id);
      }
      // Accept-time SyncInbox often races the offerer's MediaKey send — keep polling.
      // SyncInbox coalesces via poll_again_; do not assume each Request starts HTTP.
      // The worker must not read pending_answerer_call_id_ (UI-owned string): it watches the
      // key-wait generation instead, bumped whenever the pending answerer changes (TSan).
      const uint64_t key_wait_gen = key_wait_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
      AppRuntime::PostWorkerBackground([this, call_id, key_wait_gen]() {
        const auto superseded = [this, key_wait_gen]() {
          return stopping_.load(std::memory_order_acquire) ||
                 key_wait_gen_.load(std::memory_order_acquire) != key_wait_gen;
        };
        const int rounds = media_key_inbox_poll_rounds_;
        for (int i = 0; i < rounds; ++i) {
          if (superseded()) {
            return;
          }
          host_.P2pRequestInboxSync();
          // Chunked sleep so PrepareForTeardown / Leave can abort without a 1s hang.
          for (int slice = 0; slice < 20; ++slice) {
            if (superseded()) {
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          if (auto deferred_key = LoadActiveMediaKey(call_id); deferred_key) {
            log().info << "Deferred MediaKey found in store — kick start call_id=" << call_id;
            OnMediaKeyReady(call_id);
            return;
          }
        }
        // Surface failure — do not leave chrome stuck in MediaPending forever.
        AppRuntime::PostUI([this, call_id]() {
          if (pending_answerer_call_id_ != call_id) {
            return; // key arrived, Leave, or superseding Accept
          }
          pending_answerer_call_id_.clear();
          key_wait_gen_.fetch_add(1, std::memory_order_acq_rel);
          pending_answerer_peer_.clear();
          const std::string err = Tr("call.error.media_key_timeout");
          log().warning << "Deferred MediaKey wait exhausted call_id=" << call_id
                        << " — ConnectFailed";
          mesh_connect_failed_ = true;
          host_.P2pSetLastMediaError(err);
          Apply(CallDirectPlannerEvent::KeyTimeout, call_id);
          if (arming_.on_connect_failed) {
            arming_.on_connect_failed(call_id);
          }
          host_.P2pNotifyRingChanged();
        });
      });
      return;
    }
    log().info << "ScheduleStartMediaAsAnswerer key ready — BeginSession StartSfu call_id=" << call_id;
    SetDirectPlannerPhase(CallDirectPlannerPhase::Connecting, CallDirectPlannerEvent::ScheduleAnswerer,
                          call_id);
    if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
      SurfaceConnectFailed(call_id, started.error().message, /*stop_media=*/true);
    } else {
      log().info << "answerer StartSfu ok call_id=" << call_id
                 << " direct=" << (direct_.IsActive() ? 1 : 0)
                 << " engine=" << (media_.IsActive() ? 1 : 0);
    }
  };
  // Prefer inline when already on UI (AcceptSucceeded Kick). Worker Accept → PostUIFront.
  if (AppRuntime::CurrentlyOnUI()) {
    run();
    return;
  }
  log().info << "ScheduleStartMediaAsAnswerer queued (PostUIFront) call_id=" << call_id;
  AppRuntime::PostUIFront(std::move(run));
}

void CallMediaBridge::OnMediaKeyReady(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  // Wake inbound hello key-wait (if any) before hopping to UI for deferred answerer start.
  connect_.NotifyKeyAvailable();
  // Hop to UI — inbound CallMediaKey is processed on Browser IO (inside PollInbox).
  AppRuntime::PostUI([this, call_id]() {
    std::string peer = pending_answerer_peer_;
    const bool pending = (pending_answerer_call_id_ == call_id);
    if (!pending) {
      // Key stored for later Accept LoadActiveMediaKey — do NOT auto-start. Late keys from a
      // prior call were starting answerer media on the wrong call_id (Samsung dogfood).
      log().info << "CallMediaKey stored (not deferred yet) call_id=" << call_id;
      return;
    }
    if (peer.empty()) {
      if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
        peer = **resolved;
      }
    }
    log().info << "CallMediaKey ready — starting deferred answerer media call_id=" << call_id;
    pending_answerer_call_id_.clear();
    key_wait_gen_.fetch_add(1, std::memory_order_acq_rel);
    pending_answerer_peer_.clear();
    Apply(CallDirectPlannerEvent::KeyReady, call_id, peer);
    if (arming_.on_media_key_ready) {
      arming_.on_media_key_ready(call_id);
    }
    if (!peer.empty()) {
      ScheduleStartMediaAsAnswerer(call_id, peer);
    }
  });
}

void CallMediaBridge::StopMeshMedia(const std::string& call_id) {
  if (AppRuntime::CurrentlyOnUI()) {
    StopMeshMediaOnUi(call_id);
    return;
  }
  // Bridge state, the connect sequence and CallMediaEngine::Stop (SDL capture — CALLS.md) are
  // UI-thread only: hop the whole stop, not just the engine part. Front of the queue — Leave must
  // not sit behind chrome refresh while capture feeds a detached SFU (zombie TX + red reconnecting
  // on the next call). A StartSfu that lands first (AcceptInvite SoftMigrate / CallSfuAttach /
  // a new call) makes this stop stale — it must not tear down the newer session.
  const uint64_t session_gen = media_.MediaSessionGeneration();
  AppRuntime::PostUIFront([this, alive = alive_, call_id, session_gen]() {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    if (media_.MediaSessionGeneration() != session_gen) {
      log().info << "StopMeshMedia skip stale stop leave=" << call_id << " posted_gen=" << session_gen
                 << " now=" << media_.MediaSessionGeneration();
      return;
    }
    StopMeshMediaOnUi(call_id);
  });
}

void CallMediaBridge::StopMeshMediaOnUi(const std::string& call_id) {
  // Abort any Connect sequence before Detach — LeaveCall can run while Connect is mid-dial.
  reach_.AbortCircuitAttempts();
  Apply(CallDirectPlannerEvent::Stop, call_id, media_peer_identity_);
  AbortConnectSequence();
  CancelDirectHealthTimer();
  CancelReserveRenewal();
  const std::string peer = media_peer_identity_;
  if (pending_answerer_call_id_ == call_id) {
    pending_answerer_call_id_.clear();
    key_wait_gen_.fetch_add(1, std::memory_order_acq_rel);
    pending_answerer_peer_.clear();
  }
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  reach_kind_ = PeerLinkKind::Unknown;
  force_circuit_ensure_ = false;
  session_offerer_ = false;
  direct_connected_at_ms_ = 0;
  tx_only_escalation_done_ = false;
  ClearMeshConnectFailed();
  media_attempted_calls_.erase(call_id);
  media_.SetOnStateChanged({});

  // Always stop leftover media_relay even when ActiveCallId drifted or is empty
  // (dogfood cbe535: End left SFU capture running → next call Connected/reconnecting,
  // zombie RX stream, no audio). One engine serves one call.
  if (!media_.IsActive() && !media_.IsSfuMode()) {
    return;
  }
  const std::string active = media_.ActiveCallId();
  if (!active.empty() && !call_id.empty() && active != call_id) {
    log().warning << "StopMeshMedia stopping mismatched engine call_id=" << active << " leave=" << call_id;
  } else {
    log().info << "StopMeshMedia stopping engine call_id=" << active << " leave=" << call_id;
  }
  media_.Stop();
}

void CallMediaBridge::ReleaseDirectTransport() {
  if (seat_.IsBound()) {
    ReleaseDirectTransport(seat_.current_token());
    return;
  }
  ReleaseDirectTransportBody();
}

void CallMediaBridge::ReleaseDirectTransport(const CallMediaSeat::Token& token) {
  if (seat_.IsBound()) {
    if (!seat_.allows_path_op(token)) {
      log().info << "ReleaseDirectTransport skip (token not bound) call_id=" << token.call_id
                 << " bound=" << (seat_.bound_call_id ? seat_.bound_call_id() : "");
      return;
    }
  }
  ReleaseDirectTransportBody();
}

void CallMediaBridge::ReleaseDirectTransportBody() {
  // SoftMigrate: drop 1:1 transport; keep CallMediaEngine capture feeding media_relay.
  Apply(CallDirectPlannerEvent::ReleaseTransport, media_call_id_, media_peer_identity_);
  AbortConnectSequence();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  reach_kind_ = PeerLinkKind::Unknown;
  inbound_deferred_peer_id_.clear();
  inbound_remote_stream_.store(0, std::memory_order_release);
  // Do not ClearRemoteAudioTracks here — SoftMigrate+2s would wipe live media_relay tracks
  // that already replaced 1:1 (dogfood: streams look healthy then Moto silent on PreferLocal).
  // 1:1 on_audio is already ignored once P2pIsSfuAttached(); stream_id==1 is dropped in engine.
  ClearMeshConnectFailed();
  // V036 Phase 2: signaling may advance to InCall, but chrome Connected requires seat Live
  // (set by CompleteAttachLocalToSfu NoteLive — not ReleaseDirect alone).
  if (arming_.IsBound() && media_.IsActive() && media_.IsSfuMode()) {
    if (arming_.on_connected) {
      arming_.on_connected(media_.ActiveCallId());
    }
  }
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::NotePeerIdRelayMapping(const std::string& peer_id,
                                                   const std::string& relay_identity) {
  if (peer_id.empty() || relay_identity.rfind("account:", 0) != 0) {
    return;
  }
  if (inbound_deferred_peer_id_.empty() || inbound_deferred_peer_id_ != peer_id) {
    return;
  }
  media_peer_identity_ = relay_identity;
  const uint32_t stream = PublisherStreamIdForIdentity(relay_identity);
  inbound_remote_stream_.store(stream, std::memory_order_release);
  log().info << "Inbound call-media mapping from CallAccept/Invite stream_id=" << stream
             << " peer_id=" << peer_id << " account=" << relay_identity;
}

void CallMediaBridge::PrepareForTeardown(int /*timeout_ms*/) {
  stopping_.store(true, std::memory_order_release);
  reach_.AbortCircuitAttempts();
  Apply(CallDirectPlannerEvent::Stop, media_call_id_, media_peer_identity_);
  AbortConnectSequence();
  // Also releases waiting inbound hellos and drops the inbound handler before Detach, so late
  // streams cannot reach the bridge.
  connect_.Shutdown();
  CancelDirectHealthTimer();
  CancelReserveRenewal();
  const std::string peer = media_peer_identity_;
  const std::string call_id = media_call_id_;
  pending_answerer_call_id_.clear();
  key_wait_gen_.fetch_add(1, std::memory_order_acq_rel);
  pending_answerer_peer_.clear();
  reach_.ReleasePeer(peer);
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearMeshConnectFailed();

  if (!call_id.empty() && media_.IsActive() && media_.ActiveCallId() == call_id) {
    if (AppRuntime::CurrentlyOnUI()) {
      media_.Stop();
    } else {
      AppRuntime::PostUI([this, call_id]() {
        if (media_.IsActive() && media_.ActiveCallId() == call_id) {
          media_.Stop();
        }
      });
    }
  }
}

Roe<void> CallMediaBridge::RetryMeshMedia(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  // Restarts the engine and the connect sequence — UI-only. Refuse rather than race.
  if (!AppRuntime::CurrentlyOnUI()) {
    log().error << "RetryMeshMedia called off the UI thread call_id=" << call_id;
    return Error("call media retry must run on the UI thread");
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  std::string peer = media_peer_identity_;
  if (peer.empty()) {
    if (auto resolved = host_.P2pPeerIdentityForCall(call_id); resolved && resolved->has_value()) {
      peer = **resolved;
    }
  }
  if (peer.empty()) {
    return Error("No peer for call retry");
  }
  ClearMeshConnectFailed();
  reach_.ForgetPath(peer);
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  direct_.Detach();
  return BeginSession(call_id, peer, true);
}

void CallMediaBridge::NoteMediaAttempted(const std::string& call_id) {
  media_attempted_calls_.insert(call_id);
}

bool CallMediaBridge::MediaAttempted(const std::string& call_id) const {
  return media_attempted_calls_.count(call_id) > 0;
}

} // namespace pbr
