#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include <bitset>
#include <climits>
#include <cstdint>

namespace pbr {

struct CallMediaLegId {
  uint64_t value = 0;
  explicit operator bool() const { return value != 0; }
};

/** Public leg view used by tests / callers (maps from bundle phase). */
enum class CallMediaLegPhase {
  Closed = 0,
  ControlHello,
  AwaitingMedia,
  MediaReady,
};

/** AMP-native call-media bundle phase (not a libp2p stream phase). */
enum class CallMediaBundlePhase {
  Idle = 0,
  OutboundHello,
  InboundHello,
  AwaitingMedia,
  MediaReady,
  Closing,
};

enum class CallMediaChannelRole {
  OutboundControl = 0,
  InboundControl,
  Media,
};

enum class CallMediaInboundHelloDecision {
  /** Adopt inbound; no outbound to abandon. */
  Accept = 0,
  /** Abandon outbound control, then adopt inbound (lost glare). */
  AcceptAndYield,
  /** Session busy — reject inbound. */
  RejectBusy,
  /** Local wins glare — reject inbound, keep outbound. */
  RejectGlare,
};

struct CallMediaInboundHelloContext {
  CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
  bool has_outbound_control = false;
  bool offerer = false;
  /** Role-aware, antisymmetric winner (LocalWinsCallMediaGlareForRoles). */
  bool local_wins_glare = true;
  /** Another call already bound/media-ready on this peer coordinator. */
  bool other_bundle_busy = false;
};

CallMediaInboundHelloDecision DecideCallMediaInboundHello(const CallMediaInboundHelloContext& ctx);

enum class CallMediaHelloAckDecision {
  ProceedToMedia = 0,
  Fail,
  IgnoreStale,
  YieldOutbound,
};

struct CallMediaHelloAckContext {
  CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
  bool ack_ok = false;
  bool from_outbound_control = false;
  bool offerer = false;
  bool local_wins_glare = true;
};

CallMediaHelloAckDecision DecideCallMediaHelloAck(const CallMediaHelloAckContext& ctx);

enum class CallMediaChannelCloseDecision {
  Ignore = 0,
  FailLeg,
  /** Tear down without on_failed (local cancel / SoftMigrate). */
  SuppressNotify,
};

struct CallMediaChannelCloseContext {
  CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
  CallMediaChannelRole role = CallMediaChannelRole::OutboundControl;
  bool local_cancel = false;
  bool remote_terminal = false;
  bool slot_still_owned = true;
};

CallMediaChannelCloseDecision DecideCallMediaChannelClose(const CallMediaChannelCloseContext& ctx);

/** Map AMP bundle phase → libp2p session phase for transitional Phase() API. */
CallMediaSessionPhase CallMediaBundlePhaseToSessionPhase(CallMediaBundlePhase phase);

CallMediaLegPhase CallMediaBundlePhaseToLegPhase(CallMediaBundlePhase phase);

bool CallMediaBundlePhaseIsActive(CallMediaBundlePhase phase);

/** k4 (K008): heartbeat cadence per path role, and when a silent path counts as dead. */
inline constexpr int64_t kCallMediaActiveHeartbeatMs = 500;
inline constexpr int64_t kCallMediaStandbyHeartbeatMs = 10000;
inline constexpr int64_t kCallMediaActiveSilenceFailoverMs = 1500;
inline constexpr int64_t kCallMediaStandbyStaleMs = 25000;
/**
 * k7: a call that lost its path while the peer is still connected on another link moves there
 * quietly; the loss is reported (`on_path_lost`, "Reconnecting…") only if that has not landed by then.
 */
inline constexpr int64_t kCallMediaQuietRebindGraceMs = 1000;
/** After a failover, silence alone does not switch again for this long (the peer is still moving). */
inline constexpr int64_t kCallMediaFailoverHoldDownMs = 3000;

struct CallMediaFailoverInput {
  bool active_link_lost = false;
  /** Since anything (heartbeat, control, media) arrived on the active path. */
  int64_t active_silence_ms = 0;
  /** The peer sends heartbeats (seen at least one this call): silence then means a dead path. */
  bool peer_heartbeats = false;
  bool have_standby = false;
  bool standby_link_alive = false;
  int64_t standby_silence_ms = 0;
  /** Since this end last failed over (large when never). */
  int64_t since_failover_ms = INT64_MAX;
};

/**
 * k4 failover: switch TX to the standby path when the active one is lost, or silent 1.5 s while the
 * peer is known to heartbeat (a muted mic still heartbeats and sends silence frames — only a dead
 * path goes quiet). Never onto a standby that is itself gone or stale.
 */
bool ShouldFailOverToStandby(const CallMediaFailoverInput& in);

/**
 * Receive-side seq de-dupe for one media channel (call-path-resilience k3): while two paths
 * overlap a frame can arrive on both. Remembers the last kWindow seqs below the highest seen. A
 * seq more than kWindow behind the highest is a sender restart (the bridge resets its seq on a
 * new session), accepted and re-anchored.
 */
class CallMediaSeqWindow {
public:
  static constexpr uint32_t kWindow = 1024;
  /** True the first time `seq` is seen (deliver), false for a duplicate. */
  bool Accept(uint32_t seq);

private:
  bool have_ = false;
  uint32_t highest_ = 0;
  std::bitset<kWindow> seen_;  // bit i = highest_ - i
};

} // namespace pbr
