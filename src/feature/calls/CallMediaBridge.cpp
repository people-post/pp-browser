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
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Answerer waits for offerer dial; offerer retries can take ~60s — keep chrome aligned. */
constexpr int64_t kMeshConnectTimeoutMs = 75000;
/** Must stay ≤ offerer inbound grace so answerer reverse-dial usually wins first. */
constexpr int64_t kDialWaitBudgetMs = 12000;
/**
 * StartBridge (8s) + nested Establish (10s) + relay retry slack. Once circuit Ensure has started,
 * do not expire the Bridge dial wait until this budget from circuit start (dogfood 8b452388).
 */
constexpr int64_t kCircuitEnsureBudgetMs = 20000;
constexpr int kDialPollMs = 250;
constexpr int kConnectAttempts = 5;
/** Full newStream + Noise + hello; 2.5s was far too short on Android LAN. */
constexpr int kConnectAttemptTimeoutMs = 15000;
/** Cover long PollInbox HTTP + offerer MediaKey send/resend window. */
constexpr int kMediaKeyInboxPollRounds = 90;
constexpr int kInboundMediaKeyWaitMs = 8000;
/**
 * Offerer prefers inbound (answerer reverse-dial). Grace must cover answerer dial reachability
 * (kDialWaitBudgetMs) plus a short MediaKey/settle margin — shorter grace caused fallback dial
 * while Windows was still in EnsurePeerReachable → dual-stream hello deadlock.
 */
constexpr int64_t kOffererInboundGraceMs = 15000;

/** Rate-limit PeerId→relay unknown drops (PreferLocal / non-contact dogfood). */
std::atomic<uint32_t> g_inbound_unmapped_audio_drops{0};

} // namespace

CallMediaBridge::CallMediaBridge(CallMediaHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             ICallMediaTransport& direct, IDialRegistry* dial,
                                             ICircuitHopReach* circuit_reach)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct), dial_(dial),
      circuit_reach_(circuit_reach), media_key_inbox_poll_rounds_(kMediaKeyInboxPollRounds) {
  redirectLogger("CallMediaBridge");

  direct_.SetInboundHandler([this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    log().info << "Inbound call-media hello call_id=" << params.call_id
                  << " epoch=" << params.media_epoch
                  << " peer=" << (params.peer_key.empty() ? "(empty)" : params.peer_key);
    auto session = sessions_.LoadSession(params.call_id);
    if (!session || !session->has_value()) {
      log().warning << "Inbound call-media rejected: no session call_id=" << params.call_id;
      return;
    }
    // Offerer often dials before relay delivers CallMediaKey — wait briefly while inbox sync runs.
    // Cancelable wait (V033): wake on key / teardown; no bare sleep_for on the worker hop.
    {
      std::unique_lock lock(inbound_key_mu_);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(kInboundMediaKeyWaitMs);
      while (!stopping_.load(std::memory_order_acquire)) {
        auto session_now = sessions_.LoadSession(params.call_id);
        if (!session_now || !session_now->has_value() ||
            (*session_now)->state == CallSessionState::Ended) {
          return;
        }
        if (auto key = media_keys_.LoadEpochKey(params.call_id, params.media_epoch);
            key && key->has_value()) {
          params.media_key = **key;
          break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }
        host_.P2pRequestInboxSync();
        const auto slice_deadline =
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
        inbound_key_cv_.wait_until(lock, slice_deadline, [this, &params]() {
          if (stopping_.load(std::memory_order_acquire)) {
            return true;
          }
          auto key = media_keys_.LoadEpochKey(params.call_id, params.media_epoch);
          return static_cast<bool>(key && key->has_value());
        });
      }
    }
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    if (params.media_key.empty()) {
      log().info << "Inbound call-media hello before media key call_id=" << params.call_id;
    }
    // Prefer call-roster Account ID for PublisherStreamIdForIdentity. Inbound hello's
    // peer_key is the mesh PeerId (from remotePeerId); hashing that yields a different
    // stream_id than SoftMigrate (account:…). Never use P2pPeerIdentityForCall here — with
    // N≥2 remotes it returns an arbitrary peer (dogfood: Moto PeerId → wrong person stream).
    const std::string inbound_peer_id = params.peer_key;
    if (inbound_peer_id.rfind("account:", 0) != 0) {
      if (auto mapped = host_.RelayIdentityForMeshPeerId(params.call_id, inbound_peer_id);
          mapped && mapped->has_value() && !mapped->value().empty()) {
        params.peer_key = mapped->value();
      } else if (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0) {
        // Last resort for 1:1 before contacts hydrate — only when dialed peer is the sole remote.
        if (auto sole = host_.P2pPeerIdentityForCall(params.call_id);
            sole && sole->has_value() && sole->value() == media_peer_identity_) {
          params.peer_key = media_peer_identity_;
        }
      } else if (!pending_answerer_peer_.empty() && pending_answerer_peer_.rfind("account:", 0) == 0) {
        params.peer_key = pending_answerer_peer_;
      }
    }
    if (!params.peer_key.empty() && params.peer_key.rfind("account:", 0) == 0) {
      media_peer_identity_ = params.peer_key;
      inbound_remote_stream_.store(PublisherStreamIdForIdentity(params.peer_key),
                                   std::memory_order_release);
    } else {
      // Do not hash PeerId into a mixer track — SoftMigrate uses Account stream ids. Defer until
      // BeginSession / CallAccept teaches PeerId→Account (moto contact often lacks peer_id).
      inbound_remote_stream_.store(0, std::memory_order_release);
    }
    if (params.peer_key.empty() || params.peer_key.rfind("account:", 0) != 0) {
      log().warning << "Inbound call-media stream identity not account: peer_key="
                    << (params.peer_key.empty() ? "(empty)" : params.peer_key)
                    << " inbound_peer_id=" << (inbound_peer_id.empty() ? "(empty)" : inbound_peer_id)
                    << " — deferring on_audio stream_id until Account identity known";
    } else if (!inbound_peer_id.empty() && inbound_peer_id != params.peer_key) {
      log().info << "Inbound call-media mapped PeerId→account stream identity peer_id=" << inbound_peer_id
                 << " account=" << params.peer_key;
    }
    const std::string call_id = params.call_id;
    inbound_deferred_peer_id_ =
        (inbound_peer_id.rfind("account:", 0) == 0) ? std::string{} : inbound_peer_id;
    cbs.on_connected = [this, call_id]() {
      AppRuntime::PostUI([this, call_id]() {
        log().info << "Inbound call-media connected call_id=" << call_id;
        CommitDirectConnected(call_id);
      });
    };
    cbs.on_media = [this, call_id](uint8_t channel, const std::vector<uint8_t>& payload) {
      DeliverInboundDirectMedia(call_id, channel, payload);
    };
    cbs.on_failed = [this, call_id](const std::string& reason) {
      AppRuntime::PostUI([this, call_id, reason]() {
        if (media_.ActiveCallId() != call_id) {
          return;
        }
        // SoftMigrate StartSfu swaps send to media_relay but leaves the 1:1 stream until
        // ReleaseDirectTransport; peer teardown must not flip ConnectFailed over live SFU.
        // IsSfuMode() is also true for 1:1 libp2p capture — require media_relay attach.
        if (host_.P2pIsSfuAttached() && media_.IsConnected()) {
          log().info << "Ignoring inbound call-media fail after SoftMigrate/SFU call_id=" << call_id
                     << " reason=" << reason;
          direct_.Detach();
          ClearMeshConnectFailed();
          if (arming_.on_connected) {
            arming_.on_connected(call_id);
          }
          host_.P2pNotifyRingChanged();
          return;
        }
        // PreferLocal ReleaseDirect closes 1:1 while capture stays up; CallSfuAttach may still
        // be in flight (dogfood: Moto ConnectFailed when attach lagged ReleaseDirect).
        const bool soft_direct_close =
            reason.find("read_eof") != std::string::npos ||
            reason.find("stream closed") != std::string::npos;
        if (host_.P2pIsAwaitingSfuRecovery() || host_.P2pExpectGroupSfuMigration(call_id) ||
            (soft_direct_close && media_.IsActive() && media_.IsConnected())) {
          log().info << "Ignoring inbound call-media fail while awaiting SFU attach call_id="
                     << call_id << " reason=" << reason;
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
        log().warning << "Inbound call-media failed call_id=" << call_id << " reason=" << reason;
        SurfaceConnectFailed(call_id, reason, /*stop_media=*/true);
      });
    };
  });
}

void CallMediaBridge::SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach) {
  dial_ = dial;
  circuit_reach_ = circuit_reach;
}

