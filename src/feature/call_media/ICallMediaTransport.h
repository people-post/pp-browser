#pragma once

#include "domain/calls/CallMediaSessionTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/media/CallMediaHealth.h"
#include "common/media/CallMediaProtocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Why a call-media connect failed (docs/contracts/CODED_FAILURE.md, call-media leg). */
enum class CallMediaErr : int32_t {
  Ok = 0,
  NotStarted,
  /** Missing peer, call id or media key. */
  BadRequest,
  /** No endpoint or link to the peer. */
  Unreachable,
  /** The link to the peer failed to send: it is likely dead — the next attempt dials a fresh one. */
  StaleLink,
  /** Opening the control channel failed at the link (otherwise). */
  LinkFailed,
  /** A channel did not open, or the hello could not be written. */
  ChannelFailed,
  /** The peer refused the hello (or its ack failed). */
  HelloRejected,
  Timeout,
  /** The stream or the link went away. */
  Closed,
  /** The peer closed or reset the call's stream: a clean end, not a network failure. */
  StreamClosed,
  Aborted,
  Generic,
};
using CallMediaFailure = CodedFailure<CallMediaErr>;
using CallMediaRoe = CodedRoe<void, CallMediaErr>;

struct CallMediaDirectCallbacks {
  std::function<void()> on_connected;
  std::function<void(const std::vector<uint8_t>& opus_payload)> on_audio;
  /** V034: all channels (0=Opus, 1=H264). When set, preferred over on_audio. */
  /** seq/mark come from the wire frame; the playout jitter buffer orders and de-dupes on seq (B20). */
  std::function<void(uint8_t channel, uint32_t seq, uint8_t mark, const std::vector<uint8_t>& payload)> on_media;
  std::function<void(const CallMediaFailure& failure)> on_failed;
  /** k3: media moved to another path of the call (make-before-break migration); `kind` is the new path's. */
  std::function<void(CallMediaLinkKind kind)> on_path_changed;
  /**
   * k4: the call lost its last path (link gone, no standby). It is kept for a reconnect window: a
   * migration onto a new link brings it back (`on_path_changed`); the window running out fails it
   * (`on_failed`).
   */
  std::function<void()> on_path_lost;
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
  /**
   * The primary bundle's peer as an authenticated mesh PeerId; empty until bound. Unlike
   * ActiveParams().peer_key — the dial key of whatever link the call is on, which after a path
   * move can be a local alias (amp:burst:…) no relay or dial can use.
   */
  virtual std::string ActiveRemotePeerId() const { return {}; }
  /** Reliable figures of the association the primary bundle's active path rides; unavailable until bound. */
  virtual CallLinkCounters ActiveLinkCounters() const { return {}; }
  /**
   * k3 make-before-break: move the active call onto the peer's Connected link of `kind` while it
   * keeps running. `done` (on the transport's IO strand) is OK once media flows there; an error
   * leaves the call on its current path. Transports without path migration refuse.
   */
  virtual void MigrateTo(CallMediaLinkKind kind, std::function<void(Roe<void>)> done) {
    (void)kind;
    if (done) {
      done(Error("call-media path migration not supported"));
    }
  }
  /**
   * k6: whether the transport may move a relayed call onto a direct link to the peer by itself
   * (the pair policy — a mobile pair anchors on the relay). Default: transports without it ignore.
   */
  virtual void SetAutoMigrateToDirect(bool allow) { (void)allow; }
  /** k6: link kind of the call's warm standby path; Unknown when it has none. */
  virtual CallMediaLinkKind StandbyLinkKind() const { return CallMediaLinkKind::Unknown; }
  /**
   * k6 (K003): bring the peer's Connected link of `kind` up as the call's standby; the call stays on
   * its path. Best-effort: a peer without it refuses (the call is unharmed). `done` on the IO strand.
   */
  virtual void AddStandby(CallMediaLinkKind kind, std::function<void(Roe<void>)> done) {
    (void)kind;
    if (done) {
      done(Error("call-media standby not supported"));
    }
  }
  virtual void Detach() = 0;

  /**
   * Non-blocking connect: arms the outbound leg and invokes `on_done` when MediaReady,
   * failed, timed out, or superseded. Prefer this over Connect(); MeshPump drives Amp.
   */
  virtual void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                            std::function<void(CallMediaRoe)> on_done, int timeout_ms = 15000) = 0;

  /** Blocking convenience for tests/harnesses; product bridge uses ConnectAsync. */
  virtual CallMediaRoe Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                               int timeout_ms = 15000) = 0;

  virtual Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0) = 0;
  virtual Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                              uint8_t mark = 0) = 0;
};

} // namespace pbr
