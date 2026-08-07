#include "libp2p/integration/host/CallMediaDirectService.h"

#include "common/Module.h"
#include "libp2p/integration/host/CallMediaFrameCrypto.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamFrameIo.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr size_t kMaxCallMediaFrameBytes = 16 * 1024;
constexpr size_t kMaxOutboundFrames = 64;

LengthPrefixedFrameConfig CallMediaFrameConfig() {
  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = kMaxCallMediaFrameBytes;
  config.allow_empty_body = false;
  return config;
}

Roe<void> WriteJson(const std::shared_ptr<Stream>& stream, const nlohmann::json& root) {
  return BlockingWriteStreamJson(stream, root.dump());
}

Roe<nlohmann::json> ReadJson(const std::shared_ptr<Stream>& stream) {
  auto json_utf8 = BlockingReadStreamJson(stream);
  if (!json_utf8) {
    return json_utf8.error();
  }
  nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("invalid call-media json");
  }
  return root;
}

void CloseQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->close([](auto&&) {});
  }
}

} // namespace

const char* CallMediaSessionPhaseName(const CallMediaSessionPhase phase) {
  switch (phase) {
  case CallMediaSessionPhase::Idle:
    return "Idle";
  case CallMediaSessionPhase::Dialing:
    return "Dialing";
  case CallMediaSessionPhase::HelloOutbound:
    return "HelloOutbound";
  case CallMediaSessionPhase::HelloInbound:
    return "HelloInbound";
  case CallMediaSessionPhase::Adopting:
    return "Adopting";
  case CallMediaSessionPhase::MediaReady:
    return "MediaReady";
  case CallMediaSessionPhase::Detaching:
    return "Detaching";
  }
  return "?";
}

const char* CallMediaSessionEventName(const CallMediaSessionEvent ev) {
  switch (ev) {
  case CallMediaSessionEvent::ConnectRequested:
    return "ConnectRequested";
  case CallMediaSessionEvent::OpenStreamOk:
    return "OpenStreamOk";
  case CallMediaSessionEvent::OpenStreamFail:
    return "OpenStreamFail";
  case CallMediaSessionEvent::InboundStream:
    return "InboundStream";
  case CallMediaSessionEvent::HelloOk:
    return "HelloOk";
  case CallMediaSessionEvent::HelloFail:
    return "HelloFail";
  case CallMediaSessionEvent::AdoptWon:
    return "AdoptWon";
  case CallMediaSessionEvent::AdoptLost:
    return "AdoptLost";
  case CallMediaSessionEvent::DuplexStarted:
    return "DuplexStarted";
  case CallMediaSessionEvent::DuplexEof:
    return "DuplexEof";
  case CallMediaSessionEvent::DuplexError:
    return "DuplexError";
  case CallMediaSessionEvent::DetachRequested:
    return "DetachRequested";
  case CallMediaSessionEvent::ConnectTimeout:
    return "ConnectTimeout";
  case CallMediaSessionEvent::HandlerCleared:
    return "HandlerCleared";
  case CallMediaSessionEvent::ConnectSuperseded:
    return "ConnectSuperseded";
  }
  return "?";
}

struct CallMediaDirectService::Impl : Module, std::enable_shared_from_this<Impl> {
  Impl() { redirectLogger("CallMediaDirect"); }

  /** Public for CallMediaDirectService::Connect (outer class cannot see Module::log). */
  logging::Logger& Log() const { return log(); }

  Libp2pHost* host = nullptr;

  std::mutex mu;
  std::atomic<CallMediaSessionPhase> phase{CallMediaSessionPhase::Idle};
  std::string phase_call_id;
  /** Offerer outbound hello — inbound is glare loser (dogfood dual-dial). */
  bool offerer_glare = false;

  std::shared_ptr<Stream> stream;
  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> inbound_handler;

  std::shared_ptr<DuplexFrameSession> duplex;
  std::shared_ptr<std::atomic<bool>> duplex_cancelled;
  std::atomic<uint32_t> decrypt_fail_log_{0};
  std::atomic<uint32_t> drop_log_{0};

  // Connect() waiter owned by the session machine (Detach / timeout / MediaReady complete it).
  std::shared_ptr<std::atomic<bool>> connect_settled;
  std::shared_ptr<std::promise<Roe<void>>> connect_promise;

  CallMediaSessionPhase Phase() const {
    return phase.load(std::memory_order_acquire);
  }

