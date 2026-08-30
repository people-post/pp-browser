#include "feature/messaging/CallLibp2pMediaBridge.h"

#include "base/i18n/LocalizationService.h"
#include "base/messaging/SfuAttachFanout.h"
#include "base/p2p/CallMediaAdpDogfood.h"
#include "base/p2p/CallMediaAdpKey.h"
#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <atomic>
#include <chrono>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Answerer waits for offerer dial; offerer retries can take ~60s — keep chrome aligned. */
constexpr int64_t kLibp2pConnectTimeoutMs = 75000;
/** Must stay ≤ offerer inbound grace so answerer reverse-dial usually wins first. */
constexpr int64_t kDialWaitBudgetMs = 12000;
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

CallLibp2pMediaBridge::CallLibp2pMediaBridge(CallMediaHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             CallMediaDirectService& direct, IDialRegistry* dial,
                                             ICircuitHopReach* circuit_reach)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct), dial_(dial),
      circuit_reach_(circuit_reach) {
  redirectLogger("CallLibp2pMediaBridge");

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
    // Prefer call-roster Account ID for PublisherStreamIdForIdentity. Inbound hello's
    // peer_key is the libp2p PeerId (from remotePeerId); hashing that yields a different
    // stream_id than SoftMigrate (account:…). Never use P2pPeerIdentityForCall here — with
    // N≥2 remotes it returns an arbitrary peer (dogfood: Moto PeerId → wrong person stream).
    const std::string inbound_peer_id = params.peer_key;
    if (inbound_peer_id.rfind("account:", 0) != 0) {
      if (auto mapped = host_.P2pRelayIdentityForLibp2pPeerId(params.call_id, inbound_peer_id);
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
    // Local offerer mints when remote hello role is answerer (A010).
    MaybeFillLocalAdpOffer(params, /*offerer_mints_assoc=*/!params.offerer);
    const std::string call_id = params.call_id;
    inbound_deferred_peer_id_ =
        (inbound_peer_id.rfind("account:", 0) == 0) ? std::string{} : inbound_peer_id;
    cbs.on_connected = [this, call_id]() {
      AppRuntime::PostUI([this, call_id]() {
        log().info << "Inbound call-media connected call_id=" << call_id;
        MaybeActivateAdp(direct_.ActiveParams());
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
          ClearLibp2pConnectFailed();
          if (lifecycle_) {
            lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
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
          ClearLibp2pConnectFailed();
          host_.P2pRequestInboxSync();
          if (lifecycle_) {
            lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
          }
          host_.P2pNotifyRingChanged();
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

void CallLibp2pMediaBridge::MaybeFillLocalAdpOffer(CallMediaDirectConnectParams& params,
                                                   const bool offerer_mints_assoc) {
  if (!kCallMediaAdpOpusDogfood) {
    return;
  }
  auto offer = adp_.BindLocal(offerer_mints_assoc);
  if (!offer) {
    log().warning << "ADP BindLocal failed: " << offer.error().message << " — Opus stays on TCP";
    return;
  }
  // Answerer: adopt offerer-minted assoc from peer hello before ack.
  if (!offerer_mints_assoc && !params.peer_adp_assoc_hex.empty()) {
    if (auto assoc = AssocIdFromHex(params.peer_adp_assoc_hex); assoc) {
      adp_.SetLocalAssoc(*assoc);
      offer->assoc = *assoc;
    }
  }
  params.adp_port = offer->port;
  params.adp_ip = offer->ipv4;
  bool assoc_zero = true;
  for (uint8_t b : offer->assoc.bytes) {
    if (b != 0) {
      assoc_zero = false;
      break;
    }
  }
  if (!assoc_zero) {
    params.adp_assoc_hex = AssocIdToHex(offer->assoc);
  } else {
    params.adp_assoc_hex.clear();
  }
}

void CallLibp2pMediaBridge::MaybeActivateAdp(const CallMediaDirectConnectParams& params) {
  if (!kCallMediaAdpOpusDogfood) {
    return;
  }
  if (params.peer_adp_port == 0 || params.peer_adp_ip.empty()) {
    return;
  }
  if (params.media_key.empty() || params.call_id.empty()) {
    return;
  }
  CallMediaAdpHelloOffer remote;
  remote.port = params.peer_adp_port;
  remote.ipv4 = params.peer_adp_ip;
  if (!params.peer_adp_assoc_hex.empty()) {
    if (auto assoc = AssocIdFromHex(params.peer_adp_assoc_hex); assoc) {
      remote.assoc = *assoc;
      // Ensure local assoc matches offerer mint when we learned it late (answerer after ack).
      if (adp_.LocalOffer().port != 0) {
        adp_.SetLocalAssoc(*assoc);
      }
    } else {
      log().warning << "ADP peer assoc hex invalid — Opus stays on TCP";
      return;
    }
  } else if (params.adp_assoc_hex.empty()) {
    return;
  } else if (auto assoc = AssocIdFromHex(params.adp_assoc_hex); assoc) {
    remote.assoc = *assoc;
  } else {
    return;
  }
  const std::string call_id = params.call_id;
  auto activated =
      adp_.Activate(params.media_key, params.call_id, params.media_epoch, remote,
                    [this, call_id](uint8_t channel, const std::vector<uint8_t>& payload) {
                      DeliverInboundDirectMedia(call_id, channel, payload);
                    });
  if (!activated) {
    log().warning << "ADP Activate failed: " << activated.error().message << " — Opus stays on TCP";
    return;
  }
  log().info << "ADP Opus path active call_id=" << call_id << " peer=" << params.peer_adp_ip << ":"
             << params.peer_adp_port;
}

void CallLibp2pMediaBridge::StopAdpPath() {
  adp_.Stop();
}

void CallLibp2pMediaBridge::CommitDirectConnected(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  // Capture may lag the stream (inbound before BeginSession). Still advance phase so chrome
  // can leave Calling/Connecting once StartSfu runs; keep_inbound / on_connected re-enter.
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.SetConnectionState("connected");
  }
  ClearLibp2pConnectFailed();
  if (lifecycle_) {
    lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  }
  host_.P2pNotifyRingChanged();
}

void CallLibp2pMediaBridge::DeliverInboundDirectMedia(const std::string& call_id, uint8_t channel,
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
        if (auto mapped = host_.P2pRelayIdentityForLibp2pPeerId(call_id, deferred);
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
  if (kCallMediaAdpOpusDogfood && adp_.LocalOffer().port != 0) {
    adp_.Pump();
  }
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
  // Stream can be up while StartSfu→"connecting" left IsConnected false (chrome stuck).
  if (direct_.IsActive()) {
    ClearLibp2pConnectFailed();
    if (media_.IsActive() && !media_.IsConnected()) {
      log().info << "Heal call-media connected from direct stream call_id=" << call_id;
      CommitDirectConnected(call_id);
    }
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
    // Do not enter a multi-second circuit RequestBridge once shutdown/Leave has begun —
    // AbortInflightRequests only helps after the wait starts; skipping avoids new hangs.
    if (stopping_.load(std::memory_order_acquire)) {
      return Error("call-media aborted");
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

  // Brief settle after reachability; long sleeps here stacked with grace and caused dual dial.
  for (int i = 0; i < 20; ++i) {
    if (connect_generation_.load(std::memory_order_acquire) != gen) {
      return Error("call-media aborted");
    }
    if (direct_.IsActive()) {
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
  if (peer_identity.rfind("account:", 0) == 0) {
    const uint32_t stream = PublisherStreamIdForIdentity(peer_identity);
    inbound_remote_stream_.store(stream, std::memory_order_release);
  }
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
  const uint64_t send_gen = connect_generation_.load(std::memory_order_acquire);
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
        if (pkt.channel_id == 0 && adp_.IsActive()) {
          if (adp_.SendOpus(pkt.payload, seq, pkt.mark)) {
            return;
          }
          // A011: fall back to TCP stream Opus if ADP send fails.
        } else if (kCallMediaAdpOpusDogfood && adp_.LocalOffer().port != 0) {
          adp_.Pump();
        }
        (void)direct_.SendMedia(static_cast<uint8_t>(pkt.channel_id), pkt.payload, seq, pkt.mark);
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
    MaybeActivateAdp(direct_.ActiveParams());
    CommitDirectConnected(call_id);
    return {};
  }

  CallMediaDirectConnectParams params;
  params.peer_key = peer_identity;
  params.call_id = call_id;
  params.media_epoch = media_epoch;
  params.media_key = media_key;
  params.offerer = offerer;
  MaybeFillLocalAdpOffer(params, /*offerer_mints_assoc=*/offerer);

  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [this]() {
    AppRuntime::PostUI([this]() {
      log().info << "Call-media connected call_id=" << media_call_id_;
      MaybeActivateAdp(direct_.ActiveParams());
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
        ClearLibp2pConnectFailed();
        if (lifecycle_) {
          lifecycle_->Apply(CallLifecycleEvent::DirectConnected, captured_call_id);
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
        ClearLibp2pConnectFailed();
        host_.P2pRequestInboxSync();
        if (lifecycle_) {
          lifecycle_->Apply(CallLifecycleEvent::DirectConnected, captured_call_id);
        }
        host_.P2pNotifyRingChanged();
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
  // used to hang (Critical-pool hello/ack deadlock); CallMediaDirect now claims one stream and
  // runs handshake on Normal. Offerer still waits briefly for inbound before fallback dial.
  if (params.offerer) {
    // Drop any Prefetch/warm dial toward the answerer so the host can accept inbound first.
    if (dial_) {
      dial_->AbortInflightDial(params.peer_key);
    }
  }

  connect_worker_inflight_.store(true);
  const uint64_t gen = connect_generation_.load(std::memory_order_acquire);
  const char* role = params.offerer ? "offerer" : "answerer";
  // Normal lane — must not occupy Critical for grace/dial waits (inbox + hello need workers).
  AppRuntime::PostWorkerNormal([this, params, cbs, gen, role]() {
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
          // Inbound on_connected may have run before StartSfu — re-commit once capture is live.
          AppRuntime::PostUI([this, call_id = params.call_id]() {
            MaybeActivateAdp(direct_.ActiveParams());
            CommitDirectConnected(call_id);
          });
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (direct_.IsActive()) {
        connect_worker_inflight_.store(false);
        AppRuntime::PostUI([this, call_id = params.call_id]() { CommitDirectConnected(call_id); });
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
    AppRuntime::PostUI([this, connected, call_id = params.call_id, role = std::string(role)]() {
      // Dial may have lost the race to inbound; stream up always wins for chrome.
      if (direct_.IsActive()) {
        CommitDirectConnected(call_id);
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
  AppRuntime::PostUI([this, call_id, peer_identity]() {
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
  AppRuntime::PostUI([this, call_id, peer_identity]() {
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
      AppRuntime::PostWorkerBackground([this, call_id]() {
        for (int i = 0; i < kMediaKeyInboxPollRounds; ++i) {
          if (pending_answerer_call_id_ != call_id) {
            return;
          }
          host_.P2pRequestInboxSync();
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          // Belt-and-suspenders if OnMediaKeyReady raced / was missed.
          if (pending_answerer_call_id_ == call_id) {
            if (auto deferred_key = LoadActiveMediaKey(call_id); deferred_key) {
              log().info << "Deferred MediaKey found in store — kick start call_id=" << call_id;
              OnMediaKeyReady(call_id);
              return;
            }
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
          libp2p_connect_failed_ = true;
          host_.P2pSetLastMediaError(err);
          if (lifecycle_) {
            lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
          }
          host_.P2pNotifyRingChanged();
        });
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
  StopAdpPath();
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
  if (AppRuntime::CurrentlyOnUI()) {
    stop_engine();
  } else {
    AppRuntime::PostUI( std::move(stop_engine));
  }
}

void CallLibp2pMediaBridge::ReleaseDirectTransport() {
  // SoftMigrate: drop 1:1 transport; keep CallMediaEngine capture feeding media_relay.
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
  const std::string peer = media_peer_identity_;
  if (dial_ && !peer.empty()) {
    dial_->AbortInflightDial(peer);
    dial_->ClearCallMediaCircuitHop(peer);
  }
  StopAdpPath();
  direct_.Detach();
  media_peer_identity_.clear();
  inbound_deferred_peer_id_.clear();
  inbound_remote_stream_.store(0, std::memory_order_release);
  // Do not ClearRemoteAudioTracks here — SoftMigrate+2s would wipe live media_relay tracks
  // that already replaced 1:1 (dogfood: streams look healthy then Moto silent on PreferLocal).
  // 1:1 on_audio is already ignored once P2pIsSfuAttached(); stream_id==1 is dropped in engine.
  ClearLibp2pConnectFailed();
  if (lifecycle_ && media_.IsActive() && media_.IsSfuMode()) {
    lifecycle_->Apply(CallLifecycleEvent::DirectConnected, media_.ActiveCallId());
  }
  host_.P2pNotifyRingChanged();
}

void CallLibp2pMediaBridge::NotePeerIdRelayMapping(const std::string& peer_id,
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

void CallLibp2pMediaBridge::PrepareForTeardown(int timeout_ms) {
  stopping_.store(true, std::memory_order_release);
  connect_generation_.fetch_add(1, std::memory_order_acq_rel);
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
  StopAdpPath();
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearLibp2pConnectFailed();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
  while (connect_worker_inflight_.load(std::memory_order_acquire)) {
    if (!AppRuntime::IsRunning()) {
      // Pool already joined (or never started) — queued Connect tasks were dropped.
      connect_worker_inflight_.store(false, std::memory_order_release);
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      log().warning << "PrepareForTeardown: Connect worker still inflight after " << timeout_ms
                    << "ms — proceeding (WorkerPool join may still wait on the task)";
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