void CallMediaBridge::SetSeedWarm(std::function<void()> warm) {
  seed_warm_ = std::move(warm);
}

void CallMediaBridge::SetSeedReserve(std::function<void()> reserve) {
  seed_reserve_ = std::move(reserve);
}

std::string CallMediaBridge::MediaPathKind() const {
  if (!media_peer_identity_.empty() && dial_ && dial_->HasCallMediaCircuitHop(media_peer_identity_)) {
    return "circuit";
  }
  return media_path_kind_;
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
  dial_wait_budget_ms_ = budget_ms > 0 ? budget_ms : kDialWaitBudgetMs;
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
      (direct_.IsActive() || media_.IsConnected())) {
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
      (direct_.IsActive() || host_.P2pIsSfuAttached())) {
    seat_.note_live(call_id);
  }
  if (arming_.on_connected) {
    arming_.on_connected(call_id);
  }
  host_.P2pNotifyRingChanged();
}

void CallMediaBridge::DeliverInboundDirectMedia(const std::string& call_id, uint8_t channel,
                                                      const std::vector<uint8_t>& payload) {
  AppRuntime::PostUI([this, call_id, channel, payload]() {
    if (!media_.IsActive() || media_.ActiveCallId() != call_id) {
      return;
    }
    if (host_.P2pIsSfuAttached()) {
      return;
    }
    uint32_t remote_stream = inbound_remote_stream_.load(std::memory_order_acquire);
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
  if (connect_worker_inflight_.load()) {
    return;
  }
  if (media_.IsConnected() && direct_.IsActive()) {
    ClearMeshConnectFailed();
    MaybeEscalateTxOnlyDirect();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  // Stream can be up while StartSfu→"connecting" left IsConnected false (chrome stuck).
  if (direct_.IsActive()) {
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
  in.media_path_kind = media_path_kind_;
  in.has_circuit_reach = circuit_reach_ != nullptr;
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
  log().warning << "Call-media TX-only on path=" << (media_path_kind_.empty() ? "unknown" : media_path_kind_)
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
  media_path_kind_.clear();
  force_circuit_ensure_ = true;
  direct_connected_at_ms_ = 0;
  if (dial_) {
    dial_->ClearCallMediaCircuitHop(peer);
    dial_->ClearDialBackoff(peer);
  }
  // Re-open transport under circuit without full engine Stop (keep capture).
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  CancelConnectTimers();
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
  return dial_ != nullptr;
}

void CallMediaBridge::EnsurePeerReachableAsync(const std::string& peer_identity,
                                                   const uint64_t connect_gen,
                                                   std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!dial_) {
    on_done(Error("dial registry not available"));
    return;
  }
  if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
      stopping_.load(std::memory_order_acquire)) {
    on_done(Error("call-media aborted"));
    return;
  }
  if (seed_warm_) {
    seed_warm_();
  }
  // TX-only escalate: peer may already be "dialable" on a one-way path — still force circuit.
  const bool force_circuit = force_circuit_ensure_;
  force_circuit_ensure_ = false;
  const bool connected_before = dial_->IsConnected(peer_identity);
  const bool dialable_before = dial_->IsDialable(peer_identity);
  // IsDialable (has_endpoint) ≠ Connected. Dogfood 19f845: dialable skip → OpenChannel on
  // connected=0 hung with no OpenChannel/timeout logs until Leave (~46s).
  if (connected_before && !force_circuit) {
    media_path_kind_ = "direct";
    log().info << "CallLifecycle StartSfu peer connected peer=" << peer_identity;
    on_done({});
    return;
  }
  // Do not start circuit Ensure once shutdown/Leave has begun.
  if (stopping_.load(std::memory_order_acquire)) {
    on_done(Error("call-media aborted"));
    return;
  }
  log().info << "CallLifecycle StartSfu Ensure wait peer=" << peer_identity
             << " dialable=" << (dialable_before ? 1 : 0) << " connected=" << (connected_before ? 1 : 0)
             << " force_circuit=" << (force_circuit ? 1 : 0)
             << " has_circuit_reach=" << (circuit_reach_ ? 1 : 0)
             << " budget_ms=" << dial_wait_budget_ms_;

  const int64_t initial_deadline = util::NowUnixMs() + dial_wait_budget_ms_;
  auto deadline = std::make_shared<int64_t>(initial_deadline);
  auto last_error = std::make_shared<Error>(Error("call peer not connected"));
  auto circuit_started = std::make_shared<bool>(false);
  auto circuit_inflight = std::make_shared<bool>(false);
  auto assoc_started = std::make_shared<bool>(false);
  auto assoc_done = std::make_shared<bool>(false);
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish = [settled, on_done = std::move(on_done)](Roe<void> result) mutable {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(result));
    }
  };
  // Hard deadline independent of the poll tick chain (dogfood af934e: EnsureAssociation
  // never called back and poll ticks never hit "still not connected"). Cover circuit Ensure
  // slack so the watchdog cannot abort an in-flight StartBridge early.
  (void)AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(dial_wait_budget_ms_ + kCircuitEnsureBudgetMs + 250),
      [this, peer_identity, finish, settled, last_error]() mutable {
        if (settled->load(std::memory_order_acquire)) {
          return;
        }
        log().warning << "CallLifecycle StartSfu Ensure deadline watchdog peer=" << peer_identity
                      << " last=" << last_error->message
                      << " connected=" << (dial_ && dial_->IsConnected(peer_identity) ? 1 : 0);
        finish(*last_error);
      });
  auto tick = std::make_shared<std::function<void()>>();
  *tick = [this, peer_identity, connect_gen, finish, settled, deadline, initial_deadline, last_error,
           circuit_started, circuit_inflight, assoc_started, assoc_done, force_circuit, tick]() mutable {
    if (settled->load(std::memory_order_acquire)) {
      return;
    }
    if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
        stopping_.load(std::memory_order_acquire)) {
      finish(Error("call-media aborted"));
      return;
    }
    if (!dial_) {
      finish(Error("dial registry not available"));
      return;
    }
    // While forcing circuit, ignore connected until Ensure has run (or hop is installed).
    const bool wait_for_circuit =
        force_circuit && !*circuit_started && !dial_->HasCallMediaCircuitHop(peer_identity);
    if (dial_->IsConnected(peer_identity) && !wait_for_circuit) {
      if (dial_->HasCallMediaCircuitHop(peer_identity)) {
        media_path_kind_ = "circuit";
      } else if (*circuit_started) {
        media_path_kind_ = "punched";
      } else if (media_path_kind_.empty()) {
        media_path_kind_ = "direct";
      }
      log().info << "CallLifecycle StartSfu peer connected peer=" << peer_identity
                 << " path=" << media_path_kind_;
      finish({});
      return;
    }
    if (util::NowUnixMs() >= *deadline) {
      // Do not AbortPending a live StartBridge because punch/assoc burned the short dial budget.
      if (*circuit_inflight) {
        (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kDialPollMs),
                                                     [tick]() {
                                                       if (tick && *tick) {
                                                         (*tick)();
                                                       }
                                                     });
        return;
      }
      log().warning << "CallLifecycle StartSfu peer still not connected peer=" << peer_identity
                    << " last=" << last_error->message
                    << " dialable=" << (dial_->IsDialable(peer_identity) ? 1 : 0)
                    << " circuit_started=" << (*circuit_started ? 1 : 0)
                    << " assoc_started=" << (*assoc_started ? 1 : 0);
      finish(*last_error);
      return;
    }
    // Endpoint known but PeerLink not Connected — kick EnsureAssociation on Amp IO.
    if (!*assoc_started && dial_->IsDialable(peer_identity) && !wait_for_circuit) {
      *assoc_started = true;
      log().info << "CallLifecycle StartSfu EnsureAssociation start peer=" << peer_identity;
      dial_->EnsureAssociation(
          peer_identity, [this, peer_identity, connect_gen, finish, settled, last_error, assoc_done,
                          assoc_started, tick](Roe<void> assoc) mutable {
            AppRuntime::PostCoordinatorNormal(
                [this, peer_identity, connect_gen, finish = std::move(finish), settled, last_error,
                 assoc_done, assoc_started, tick, assoc = std::move(assoc)]() mutable {
                  if (settled->load(std::memory_order_acquire)) {
                    return;
                  }
                  *assoc_done = true;
                  if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
                      stopping_.load(std::memory_order_acquire)) {
                    finish(Error("call-media aborted"));
                    return;
                  }
                  if (assoc) {
                    if (dial_ && dial_->IsConnected(peer_identity)) {
                      media_path_kind_ = "direct";
                      log().info << "CallLifecycle StartSfu EnsureAssociation ok peer=" << peer_identity;
                      finish({});
                      return;
                    }
                    log().info << "CallLifecycle StartSfu EnsureAssociation ok but not connected peer="
                               << peer_identity;
                  } else {
                    *last_error = assoc.error();
                    log().info << "CallLifecycle StartSfu EnsureAssociation miss peer=" << peer_identity
                               << " err=" << last_error->message;
                    // Dogfood: dialable private MA → dial timeout → DialInBackoff. Abort + clear
                    // so parallel circuit nested can proceed (hard-lab dirty heal). Keep
                    // assoc_started so we do not hammer EnsureAssociation every poll.
                    if (dial_) {
                      dial_->AbortInflightDial(peer_identity);
                      dial_->ClearDialBackoff(peer_identity);
                    }
                  }
                  (void)AppRuntime::ScheduleCoordinatorOneShot(
                      std::chrono::milliseconds(kDialPollMs), [tick]() {
                        if (tick && *tick) {
                          (*tick)();
                        }
                      });
                });
          });
      // Fall through: also start circuit/punch in parallel (do not wait forever on assoc).
    }
    // Circuit/punch when forced, undialable, or ADP assoc already kicked / finished.
    if (circuit_reach_ && !*circuit_started &&
        (force_circuit || !dial_->IsDialable(peer_identity) || *assoc_started ||
         (*assoc_done && !dial_->IsConnected(peer_identity)))) {
      *circuit_started = true;
      *circuit_inflight = true;
      const int64_t circuit_deadline = util::NowUnixMs() + kCircuitEnsureBudgetMs;
      if (circuit_deadline > *deadline) {
        *deadline = circuit_deadline;
      }
      log().info << "CallLifecycle StartSfu Ensure circuit/punch start peer=" << peer_identity
                 << " circuit_budget_ms=" << kCircuitEnsureBudgetMs;
      circuit_reach_->TryEnsureCallMediaReachableAsync(
          peer_identity,
          [this, peer_identity, connect_gen, finish, settled, last_error, circuit_inflight, deadline,
           initial_deadline, tick](Roe<void> via) mutable {
            AppRuntime::PostCoordinatorNormal(
                [this, peer_identity, connect_gen, finish = std::move(finish), settled, last_error,
                 circuit_inflight, deadline, initial_deadline, tick,
                 via = std::move(via)]() mutable {
                  *circuit_inflight = false;
                  if (settled->load(std::memory_order_acquire)) {
                    return;
                  }
                  if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
                      stopping_.load(std::memory_order_acquire)) {
                    finish(Error("call-media aborted"));
                    return;
                  }
                  // has_endpoint / punch "ok" is not PeerLink Connected — dogfood 612b: via_ok=1
                  // then OpenChannel hung on connected=0.
                  if (dial_ && dial_->IsConnected(peer_identity)) {
                    if (dial_->HasCallMediaCircuitHop(peer_identity)) {
                      media_path_kind_ = "circuit";
                    } else {
                      media_path_kind_ = "punched";
                    }
                    log().info << "CallLifecycle StartSfu peer reachable via circuit peer="
                               << peer_identity << " path=" << media_path_kind_
                               << " via_ok=" << (via ? 1 : 0);
                    finish({});
                    return;
                  }
                  if (!via) {
                    *last_error = via.error();
                  } else {
                    *last_error = Error("call peer not connected after circuit/punch");
                  }
                  log().info << "CallLifecycle StartSfu Ensure circuit/punch miss peer=" << peer_identity
                             << " err=" << last_error->message
                             << " via_ok=" << (via ? 1 : 0)
                             << " connected=" << (dial_ && dial_->IsConnected(peer_identity) ? 1 : 0);
                  // Circuit slack only applies while StartBridge/nested is in flight. Once it
                  // misses, fall back to the original dial budget (gtest uses 400ms).
                  *deadline = initial_deadline;
                  if (util::NowUnixMs() >= *deadline) {
                    finish(*last_error);
                    return;
                  }
                  (void)AppRuntime::ScheduleCoordinatorOneShot(
                      std::chrono::milliseconds(kDialPollMs), [tick]() {
                        if (tick && *tick) {
                          (*tick)();
                        }
                      });
                });
          });
      (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kDialPollMs),
                                                   [tick]() {
                                                     if (tick && *tick) {
                                                       (*tick)();
                                                     }
                                                   });
      return;
    }
    (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kDialPollMs), [tick]() {
      if (tick && *tick) {
        (*tick)();
      }
    });
  };
  (*tick)();
}

