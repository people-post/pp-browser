#include "base/p2p/CallMediaSession.h"

#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/p2p/CallMediaSessionLogic.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/StreamJsonFrame.h"
#include "common/ValueJson.h"

#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr int kDefaultHandshakeTimeoutMs = 15000;

void ResetQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->reset();
  }
}

Roe<Object> ParseJsonObject(const std::string& json_utf8) {
  auto root = TryParseObject(json_utf8);
  if (!root) {
    return Error("invalid call-media json");
  }
  return *root;
}

} // namespace

CallMediaSession::CallMediaSession() { redirectLogger("CallMediaDirect"); }

CallMediaSessionPhase CallMediaSession::Phase() const {
  return phase.load(std::memory_order_acquire);
}

CallMediaDirectConnectParams CallMediaSession::ActiveParams() const {
  std::lock_guard lock(mu);
  return active_params;
}

void CallMediaSession::SetPhaseLocked(CallMediaSessionPhase next, CallMediaSessionEvent ev,
                    const std::string& call_id) {
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

void CallMediaSession::IgnoreEventLocked(CallMediaSessionEvent ev, const char* reason) {
  log().warning << "call_media_session ignore event=" << CallMediaSessionEventName(ev)
                << " phase=" << CallMediaSessionPhaseName(Phase()) << " reason="
                << (reason ? reason : "");
}

bool CallMediaSession::ConnectWaiterActiveLocked() const {
  return connect_settled && !connect_settled->load(std::memory_order_acquire);
}

void CallMediaSession::CancelHandshakeTimerLocked() {
  if (handshake_timer) {
    (void)handshake_timer->cancel();
    handshake_timer.reset();
  }
}

void CallMediaSession::StartHandshakeTimerLocked() {
  CancelHandshakeTimerLocked();
  if (!host) {
    return;
  }
  const auto ex = host->IoExecutor();
  if (!ex) {
    return;
  }
  const int ms = handshake_timeout_ms > 0 ? handshake_timeout_ms : kDefaultHandshakeTimeoutMs;
  auto timer = std::make_shared<asio::steady_timer>(ex);
  handshake_timer = timer;
  auto token = handshake_cancelled;
  auto self = shared_from_this();
  timer->expires_after(std::chrono::milliseconds(ms));
  timer->async_wait([self, timer, token](const std::error_code& ec) {
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

void CallMediaSession::ArmHandshakeLocked(std::shared_ptr<Stream> s) {
  CancelHandshakeTimerLocked();
  handshake_stream = std::move(s);
  handshake_cancelled = std::make_shared<std::atomic<bool>>(false);
  StartHandshakeTimerLocked();
}

void CallMediaSession::ClearHandshakeLocked() {
  CancelHandshakeTimerLocked();
  handshake_stream.reset();
  handshake_cancelled.reset();
}

StreamCancelCheck CallMediaSession::HandshakeCancelCheck() const {
  auto cancelled = handshake_cancelled;
  return [cancelled]() {
    return cancelled && cancelled->load(std::memory_order_acquire);
  };
}

bool CallMediaSession::HandshakeCancelledLocked() const {
  return handshake_cancelled && handshake_cancelled->load(std::memory_order_acquire);
}

bool CallMediaSession::LocalWinsGlareFor(const std::shared_ptr<Stream>& s) const {
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

void CallMediaSession::AbandonOutboundHandshakeLocked() {
  offerer_glare = false;
  if (handshake_cancelled) {
    handshake_cancelled->store(true, std::memory_order_release);
  }
  if (handshake_stream) {
    ResetQuiet(handshake_stream);
  }
  ClearHandshakeLocked();
}

bool CallMediaSession::SuppressOutboundHelloFailLocked(const std::shared_ptr<Stream>& s) {
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

bool CallMediaSession::ApplyLocked(CallMediaSessionEvent ev, const std::string& call_id) {
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

bool CallMediaSession::MediaReady() const {
  return Phase() == CallMediaSessionPhase::MediaReady;
}

bool CallMediaSession::HandleMediaFrame(Roe<std::vector<uint8_t>> frame_res) {
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
  auto decoded = DecryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, *frame_res);
  if (!decoded) {
    if ((decrypt_fail_log_.fetch_add(1) % 25) == 0) {
      log().warning << "Call-media decrypt failed call_id=" << params.call_id
                    << " epoch=" << params.media_epoch << " err=" << decoded.error().message;
    }
    return true;
  }
  if (cbs.on_media) {
    cbs.on_media(decoded->channel, decoded->payload);
  } else if (decoded->channel == kCallMediaChannelAudio && cbs.on_audio) {
    cbs.on_audio(decoded->payload);
  }
  return true;
}

void CallMediaSession::CompleteConnectLocked(Roe<void> value) {
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

void CallMediaSession::FinishOutboundConnectLocked(Roe<void> value, const std::string& call_id) {
  offerer_glare = false;
  if (!value) {
    (void)ApplyLocked(CallMediaSessionEvent::OpenStreamFail, call_id);
  } else if (!stream) {
    (void)ApplyLocked(CallMediaSessionEvent::AdoptLost, call_id);
  }
  CompleteConnectLocked(std::move(value));
}

void CallMediaSession::TeardownTransportLocked() {
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

void CallMediaSession::DetachLocked(bool abort_connect, CallMediaSessionEvent ev) {
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

void CallMediaSession::Fail(const std::string& message, CallMediaSessionEvent ev) {
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

bool CallMediaSession::TryAdoptStreamLocked(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
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

void CallMediaSession::StartMediaDuplex(std::function<void()> on_ready) {
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

bool CallMediaSession::EnqueueOutbound(std::vector<uint8_t> body) {
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

void CallMediaSession::FailInboundHello(const std::shared_ptr<Stream>& s, const std::string& call_id) {
  std::lock_guard lock(mu);
  if (Phase() == CallMediaSessionPhase::HelloInbound) {
    (void)ApplyLocked(CallMediaSessionEvent::HelloFail, call_id);
  }
  ClearHandshakeLocked();
  ResetQuiet(s);
}

void CallMediaSession::WriteInboundAckAndFinish(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
                              CallMediaDirectCallbacks cbs, bool ok, const char* error) {
  Object ack;
  ack.set("v", int64_t{1});
  ack.set("type", "hello_ack");
  ack.set("ok", ok);
  if (!ok && error) {
    ack.set("error", error);
  }
  if (ok && params.adp_port != 0 && !params.adp_assoc_hex.empty()) {
    ack.setJsonUInt("adp_v", 1);
    ack.setJsonUInt("adp_port", params.adp_port);
    ack.set("adp_assoc", params.adp_assoc_hex);
    if (!params.adp_ip.empty()) {
      ack.set("adp_ip", params.adp_ip);
    }
  }
  auto self = shared_from_this();
  AsyncWriteStreamJson(
      s, DumpJson(ack),
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

CallMediaSession::InboundAdmit CallMediaSession::AdmitInboundLocked(const std::shared_ptr<Stream>& inbound) {
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

void CallMediaSession::HandleInbound(libp2p::StreamAndProtocol stream_in) {
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
        if (!hello || hello->getString("type").value_or("") != "hello") {
          self->log().warning << "Inbound call-media hello read failed err="
                              << (hello ? "bad type" : hello.error().message);
          self->FailInboundHello(stream);
          return;
        }

        CallMediaDirectConnectParams params;
        params.call_id = hello->getString("call_id").value_or("");
        params.media_epoch =
            static_cast<uint32_t>(hello->getNonNegInt("media_epoch").value_or(1));
        params.offerer = hello->getString("role").value_or("") == "offerer";
        if (auto peer = stream->remotePeerId()) {
          params.peer_key = peer.value().toBase58();
        }
        if (hello->getNonNegInt("adp_v").value_or(0) == 1) {
          params.peer_adp_port =
              static_cast<uint16_t>(hello->getNonNegInt("adp_port").value_or(0));
          params.peer_adp_ip = hello->getString("adp_ip").value_or("");
          params.peer_adp_assoc_hex = hello->getString("adp_assoc").value_or("");
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

void CallMediaSession::BeginOutboundHello(std::shared_ptr<Stream> stream,
                                          CallMediaDirectConnectParams params,
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
  Object hello_obj;
  hello_obj.set("v", int64_t{1});
  hello_obj.set("type", "hello");
  hello_obj.set("call_id", params.call_id);
  hello_obj.setJsonUInt("media_epoch", params.media_epoch);
  hello_obj.set("role", role);
  // Answerer may advertise port/ip before learning offerer-minted assoc (A010).
  if (params.adp_port != 0) {
    hello_obj.setJsonUInt("adp_v", 1);
    hello_obj.setJsonUInt("adp_port", params.adp_port);
    if (!params.adp_assoc_hex.empty()) {
      hello_obj.set("adp_assoc", params.adp_assoc_hex);
    }
    if (!params.adp_ip.empty()) {
      hello_obj.set("adp_ip", params.adp_ip);
    }
  }
  const std::string hello = DumpJson(hello_obj);
  auto self = shared_from_this();
  AsyncWriteStreamJson(
      stream, hello,
      [self, stream, params = std::move(params), callbacks = std::move(callbacks), settled,
       cancel_check](Roe<void> write_res) mutable {
        self->OnHelloWritten(std::move(stream), std::move(params), std::move(callbacks), settled,
                             std::move(cancel_check), std::move(write_res));
      });
}

void CallMediaSession::OnHelloWritten(std::shared_ptr<Stream> stream,
                                      CallMediaDirectConnectParams params,
                                      CallMediaDirectCallbacks callbacks,
                                      std::shared_ptr<std::atomic<bool>> settled,
                                      StreamCancelCheck cancel_check, Roe<void> write_res) {
  const auto outbound_cancelled = [&] { return cancel_check && cancel_check(); };
  if (outbound_cancelled()) {
    ResetQuiet(stream);
    return;
  }
  if (!write_res) {
    Log().warning << "Call-media hello write failed peer=" << params.peer_key;
    {
      std::lock_guard lock(mu);
      if (SuppressOutboundHelloFailLocked(stream)) {
        ResetQuiet(stream);
        return;
      }
      offerer_glare = false;
      (void)ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
      ClearHandshakeLocked();
      CompleteConnectLocked(Error("call-media hello write failed"));
    }
    ResetQuiet(stream);
    return;
  }
  {
    std::lock_guard lock(mu);
    if (settled->load(std::memory_order_acquire) || outbound_cancelled() || this->stream ||
        Phase() == CallMediaSessionPhase::Idle || Phase() == CallMediaSessionPhase::Detaching) {
      offerer_glare = false;
      if (this->stream) {
        ResetQuiet(stream);
        FinishOutboundConnectLocked({}, params.call_id);
      } else {
        ResetQuiet(stream);
      }
      return;
    }
  }

  auto self = shared_from_this();
  AsyncReadStreamJson(
      stream,
      [self, stream, params = std::move(params), callbacks = std::move(callbacks), settled,
       cancel_check](Roe<std::string> ack_utf8) mutable {
        self->OnHelloAck(std::move(stream), std::move(params), std::move(callbacks), settled,
                         std::move(cancel_check), std::move(ack_utf8));
      },
      std::move(cancel_check));
}

void CallMediaSession::OnHelloAck(std::shared_ptr<Stream> stream, CallMediaDirectConnectParams params,
                                 CallMediaDirectCallbacks callbacks,
                                 std::shared_ptr<std::atomic<bool>> settled,
                                 StreamCancelCheck cancel_check, Roe<std::string> ack_utf8) {
  if (settled->load(std::memory_order_acquire) || (cancel_check && cancel_check())) {
    ResetQuiet(stream);
    return;
  }
  {
    std::lock_guard lock(mu);
    if (HandshakeCancelledLocked()) {
      ResetQuiet(stream);
      return;
    }
    if (this->stream) {
      ResetQuiet(stream);
      FinishOutboundConnectLocked({}, params.call_id);
      return;
    }
    if (Phase() == CallMediaSessionPhase::Idle || Phase() == CallMediaSessionPhase::Detaching ||
        Phase() == CallMediaSessionPhase::MediaReady || Phase() == CallMediaSessionPhase::Adopting) {
      ResetQuiet(stream);
      if (Phase() == CallMediaSessionPhase::MediaReady || Phase() == CallMediaSessionPhase::Adopting) {
        FinishOutboundConnectLocked({}, params.call_id);
      } else {
        IgnoreEventLocked(CallMediaSessionEvent::HelloOk, "connect no longer active");
        ClearHandshakeLocked();
      }
      return;
    }
  }

  Roe<Object> ack = Error("hello ack missing");
  if (ack_utf8) {
    ack = ParseJsonObject(*ack_utf8);
  } else {
    ack = ack_utf8.error();
  }
  if (!ack || !ack->getIf<bool>("ok").value_or(false)) {
    const std::string why =
        ack ? ack->getString("error").value_or("hello rejected") : ack.error().message;
    Log().warning << "Call-media hello rejected peer=" << params.peer_key << " err=" << why;
    {
      std::lock_guard lock(mu);
      if (SuppressOutboundHelloFailLocked(stream)) {
        ResetQuiet(stream);
        return;
      }
      (void)ApplyLocked(CallMediaSessionEvent::HelloFail, params.call_id);
      ClearHandshakeLocked();
      CompleteConnectLocked(Error(why));
    }
    ResetQuiet(stream);
    return;
  }
  if (ack->getNonNegInt("adp_v").value_or(0) == 1) {
    params.peer_adp_port = static_cast<uint16_t>(ack->getNonNegInt("adp_port").value_or(0));
    params.peer_adp_ip = ack->getString("adp_ip").value_or("");
    params.peer_adp_assoc_hex = ack->getString("adp_assoc").value_or("");
  }

  CallMediaDirectCallbacks adopted_cbs;
  bool adopted = false;
  {
    std::lock_guard lock(mu);
    if (this->stream || Phase() == CallMediaSessionPhase::Idle ||
        Phase() == CallMediaSessionPhase::Detaching || HandshakeCancelledLocked() ||
        (cancel_check && cancel_check())) {
      ResetQuiet(stream);
      if (this->stream) {
        FinishOutboundConnectLocked({}, params.call_id);
      } else {
        ClearHandshakeLocked();
      }
      return;
    }
    if (Phase() == CallMediaSessionPhase::HelloInbound && !LocalWinsGlareFor(stream)) {
      log().info << "Call-media outbound yields glare call_id=" << params.call_id;
      ResetQuiet(stream);
      return;
    }
    (void)ApplyLocked(CallMediaSessionEvent::HelloOk, params.call_id);
    adopted = TryAdoptStreamLocked(stream, params, std::move(callbacks));
    if (adopted) {
      adopted_cbs = this->callbacks;
    } else {
      (void)ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
      ClearHandshakeLocked();
    }
  }
  if (!adopted) {
    Log().info << "Call-media outbound lost adopt race call_id=" << params.call_id;
    ResetQuiet(stream);
    {
      std::lock_guard lock(mu);
      FinishOutboundConnectLocked({}, params.call_id);
    }
    return;
  }
  auto self = shared_from_this();
  StartMediaDuplex([self, adopted_cbs = std::move(adopted_cbs)]() mutable {
    if (adopted_cbs.on_connected) {
      adopted_cbs.on_connected();
    }
    std::lock_guard lock(self->mu);
    self->CompleteConnectLocked(Roe<void>{});
  });
}

void CallMediaSession::OnOutboundStream(libp2p::StreamAndProtocolOrError stream_res,
                                        CallMediaDirectConnectParams params,
                                        CallMediaDirectCallbacks callbacks,
                                        std::shared_ptr<std::atomic<bool>> settled) {
  if (settled->load(std::memory_order_acquire)) {
    if (stream_res) {
      ResetQuiet(stream_res.value().stream);
    }
    return;
  }
  {
    std::lock_guard lock(mu);
    if (stream) {
      if (stream_res) {
        ResetQuiet(stream_res.value().stream);
      }
      FinishOutboundConnectLocked({}, params.call_id);
      return;
    }
    if (Phase() == CallMediaSessionPhase::MediaReady || Phase() == CallMediaSessionPhase::Adopting) {
      if (stream_res) {
        ResetQuiet(stream_res.value().stream);
      }
      FinishOutboundConnectLocked({}, params.call_id);
      return;
    }
    if (Phase() == CallMediaSessionPhase::Idle || Phase() == CallMediaSessionPhase::Detaching) {
      if (!ApplyLocked(CallMediaSessionEvent::OpenStreamOk, params.call_id)) {
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
    Log().warning << "Call-media OpenStream failed peer=" << params.peer_key
                  << " role=" << (params.offerer ? "offerer" : "answerer") << " err=" << detail;
    {
      std::lock_guard lock(mu);
      FinishOutboundConnectLocked(Error(detail), params.call_id);
    }
    return;
  }
  Log().warning << "Call-media OpenStream ok peer=" << params.peer_key
                << " role=" << (params.offerer ? "offerer" : "answerer")
                << " call_id=" << params.call_id;
  auto opened = std::move(stream_res.value().stream);
  {
    std::lock_guard lock(mu);
    if (stream) {
      ResetQuiet(opened);
      FinishOutboundConnectLocked({}, params.call_id);
      return;
    }
    if (!ApplyLocked(CallMediaSessionEvent::OpenStreamOk, params.call_id)) {
      ResetQuiet(opened);
      return;
    }
    offerer_glare = params.offerer;
    if (Phase() == CallMediaSessionPhase::HelloInbound && params.offerer &&
        !LocalWinsGlareFor(opened)) {
      offerer_glare = false;
      Log().info << "Call-media skip outbound hello (inbound wins glare) call_id=" << params.call_id;
      ResetQuiet(opened);
      return;
    }
    if (Phase() == CallMediaSessionPhase::HelloOutbound) {
      ArmHandshakeLocked(opened);
    }
    if (settled->load(std::memory_order_acquire)) {
      offerer_glare = false;
      (void)ApplyLocked(CallMediaSessionEvent::DetachRequested, params.call_id);
      if (handshake_stream == opened) {
        ClearHandshakeLocked();
      }
      ResetQuiet(opened);
      return;
    }
    if (stream) {
      offerer_glare = false;
      (void)ApplyLocked(CallMediaSessionEvent::AdoptLost, params.call_id);
      if (handshake_stream == opened) {
        ClearHandshakeLocked();
      }
      ResetQuiet(opened);
      FinishOutboundConnectLocked({}, params.call_id);
      return;
    }
  }
  BeginOutboundHello(std::move(opened), std::move(params), std::move(callbacks), settled);
}

void CallMediaSession::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(mu);
  inbound_handler = std::move(handler);
}

void CallMediaSession::ClearInboundHandler() {
  std::lock_guard lock(mu);
  inbound_handler = {};
  Log().info << "phase=" << CallMediaSessionPhaseName(Phase())
             << " event=" << CallMediaSessionEventName(CallMediaSessionEvent::HandlerCleared);
}

bool CallMediaSession::IsActive() const {
  std::lock_guard lock(mu);
  return stream != nullptr;
}

void CallMediaSession::Detach() {
  std::lock_guard lock(mu);
  DetachLocked(/*abort_connect=*/true, CallMediaSessionEvent::DetachRequested);
}

Roe<void> CallMediaSession::Connect(PeerSessionManager& sessions,
                                    const CallMediaDirectConnectParams& params,
                                    CallMediaDirectCallbacks callbacks, int timeout_ms) {
  if (!host || !host->IsRunning()) {
    return Error("call-media host not running");
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    return Error("call-media connect params incomplete");
  }
  if (!sessions.IsReachableForProtocol(params.peer_key, kCallMediaDirectProtocolId)) {
    return Error("call-media peer not dialable");
  }

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 15000) + 1000;

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  {
    std::lock_guard lock(mu);
    if (stream && Phase() == CallMediaSessionPhase::MediaReady) {
      return {};
    }
    if (stream) {
      return {};
    }
    if (Phase() != CallMediaSessionPhase::Idle) {
      DetachLocked(/*abort_connect=*/true, CallMediaSessionEvent::ConnectSuperseded);
    }
    if (connect_settled && !connect_settled->exchange(true)) {
      try {
        if (connect_promise) {
          connect_promise->set_value(Error("call-media superseded"));
        }
      } catch (const std::future_error&) {
      }
    }
    connect_settled = settled;
    connect_promise = result_promise;
    handshake_timeout_ms = timeout_ms > 0 ? timeout_ms : kDefaultHandshakeTimeoutMs;
    (void)ApplyLocked(CallMediaSessionEvent::ConnectRequested, params.call_id);
  }

  auto self = shared_from_this();
  sessions.OpenStream(params.peer_key, {ProtocolName{kCallMediaDirectProtocolId}},
                      [self, params, callbacks = std::move(callbacks), settled](
                          libp2p::StreamAndProtocolOrError stream_res) mutable {
                        self->OnOutboundStream(std::move(stream_res), params, std::move(callbacks),
                                               settled);
                      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    const auto status = result_future.wait_for(std::chrono::milliseconds(50));
    if (status == std::future_status::ready) {
      std::lock_guard lock(mu);
      if (connect_settled == settled) {
        connect_settled.reset();
        connect_promise.reset();
      }
      return result_future.get();
    }
    {
      std::lock_guard lock(mu);
      if (stream != nullptr && MediaReady()) {
        settled->store(true, std::memory_order_release);
        if (connect_settled == settled) {
          connect_settled.reset();
          connect_promise.reset();
        }
        return {};
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      settled->exchange(true);
      bool active_ready = false;
      {
        std::lock_guard lock(mu);
        if (connect_settled == settled) {
          connect_settled.reset();
          connect_promise.reset();
        }
        active_ready = stream != nullptr && MediaReady();
        if (!active_ready) {
          TeardownTransportLocked();
          (void)ApplyLocked(CallMediaSessionEvent::ConnectTimeout, params.call_id);
        }
      }
      if (active_ready) {
        return {};
      }
      return Error("call-media connect timed out");
    }
  }
}

Roe<void> CallMediaSession::SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq,
                                      uint8_t mark) {
  return SendMedia(kCallMediaChannelAudio, opus_payload, seq, mark);
}

Roe<void> CallMediaSession::SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                                      uint8_t mark) {
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(mu);
    if (!stream || !MediaReady()) {
      return Error("call-media not connected");
    }
    params = active_params;
  }
  auto body = EncryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, seq, mark, channel,
                                    payload);
  if (!body) {
    return body.error();
  }
  if (!EnqueueOutbound(std::move(*body))) {
    return Error("call-media pump not running");
  }
  return {};
}

} // namespace pbr