  void SetPhaseLocked(CallMediaSessionPhase next, CallMediaSessionEvent ev,
                      const std::string& call_id = {}) {
    const CallMediaSessionPhase prev = phase.load(std::memory_order_relaxed);
    if (!call_id.empty()) {
      phase_call_id = call_id;
    }
    if (next == CallMediaSessionPhase::Idle || next == CallMediaSessionPhase::Detaching) {
      offerer_glare = false;
    }
    if (next == CallMediaSessionPhase::Idle) {
      phase_call_id.clear();
    }
    if (prev == next) {
      log().info << "phase=" << CallMediaSessionPhaseName(prev)
                 << " event=" << CallMediaSessionEventName(ev) << " call_id=" << phase_call_id;
      return;
    }
    phase.store(next, std::memory_order_release);
    log().info << "phase=" << CallMediaSessionPhaseName(prev) << "->"
               << CallMediaSessionPhaseName(next) << " event=" << CallMediaSessionEventName(ev)
               << " call_id=" << phase_call_id;
  }

  void IgnoreEventLocked(CallMediaSessionEvent ev, const char* reason) {
    log().warning << "call_media_session ignore event=" << CallMediaSessionEventName(ev)
                  << " phase=" << CallMediaSessionPhaseName(Phase()) << " reason="
                  << (reason ? reason : "");
  }

  /** True while Connect() waiter is still outstanding (not yet settled). */
  bool ConnectWaiterActiveLocked() const {
    return connect_settled && !connect_settled->load(std::memory_order_acquire);
  }

  /**
   * Sole legal phase-transition entry (CallLifecycle-style). Must hold mu.
   * Returns false when the event is ignored for the current phase.
   * Side effects (stream IO, waiters, duplex) stay in callers; this only moves phase.
   */
  bool ApplyLocked(CallMediaSessionEvent ev, const std::string& call_id = {}) {
    const CallMediaSessionPhase p = Phase();
    switch (ev) {
    case CallMediaSessionEvent::ConnectRequested:
      SetPhaseLocked(CallMediaSessionPhase::Dialing, ev, call_id);
      return true;

    case CallMediaSessionEvent::OpenStreamOk:
      if (p == CallMediaSessionPhase::Dialing) {
        SetPhaseLocked(CallMediaSessionPhase::HelloOutbound, ev, call_id);
        return true;
      }
      // Inbound hello failed while Connect waiter still live — resume outbound.
      if (p == CallMediaSessionPhase::Idle && ConnectWaiterActiveLocked()) {
        SetPhaseLocked(CallMediaSessionPhase::Dialing, ev, call_id);
        return true;
      }
      // Dual-dial: outbound hello proceeds without stealing HelloInbound.
      if (p == CallMediaSessionPhase::HelloInbound) {
        log().info << "phase=" << CallMediaSessionPhaseName(p)
                   << " event=" << CallMediaSessionEventName(ev) << " call_id=" << call_id
                   << " (outbound hello; inbound in flight)";
        return true;
      }
      if (p == CallMediaSessionPhase::Detaching) {
        IgnoreEventLocked(ev, "detaching");
        return false;
      }
      if (p == CallMediaSessionPhase::Idle) {
        IgnoreEventLocked(ev, "connect no longer active");
        return false;
      }
      IgnoreEventLocked(ev, "unexpected phase for OpenStreamOk");
      return false;

    case CallMediaSessionEvent::OpenStreamFail:
      if (p == CallMediaSessionPhase::Dialing || p == CallMediaSessionPhase::HelloOutbound) {
        SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        return true;
      }
      IgnoreEventLocked(ev, "not in outbound dial");
      return false;

    case CallMediaSessionEvent::InboundStream:
      SetPhaseLocked(CallMediaSessionPhase::HelloInbound, ev, call_id);
      return true;

    case CallMediaSessionEvent::HelloOk:
      if (p == CallMediaSessionPhase::HelloInbound) {
        SetPhaseLocked(CallMediaSessionPhase::HelloInbound, ev, call_id);
        return true;
      }
      if (p == CallMediaSessionPhase::HelloOutbound || p == CallMediaSessionPhase::Dialing) {
        SetPhaseLocked(CallMediaSessionPhase::HelloOutbound, ev, call_id);
        return true;
      }
      IgnoreEventLocked(ev, "unexpected phase for HelloOk");
      return false;

    case CallMediaSessionEvent::HelloFail:
      if (p == CallMediaSessionPhase::HelloInbound) {
        // Restore Dialing if outbound Connect waiter still active (dogfood hang fix).
        if (ConnectWaiterActiveLocked() && !stream) {
          SetPhaseLocked(CallMediaSessionPhase::Dialing, ev, call_id);
        } else {
          SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        }
        return true;
      }
      if (p == CallMediaSessionPhase::HelloOutbound || p == CallMediaSessionPhase::Dialing) {
        SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        return true;
      }
      IgnoreEventLocked(ev, "unexpected phase for HelloFail");
      return false;

    case CallMediaSessionEvent::AdoptWon:
      SetPhaseLocked(CallMediaSessionPhase::Adopting, ev, call_id);
      return true;

    case CallMediaSessionEvent::AdoptLost:
      if (p == CallMediaSessionPhase::HelloInbound) {
        if (ConnectWaiterActiveLocked() && !stream) {
          SetPhaseLocked(CallMediaSessionPhase::Dialing, ev, call_id);
        } else {
          SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        }
        return true;
      }
      if (p == CallMediaSessionPhase::HelloOutbound || p == CallMediaSessionPhase::Dialing) {
        SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        return true;
      }
      IgnoreEventLocked(ev, "unexpected phase for AdoptLost");
      return false;

    case CallMediaSessionEvent::DuplexStarted:
      if (p == CallMediaSessionPhase::Adopting || p == CallMediaSessionPhase::HelloOutbound ||
          p == CallMediaSessionPhase::HelloInbound) {
        SetPhaseLocked(CallMediaSessionPhase::MediaReady, ev, call_id);
        return true;
      }
      if (p == CallMediaSessionPhase::MediaReady) {
        return true;
      }
      IgnoreEventLocked(ev, "detach raced duplex start");
      return false;

    case CallMediaSessionEvent::ConnectTimeout:
      if (p == CallMediaSessionPhase::Dialing || p == CallMediaSessionPhase::HelloOutbound) {
        SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        return true;
      }
      IgnoreEventLocked(ev, "not waiting on connect");
      return false;

    case CallMediaSessionEvent::DetachRequested:
      // Late outbound abort (settled mid-hello) — Idle without full DetachLocked teardown.
      // MediaReady / Adopting teardown uses DetachLocked → SetPhase(Detaching→Idle).
      if (p == CallMediaSessionPhase::Dialing || p == CallMediaSessionPhase::HelloOutbound) {
        SetPhaseLocked(CallMediaSessionPhase::Idle, ev, call_id);
        return true;
      }
      return true;

    case CallMediaSessionEvent::DuplexEof:
    case CallMediaSessionEvent::DuplexError:
    case CallMediaSessionEvent::ConnectSuperseded:
    case CallMediaSessionEvent::HandlerCleared:
      // Compound effects live in Fail / DetachLocked / ClearInboundHandler.
      return true;
    }
    IgnoreEventLocked(ev, "unhandled event");
    return false;
  }

