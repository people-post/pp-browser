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

/** Kind of mesh link the active call-media channels are bound on (path label truth). */
enum class CallMediaLinkKind {
  Unknown,
  /** ADP association (dialed or punched). */
  Direct,
  /** Nested link over a relay circuit carrier. */
  Relayed,
};

struct CallMediaDirectCallbacks {
  std::function<void()> on_connected;
  std::function<void(const std::vector<uint8_t>& opus_payload)> on_audio;
  /** V034: all channels (0=Opus, 1=H264). When set, preferred over on_audio. */
  /** seq/mark come from the wire frame; the playout jitter buffer orders and de-dupes on seq (B20). */
  std::function<void(uint8_t channel, uint32_t seq, uint8_t mark, const std::vector<uint8_t>& payload)> on_media;
  std::function<void(const std::string& error)> on_failed;
  /** k3: media moved to another path of the call (make-before-break migration); `kind` is the new path's. */
  std::function<void(CallMediaLinkKind kind)> on_path_changed;
};

/** Answer to an inbound hello — any thread, at most once. An empty `media_key` NACKs the hello. */
using CallMediaInboundAnswer = std::function<void(CallMediaDirectConnectParams, CallMediaDirectCallbacks)>;
/**
 * Inbound hello handler (V033): called on the transport's IO hop with the hello's params; returns
 * at once and answers when it knows (e.g. once the media key is stored). A hello never answered is
 * left to Detach / ClearInboundHandler / the leg timeout.
 */
using CallMediaInboundHandler =
    std::function<void(CallMediaDirectConnectParams params, CallMediaInboundAnswer answer)>;

/** For handlers that decide on the spot (fakes, tools): run `decide`, then answer inline. */
inline CallMediaInboundHandler AnswerInline(
    std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> decide) {
  return [decide = std::move(decide)](CallMediaDirectConnectParams params, CallMediaInboundAnswer answer) {
    CallMediaDirectCallbacks callbacks;
    if (decide) {
      decide(params, callbacks);
    }
    answer(std::move(params), std::move(callbacks));
  };
}

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
   * Install the product callback for inbound call-media hello (`CallMediaInboundHandler`: returns
   * at once, answers later — nothing waits on a thread). Detach / ClearInboundHandler / Connect
   * timeout still `reset()` the stream independently of this callback.
   */
  virtual void SetInboundHandler(CallMediaInboundHandler handler) = 0;
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