void CallMediaBridge::CancelConnectTimers() {
  if (offerer_grace_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(offerer_grace_timer_id_);
    offerer_grace_timer_id_ = 0;
  }
  if (connect_retry_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(connect_retry_timer_id_);
    connect_retry_timer_id_ = 0;
  }
}

void CallMediaBridge::FinishConnectSequence(const uint64_t gen, const std::string& call_id,
                                            Roe<void> connected, const char* role) {
  if (connect_generation_.load(std::memory_order_acquire) != gen) {
    connect_worker_inflight_.store(false, std::memory_order_release);
    return;
  }
  connect_worker_inflight_.store(false, std::memory_order_release);
  AppRuntime::PostUI([this, connected = std::move(connected), call_id, role = std::string(role ? role : "")]() {
    if (direct_.IsActive()) {
      CommitDirectConnected(call_id);
      return;
    }
    if (!connected) {
      log().info << "CallLifecycle StartSfu Connect give up call_id=" << call_id << " role=" << role
                 << " err=" << connected.error().message;
      SurfaceConnectFailed(call_id, connected.error().message, /*stop_media=*/true);
    }
  });
}

void CallMediaBridge::SurfaceConnectFailed(const std::string& call_id, const std::string& err,
                                           const bool stop_media) {
  if (!err.empty()) {
    host_.P2pSetLastMediaError(err);
  }
  // Stop late EnsureViaCircuit / StartBridge before chrome refresh (dogfood SIGSEGV after give-up).
  if (circuit_reach_) {
    circuit_reach_->AbortPending();
  }
  if (stop_media && (media_.IsActive() || media_.IsSfuMode())) {
    // StopMeshMedia clears mesh_connect_failed_ for Leave hygiene — re-assert below.
    StopMeshMedia(call_id);
  } else if (dial_ && !media_peer_identity_.empty()) {
    dial_->AbortInflightDial(media_peer_identity_);
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

void CallMediaBridge::ScheduleOffererGracePoll(CallMediaDirectConnectParams params,
                                               CallMediaDirectCallbacks cbs, const uint64_t gen,
                                               const int64_t grace_deadline_ms) {
  if (connect_generation_.load(std::memory_order_acquire) != gen ||
      stopping_.load(std::memory_order_acquire)) {
    connect_worker_inflight_.store(false, std::memory_order_release);
    return;
  }
  if (direct_.IsActive()) {
    log().info << "CallLifecycle StartSfu offerer inbound during grace call_id=" << params.call_id;
    FinishConnectSequence(gen, params.call_id, {}, "offerer");
    return;
  }
  if (util::NowUnixMs() >= grace_deadline_ms) {
    log().info << "CallLifecycle StartSfu offerer fallback dial call_id=" << params.call_id
               << " peer=" << params.peer_key;
    BeginConnectAttempt(std::move(params), std::move(cbs), gen, 1);
    return;
  }
  offerer_grace_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(50),
      [this, params = std::move(params), cbs = std::move(cbs), gen, grace_deadline_ms]() mutable {
        offerer_grace_timer_id_ = 0;
        ScheduleOffererGracePoll(std::move(params), std::move(cbs), gen, grace_deadline_ms);
      });
}

void CallMediaBridge::OnConnectAttemptFinished(CallMediaDirectConnectParams params,
                                               CallMediaDirectCallbacks cbs, const uint64_t gen,
                                               const int attempt, Roe<void> connected) {
  if (connect_generation_.load(std::memory_order_acquire) != gen ||
      stopping_.load(std::memory_order_acquire)) {
    direct_.Detach();
    connect_worker_inflight_.store(false, std::memory_order_release);
    return;
  }
  if (connected || direct_.IsActive()) {
    log().info << "CallLifecycle StartSfu Connect ok call_id=" << params.call_id
               << " role=" << (params.offerer ? "offerer" : "answerer");
    FinishConnectSequence(gen, params.call_id, {}, params.offerer ? "offerer" : "answerer");
    return;
  }
  log().warning << "CallLifecycle StartSfu Connect failed attempt=" << attempt
                << " err=" << connected.error().message;
  if (dial_) {
    dial_->AbortInflightDial(params.peer_key);
    dial_->ClearDialBackoff(params.peer_key);
  }
  if (attempt >= kConnectAttempts) {
    FinishConnectSequence(gen, params.call_id, connected, params.offerer ? "offerer" : "answerer");
    return;
  }
  // Drain abandoned stream callbacks before the next attempt (coordinator, not MeshControl sleep).
  connect_retry_timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(1500),
      [this, params = std::move(params), cbs = std::move(cbs), gen, attempt]() mutable {
        connect_retry_timer_id_ = 0;
        BeginConnectAttempt(std::move(params), std::move(cbs), gen, attempt + 1);
      });
}

