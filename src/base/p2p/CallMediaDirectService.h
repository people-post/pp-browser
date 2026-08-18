#pragma once

#include "base/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kCallMediaDirectProtocolId = "/pp-browser/call-media/1.0.0";

/** Transport session phases for 1:1 call-media (V033). Product UX phases stay in CallLifecycle. */
enum class CallMediaSessionPhase {
  Idle = 0,
  Dialing,
  HelloOutbound,
  HelloInbound,
  Adopting,
  MediaReady,
  Detaching,
};

enum class CallMediaSessionEvent {
  ConnectRequested = 0,
  OpenStreamOk,
  OpenStreamFail,
  InboundStream,
  HelloOk,
  HelloFail,
  AdoptWon,
  AdoptLost,
  DuplexStarted,
  DuplexEof,
  DuplexError,
  DetachRequested,
  ConnectTimeout,
  HandlerCleared,
  ConnectSuperseded,
};

const char* CallMediaSessionPhaseName(CallMediaSessionPhase phase);
const char* CallMediaSessionEventName(CallMediaSessionEvent ev);

struct CallMediaDirectConnectParams {
  std::string peer_key;
  std::string call_id;
  uint32_t media_epoch = 1;
  ByteVector media_key;
  bool offerer = true;
};

struct CallMediaDirectCallbacks {
  std::function<void()> on_connected;
  std::function<void(const std::vector<uint8_t>& opus_payload)> on_audio;
  /** V034: all channels (0=Opus, 1=H264). When set, preferred over on_audio. */
  std::function<void(uint8_t channel, const std::vector<uint8_t>& payload)> on_media;
  std::function<void(const std::string& error)> on_failed;
};

class CallMediaSession;

/**
 * 1:1 libp2p call-media transport (m1 / V026).
 * Opus payloads are AEAD-encrypted under the shared call media key before send.
 * Session legality is an explicit phase machine (V033 / SESSION_MACHINES.md).
 */
class CallMediaDirectService {
public:
  CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions);
  ~CallMediaDirectService();

  CallMediaDirectService(const CallMediaDirectService&) = delete;
  CallMediaDirectService& operator=(const CallMediaDirectService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Register handler for inbound streams (answerer / offerer reverse-dial). */
  void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler);
  /** Drop handler so late protocol deliveries cannot touch a destroyed bridge. */
  void ClearInboundHandler();

  /** Active outbound/inbound session (adopted stream). */
  bool IsActive() const;
  /** Diagnostics: current transport session phase. */
  CallMediaSessionPhase Phase() const;
  /** Close stream / unblock Connect; does not clear inbound handler (retry uses Detach). */
  void Detach();

  /** Client: dial peer and run hello handshake; starts async IO-thread pump. */
  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    int timeout_ms = 15000);

  /** Encrypt and enqueue Opus frame; IO thread owns stream write (non-blocking). */
  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);
  /** Encrypt and enqueue a media frame (`channel` 0=Opus, 1=H264 video_lo). */
  Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq, uint8_t mark = 0);

private:
  std::shared_ptr<CallMediaSession> session_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
