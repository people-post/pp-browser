#include "feature/messaging/CallLibp2pMediaBridge.h"

#include "base/platform/BrowserThread.h"
#include "base/platform/PlatformRuntime.h"
#include "common/Utilities.h"

#include <chrono>
#include <thread>

namespace pbr {
namespace {

/** Answerer waits for offerer dial; offerer retries can take ~60s — keep chrome aligned. */
constexpr int64_t kLibp2pConnectTimeoutMs = 75000;
constexpr int64_t kDialWaitBudgetMs = 12000;
constexpr int kDialPollMs = 250;
constexpr int kConnectAttempts = 5;
/** Full newStream + Noise + hello; 2.5s was far too short on Android LAN. */
constexpr int kConnectAttemptTimeoutMs = 15000;
/** Cover long PollInbox HTTP + offerer MediaKey send/resend window. */
constexpr int kMediaKeyInboxPollRounds = 90;
constexpr int kInboundMediaKeyWaitMs = 8000;
/**
 * Offerer prefers inbound (answerer reverse-dial). After this grace, dial the answerer if
 * dialable — covers asymmetric LAN (e.g. Windows announce missing, Linux still reachable).
 * Keep long enough that a successful answerer dial usually wins first; avoid overlapping
 * newStream when both peers are dialable (simultaneous dial hangs on some LAN paths).
 */
constexpr int64_t kOffererInboundGraceMs = 8000;

} // namespace

CallLibp2pMediaBridge::CallLibp2pMediaBridge(CallP2pSignalingHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             CallMediaDirectService& direct, IDialRegistry* dial,
                                             ICircuitHopReach* circuit_reach)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct), dial_(dial),
      circuit_reach_(circuit_reach) {
  redirectLogger("CallLibp2pMediaBridge");

  direct_.SetInboundHandler([this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    log().info << "Inbound call-media hello call_id=" << params.call_id
                  << " epoch=" << params.media_epoch;
    auto session = sessions_.LoadSession(params.call_id);
    if (!session || !session->has_value()) {
      log().warning << "Inbound call-media rejected: no session call_id=" << params.call_id;
      return;
    }
    // Offerer often dials before relay delivers CallMediaKey — wait briefly while inbox sync runs.
    const int64_t key_deadline = util::NowUnixMs() + kInboundMediaKeyWaitMs;
    while (util::NowUnixMs() < key_deadline) {
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      auto session_now = sessions_.LoadSession(params.call_id);
      if (!session_now || !session_now->has_value() ||
          (*session_now)->state == CallSessionState::Ended) {
        return;
      }
      if (auto key = media_keys_.LoadEpochKey(params.call_id, params.media_epoch); key && key->has_value()) {
        params.media_key = **key;
        break;
      }
      host_.P2pRequestInboxSync();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (params.media_key.empty()) {
      log().info << "Inbound call-media hello before media key call_id=" << params.call_id;
    }
    const std::string call_id = params.call_id;
    cbs.on_connected = [this, call_id]() {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
        log().info << "Inbound call-media connected call_id=" << call_id;
        if (media_.IsActive() && media_.ActiveCallId() == call_id) {
          media_.SetConnectionState("connected");
          ClearLibp2pConnectFailed();
          if (lifecycle_) {
            lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
          }
          host_.P2pNotifyRingChanged();
        }
      });
    };
    cbs.on_audio = [this, call_id](const std::vector<uint8_t>& opus) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, opus]() {
        if (!media_.IsActive() || media_.ActiveCallId() != call_id) {
          return;
        }
        CallMediaEngine::SfuPacket pkt;
        pkt.channel_id = 0;
        pkt.payload = opus;
        media_.OnSfuPacket(pkt);
      });
    };
    cbs.on_failed = [this, call_id](const std::string& reason) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, reason]() {
        if (media_.ActiveCallId() != call_id) {
          return;
        }
        log().warning << "Inbound call-media failed call_id=" << call_id << " reason=" << reason;
        libp2p_connect_failed_ = true;
        host_.P2pSetLastMediaError(reason);
        if (lifecycle_) {
          lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
        }
        host_.P2pNotifyRingChanged();
      });
    };
  });
}

