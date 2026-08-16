#include "libp2p/integration/host/CallMediaDirectService.h"

#include "common/Module.h"
#include "libp2p/integration/host/CallMediaFrameCrypto.h"
#include "libp2p/integration/host/CallMediaSessionLogic.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamFrameIo.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
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

/** Default bound for hello/ack IO when Connect does not supply timeout_ms (inbound-only). */
constexpr int kDefaultHandshakeTimeoutMs = 15000;

void ResetQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->reset();
  }
}

Roe<nlohmann::json> ParseJsonObject(const std::string& json_utf8) {
  nlohmann::json root = nlohmann::json::parse(json_utf8, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("invalid call-media json");
  }
  return root;
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
  /** In-flight hello (inbound or outbound), not yet adopted. */
  std::shared_ptr<Stream> handshake_stream;
  std::shared_ptr<std::atomic<bool>> handshake_cancelled;
  std::shared_ptr<boost::asio::steady_timer> handshake_timer;
  /** Armed with Connect(timeout_ms) or kDefaultHandshakeTimeoutMs for inbound-only. */
  int handshake_timeout_ms = kDefaultHandshakeTimeoutMs;
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

  void CancelHandshakeTimerLocked() {
    if (handshake_timer) {
      (void)handshake_timer->cancel();
      handshake_timer.reset();
    }
  }

  /**
   * Peer may stall forever — bound every hello/ack wait. On expiry: cancel flag + stream
   * reset (same compound effect as Detach), complete Connect waiter if any.
   */
  void StartHandshakeTimerLocked() {
    CancelHandshakeTimerLocked();
    if (!host) {
      return;
    }
    const auto ex = host->IoExecutor();
    if (!ex) {
      return;
    }
    const int ms = handshake_timeout_ms > 0 ? handshake_timeout_ms : kDefaultHandshakeTimeoutMs;
    auto timer = std::make_shared<boost::asio::steady_timer>(ex);
    handshake_timer = timer;
    auto token = handshake_cancelled;
    auto self = shared_from_this();
    timer->expires_after(std::chrono::milliseconds(ms));
    timer->async_wait([self, timer, token](const boost::system::error_code& ec) {
      if (ec) {
        return; // cancelled
      }
      std::lock_guard lock(self->mu);
      if (!token || token->load(std::memory_order_acquire)) {
        return;
      }
      if (self->handshake_cancelled != token) {
        return; // superseded handshake
      }
      const auto p = self->Phase();
      if (p != CallMediaSessionPhase::HelloOutbound && p != CallMediaSessionPhase::HelloInbound) {
        return;
      }
      self->log().warning << "call-media handshake timed out phase=" << CallMediaSessionPhaseName(p)
                          << " call_id=" << self->phase_call_id;
      if (p != CallMediaSessionPhase::Idle) {
        self->SetPhaseLocked(CallMediaSessionPhase::Detaching, CallMediaSessionEvent::ConnectTimeout,
                             self->phase_call_id);
      }
      self->CompleteConnectLocked(Error("call-media handshake timed out"));
      self->TeardownTransportLocked();
      self->SetPhaseLocked(CallMediaSessionPhase::Idle, CallMediaSessionEvent::ConnectTimeout);
    });
  }

  void ArmHandshakeLocked(std::shared_ptr<Stream> s) {
    CancelHandshakeTimerLocked();
    handshake_stream = std::move(s);
    handshake_cancelled = std::make_shared<std::atomic<bool>>(false);
    StartHandshakeTimerLocked();
  }

  void ClearHandshakeLocked() {
    CancelHandshakeTimerLocked();
    handshake_stream.reset();
    handshake_cancelled.reset();
  }

  StreamCancelCheck HandshakeCancelCheck() const {
    auto cancelled = handshake_cancelled;
    return [cancelled]() {
      return cancelled && cancelled->load(std::memory_order_acquire);
    };
  }

  bool HandshakeCancelledLocked() const {
    return handshake_cancelled && handshake_cancelled->load(std::memory_order_acquire);
  }

  bool LocalWinsGlareFor(const std::shared_ptr<Stream>& s) const {
    if (!host || !s) {
      return true;
    }
    const auto local = host->LocalPeerIdBase58();
    const auto remote = s->remotePeerId();
    if (!local || !remote) {
      return true;
    }
    return LocalWinsCallMediaGlare(*local, remote.value().toBase58());
  }

  /** Glare loser: drop outbound hello so inbound (peer's outbound) can be the one stream. */
  void AbandonOutboundHandshakeLocked() {
    offerer_glare = false;
    if (handshake_cancelled) {
      handshake_cancelled->store(true, std::memory_order_release);
    }
    if (handshake_stream) {
      ResetQuiet(handshake_stream);
    }
    ClearHandshakeLocked();
  }

  /**
   * Outbound hello failed but inbound can still win (dual-dial / glare yield). Keep the
   * Connect waiter instead of Idling.
   */
  bool SuppressOutboundHelloFailLocked(const std::shared_ptr<Stream>& s) {
    if (!ConnectWaiterActiveLocked() || stream) {
      return false;
    }
    if (Phase() == CallMediaSessionPhase::HelloInbound) {
      offerer_glare = false;
      if (handshake_stream == s) {
        ClearHandshakeLocked();
      }
      return true;
    }
    if (Phase() == CallMediaSessionPhase::HelloOutbound && !LocalWinsGlareFor(s)) {
      log().info << "Call-media outbound hello lost glare; wait for inbound";
      offerer_glare = false;
      SetPhaseLocked(CallMediaSessionPhase::Dialing, CallMediaSessionEvent::HelloFail, phase_call_id);
      if (handshake_stream == s) {
        ClearHandshakeLocked();
      }
      return true;
    }
    return false;
  }

  /**
   * Sole legal phase-transition entry (CallLifecycle-style). Must hold mu.
   * Returns false when the event is ignored for the current phase.
   * Side effects (stream IO, waiters, duplex) stay in callers; this only moves phase.
   */
  bool ApplyLocked(CallMediaSessionEvent ev, const std::string& call_id = {}) {
    const CallMediaSessionPhase p = Phase();
    CallMediaSessionApplyContext ctx;
    ctx.connect_waiter_active = ConnectWaiterActiveLocked();
    ctx.has_stream = static_cast<bool>(stream);
    const CallMediaSessionPhaseOutcome outcome = DecideCallMediaSessionPhase(p, ev, ctx);
    if (outcome.decision == CallMediaSessionPhaseDecision::Ignore) {
      if (ev == CallMediaSessionEvent::OpenStreamOk) {
        if (p == CallMediaSessionPhase::Detaching) {
          IgnoreEventLocked(ev, "detaching");
        } else if (p == CallMediaSessionPhase::Idle) {
          IgnoreEventLocked(ev, "connect no longer active");
        } else {
          IgnoreEventLocked(ev, "unexpected phase for OpenStreamOk");
        }
      } else if (ev == CallMediaSessionEvent::OpenStreamFail) {
        IgnoreEventLocked(ev, "not in outbound dial");
      } else if (ev == CallMediaSessionEvent::HelloOk) {
        IgnoreEventLocked(ev, "unexpected phase for HelloOk");
      } else if (ev == CallMediaSessionEvent::HelloFail) {
        IgnoreEventLocked(ev, "unexpected phase for HelloFail");
      } else if (ev == CallMediaSessionEvent::AdoptLost) {
        IgnoreEventLocked(ev, "unexpected phase for AdoptLost");
      } else if (ev == CallMediaSessionEvent::DuplexStarted) {
        IgnoreEventLocked(ev, "detach raced duplex start");
      } else if (ev == CallMediaSessionEvent::ConnectTimeout) {
        IgnoreEventLocked(ev, "not waiting on connect");
      } else {
        IgnoreEventLocked(ev, "unhandled event");
      }
      return false;
    }
    if (outcome.decision == CallMediaSessionPhaseDecision::Keep) {
      if (ev == CallMediaSessionEvent::OpenStreamOk && p == CallMediaSessionPhase::HelloInbound) {
        log().info << "phase=" << CallMediaSessionPhaseName(p)
                   << " event=" << CallMediaSessionEventName(ev) << " call_id=" << call_id
                   << " (outbound hello; inbound in flight)";
      }
      return true;
    }
    SetPhaseLocked(outcome.next, ev, call_id);
    return true;
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

  /** OpenStream fail / adopt-lost success paths. Must hold mu. */
  void FinishOutboundConnectLocked(Roe<void> value, const std::string& call_id) {
    offerer_glare = false;
    if (!value) {
      (void)ApplyLocked(CallMediaSessionEvent::OpenStreamFail, call_id);
    } else if (!stream) {
      (void)ApplyLocked(CallMediaSessionEvent::AdoptLost, call_id);
    }
    CompleteConnectLocked(std::move(value));
  }

  void TeardownTransportLocked() {
    offerer_glare = false;
    if (handshake_cancelled) {
      handshake_cancelled->store(true, std::memory_order_release);
    }
    if (handshake_stream) {
      ResetQuiet(handshake_stream);
    }
    ClearHandshakeLocked();
    if (duplex_cancelled) {
      duplex_cancelled->store(true, std::memory_order_release);
    }
    if (duplex) {
      duplex->Stop();
      duplex.reset();
    }
    duplex_cancelled.reset();
    if (stream) {
      ResetQuiet(stream);
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
      if (CallMediaFailNotifySuppressed(Phase())) {
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
    ClearHandshakeLocked();
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
      auto policy = CallMediaIoPolicy();
      policy.on_outbound_drop = on_drop;
      self->duplex->Start(
          s,
          [self](Roe<std::vector<uint8_t>> frame) { return self->HandleMediaFrame(std::move(frame)); },
          [cancelled = self->duplex_cancelled]() {
            return cancelled && cancelled->load(std::memory_order_acquire);
          },
          std::move(policy),
          [self](const char* reason) {
            self->Fail(std::string("call-media stream closed (") +
                           (reason && reason[0] ? reason : "unknown") + ")",
                       CallMediaSessionEvent::DuplexEof);
          });
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

  void FailInboundHello(const std::shared_ptr<Stream>& s, const std::string& call_id = {}) {
    std::lock_guard lock(mu);
    if (Phase() == CallMediaSessionPhase::HelloInbound) {
      (void)ApplyLocked(CallMediaSessionEvent::HelloFail, call_id);
    }
    ClearHandshakeLocked();
    ResetQuiet(s);
  }

  void WriteInboundAckAndFinish(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
                                CallMediaDirectCallbacks cbs, bool ok, const char* error) {
    nlohmann::json ack{{"v", 1}, {"type", "hello_ack"}, {"ok", ok}};
    if (!ok && error) {
      ack["error"] = error;
    }
    auto self = shared_from_this();
    AsyncWriteStreamJson(
        s, ack.dump(),
        [self, s, params = std::move(params), cbs = std::move(cbs), ok](Roe<void> write_res) mutable {
          if (!ok) {
            self->FailInboundHello(s, params.call_id);
            return;
          }
          if (!write_res) {
            self->FailInboundHello(s, params.call_id);
            return;
          }
          CallMediaDirectCallbacks adopted_cbs;
          bool adopted = false;
          {
            std::lock_guard lock(self->mu);
            if (self->HandshakeCancelledLocked()) {
              ResetQuiet(s);
              return;
            }
            if (self->Phase() != CallMediaSessionPhase::HelloInbound) {
              self->ClearHandshakeLocked();
              ResetQuiet(s);
              return;
            }
            (void)self->ApplyLocked(CallMediaSessionEvent::HelloOk, params.call_id);
            adopted = self->TryAdoptStreamLocked(s, params, std::move(cbs));
            if (adopted) {
              adopted_cbs = self->callbacks;
            } else {
              (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
              self->ClearHandshakeLocked();
            }
          }
          if (!adopted) {
            self->log().info << "Inbound call-media lost adopt race call_id=" << params.call_id;
            ResetQuiet(s);
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

  /** Guard for inbound admit (HOST_RECEIVE_POLICY / V033 glare note). */
  enum class InboundAdmit { Accept, RejectNoHandler, RejectActive, RejectGlare };

  InboundAdmit AdmitInboundLocked(const std::shared_ptr<Stream>& inbound) {
    if (!inbound_handler) {
      return InboundAdmit::RejectNoHandler;
    }
    if (stream || Phase() == CallMediaSessionPhase::MediaReady ||
        Phase() == CallMediaSessionPhase::Adopting || Phase() == CallMediaSessionPhase::Detaching ||
        Phase() == CallMediaSessionPhase::HelloInbound) {
      return InboundAdmit::RejectActive;
    }
    // Offerer HelloOutbound: higher PeerId keeps outbound; lower PeerId yields to inbound
    // so dual-dial shares one stream. Dialing still accepts (answerer reverse-dial).
    if (Phase() == CallMediaSessionPhase::HelloOutbound && offerer_glare &&
        LocalWinsGlareFor(inbound)) {
      return InboundAdmit::RejectGlare;
    }
    return InboundAdmit::Accept;
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_in) {
    log().info << "Inbound call-media stream (protocol negotiated)";
    if (!host) {
      ResetQuiet(stream_in.stream);
      return;
    }
    auto stream = std::move(stream_in.stream);
    StreamCancelCheck cancel_check;
    {
      std::lock_guard lock(mu);
      const auto admit = AdmitInboundLocked(stream);
      if (admit == InboundAdmit::RejectNoHandler) {
        IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "handler cleared");
        ResetQuiet(stream);
        return;
      }
      if (admit == InboundAdmit::RejectActive) {
        IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "session already active");
        ResetQuiet(stream);
        return;
      }
      if (admit == InboundAdmit::RejectGlare) {
        IgnoreEventLocked(CallMediaSessionEvent::InboundStream, "glare outbound hello");
        ResetQuiet(stream);
        return;
      }
      if (Phase() == CallMediaSessionPhase::HelloOutbound) {
        AbandonOutboundHandshakeLocked();
      }
      (void)ApplyLocked(CallMediaSessionEvent::InboundStream);
      ArmHandshakeLocked(stream);
      cancel_check = HandshakeCancelCheck();
    }

    auto self = shared_from_this();
    AsyncReadStreamJson(
        stream,
        [self, stream](Roe<std::string> json_utf8) {
          {
            std::lock_guard lock(self->mu);
            if (self->HandshakeCancelledLocked()) {
              return;
            }
          }
          if (!json_utf8) {
            self->log().warning << "Inbound call-media hello read failed err=" << json_utf8.error().message;
            self->FailInboundHello(stream);
            return;
          }
          auto hello = ParseJsonObject(*json_utf8);
          if (!hello || hello->value("type", "") != "hello") {
            self->log().warning << "Inbound call-media hello read failed err="
                                << (hello ? "bad type" : hello.error().message);
            self->FailInboundHello(stream);
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
            if (self->HandshakeCancelledLocked()) {
              return;
            }
            handler = self->inbound_handler;
            if (self->stream || self->Phase() == CallMediaSessionPhase::MediaReady ||
                self->Phase() == CallMediaSessionPhase::Adopting) {
              self->IgnoreEventLocked(CallMediaSessionEvent::HelloOk, "lost race after hello");
              if (self->Phase() == CallMediaSessionPhase::HelloInbound) {
                (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
              }
              self->ClearHandshakeLocked();
              ResetQuiet(stream);
              return;
            }
            if (self->Phase() != CallMediaSessionPhase::HelloInbound) {
              // Detach during hello read.
              self->ClearHandshakeLocked();
              ResetQuiet(stream);
              return;
            }
          }

          if (!self->host) {
            self->FailInboundHello(stream, params.call_id);
            return;
          }

          // Normal lane only for inbound_handler (may block in tests / key fill) — not for stream IO.
          PostLibp2pWorker(
              *self->host, WorkerLane::Normal,
              [self, stream, params = std::move(params), handler = std::move(handler)]() mutable {
                {
                  std::lock_guard lock(self->mu);
                  if (self->HandshakeCancelledLocked() ||
                      self->Phase() != CallMediaSessionPhase::HelloInbound) {
                    ResetQuiet(stream);
                    return;
                  }
                }
                CallMediaDirectCallbacks cbs;
                if (!handler) {
                  self->IgnoreEventLocked(CallMediaSessionEvent::HelloOk, "handler cleared mid-hello");
                  self->WriteInboundAckAndFinish(std::move(stream), std::move(params), {}, false,
                                                 "unavailable");
                  return;
                }
                handler(params, cbs);
                {
                  std::lock_guard lock(self->mu);
                  if (self->HandshakeCancelledLocked() ||
                      self->Phase() != CallMediaSessionPhase::HelloInbound) {
                    ResetQuiet(stream);
                    return;
                  }
                  // Dual-dial: higher PeerId keeps outbound — do not ack this inbound.
                  if (self->offerer_glare && self->LocalWinsGlareFor(stream)) {
                    self->log().info << "Inbound call-media yields glare call_id=" << params.call_id;
                    if (self->handshake_cancelled) {
                      self->handshake_cancelled->store(true, std::memory_order_release);
                    }
                    (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                    if (self->handshake_stream == stream) {
                      self->ClearHandshakeLocked();
                    }
                    ResetQuiet(stream);
                    return;
                  }
                }
                if (params.media_key.empty() || params.call_id.empty()) {
                  self->log().warning << "Inbound call-media hello rejected call_id=" << params.call_id
                                      << " key_empty=" << (params.media_key.empty() ? 1 : 0);
                  self->WriteInboundAckAndFinish(std::move(stream), std::move(params), {}, false,
                                                 "rejected");
                  return;
                }
                self->WriteInboundAckAndFinish(std::move(stream), std::move(params), std::move(cbs),
                                               true, nullptr);
              });
        },
        std::move(cancel_check));
  }

  void BeginOutboundHello(std::shared_ptr<Stream> stream, CallMediaDirectConnectParams params,
                          CallMediaDirectCallbacks callbacks,
                          std::shared_ptr<std::atomic<bool>> settled) {
    StreamCancelCheck cancel_check;
    {
      std::lock_guard lock(mu);
      if (settled->load(std::memory_order_acquire) || HandshakeCancelledLocked()) {
        offerer_glare = false;
        ResetQuiet(stream);
        return;
      }
      if (this->stream) {
        offerer_glare = false;
        ResetQuiet(stream);
        FinishOutboundConnectLocked({}, params.call_id);
        return;
      }
      if (Phase() == CallMediaSessionPhase::Idle || Phase() == CallMediaSessionPhase::Detaching) {
        offerer_glare = false;
        ResetQuiet(stream);
        return;
      }
      // HelloInbound already armed the inbound handshake token — do not share it with
      // outbound hello (yielding inbound would cancel the winning outbound).
      if (Phase() == CallMediaSessionPhase::HelloInbound) {
        cancel_check = []() { return false; };
      } else {
        cancel_check = HandshakeCancelCheck();
      }
    }

    const std::string role = params.offerer ? "offerer" : "answerer";
    const std::string hello =
        nlohmann::json{{"v", 1},
                       {"type", "hello"},
                       {"call_id", params.call_id},
                       {"media_epoch", params.media_epoch},
                       {"role", role}}
            .dump();
    auto self = shared_from_this();
    AsyncWriteStreamJson(
        stream, hello,
        [self, stream, params = std::move(params), callbacks = std::move(callbacks), settled,
         cancel_check](Roe<void> write_res) mutable {
          const auto outbound_cancelled = [&] {
            return cancel_check && cancel_check();
          };
          if (outbound_cancelled()) {
            ResetQuiet(stream);
            return;
          }
          if (!write_res) {
            self->Log().warning << "Call-media hello write failed peer=" << params.peer_key;
            {
              std::lock_guard lock(self->mu);
              if (self->SuppressOutboundHelloFailLocked(stream)) {
                ResetQuiet(stream);
                return;
              }
              self->offerer_glare = false;
              (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
              self->ClearHandshakeLocked();
              self->CompleteConnectLocked(Error("call-media hello write failed"));
            }
            ResetQuiet(stream);
            return;
          }
          {
            std::lock_guard lock(self->mu);
            if (settled->load(std::memory_order_acquire) || outbound_cancelled() || self->stream ||
                self->Phase() == CallMediaSessionPhase::Idle ||
                self->Phase() == CallMediaSessionPhase::Detaching) {
              self->offerer_glare = false;
              if (self->stream) {
                ResetQuiet(stream);
                self->FinishOutboundConnectLocked({}, params.call_id);
              } else {
                ResetQuiet(stream);
              }
              return;
            }
          }

          AsyncReadStreamJson(
              stream,
              [self, stream, params = std::move(params), callbacks = std::move(callbacks), settled,
               cancel_check](Roe<std::string> ack_utf8) mutable {
                if (settled->load(std::memory_order_acquire) ||
                    (cancel_check && cancel_check())) {
                  ResetQuiet(stream);
                  return;
                }
                {
                  std::lock_guard lock(self->mu);
                  if (self->HandshakeCancelledLocked()) {
                    ResetQuiet(stream);
                    return;
                  }
                  if (self->stream) {
                    ResetQuiet(stream);
                    self->FinishOutboundConnectLocked({}, params.call_id);
                    return;
                  }
                  if (self->Phase() == CallMediaSessionPhase::Idle ||
                      self->Phase() == CallMediaSessionPhase::Detaching ||
                      self->Phase() == CallMediaSessionPhase::MediaReady ||
                      self->Phase() == CallMediaSessionPhase::Adopting) {
                    ResetQuiet(stream);
                    if (self->Phase() == CallMediaSessionPhase::MediaReady ||
                        self->Phase() == CallMediaSessionPhase::Adopting) {
                      self->FinishOutboundConnectLocked({}, params.call_id);
                    } else {
                      self->IgnoreEventLocked(CallMediaSessionEvent::HelloOk,
                                              "connect no longer active");
                      self->ClearHandshakeLocked();
                    }
                    return;
                  }
                }

                Roe<nlohmann::json> ack = Error("hello ack missing");
                if (ack_utf8) {
                  ack = ParseJsonObject(*ack_utf8);
                } else {
                  ack = ack_utf8.error();
                }
                if (!ack || !ack->value("ok", false)) {
                  const std::string why =
                      ack ? ack->value("error", "hello rejected") : ack.error().message;
                  self->Log().warning << "Call-media hello rejected peer=" << params.peer_key
                                      << " err=" << why;
                  {
                    std::lock_guard lock(self->mu);
                    if (self->SuppressOutboundHelloFailLocked(stream)) {
                      ResetQuiet(stream);
                      return;
                    }
                    (void)self->ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
                    self->ClearHandshakeLocked();
                    self->CompleteConnectLocked(Error(why));
                  }
                  ResetQuiet(stream);
                  return;
                }

                CallMediaDirectCallbacks adopted_cbs;
                bool adopted = false;
                {
                  std::lock_guard lock(self->mu);
                  if (self->stream || self->Phase() == CallMediaSessionPhase::Idle ||
                      self->Phase() == CallMediaSessionPhase::Detaching ||
                      self->HandshakeCancelledLocked() || (cancel_check && cancel_check())) {
                    ResetQuiet(stream);
                    if (self->stream) {
                      self->FinishOutboundConnectLocked({}, params.call_id);
                    } else {
                      self->ClearHandshakeLocked();
                    }
                    return;
                  }
                  if (self->Phase() == CallMediaSessionPhase::HelloInbound &&
                      !self->LocalWinsGlareFor(stream)) {
                    self->log().info << "Call-media outbound yields glare call_id=" << params.call_id;
                    ResetQuiet(stream);
                    return;
                  }
                  (void)self->ApplyLocked(CallMediaSessionEvent::HelloOk, params.call_id);
                  adopted = self->TryAdoptStreamLocked(stream, params, std::move(callbacks));
                  if (adopted) {
                    adopted_cbs = self->callbacks;
                  } else {
                    (void)self->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                    self->ClearHandshakeLocked();
                  }
                }
                if (!adopted) {
                  self->Log().info << "Call-media outbound lost adopt race call_id=" << params.call_id;
                  ResetQuiet(stream);
                  {
                    std::lock_guard lock(self->mu);
                    self->FinishOutboundConnectLocked({}, params.call_id);
                  }
                  return;
                }
                self->StartMediaDuplex([self, adopted_cbs = std::move(adopted_cbs)]() mutable {
                  if (adopted_cbs.on_connected) {
                    adopted_cbs.on_connected();
                  }
                  std::lock_guard lock(self->mu);
                  self->CompleteConnectLocked(Roe<void>{});
                });
              },
              std::move(cancel_check));
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
  if (!sessions_.IsReachableForProtocol(params.peer_key, kCallMediaDirectProtocolId)) {
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
    // Bound hello/ack IO to the same budget as Connect — peer may stall forever.
    impl_->handshake_timeout_ms = timeout_ms > 0 ? timeout_ms : kDefaultHandshakeTimeoutMs;
    (void)impl_->ApplyLocked(CallMediaSessionEvent::ConnectRequested, params.call_id);
  }

  sessions_.OpenStream(params.peer_key, {ProtocolName{kCallMediaDirectProtocolId}},
                       [impl = impl_, params, callbacks = std::move(callbacks), settled](
                           outcome::result<libp2p::StreamAndProtocol> stream_res) mutable {
                         if (settled->load(std::memory_order_acquire)) {
                           if (stream_res) {
                             ResetQuiet(stream_res.value().stream);
                           }
                           return; // Connect already aborted / timed out / inbound won
                         }
                         // Outbound Connect may overlap inbound HelloInbound (dual-dial): do not
                         // require Phase==Dialing exclusively. Key off settled/stream + offerer_glare.
                         // Hello IO is async on the host io_context — no Normal worker for R/W.
                         {
                           std::lock_guard lock(impl->mu);
                           if (impl->stream) {
                             // Inbound won while OpenStream was in flight.
                             if (stream_res) {
                               ResetQuiet(stream_res.value().stream);
                             }
                             impl->FinishOutboundConnectLocked({}, params.call_id);
                             return;
                           }
                           // MediaReady/Adopting: inbound already won — complete Connect OK.
                           if (impl->Phase() == CallMediaSessionPhase::MediaReady ||
                               impl->Phase() == CallMediaSessionPhase::Adopting) {
                             if (stream_res) {
                               ResetQuiet(stream_res.value().stream);
                             }
                             impl->FinishOutboundConnectLocked({}, params.call_id);
                             return;
                           }
                           // Idle+waiter → Dialing; Detaching / Idle without waiter → ignore.
                           if (impl->Phase() == CallMediaSessionPhase::Idle ||
                               impl->Phase() == CallMediaSessionPhase::Detaching) {
                             if (!impl->ApplyLocked(CallMediaSessionEvent::OpenStreamOk,
                                                    params.call_id)) {
                               if (stream_res) {
                                 ResetQuiet(stream_res.value().stream);
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
                           {
                             std::lock_guard lock(impl->mu);
                             impl->FinishOutboundConnectLocked(Error(detail), params.call_id);
                           }
                           return;
                         }
                         impl->Log().warning << "Call-media OpenStream ok peer=" << params.peer_key
                                             << " role=" << (params.offerer ? "offerer" : "answerer")
                                             << " call_id=" << params.call_id;
                         auto stream = std::move(stream_res.value().stream);
                         {
                           std::lock_guard lock(impl->mu);
                           if (impl->stream) {
                             ResetQuiet(stream);
                             impl->FinishOutboundConnectLocked({}, params.call_id);
                             return;
                           }
                           // Dialing → HelloOutbound; HelloInbound → log-only (dual-dial).
                           if (!impl->ApplyLocked(CallMediaSessionEvent::OpenStreamOk, params.call_id)) {
                             ResetQuiet(stream);
                             return;
                           }
                           // Offerer outbound hello: glare bit lets HelloOutbound reject inbound
                           // only when this PeerId wins the dual-dial tie-break.
                           impl->offerer_glare = params.offerer;
                           if (impl->Phase() == CallMediaSessionPhase::HelloInbound &&
                               params.offerer && !impl->LocalWinsGlareFor(stream)) {
                             impl->offerer_glare = false;
                             impl->Log().info
                                 << "Call-media skip outbound hello (inbound wins glare) call_id="
                                 << params.call_id;
                             ResetQuiet(stream);
                             return;
                           }
                           if (impl->Phase() == CallMediaSessionPhase::HelloOutbound) {
                             impl->ArmHandshakeLocked(stream);
                           }
                           if (settled->load(std::memory_order_acquire)) {
                             impl->offerer_glare = false;
                             (void)impl->ApplyLocked(CallMediaSessionEvent::DetachRequested,
                                                     params.call_id);
                             if (impl->handshake_stream == stream) {
                               impl->ClearHandshakeLocked();
                             }
                             ResetQuiet(stream);
                             return;
                           }
                           if (impl->stream) {
                             impl->offerer_glare = false;
                             (void)impl->ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
                             if (impl->handshake_stream == stream) {
                               impl->ClearHandshakeLocked();
                             }
                             ResetQuiet(stream);
                             impl->FinishOutboundConnectLocked({}, params.call_id);
                             return;
                           }
                         }
                         impl->BeginOutboundHello(std::move(stream), params, std::move(callbacks),
                                                  settled);
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
          // Same compound effect as Detach: cancel + reset handshake so a silent peer
          // cannot leave async read/write alive after Connect returns.
          impl_->TeardownTransportLocked();
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