  bool MediaReady() const {
    return Phase() == CallMediaSessionPhase::MediaReady;
  }

  bool HandleMediaFrame(Roe<std::vector<uint8_t>> frame_res) {
    if (!frame_res || frame_res->empty()) {
      return false;
    }
    if ((*frame_res)[0] == '{') {
      return true;
    }
    CallMediaDirectConnectParams params;
    CallMediaDirectCallbacks cbs;
    {
      std::lock_guard lock(mu);
      params = active_params;
      cbs = callbacks;
    }
    auto opus = DecryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, *frame_res);
    if (!opus) {
      if ((decrypt_fail_log_.fetch_add(1) % 25) == 0) {
        log().warning << "Call-media decrypt failed call_id=" << params.call_id
                      << " epoch=" << params.media_epoch << " err=" << opus.error().message;
      }
      return true;
    }
    if (cbs.on_audio) {
      cbs.on_audio(*opus);
    }
    return true;
  }

  void CompleteConnectLocked(Roe<void> value) {
    if (!connect_settled) {
      return;
    }
    if (!connect_settled->exchange(true)) {
      try {
        if (connect_promise) {
          connect_promise->set_value(std::move(value));
        }
      } catch (const std::future_error&) {
      }
    }
    connect_settled.reset();
    connect_promise.reset();
  }

  void TeardownTransportLocked() {
    offerer_glare = false;
    if (duplex_cancelled) {
      duplex_cancelled->store(true, std::memory_order_release);
    }
    if (duplex) {
      duplex->Stop();
      duplex.reset();
    }
    duplex_cancelled.reset();
    if (stream) {
      CloseQuiet(stream);
      stream.reset();
    }
    callbacks = {};
    active_params = {};
  }

  /** DetachRequested — abort waiter, tear down, Idle. */
  void DetachLocked(bool abort_connect, CallMediaSessionEvent ev = CallMediaSessionEvent::DetachRequested) {
    const CallMediaSessionPhase prev = Phase();
    if (prev != CallMediaSessionPhase::Idle) {
      SetPhaseLocked(CallMediaSessionPhase::Detaching, ev, phase_call_id);
    }
    if (abort_connect) {
      CompleteConnectLocked(Error("call-media aborted"));
    }
    TeardownTransportLocked();
    if (prev != CallMediaSessionPhase::Idle) {
      SetPhaseLocked(CallMediaSessionPhase::Idle, ev);
    }
  }

  void Fail(const std::string& message, CallMediaSessionEvent ev = CallMediaSessionEvent::DuplexError) {
    CallMediaDirectCallbacks cbs;
    {
      std::lock_guard lock(mu);
      // Intentional Detach / already torn down: ignore late duplex EOF (SoftMigrate ReleaseDirect).
      if (Phase() == CallMediaSessionPhase::Idle || Phase() == CallMediaSessionPhase::Detaching) {
        IgnoreEventLocked(ev, "already detaching or idle");
        return;
      }
      cbs = callbacks;
      // Instant Failed → Idle (s1 freeze): log via SetPhase path inside DetachLocked.
      DetachLocked(/*abort_connect=*/true, ev);
    }
    if (cbs.on_failed) {
      cbs.on_failed(message);
    }
  }

  /** First successful hello wins; loser closes. */
  bool TryAdoptStreamLocked(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
                            CallMediaDirectCallbacks cbs) {
    if (stream) {
      return false;
    }
    (void)ApplyLocked(CallMediaSessionEvent::AdoptWon, params.call_id);
    stream = std::move(s);
    active_params = std::move(params);
    callbacks = std::move(cbs);
    offerer_glare = false;
    return true;
  }

  void StartMediaDuplex(std::function<void()> on_ready = {}) {
    std::shared_ptr<Stream> s;
    {
      std::lock_guard lock(mu);
      s = stream;
    }
    if (!s || !host) {
      return;
    }
    duplex = std::make_shared<DuplexFrameSession>();
    duplex_cancelled = std::make_shared<std::atomic<bool>>(false);
    auto self = shared_from_this();
    host->Post([self, s, on_ready = std::move(on_ready)]() mutable {
      if (!self->duplex || self->Phase() == CallMediaSessionPhase::Idle ||
          self->Phase() == CallMediaSessionPhase::Detaching) {
        return;
      }
      const auto on_drop = [self]() {
        if ((self->drop_log_.fetch_add(1) % 50) == 0) {
          self->log().warning << "Call-media outbound queue full; dropping oldest";
        }
      };
      self->duplex->Start(
          s,
          [self](Roe<std::vector<uint8_t>> frame) { return self->HandleMediaFrame(std::move(frame)); },
          [cancelled = self->duplex_cancelled]() {
            return cancelled && cancelled->load(std::memory_order_acquire);
          },
          CallMediaFrameConfig(),
          [self](const char* reason) {
            self->Fail(std::string("call-media stream closed (") +
                           (reason && reason[0] ? reason : "unknown") + ")",
                       CallMediaSessionEvent::DuplexEof);
          },
          kMaxOutboundFrames, on_drop,
          /*write_preferred=*/true);
      {
        std::lock_guard lock(self->mu);
        if (!self->ApplyLocked(CallMediaSessionEvent::DuplexStarted, self->active_params.call_id) &&
            self->Phase() != CallMediaSessionPhase::MediaReady) {
          // Detach raced the io post — do not claim ready.
          return;
        }
      }
      if (on_ready) {
        on_ready();
      }
    });
  }

  bool EnqueueOutbound(std::vector<uint8_t> body) {
    if (!host || !MediaReady()) {
      return false;
    }
    auto self = shared_from_this();
    host->Post([self, body = std::move(body)]() mutable {
      if (!self->duplex || !self->MediaReady()) {
        return;
      }
      self->duplex->EnqueueOutbound(std::move(body));
    });
    return true;
  }

  /** Guard for inbound admit (HOST_RECEIVE_POLICY / V033 glare note). */
  enum class InboundAdmit { Accept, RejectNoHandler, RejectActive, RejectGlare };

  InboundAdmit AdmitInboundLocked() {
    if (!inbound_handler) {
      return InboundAdmit::RejectNoHandler;
    }
    if (stream || Phase() == CallMediaSessionPhase::MediaReady ||
        Phase() == CallMediaSessionPhase::Adopting || Phase() == CallMediaSessionPhase::Detaching ||
        Phase() == CallMediaSessionPhase::HelloInbound) {
      return InboundAdmit::RejectActive;
    }
    // Glare: only offerer HelloOutbound rejects inbound. Dialing must still accept reverse-dial.
    if (Phase() == CallMediaSessionPhase::HelloOutbound && offerer_glare) {
      return InboundAdmit::RejectGlare;
    }
    return InboundAdmit::Accept;
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_in) {
    log().info << "Inbound call-media stream (protocol negotiated)";
    if (!host) {
      CloseQuiet(stream_in.stream);
      return;
    }
    auto stream = std::move(stream_in.stream);
    // Normal lane: hello/ack must not share Critical with Connect waiters (2-thread deadlock
    // when dual-dial races — Linux/Windows dogfood hang + late inbound segfault).
    PostLibp2pWorker(*host, WorkerLane::Normal, [self = shared_from_this(), stream = std::move(stream)]() mutable {
      {
        std::lock_guard lock(self->mu);
        const auto admit = self->AdmitInboundLocked();
        if (admit == InboundAdmit::RejectNoHandler) {
          self->IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "handler cleared");
          CloseQuiet(stream);
          return;
        }
        if (admit == InboundAdmit::RejectActive) {
          self->IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "session already active");
          CloseQuiet(stream);
          return;
        }
        if (admit == InboundAdmit::RejectGlare) {
          self->IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "glare outbound hello");
          CloseQuiet(stream);
          return;
        }
        (void)self->ApplyLocked(CallMediaSessionEvent::InboundStream);
      }

      auto hello = ReadJson(stream);
      if (!hello || hello->value("type", "") != "hello") {
        self->log().warning << "Inbound call-media hello read failed err="
                            << (hello ? "bad type" : hello.error().message);
        {
          std::lock_guard lock(self->mu);
          if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
            (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail);
          }
        }
        CloseQuiet(stream);
        return;
      }
      CallMediaDirectConnectParams params;
      params.call_id = hello->value("call_id", "");
      params.media_epoch = hello->value("media_epoch", 1u);
      params.offerer = hello->value("role", "") == "offerer";
      if (auto peer = stream->remotePeerId()) {
        params.peer_key = peer.value().toBase58();
      }

      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler;
      {
        std::lock_guard lock(self->mu);
        handler = self->inbound_handler;
        if (self->stream || self->Phase() == CallMediaSessionPhase::MediaReady ||
            self->Phase() == CallMediaSessionPhase::Adopting) {
          self->IgnoreEventLocked(CallMediaSessionEvent::HelloOk, "lost race after hello");
          if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
            (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
          }
          CloseQuiet(stream);
          return;
        }
        if (self->Phase() != CallMediaSessionPhase::HelloInbound) {
          // Detach during hello read.
          CloseQuiet(stream);
          return;
        }
      }
      CallMediaDirectCallbacks cbs;
      if (!handler) {
        self->IgnoreEventLocked(CallMediaSessionEvent::HelloOk, "handler cleared mid-hello");
        (void)WriteJson(stream, {{"v", 1}, {"type", "hello_ack"}, {"ok", false}, {"error", "unavailable"}});
        {
          std::lock_guard lock(self->mu);
          if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
            (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
          }
        }
        CloseQuiet(stream);
        return;
      }
      handler(params, cbs);
      if (params.media_key.empty() || params.call_id.empty()) {
        self->log().warning << "Inbound call-media hello rejected call_id=" << params.call_id
                            << " key_empty=" << (params.media_key.empty() ? 1 : 0);
        (void)WriteJson(stream, {{"v", 1}, {"type", "hello_ack"}, {"ok", false}, {"error", "rejected"}});
        {
          std::lock_guard lock(self->mu);
          if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
            (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
          }
        }
        CloseQuiet(stream);
        return;
      }
      if (!(WriteJson(stream, {{"v", 1}, {"type", "hello_ack"}, {"ok", true}}))) {
        {
          std::lock_guard lock(self->mu);
          if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
            (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
          }
        }
        CloseQuiet(stream);
        return;
      }

      CallMediaDirectCallbacks adopted_cbs;
      bool adopted = false;
      {
        std::lock_guard lock(self->mu);
        if (self->Phase() != CallMediaSessionPhase::HelloInbound) {
          CloseQuiet(stream);
          return;
        }
        (void)self->ApplyLocked(CallMediaSessionEvent::HelloOk, params.call_id);
        adopted = self->TryAdoptStreamLocked(stream, params, std::move(cbs));
        if (adopted) {
          adopted_cbs = self->callbacks;
        } else {
          (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
        }
      }
      if (!adopted) {
        self->log().info << "Inbound call-media lost adopt race call_id=" << params.call_id;
        CloseQuiet(stream);
        return;
      }
      self->StartMediaDuplex([self, adopted_cbs = std::move(adopted_cbs)]() {
        {
          std::lock_guard lock(self->mu);
          self->CompleteConnectLocked({});
        }
        if (adopted_cbs.on_connected) {
          adopted_cbs.on_connected();
        }
      });
    });
  }
};