void CallMediaBridge::BeginConnectAttempt(CallMediaDirectConnectParams params,
                                          CallMediaDirectCallbacks cbs, const uint64_t gen,
                                          const int attempt) {
  if (connect_generation_.load(std::memory_order_acquire) != gen ||
      stopping_.load(std::memory_order_acquire)) {
    connect_worker_inflight_.store(false, std::memory_order_release);
    return;
  }
  if (direct_.IsActive()) {
    FinishConnectSequence(gen, params.call_id, {}, params.offerer ? "offerer" : "answerer");
    return;
  }

  // Prefer answerer reverse-dial progress on UI when reachable — coordinator can lag Pause/Resume.
  // Copy peer_key before the call: init-captures may std::move(params) before peer_key is read
  // (unspecified arg order — dogfood c438: Ensure saw peer="" → "missing call peer").
  const std::string peer_key = params.peer_key;
  EnsurePeerReachableAsync(
      peer_key, gen,
      [this, params = std::move(params), cbs = std::move(cbs), gen, attempt](Roe<void> ready) mutable {
        auto cont = [this, params = std::move(params), cbs = std::move(cbs), gen, attempt,
                     ready = std::move(ready)]() mutable {
          if (connect_generation_.load(std::memory_order_acquire) != gen ||
              stopping_.load(std::memory_order_acquire)) {
            connect_worker_inflight_.store(false, std::memory_order_release);
            return;
          }
          if (!ready) {
            FinishConnectSequence(gen, params.call_id, ready, params.offerer ? "offerer" : "answerer");
            return;
          }
          ContinueConnectAttemptAfterReachable(std::move(params), std::move(cbs), gen, attempt);
        };
        if (AppRuntime::CurrentlyOnUI()) {
          cont();
          return;
        }
        AppRuntime::PostUI(std::move(cont));
      });
}