void CallLibp2pMediaBridge::SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach) {
  dial_ = dial;
  circuit_reach_ = circuit_reach;
}

void CallLibp2pMediaBridge::SetLifecycle(CallLifecycle* lifecycle) {
  lifecycle_ = lifecycle;
}

bool CallLibp2pMediaBridge::IsLibp2pConnectFailed() const {
  return libp2p_connect_failed_;
}

bool CallLibp2pMediaBridge::Libp2pConnectMissingMic() const {
  return libp2p_connect_missing_mic_;
}

void CallLibp2pMediaBridge::ClearLibp2pConnectFailed() {
  libp2p_connect_failed_ = false;
  libp2p_connect_missing_mic_ = false;
}

void CallLibp2pMediaBridge::PollLibp2pConnectHealth() {
  if (libp2p_connect_failed_ || host_.P2pIsAwaitingSfuRecovery() || !media_.IsActive() || !media_.IsSfuMode()) {
    return;
  }
  // ConnectOffererWithRetry runs on a worker thread — do not UI-timeout while it is dialing.
  if (connect_worker_inflight_.load()) {
    return;
  }
  if (media_.IsConnected() && direct_.IsActive()) {
    ClearLibp2pConnectFailed();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
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
  if (util::NowUnixMs() - started < kLibp2pConnectTimeoutMs) {
    return;
  }
  if (direct_.IsActive()) {
    ClearLibp2pConnectFailed();
    return;
  }
  log().warning << "Libp2p connect timeout call_id=" << call_id;
  libp2p_connect_failed_ = true;
  libp2p_connect_missing_mic_ = !media_.HasLocalCapture();
  if (lifecycle_) {
    lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  }
  host_.P2pNotifyRingChanged();
}

bool CallLibp2pMediaBridge::ShouldUseLibp2pForPeer(const std::string& /*peer_identity*/) const {
  return dial_ != nullptr;
}

Roe<void> CallLibp2pMediaBridge::EnsurePeerReachableOnIo(const std::string& peer_identity,
                                                        const uint64_t connect_gen) {
  if (!dial_) {
    return Error("dial registry not available");
  }
  const int64_t deadline = util::NowUnixMs() + kDialWaitBudgetMs;
  Error last_error("call peer not dialable");
  while (util::NowUnixMs() < deadline) {
    if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
        stopping_.load(std::memory_order_acquire)) {
      return Error("call-media aborted");
    }
    if (dial_->IsDialable(peer_identity)) {
      log().info << "Call-media peer dialable peer=" << peer_identity;
      return {};
    }
    if (circuit_reach_) {
      auto via_circuit = circuit_reach_->TryEnsureCallMediaReachable(peer_identity);
      if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
          stopping_.load(std::memory_order_acquire)) {
        return Error("call-media aborted");
      }
      if (via_circuit) {
        log().info << "Call-media peer reachable via circuit peer=" << peer_identity;
        return {};
      }
      last_error = via_circuit.error();
    } else {
      last_error = Error("call peer not dialable");
    }
    for (int i = 0; i < kDialPollMs / 10; ++i) {
      if (connect_generation_.load(std::memory_order_acquire) != connect_gen ||
          stopping_.load(std::memory_order_acquire)) {
        return Error("call-media aborted");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  log().warning << "Call-media peer still undialable peer=" << peer_identity
                << " last=" << last_error.message;
  return last_error;
}

Roe<void> CallLibp2pMediaBridge::ConnectOffererWithRetry(const CallMediaDirectConnectParams& params,
                                                         const CallMediaDirectCallbacks& cbs) {
  const uint64_t gen = connect_generation_.load(std::memory_order_acquire);
  if (direct_.IsActive()) {
    log().info << "Call-media already active (peer dialed us) call_id=" << params.call_id;
    return {};
  }

  Roe<void> ready = EnsurePeerReachableOnIo(params.peer_key, gen);
  if (!ready) {
    return ready;
  }

  // Offerer: peer Accept/N025 often lag CallAccept. Answerer reverse-dial can start sooner.
  if (params.offerer) {
    for (int i = 0; i < 200; ++i) {
      if (connect_generation_.load(std::memory_order_acquire) != gen) {
        return Error("call-media aborted");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  Error last_error("call-media connect failed");
  for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
    if (connect_generation_.load(std::memory_order_acquire) != gen) {
      log().info << "Call-media Connect aborted call_id=" << params.call_id;
      return Error("call-media aborted");
    }
    if (direct_.IsActive()) {
      log().info << "Call-media already active mid-retry call_id=" << params.call_id;
      return {};
    }
    if (dial_) {
      dial_->AbortInflightDial(params.peer_key);
      dial_->ClearDialBackoff(params.peer_key);
      if (auto ma = dial_->PreferredMultiaddr(params.peer_key)) {
        log().info << "Call-media dial ma=" << *ma << " peer=" << params.peer_key
                      << " role=" << (params.offerer ? "offerer" : "answerer");
      }
    }
    // Offerer resends key each attempt — answerer Defers until Inbound CallMediaKey.
    if (params.offerer) {
      host_.P2pResendMediaKey(params.call_id, params.peer_key);
    }
    // Prior attempt may have left a half-open stream after hello reject / timeout.
    if (!direct_.IsActive()) {
      direct_.Detach();
    }
    log().info << "Call-media Connect attempt=" << attempt << "/" << kConnectAttempts
               << " call_id=" << params.call_id << " peer=" << params.peer_key
               << " role=" << (params.offerer ? "offerer" : "answerer")
               << " timeout_ms=" << kConnectAttemptTimeoutMs;
    Roe<void> connected = direct_.Connect(params, cbs, kConnectAttemptTimeoutMs);
    if (connect_generation_.load(std::memory_order_acquire) != gen) {
      direct_.Detach();
      log().info << "Call-media Connect aborted after attempt call_id=" << params.call_id;
      return Error("call-media aborted");
    }
    if (connected) {
      log().info << "Call-media Connect ok call_id=" << params.call_id
                    << " role=" << (params.offerer ? "offerer" : "answerer");
      return {};
    }
    if (direct_.IsActive()) {
      log().info << "Call-media peer connected us during attempt call_id=" << params.call_id;
      return {};
    }
    last_error = connected.error();
    log().warning << "Call-media Connect failed attempt=" << attempt << " err=" << last_error.message;
    if (dial_) {
      dial_->AbortInflightDial(params.peer_key);
      dial_->ClearDialBackoff(params.peer_key);
    }
    // Let abandoned newStream callbacks drain before the next attempt (interruptible).
    for (int i = 0; i < 150; ++i) {
      if (connect_generation_.load(std::memory_order_acquire) != gen) {
        return Error("call-media aborted");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  return last_error;
}

Roe<ByteVector> CallLibp2pMediaBridge::LoadActiveMediaKey(const std::string& call_id) const {
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

Roe<void> CallLibp2pMediaBridge::BeginSession(const std::string& call_id, const std::string& peer_identity,
                                              bool offerer) {
  auto key = LoadActiveMediaKey(call_id);
  if (!key) {
    return key.error();
  }

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }

  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  media_peer_identity_ = peer_identity;
  audio_seq_.store(0);
  ClearLibp2pConnectFailed();
  if (!offerer) {
    pending_answerer_call_id_.clear();
    pending_answerer_peer_.clear();
  }

  // Answerer may already have accepted inbound hello (key landed first). Offerer must NOT Detach:
  // answerer-only dial often negotiates the stream before BeginSession runs on the offerer
  // (dogfood: Detach raced inbound → phone never read hello → "Failed to read call-media frame header").
  const bool keep_inbound = direct_.IsActive();
  if (media_.IsActive()) {
    media_.Stop();
  }
  if (!offerer && !keep_inbound) {
    direct_.Detach();
  }

  const uint32_t media_epoch = (*session)->media_epoch;
  const ByteVector media_key = *key;

  log().info << "BeginSession role=" << (offerer ? "offerer" : "answerer") << " call_id=" << call_id
                << " peer=" << peer_identity << " epoch=" << media_epoch
                << " keep_inbound=" << (keep_inbound ? 1 : 0);

  media_.SetOnStateChanged([this](const std::string& state) {
    if (state == "connected") {
      ClearLibp2pConnectFailed();
      host_.P2pNotifyRingChanged();
      return;
    }
    host_.P2pNotifyRingChanged();
  });

  const std::string captured_call_id = call_id;
  const std::string captured_peer = peer_identity;
  if (auto started = media_.StartSfu(call_id, [this, captured_call_id, captured_peer, media_epoch, media_key](
                                                  const CallMediaEngine::SfuPacket& pkt) {
        if (pkt.channel_id != 0) {
          return;
        }
        const uint32_t seq = audio_seq_.fetch_add(1) + 1;
        (void)direct_.SendAudio(pkt.payload, seq, pkt.mark);
      });
      !started) {
    return started;
  }

  // StartSfu marks connected immediately for SFU capture; 1:1 chrome waits on the direct stream.
  if (!direct_.IsActive()) {
    media_.SetConnectionState("connecting");
  }

  if (keep_inbound) {
    log().info << "Media started with existing inbound stream call_id=" << call_id
                  << " role=" << (offerer ? "offerer" : "answerer");
    media_.SetConnectionState("connected");
    host_.P2pNotifyRingChanged();
    return {};
  }

  CallMediaDirectConnectParams params;
  params.peer_key = peer_identity;
  params.call_id = call_id;
  params.media_epoch = media_epoch;
  params.media_key = media_key;
  params.offerer = offerer;

  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [this]() {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      log().info << "Call-media connected call_id=" << media_call_id_;
      if (media_.IsActive() && media_.ActiveCallId() == media_call_id_) {
        media_.SetConnectionState("connected");
      }
      ClearLibp2pConnectFailed();
      if (lifecycle_) {
        lifecycle_->Apply(CallLifecycleEvent::DirectConnected, media_call_id_);
      }
      host_.P2pNotifyRingChanged();
    });
  };
  cbs.on_audio = [this, captured_call_id](const std::vector<uint8_t>& opus) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this, captured_call_id, opus]() {
      if (!media_.IsActive() || media_.ActiveCallId() != captured_call_id) {
        return;
      }
      CallMediaEngine::SfuPacket pkt;
      pkt.channel_id = 0;
      pkt.payload = opus;
      media_.OnSfuPacket(pkt);
    });
  };
  cbs.on_failed = [this, captured_call_id](const std::string& reason) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this, captured_call_id, reason]() {
      if (media_.ActiveCallId() != captured_call_id) {
        return;
      }
      // Ignore late fail if the other direction already connected.
      if (direct_.IsActive() && media_.IsConnected()) {
        return;
      }
      log().warning << "Call-media failed call_id=" << captured_call_id << " reason=" << reason;
      libp2p_connect_failed_ = true;
      host_.P2pSetLastMediaError(reason);
      if (lifecycle_) {
        lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, captured_call_id);
      }
      host_.P2pNotifyRingChanged();
    });
  };

  // Prefer answerer reverse-dial (inbound on offerer). Simultaneous offerer↔answerer newStream
  // hangs on some LAN paths (dogfood: both timed out with no OpenStream callback).
  // Offerer: wait briefly for inbound, then dial if the answerer is reachable (asymmetric LAN).
  // Must not use Browser IO — PollInbox starvation (moto dogfood).
  if (params.offerer) {
    // Drop any Prefetch/warm dial toward the answerer so the host can accept inbound first.
    if (dial_) {
      dial_->AbortInflightDial(params.peer_key);
    }
  }

  connect_worker_inflight_.store(true);
  const uint64_t gen = connect_generation_.load(std::memory_order_acquire);
  const char* role = params.offerer ? "offerer" : "answerer";
  PlatformRuntime::PostWorkerCritical([this, params, cbs, gen, role]() {
    if (connect_generation_.load(std::memory_order_acquire) != gen) {
      connect_worker_inflight_.store(false);
      log().info << "Connect worker aborted before dial call_id=" << params.call_id;
      return;
    }

    Roe<void> connected = {};
    if (params.offerer) {
      log().info << "Offerer waiting for inbound call-media call_id=" << params.call_id
                 << " grace_ms=" << kOffererInboundGraceMs;
      const int64_t grace_deadline = util::NowUnixMs() + kOffererInboundGraceMs;
      while (util::NowUnixMs() < grace_deadline) {
        if (connect_generation_.load(std::memory_order_acquire) != gen ||
            stopping_.load(std::memory_order_acquire)) {
          connect_worker_inflight_.store(false);
          log().info << "Connect worker aborted during inbound grace call_id=" << params.call_id;
          return;
        }
        if (direct_.IsActive()) {
          log().info << "Offerer got inbound call-media during grace call_id=" << params.call_id;
          connect_worker_inflight_.store(false);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (direct_.IsActive()) {
        connect_worker_inflight_.store(false);
        return;
      }
      log().info << "Offerer fallback dial call_id=" << params.call_id << " peer=" << params.peer_key;
    }

    log().info << "Connect worker enter call_id=" << params.call_id << " peer=" << params.peer_key
               << " role=" << role;
    connected = ConnectOffererWithRetry(params, cbs);
    connect_worker_inflight_.store(false);
    if (connect_generation_.load(std::memory_order_acquire) != gen) {
      log().info << "Connect worker aborted after dial call_id=" << params.call_id;
      return;
    }
    PlatformRuntime::PostUI([this, connected, call_id = params.call_id, role = std::string(role)]() {
      if (direct_.IsActive() && media_.IsConnected()) {
        return;
      }
      if (!connected) {
        log().warning << "Call-media give up call_id=" << call_id << " role=" << role
                      << " err=" << connected.error().message;
        libp2p_connect_failed_ = true;
        host_.P2pSetLastMediaError(connected.error().message);
        if (lifecycle_) {
          lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
        }
        host_.P2pNotifyRingChanged();
      }
    });
  });

  libp2p_connect_missing_mic_ = false;
  host_.P2pNotifyRingChanged();
  return {};
}

Roe<void> CallLibp2pMediaBridge::StartMediaAsOfferer(const std::string& call_id,
                                                     const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, true);
}

