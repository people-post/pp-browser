#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "base/p2p/CallMediaDirectService.h"
#include "base/p2p/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/connection/stream_and_protocol.hpp>

#include <asio/steady_timer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class Libp2pHost;
class PeerSessionManager;

/**
 * Stateful 1:1 call-media transport session (V033). Owned by CallMediaDirectService
 * via shared_ptr so OpenStream / host.Post / duplex callbacks cannot UAF.
 */
class CallMediaSession : public Module, public std::enable_shared_from_this<CallMediaSession> {
public:
  using Stream = libp2p::connection::Stream;
  using InboundHandler =
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)>;

  CallMediaSession();

  logging::Logger& Log() const { return log(); }
  void SetHost(Libp2pHost* h) { host = h; }

  void HandleInbound(libp2p::StreamAndProtocol stream_in);
  void SetInboundHandler(InboundHandler handler);
  void ClearInboundHandler();
  bool IsActive() const;
  CallMediaSessionPhase Phase() const;
  /** Negotiated connect params (includes peer ADP hello fields when present). */
  CallMediaDirectConnectParams ActiveParams() const;
  void Detach();
  Roe<void> Connect(PeerSessionManager& sessions, const CallMediaDirectConnectParams& params,
                    CallMediaDirectCallbacks callbacks, int timeout_ms);
  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark);
  Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq, uint8_t mark);

private:
  enum class InboundAdmit { Accept, RejectNoHandler, RejectActive, RejectGlare };

  void SetPhaseLocked(CallMediaSessionPhase next, CallMediaSessionEvent ev,
                      const std::string& call_id = {});
  void IgnoreEventLocked(CallMediaSessionEvent ev, const char* reason);
  bool ConnectWaiterActiveLocked() const;
  void CancelHandshakeTimerLocked();
  void StartHandshakeTimerLocked();
  void ArmHandshakeLocked(std::shared_ptr<Stream> s);
  void ClearHandshakeLocked();
  StreamCancelCheck HandshakeCancelCheck() const;
  bool HandshakeCancelledLocked() const;
  bool LocalWinsGlareFor(const std::shared_ptr<Stream>& s) const;
  void AbandonOutboundHandshakeLocked();
  bool SuppressOutboundHelloFailLocked(const std::shared_ptr<Stream>& s);
  bool ApplyLocked(CallMediaSessionEvent ev, const std::string& call_id = {});
  bool MediaReady() const;
  bool HandleMediaFrame(Roe<std::vector<uint8_t>> frame_res);
  void CompleteConnectLocked(Roe<void> value);
  void FinishOutboundConnectLocked(Roe<void> value, const std::string& call_id);
  void TeardownTransportLocked();
  void DetachLocked(bool abort_connect,
                    CallMediaSessionEvent ev = CallMediaSessionEvent::DetachRequested);
  void Fail(const std::string& message,
            CallMediaSessionEvent ev = CallMediaSessionEvent::DuplexError);
  bool TryAdoptStreamLocked(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
                            CallMediaDirectCallbacks cbs);
  void StartMediaDuplex(std::function<void()> on_ready = {});
  bool EnqueueOutbound(std::vector<uint8_t> body);
  void FailInboundHello(const std::shared_ptr<Stream>& s, const std::string& call_id = {});
  void WriteInboundAckAndFinish(std::shared_ptr<Stream> s, CallMediaDirectConnectParams params,
                                CallMediaDirectCallbacks cbs, bool ok, const char* error);
  InboundAdmit AdmitInboundLocked(const std::shared_ptr<Stream>& inbound);
  void BeginOutboundHello(std::shared_ptr<Stream> stream, CallMediaDirectConnectParams params,
                          CallMediaDirectCallbacks callbacks,
                          std::shared_ptr<std::atomic<bool>> settled);
  void OnOutboundStream(libp2p::StreamAndProtocolOrError stream_res,
                        CallMediaDirectConnectParams params, CallMediaDirectCallbacks callbacks,
                        std::shared_ptr<std::atomic<bool>> settled);
  void OnHelloWritten(std::shared_ptr<Stream> stream, CallMediaDirectConnectParams params,
                      CallMediaDirectCallbacks callbacks,
                      std::shared_ptr<std::atomic<bool>> settled, StreamCancelCheck cancel_check,
                      Roe<void> write_res);
  void OnHelloAck(std::shared_ptr<Stream> stream, CallMediaDirectConnectParams params,
                  CallMediaDirectCallbacks callbacks, std::shared_ptr<std::atomic<bool>> settled,
                  StreamCancelCheck cancel_check, Roe<std::string> ack_utf8);

  Libp2pHost* host = nullptr;

  mutable std::mutex mu;
  std::atomic<CallMediaSessionPhase> phase{CallMediaSessionPhase::Idle};
  std::string phase_call_id;
  bool offerer_glare = false;

  std::shared_ptr<Stream> stream;
  std::shared_ptr<Stream> handshake_stream;
  std::shared_ptr<std::atomic<bool>> handshake_cancelled;
  std::shared_ptr<asio::steady_timer> handshake_timer;
  int handshake_timeout_ms = 15000;
  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  InboundHandler inbound_handler;

  std::shared_ptr<DuplexFrameSession> duplex;
  std::shared_ptr<std::atomic<bool>> duplex_cancelled;
  std::atomic<uint32_t> decrypt_fail_log_{0};
  std::atomic<uint32_t> drop_log_{0};

  std::shared_ptr<std::atomic<bool>> connect_settled;
  std::shared_ptr<std::promise<Roe<void>>> connect_promise;
};

} // namespace pbr