void CallMediaBridge::ContinueConnectAttemptAfterReachable(CallMediaDirectConnectParams params,
                                                           CallMediaDirectCallbacks cbs,
                                                           const uint64_t gen, const int attempt) {
  if (connect_generation_.load(std::memory_order_acquire) != gen ||
      stopping_.load(std::memory_order_acquire)) {
    connect_worker_inflight_.store(false, std::memory_order_release);
    return;
  }
  if (direct_.IsActive()) {
    FinishConnectSequence(gen, params.call_id, {}, params.offerer ? "offerer" : "answerer");
    return;
  }
  if (dial_) {
    dial_->AbortInflightDial(params.peer_key);
    dial_->ClearDialBackoff(params.peer_key);
    if (auto ma = dial_->PreferredMultiaddr(params.peer_key)) {
      log().info << "Call-media dial ma=" << *ma << " peer=" << params.peer_key
                 << " role=" << (params.offerer ? "offerer" : "answerer");
    }
  }
  if (params.offerer) {
    // MediaKey is addressed by roster account:, not the mesh dial PeerId.
    const std::string key_peer =
        (!media_peer_identity_.empty() && media_peer_identity_.rfind("account:", 0) == 0)
            ? media_peer_identity_
            : params.peer_key;
    host_.P2pResendMediaKey(params.call_id, key_peer);
  }
  if (!direct_.IsActive()) {
    direct_.Detach();
  }
  log().info << "CallLifecycle StartSfu ConnectAsync attempt=" << attempt << "/" << kConnectAttempts
             << " call_id=" << params.call_id << " peer=" << params.peer_key
             << " role=" << (params.offerer ? "offerer" : "answerer")
             << " timeout_ms=" << kConnectAttemptTimeoutMs;
  if (dial_) {
    if (auto ma = dial_->PreferredMultiaddr(params.peer_key)) {
      log().info << "CallLifecycle StartSfu ConnectAsync ma=" << *ma << " peer=" << params.peer_key;
    } else {
      log().info << "CallLifecycle StartSfu ConnectAsync ma=(none) peer=" << params.peer_key
                 << " dialable=" << (dial_->IsDialable(params.peer_key) ? 1 : 0);
    }
  }
  const std::string connect_peer = params.peer_key;
  const std::string connect_call = params.call_id;
  log().info << "CallLifecycle StartSfu ConnectAsync params call_id_len=" << params.call_id.size()
             << " peer_len=" << params.peer_key.size() << " media_key_len=" << params.media_key.size();
  // Snapshot for the ConnectAsync args — never std::move(params) in the same call expression as
  // the `params` argument (unspecified eval order; dogfood c40c: empty media_key →
  // "invalid connect params" with no leg outbound begin).
  CallMediaDirectConnectParams connect_params = params;
  CallMediaDirectCallbacks connect_cbs = cbs;
  const bool offerer_role = connect_params.offerer;
  direct_.ConnectAsync(
      connect_params, connect_cbs,
      [this, params = std::move(params), cbs = std::move(cbs), gen, attempt, connect_peer,
       connect_call](Roe<void> connected) mutable {
        // UI — not coordinator (Pause/backlog can drop Connect timeout → stuck Connecting).
        auto cont = [this, params = std::move(params), cbs = std::move(cbs), gen, attempt,
                     connected = std::move(connected), connect_peer, connect_call]() mutable {
          log().info << "CallLifecycle StartSfu ConnectAsync done call_id=" << connect_call
                     << " peer=" << connect_peer << " ok=" << (connected ? 1 : 0)
                     << (connected ? "" : (" err=" + connected.error().message));
          OnConnectAttemptFinished(std::move(params), std::move(cbs), gen, attempt,
                                   std::move(connected));
        };
        if (AppRuntime::CurrentlyOnUI()) {
          cont();
          return;
        }
        AppRuntime::PostUI(std::move(cont));
      },
      kConnectAttemptTimeoutMs);
  // Bridge-side watchdog: CallMediaLeg TickDeadlines may not fire while OpenChannel/dial is stuck
  // on nested MeshRuntime::Pump (dogfood 19f845/612b: no ConnectAsync done until Leave).
  (void)AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(kConnectAttemptTimeoutMs + 1000),
      [this, gen, attempt, connect_peer, connect_call, offerer_role]() {
        if (connect_generation_.load(std::memory_order_acquire) != gen ||
            stopping_.load(std::memory_order_acquire)) {
          return;
        }
        if (!connect_worker_inflight_.load(std::memory_order_acquire)) {
          return;
        }
        log().warning << "CallLifecycle StartSfu ConnectAsync watchdog call_id=" << connect_call
                      << " peer=" << connect_peer << " attempt=" << attempt;
        direct_.Detach();
        // Detach may not deliver on_finished if Mesh IO is wedged — force sequence progress.
        FinishConnectSequence(gen, connect_call,
                              Error("amp call-media connect timed out (watchdog)"),
                              offerer_role ? "offerer" : "answerer");
      });
}

