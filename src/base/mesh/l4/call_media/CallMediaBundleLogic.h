#pragma once

#include "base/mesh/l4/call_media/ICallMediaTransport.h"

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

} // namespace pbr
