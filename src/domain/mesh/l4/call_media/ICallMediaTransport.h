#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kRealtimeProtocolId = "/pp-browser/realtime/1.0.0";
inline constexpr const char* kCallMediaDirectProtocolId = kRealtimeProtocolId;

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
  /** seq/mark come from the wire frame; the playout jitter buffer orders and de-dupes on seq (B20). */
  std::function<void(uint8_t channel, uint32_t seq, uint8_t mark, const std::vector<uint8_t>& payload)> on_media;
  std::function<void(const std::string& error)> on_failed;
};

/** Kind of mesh link the active call-media channels are bound on (path label truth). */
enum class CallMediaLinkKind {
  Unknown,
  /** ADP association (dialed or punched). */
  Direct,
  /** Nested link over a relay circuit carrier. */
  Relayed,
};

/**
 * Single product entry for 1:1 call-media transport ([A020]).
 * Amp: CallMediaAmpTransport → CallMediaLegCoordinator.
 */
class ICallMediaTransport {
public:
  virtual ~ICallMediaTransport() = default;

  virtual void Start() = 0;
  virtual void Stop() = 0;

  /**
   * Install the product callback for inbound call-media hello.
   *
   * **Contract (V033 / SESSION_MACHINES):** the handler runs on a worker hop and must not
   * stall the pool with bare `sleep_for` / blocking I/O. Prefer a cancelable wait
   * (condition_variable + teardown/key notify) or return promptly without a media key
   * (transport will NACK the hello). Detach / ClearInboundHandler / Connect timeout still
   * `reset()` the stream independently of this callback.
   */
  virtual void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) = 0;
  virtual void ClearInboundHandler() = 0;

  virtual bool IsActive() const = 0;
  virtual CallMediaDirectConnectParams ActiveParams() const = 0;
  virtual CallMediaSessionPhase Phase() const = 0;
  /** Link kind carrying the primary bundle's channels; Unknown until bound. */
  virtual CallMediaLinkKind ActiveLinkKind() const { return CallMediaLinkKind::Unknown; }
  virtual void Detach() = 0;

  /**
   * Non-blocking connect: arms the outbound leg and invokes `on_done` when MediaReady,
   * failed, timed out, or superseded. Prefer this over Connect(); MeshPump drives Amp.
   */
  virtual void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                            std::function<void(Roe<void>)> on_done, int timeout_ms = 15000) = 0;

  /** Blocking convenience for tests/harnesses; product bridge uses ConnectAsync. */
  virtual Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                            int timeout_ms = 15000) = 0;

  virtual Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0) = 0;
  virtual Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                              uint8_t mark = 0) = 0;
};

} // namespace pbr