void CallMediaBridge::StartConnectSequence(CallMediaDirectConnectParams params,
                                           CallMediaDirectCallbacks cbs, const uint64_t gen) {
  if (AppRuntime::IsShuttingDown() || stopping_.load(std::memory_order_acquire)) {
    log().debug << "StartConnectSequence rejected: shutting down call_id=" << params.call_id;
    connect_worker_inflight_.store(false, std::memory_order_release);
    if (cbs.on_failed) {
      cbs.on_failed("shutdown in progress");
    }
    return;
  }
  connect_worker_inflight_.store(true, std::memory_order_release);
  CancelConnectTimers();
  if (params.offerer) {
    if (dial_) {
      dial_->AbortInflightDial(params.peer_key);
    }
    log().info << "CallLifecycle StartSfu Connect offerer wait inbound call_id=" << params.call_id
               << " grace_ms=" << kOffererInboundGraceMs;
    const int64_t grace_deadline = util::NowUnixMs() + kOffererInboundGraceMs;
    ScheduleOffererGracePoll(std::move(params), std::move(cbs), gen, grace_deadline);
    return;
  }
  log().info << "CallLifecycle StartSfu Connect answerer dial call_id=" << params.call_id
             << " peer=" << params.peer_key;
  BeginConnectAttempt(std::move(params), std::move(cbs), gen, 1);
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
    pending_answerer_peer_.clear();
  }

  // Answerer may already have accepted inbound hello (key landed first). Offerer must NOT Detach:
  // answerer-only dial often negotiates the stream before BeginSession runs on the offerer
  // (dogfood: Detach raced inbound → phone never read hello → "Failed to read call-media frame header").
  const bool keep_inbound = direct_.IsActive();
  const bool restarting =
      media_.IsActive() || connect_worker_inflight_.load(std::memory_order_acquire);
  if (media_.IsActive()) {
    media_.Stop();
  }
  if (!offerer && !keep_inbound) {
    direct_.Detach();
  }
  // Abort in-flight Connect from a prior BeginSession (Kick thrash used to Stop without bumping).
  if (restarting && !keep_inbound) {
    connect_generation_.fetch_add(1, std::memory_order_acq_rel);
    CancelConnectTimers();
    connect_worker_inflight_.store(false, std::memory_order_release);
  }

  const uint32_t media_epoch = (*session)->media_epoch;
  const ByteVector media_key = *key;

  log().info << "CallLifecycle StartSfu BeginSession role=" << (offerer ? "offerer" : "answerer")
             << " call_id=" << call_id << " peer=" << peer_identity << " epoch=" << media_epoch
             << " keep_inbound=" << (keep_inbound ? 1 : 0);

  // Both roles park on org seed: answerer reverse-dial makes the *offerer* the circuit target
  // (dogfood 997c1c6f). Reserve keeps a Connected PeerLink so peer-id-only StartBridge can
  // EnsureAssociation without dialing into NAT / requiring a dial-book MA.
  if (seed_warm_) {
    seed_warm_();
  }
  if (seed_reserve_) {
    seed_reserve_();
  }

  media_.SetOnStateChanged([this](const std::string& state) {
    if (state == "connected") {
      ClearMeshConnectFailed();
      host_.P2pNotifyRingChanged();
      return;
    }
    host_.P2pNotifyRingChanged();
  });

  const std::string captured_call_id = call_id;
  const std::string captured_peer = peer_identity;
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
    if (!direct_.IsActive()) {
      seat_.note_connecting(call_id);
    }
  }

  // StartSfu marks connected immediately for SFU capture; 1:1 chrome waits on the direct stream.
  if (!direct_.IsActive()) {
    media_.SetConnectionState("connecting");
  }

  if (keep_inbound) {
    log().info << "Media started with existing inbound stream call_id=" << call_id
                  << " role=" << (offerer ? "offerer" : "answerer");
    CommitDirectConnected(call_id);
    return {};
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
    if (auto mapped = host_.MeshPeerIdForAccount(peer_identity);
        mapped && mapped->has_value() && !mapped->value().empty()) {
      const std::string mesh_peer = **mapped;
      const bool account_dialable = dial_ && dial_->IsDialable(peer_identity);
      const bool mesh_dialable = dial_ && dial_->IsDialable(mesh_peer);
      if (dial_ && account_dialable && !mesh_dialable) {
        if (auto ma = dial_->PreferredMultiaddr(peer_identity)) {
          (void)dial_->RegisterEndpoint(mesh_peer, *ma);
        }
      }
      const bool mesh_after = dial_ && dial_->IsDialable(mesh_peer);
      log().info << "CallLifecycle StartSfu dial key account→PeerId account=" << peer_identity
                 << " peer_id=" << mesh_peer << " account_dialable=" << (account_dialable ? 1 : 0)
                 << " peer_dialable=" << (mesh_after ? 1 : 0);
      if (mesh_after || !account_dialable) {
        params.peer_key = mesh_peer;
      } else {
        log().info << "CallLifecycle StartSfu dial key keep account (PeerId still undialable) account="
                   << peer_identity;
      }
    } else {
      log().info << "CallLifecycle StartSfu dial key account (no PeerId map) account=" << peer_identity
                 << " dialable=" << (dial_ && dial_->IsDialable(peer_identity) ? 1 : 0);
    }
  }

  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [this]() {
    AppRuntime::PostUI([this]() {
      log().info << "Call-media connected call_id=" << media_call_id_;
      CommitDirectConnected(media_call_id_);
    });
  };
  cbs.on_media = [this, captured_call_id, remote_stream = PublisherStreamIdForIdentity(captured_peer)](
                     uint8_t channel, const std::vector<uint8_t>& payload) {
    AppRuntime::PostUI([this, captured_call_id, remote_stream, channel, payload]() {
      if (!media_.IsActive() || media_.ActiveCallId() != captured_call_id) {
        return;
      }
      if (host_.P2pIsSfuAttached()) {
        return;
      }
      CallMediaEngine::SfuPacket pkt;
      pkt.stream_id = remote_stream;
      pkt.channel_id = channel;
      pkt.payload = payload;
      media_.OnSfuPacket(pkt);
    });
  };
  cbs.on_failed = [this, captured_call_id](const std::string& reason) {
    AppRuntime::PostUI([this, captured_call_id, reason]() {
      if (media_.ActiveCallId() != captured_call_id) {
        return;
      }
      // Ignore late fail if the other direction already connected.
      if (direct_.IsActive() && media_.IsConnected()) {
        return;
      }
      // SoftMigrate → media_relay: 1:1 stream reset is expected; keep InCall on SFU.
      // IsSfuMode() is also true for 1:1 libp2p capture — require media_relay attach.
      if (host_.P2pIsSfuAttached() && media_.IsConnected()) {
        log().info << "Ignoring call-media fail after SoftMigrate/SFU call_id=" << captured_call_id
                   << " reason=" << reason;
        direct_.Detach();
        ClearMeshConnectFailed();
        if (arming_.on_connected) {
          arming_.on_connected(captured_call_id);
        }
        host_.P2pNotifyRingChanged();
        return;
      }
      // PreferLocal ReleaseDirect closes 1:1 while capture stays up; CallSfuAttach may still
      // be in flight (dogfood: Moto ConnectFailed when attach lagged ReleaseDirect).
      const bool soft_direct_close =
          reason.find("read_eof") != std::string::npos ||
          reason.find("stream closed") != std::string::npos;
      if (host_.P2pIsAwaitingSfuRecovery() || host_.P2pExpectGroupSfuMigration(captured_call_id) ||
          (soft_direct_close && media_.IsActive() && media_.IsConnected())) {
        log().info << "Ignoring call-media fail while awaiting SFU attach call_id=" << captured_call_id
                   << " reason=" << reason;
        host_.P2pNoteExpectSfuAttach(captured_call_id);
        direct_.Detach();
        ClearMeshConnectFailed();
        host_.P2pRequestInboxSync();
        if (arming_.on_connected) {
          arming_.on_connected(captured_call_id);
        }
        host_.P2pNotifyRingChanged();
        return;
      }
      log().warning << "Call-media failed call_id=" << captured_call_id << " reason=" << reason;
      SurfaceConnectFailed(captured_call_id, reason, /*stop_media=*/true);
    });
  };

  // Prefer answerer reverse-dial (inbound on offerer). Simultaneous offerer↔answerer newStream
  // used to hang (Critical-pool hello/ack deadlock); CallMediaDirect now claims one stream and
  // runs handshake on Normal. Offerer still waits briefly for inbound before fallback dial.
  const uint64_t gen = connect_generation_.load(std::memory_order_acquire);
  StartConnectSequence(std::move(params), std::move(cbs), gen);

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
      AppRuntime::PostWorkerBackground([this, call_id]() {
        const int rounds = media_key_inbox_poll_rounds_;
        for (int i = 0; i < rounds; ++i) {
          if (stopping_.load(std::memory_order_acquire) || pending_answerer_call_id_ != call_id) {
            return;
          }
          host_.P2pRequestInboxSync();
          // Chunked sleep so PrepareForTeardown / Leave can abort without a 1s hang.
          for (int slice = 0; slice < 20; ++slice) {
            if (stopping_.load(std::memory_order_acquire) || pending_answerer_call_id_ != call_id) {
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
  inbound_key_cv_.notify_all();
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
  // Abort any Connect sequence before Detach — LeaveCall can run while Connect is mid-dial.
  // Do not clear connect_worker_inflight_ here — only the sequence clears it (shutdown waits).
  if (circuit_reach_) {
    circuit_reach_->AbortPending();
  }
  Apply(CallDirectPlannerEvent::Stop, call_id, media_peer_identity_);
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  CancelConnectTimers();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  if (pending_answerer_call_id_ == call_id) {
    pending_answerer_call_id_.clear();
    pending_answerer_peer_.clear();
  }
  if (dial_ && !peer.empty()) {
    dial_->AbortInflightDial(peer);
    dial_->ClearCallMediaCircuitHop(peer);
  }
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  media_path_kind_.clear();
  force_circuit_ensure_ = false;
  session_offerer_ = false;
  direct_connected_at_ms_ = 0;
  tx_only_escalation_done_ = false;
  ClearMeshConnectFailed();
  media_attempted_calls_.erase(call_id);
  media_.SetOnStateChanged({});

  // CallMediaEngine::Stop tears down SDL capture — UI thread only (CALLS.md).
  // Always stop leftover media_relay even when ActiveCallId drifted or is empty
  // (dogfood cbe535: End left SFU capture running → next call Connected/reconnecting,
  // zombie RX stream, no audio). One engine serves one call.
  // Capture session generation so a later StartSfu (AcceptInvite SoftMigrate / CallSfuAttach)
  // invalidates this Stop — otherwise PostUIFront Stop kills the new duplex (both sides Calling).
  const uint64_t session_gen = media_.MediaSessionGeneration();
  auto stop_engine = [this, call_id, session_gen]() {
    if (media_.MediaSessionGeneration() != session_gen) {
      log().info << "StopMeshMedia skip stale stop leave=" << call_id << " posted_gen=" << session_gen
                 << " now=" << media_.MediaSessionGeneration();
      return;
    }
    if (!media_.IsActive() && !media_.IsSfuMode()) {
      return;
    }
    const std::string active = media_.ActiveCallId();
    if (!active.empty() && !call_id.empty() && active != call_id) {
      log().warning << "StopMeshMedia stopping mismatched engine call_id=" << active
                    << " leave=" << call_id;
    } else {
      log().info << "StopMeshMedia stopping engine call_id=" << active << " leave=" << call_id;
    }
    media_.Stop();
  };
  if (AppRuntime::CurrentlyOnUI()) {
    stop_engine();
  } else {
    // Front of UI queue — Leave/Accept must not sit behind chrome refresh while capture
    // keeps feeding a detached SFU (zombie TX + red reconnecting on the next call).
    AppRuntime::PostUIFront(std::move(stop_engine));
  }
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
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  CancelConnectTimers();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  if (dial_ && !peer.empty()) {
    dial_->AbortInflightDial(peer);
    dial_->ClearCallMediaCircuitHop(peer);
  }
  direct_.Detach();
  media_peer_identity_.clear();
  media_path_kind_.clear();
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

void CallMediaBridge::PrepareForTeardown(int timeout_ms) {
  stopping_.store(true, std::memory_order_release);
  inbound_key_cv_.notify_all();
  if (circuit_reach_) {
    circuit_reach_->AbortPending();
  }
  Apply(CallDirectPlannerEvent::Stop, media_call_id_, media_peer_identity_);
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  CancelConnectTimers();
  CancelDirectHealthTimer();
  const std::string peer = media_peer_identity_;
  const std::string call_id = media_call_id_;
  pending_answerer_call_id_.clear();
  pending_answerer_peer_.clear();
  // Drop raw `this` inbound handler before Detach so late streams cannot UAF the bridge.
  direct_.ClearInboundHandler();
  if (dial_ && !peer.empty()) {
    dial_->AbortInflightDial(peer);
    dial_->ClearCallMediaCircuitHop(peer);
  }
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearMeshConnectFailed();

  // Shutdown path: do not sleep-spin on the caller (was up to 2s and dominated close latency).
  // ConnectAsync / EnsurePeerReachableAsync observe stopping_ + connect_generation_ and exit.
  if (timeout_ms <= 0) {
    if (connect_worker_inflight_.load(std::memory_order_acquire)) {
      log().info << "PrepareForTeardown: Connect still inflight — continuing without wait "
                    "(gen abort; MeshControl may still drain)";
    }
  } else {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (connect_worker_inflight_.load(std::memory_order_acquire)) {
      if (!AppRuntime::IsRunning()) {
        connect_worker_inflight_.store(false, std::memory_order_release);
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        log().warning << "PrepareForTeardown: Connect sequence still inflight after " << timeout_ms
                      << "ms — proceeding (MeshControl/reachability may still be draining)";
        break;
      }
      if (dial_ && !peer.empty()) {
        dial_->AbortInflightDial(peer);
      }
      direct_.Detach();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

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
  if (dial_) {
    dial_->ClearCallMediaCircuitHop(peer);
    dial_->ClearDialBackoff(peer);
  }
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
