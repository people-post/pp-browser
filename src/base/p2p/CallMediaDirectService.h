#pragma once

#include "base/p2p/ICallMediaTransport.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaSession;

/**
 * 1:1 libp2p call-media transport (m1 / V026).
 * Audio/video payloads are AEAD-encrypted under the shared call media key before send.
 * Session legality is an explicit phase machine (V033 / SESSION_MACHINES.md).
 * Implements ICallMediaTransport for composition with Amp ([A020]).
 */
class CallMediaDirectService : public ICallMediaTransport {
public:
  CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions);
  ~CallMediaDirectService() override;

  CallMediaDirectService(const CallMediaDirectService&) = delete;
  CallMediaDirectService& operator=(const CallMediaDirectService&) = delete;

  void Start() override;
  void Stop() override;
  bool IsStarted() const { return started_; }

  /** Register handler for inbound streams (answerer / offerer reverse-dial). */
  void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) override;
  /** Drop handler so late protocol deliveries cannot touch a destroyed bridge. */
  void ClearInboundHandler() override;

  /** Active outbound/inbound session (adopted stream). */
  bool IsActive() const override;
  /** Negotiated connect params (includes peer ADP hello fields when present). */
  CallMediaDirectConnectParams ActiveParams() const override;
  /** Diagnostics: current transport session phase. */
  CallMediaSessionPhase Phase() const override;
  /** Close stream / unblock Connect; does not clear inbound handler (retry uses Detach). */
  void Detach() override;

  /** Client: dial peer and run hello handshake; starts async IO-thread pump. */
  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    int timeout_ms = 15000) override;

  /** Encrypt and enqueue Opus frame; IO thread owns stream write (non-blocking). */
  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0) override;
  /** Encrypt and enqueue a media frame (`channel` 0=Opus, 1=H264 video_lo). */
  Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0) override;

private:
  std::shared_ptr<CallMediaSession> session_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