Roe<void> CallLibp2pMediaBridge::StartMediaAsAnswerer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, false);
}

void CallLibp2pMediaBridge::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                        const std::string& peer_identity) {
  // Mark before UI hop so CallController orphan auto-Leave cannot race CallAccept→Active.
  media_attempted_calls_.insert(call_id);
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value()) {
      log().info << "StartMediaAsOfferer skip: no session call_id=" << call_id;
      return;
    }
    if ((*session)->state == CallSessionState::Ended) {
      log().info << "StartMediaAsOfferer skip: session ended call_id=" << call_id;
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      log().info << "StartMediaAsOfferer skip: media already active call_id=" << call_id;
      return;
    }
    log().info << "StartMediaAsOfferer UI enter call_id=" << call_id << " peer=" << peer_identity;
    if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsOfferer failed: " << started.error().message;
      host_.P2pSetLastMediaError(started.error().message);
      host_.P2pNotifyRingChanged();
    }
  });
}

void CallLibp2pMediaBridge::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                         const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id && media_.IsSfuMode()) {
      return;
    }
    auto key = LoadActiveMediaKey(call_id);
    if (!key) {
      // V015: epoch-1 key is sent by offerer on CallAccept — defer until it lands.
      log().info << "Defer answerer media until CallMediaKey call_id=" << call_id
                    << " reason=" << key.error().message;
      pending_answerer_call_id_ = call_id;
      pending_answerer_peer_ = peer_identity;
      media_attempted_calls_.insert(call_id);
      if (lifecycle_) {
        lifecycle_->Apply(CallLifecycleEvent::MediaDeferred, call_id);
      }
      // Accept-time SyncInbox often races the offerer's MediaKey send — keep polling.
      // SyncInbox coalesces via poll_again_; do not assume each Request starts HTTP.
      PlatformRuntime::PostWorkerBackground([this, call_id]() {
        for (int i = 0; i < kMediaKeyInboxPollRounds; ++i) {
          if (pending_answerer_call_id_ != call_id) {
            return;
          }
          host_.P2pRequestInboxSync();
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          // Belt-and-suspenders if OnMediaKeyReady raced / was missed.
          if (pending_answerer_call_id_ == call_id) {
            if (auto key = LoadActiveMediaKey(call_id); key) {
              log().info << "Deferred MediaKey found in store — kick start call_id=" << call_id;
              OnMediaKeyReady(call_id);
              return;
            }
          }
        }
        log().warning << "Deferred MediaKey wait exhausted call_id=" << call_id;
      });
      return;
    }
    if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
      host_.P2pSetLastMediaError(started.error().message);
      host_.P2pNotifyRingChanged();
    }
  });
}