CallMediaDirectService::CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
}

CallMediaDirectService::~CallMediaDirectService() {
  Stop();
}

void CallMediaDirectService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  auto impl = impl_;
  host_.GetHost().setProtocolHandler({ProtocolName{kCallMediaDirectProtocolId}},
                                     [impl](libp2p::StreamAndProtocol stream) {
                                       impl->HandleInbound(std::move(stream));
                                     });
}

void CallMediaDirectService::Stop() {
  started_ = false;
  ClearInboundHandler();
  Detach();
}

void CallMediaDirectService::SetInboundHandler(
    std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) {
  std::lock_guard lock(impl_->mu);
  impl_->inbound_handler = std::move(handler);
}

void CallMediaDirectService::ClearInboundHandler() {
  std::lock_guard lock(impl_->mu);
  impl_->inbound_handler = {};
  impl_->Log().info << "phase=" << CallMediaSessionPhaseName(impl_->Phase())
                    << " event=" << CallMediaSessionEventName(CallMediaSessionEvent::HandlerCleared);
}

bool CallMediaDirectService::IsActive() const {
  std::lock_guard lock(impl_->mu);
  return impl_->stream != nullptr;
}

CallMediaSessionPhase CallMediaDirectService::Phase() const {
  return impl_->Phase();
}

void CallMediaDirectService::Detach() {
  std::lock_guard lock(impl_->mu);
  impl_->DetachLocked(/*abort_connect=*/true, CallMediaSessionEvent::DetachRequested);
}

Roe<void> CallMediaDirectService::Connect(const CallMediaDirectConnectParams& params,
                                          CallMediaDirectCallbacks callbacks, int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("call-media host not running");
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    return Error("call-media connect params incomplete");
  }
  if (!sessions_.IsDialable(params.peer_key)) {
    return Error("call-media peer not dialable");
  }

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 15000) + 1000;

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  {
    std::lock_guard lock(impl_->mu);
    // Already MediaReady with a stream — idempotent success (bridge may retry).
    if (impl_->stream && impl_->Phase() == CallMediaSessionPhase::MediaReady) {
      return {};
    }
    if (impl_->stream) {
      return {};
    }
    // s1: Detach-then-Connect when a prior dial/hello is in flight.
    if (impl_->Phase() != CallMediaSessionPhase::Idle) {
      impl_->DetachLocked(/*abort_connect=*/true, CallMediaSessionEvent::ConnectSuperseded);
    }
    if (impl_->connect_settled && !impl_->connect_settled->exchange(true)) {
      try {
        if (impl_->connect_promise) {
          impl_->connect_promise->set_value(Error("call-media superseded"));
        }
      } catch (const std::future_error&) {
      }
    }
    impl_->connect_settled = settled;
    impl_->connect_promise = result_promise;
    (void)impl_->ApplyLocked(CallMediaSessionEvent::ConnectRequested, params.call_id);
  }

  sessions_.OpenStream(params.peer_key, {ProtocolName{kCallMediaDirectProtocolId}},
                       [impl = impl_, params, callbacks = std::move(callbacks), settled](
                           outcome::result<libp2p::StreamAndProtocol> stream_res) mutable {
                         if (settled->load(std::memory_order_acquire)) {
                           if (stream_res) {
                             CloseQuiet(stream_res.value().stream);
                           }
                           return; // Connect already aborted / timed out / inbound won
                         }
                         auto* host = impl->host;
                         if (!host) {
                           return;
                         }
                         // Normal lane — must not block Critical (Connect waiter + inbound key wait).
                         PostLibp2pWorker(*host, WorkerLane::Normal,
                                          [impl, params, callbacks = std::move(callbacks), settled,
                                           stream_res = std::move(stream_res)]() mutable {
                           // Outbound Connect may overlap inbound HelloInbound (dual-dial): do not
                           // require Phase==Dialing exclusively. Key off settled/stream + offerer_glare.
                           // Phase moves go through ApplyLocked only.
                           auto finish = [&](Roe<void> value) {
                             std::lock_guard lock(impl->mu);
                             impl->offerer_glare = false;
                             if (!value) {
                               (void)impl->ApplyLocked(CallMediaSessionEvent::OpenStreamFail,
                                                       params.call_id);
                             } else if (!impl->stream) {
                               (void)impl->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                             }
                             impl->CompleteConnectLocked(std::move(value));
                           };
                           if (settled->load(std::memory_order_acquire)) {
                             if (stream_res) {
                               CloseQuiet(stream_res.value().stream);
                             }
                             return;
                           }
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream) {
                               // Inbound won while OpenStream was in flight.
                               if (stream_res) {
                                 CloseQuiet(stream_res.value().stream);
                               }
                               finish({});
                               return;
                             }
                             // MediaReady/Adopting: inbound already won — complete Connect OK.
                             if (impl->Phase() == CallMediaSessionPhase::MediaReady ||
                                 impl->Phase() == CallMediaSessionPhase::Adopting) {
                               if (stream_res) {
                                 CloseQuiet(stream_res.value().stream);
                               }
                               finish({});
                               return;
                             }
                             // Idle+waiter → Dialing; Detaching / Idle without waiter → ignore.
                             if (impl->Phase() == CallMediaSessionPhase::Idle ||
                                 impl->Phase() == CallMediaSessionPhase::Detaching) {
                               if (!impl->ApplyLocked(CallMediaSessionEvent::OpenStreamOk,
                                                      params.call_id)) {
                                 if (stream_res) {
                                   CloseQuiet(stream_res.value().stream);
                                 }
                                 return;
                               }
                             }
                           }
                                                      if (!stream_res) {
                             std::string detail = "call-media dial failed";
                             try {
                               detail += ": ";
                               detail += stream_res.error().message();
                             } catch (...) {
                             }
                             impl->Log().warning << "Call-media OpenStream failed peer=" << params.peer_key
                                                 << " role=" << (params.offerer ? "offerer" : "answerer")
                                                 << " err=" << detail;
                             finish(Error(detail));
                             return;
                           }
                           impl->Log().warning << "Call-media OpenStream ok peer=" << params.peer_key
                                               << " role=" << (params.offerer ? "offerer" : "answerer")
                                               << " call_id=" << params.call_id;
                           auto stream = std::move(stream_res.value().stream);
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream) {
                               CloseQuiet(stream);
                               finish({});
                               return;
                             }
                             // Dialing → HelloOutbound; HelloInbound → log-only (dual-dial).
                             if (!impl->ApplyLocked(CallMediaSessionEvent::OpenStreamOk, params.call_id)) {
                               CloseQuiet(stream);
                               return;
                             }
                             // Preserve dogfood glare: only offerer outbound rejects inbound.
                             impl->offerer_glare = params.offerer;
                           }
                           if (settled->load(std::memory_order_acquire)) {
                             {
                               std::lock_guard lock(impl->mu);
                               impl->offerer_glare = false;
                               (void)impl->ApplyLocked(CallMediaSessionEvent::DetachRequested,
                                                       params.call_id);
                             }
                             CloseQuiet(stream);
                             return;
                           }
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream) {
                               impl->offerer_glare = false;
                               (void)impl->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                               CloseQuiet(stream);
                               finish({});
                               return;
                             }
                           }
                           const std::string role = params.offerer ? "offerer" : "answerer";
                           if (!(WriteJson(stream, {{"v", 1},
                                                    {"type", "hello"},
                                                    {"call_id", params.call_id},
                                                    {"media_epoch", params.media_epoch},
                                                    {"role", role}}))) {
                             impl->Log().warning << "Call-media hello write failed peer=" << params.peer_key;
                             {
                               std::lock_guard lock(impl->mu);
                               impl->offerer_glare = false;
                               (void)impl->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
                               impl->CompleteConnectLocked(Error("call-media hello write failed"));
                             }
                             CloseQuiet(stream);
                             return;
                           }
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream) {
                               // Inbound adopted during hello write — drop our dialed stream.
                               impl->offerer_glare = false;
                               CloseQuiet(stream);
                               finish({});
                               return;
                             }
                           }
                           auto ack = ReadJson(stream);
                           {
                             std::lock_guard lock(impl->mu);
                             impl->offerer_glare = false;
                           }
                           if (settled->load(std::memory_order_acquire)) {
                             CloseQuiet(stream);
                             return;
                           }
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream) {
                               CloseQuiet(stream);
                               finish({});
                               return;
                             }
                             // Idle/Detaching: Connect was aborted; drop late hello.
                             if (impl->Phase() == CallMediaSessionPhase::Idle ||
                                 impl->Phase() == CallMediaSessionPhase::Detaching ||
                                 impl->Phase() == CallMediaSessionPhase::MediaReady ||
                                 impl->Phase() == CallMediaSessionPhase::Adopting) {
                               CloseQuiet(stream);
                               if (impl->Phase() == CallMediaSessionPhase::MediaReady ||
                                   impl->Phase() == CallMediaSessionPhase::Adopting) {
                                 finish({});
                               } else {
                                 impl->IgnoreEventLocked(CallMediaSessionEvent::HelloOk,
                                                         "connect no longer active");
                               }
                               return;
                             }
                           }
                           if (!ack || !ack->value("ok", false)) {
                             const std::string why =
                                 ack ? ack->value("error", "hello rejected") : ack.error().message;
                             impl->Log().warning << "Call-media hello rejected peer=" << params.peer_key
                                                 << " err=" << why;
                             {
                               std::lock_guard lock(impl->mu);
                               (void)impl->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
                               impl->CompleteConnectLocked(Error(why));
                             }
                             CloseQuiet(stream);
                             return;
                           }
                           CallMediaDirectCallbacks adopted_cbs;
                           bool adopted = false;
                           {
                             std::lock_guard lock(impl->mu);
                             if (impl->stream || impl->Phase() == CallMediaSessionPhase::Idle ||
                                 impl->Phase() == CallMediaSessionPhase::Detaching) {
                               CloseQuiet(stream);
                               if (impl->stream) {
                                 finish({});
                               }
                               return;
                             }
                             (void)impl->ApplyLocked(CallMediaSessionEvent::HelloOk, params.call_id);
                             adopted = impl->TryAdoptStreamLocked(stream, params, std::move(callbacks));
                             if (adopted) {
                               adopted_cbs = impl->callbacks;
                             } else {
                               (void)impl->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                             }
                           }
                           if (!adopted) {
                             impl->Log().info << "Call-media outbound lost adopt race call_id="
                                              << params.call_id;
                             CloseQuiet(stream);
                             finish({});
                             return;
                           }
                           impl->StartMediaDuplex([impl, adopted_cbs = std::move(adopted_cbs)]() mutable {
                             if (adopted_cbs.on_connected) {
                               adopted_cbs.on_connected();
                             }
                             std::lock_guard lock(impl->mu);
                             impl->CompleteConnectLocked(Roe<void>{});
                           });
                         });
                       });

  // Slice the wait so Detach can complete the promise without blocking Leave/shutdown for 15s+.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    const auto status = result_future.wait_for(std::chrono::milliseconds(50));
    if (status == std::future_status::ready) {
      std::lock_guard lock(impl_->mu);
      if (impl_->connect_settled == settled) {
        impl_->connect_settled.reset();
        impl_->connect_promise.reset();
      }
      return result_future.get();
    }
    {
      std::lock_guard lock(impl_->mu);
      if (impl_->stream != nullptr && impl_->MediaReady()) {
        settled->store(true, std::memory_order_release);
        if (impl_->connect_settled == settled) {
          impl_->connect_settled.reset();
          impl_->connect_promise.reset();
        }
        return {};
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      settled->exchange(true);
      bool active_ready = false;
      {
        std::lock_guard lock(impl_->mu);
        if (impl_->connect_settled == settled) {
          impl_->connect_settled.reset();
          impl_->connect_promise.reset();
        }
        active_ready = impl_->stream != nullptr && impl_->MediaReady();
        if (!active_ready) {
          // Late OpenStreamOk / hello must be ignored once we leave Dialing/HelloOutbound.
          impl_->offerer_glare = false;
          (void)impl_->ApplyLocked(CallMediaSessionEvent::ConnectTimeout, params.call_id);
        }
      }
      if (active_ready) {
        return {};
      }
      return Error("call-media connect timed out");
    }
  }
}

Roe<void> CallMediaDirectService::SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq,
                                            uint8_t mark) {
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    if (!impl_->stream || !impl_->MediaReady()) {
      return Error("call-media not connected");
    }
    params = impl_->active_params;
  }
  auto body = EncryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, seq, mark,
                                         opus_payload);
  if (!body) {
    return body.error();
  }
  if (!impl_->EnqueueOutbound(std::move(*body))) {
    return Error("call-media pump not running");
  }
  return {};
}

} // namespace pbr