void CallLibp2pMediaBridge::OnMediaKeyReady(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  // Hop to UI — inbound CallMediaKey is processed on Browser IO (inside PollInbox).
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
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
    if (lifecycle_) {
      lifecycle_->Apply(CallLifecycleEvent::MediaKeyReady, call_id);
    }
    if (!peer.empty()) {
      ScheduleStartMediaAsAnswerer(call_id, peer);
    }
  });
}

void CallLibp2pMediaBridge::StopLibp2pMedia(const std::string& call_id) {
  // Abort any Connect worker before Detach — LeaveCall can run while Connect is mid-dial.
  // Do not clear connect_worker_inflight_ here — only the worker clears it (shutdown waits).
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
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
  ClearLibp2pConnectFailed();
  media_attempted_calls_.erase(call_id);

  // CallMediaEngine::Stop tears down SDL capture — UI thread only (CALLS.md).
  auto stop_engine = [this, call_id]() {
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      media_.Stop();
    }
  };
  if (BrowserThread::CurrentlyOn(BrowserThreadId::UI)) {
    stop_engine();
  } else {
    BrowserThread::PostTask(BrowserThreadId::UI, std::move(stop_engine));
  }
}

void CallLibp2pMediaBridge::PrepareForTeardown(int timeout_ms) {
  stopping_.store(true, std::memory_order_release);
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  const std::string peer = media_peer_identity_;
  const std::string call_id = media_call_id_;
  pending_answerer_call_id_.clear();
  pending_answerer_peer_.clear();
  if (dial_ && !peer.empty()) {
    dial_->AbortInflightDial(peer);
    dial_->ClearCallMediaCircuitHop(peer);
  }
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearLibp2pConnectFailed();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
  while (connect_worker_inflight_.load(std::memory_order_acquire)) {
    if (!PlatformRuntime::IsRunning()) {
      // Pool already joined (or never started) — queued Connect tasks were dropped.
      connect_worker_inflight_.store(false, std::memory_order_release);
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      log().warning << "PrepareForTeardown: Connect worker still inflight after " << timeout_ms
                    << "ms";
      break;
    }
    // Keep poking the dial/stream so a blocked Connect returns and sees the generation bump.
    if (dial_ && !peer.empty()) {
      dial_->AbortInflightDial(peer);
    }
    direct_.Detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!call_id.empty() && media_.IsActive() && media_.ActiveCallId() == call_id) {
    if (BrowserThread::CurrentlyOn(BrowserThreadId::UI)) {
      media_.Stop();
    } else {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
        if (media_.IsActive() && media_.ActiveCallId() == call_id) {
          media_.Stop();
        }
      });
    }
  }
}

Roe<void> CallLibp2pMediaBridge::RetryLibp2pMedia(const std::string& call_id) {
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
  ClearLibp2pConnectFailed();
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

void CallLibp2pMediaBridge::NoteMediaAttempted(const std::string& call_id) {
  media_attempted_calls_.insert(call_id);
}

bool CallLibp2pMediaBridge::MediaAttempted(const std::string& call_id) const {
  return media_attempted_calls_.count(call_id) > 0;
}

} // namespace pbr
