#pragma once

#include "lib/calls/CallMediaSessionTypes.h"

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
  /** The call's bundle belongs to another peer (PB-018) — reject inbound, keep the bundle. */
  RejectPeer,
};

struct CallMediaInboundHelloContext {
  CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
  bool has_outbound_control = false;
  bool offerer = false;
  /** Role-aware, antisymmetric winner (LocalWinsCallMediaGlareForRoles). */
  bool local_wins_glare = true;
  /** Another call already bound/media-ready on this peer coordinator. */
  bool other_bundle_busy = false;
  /**
   * The call's bundle is bound to an authenticated PeerId other than the hello's. Anyone who learned
   * the call id can send one; a peer that lost glare must not take a stranger's hello as the answer.
   */
  bool from_another_peer = false;
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

/** Which of a call's paths holds a channel (call-path-resilience k3 / k4). */
enum class CallMediaPathSlot {
  /** The path media flows on. */
  Active = 0,
  /** A migration's / `path_add`'s path being brought up. */
  Candidate,
  /** The path the call just left, draining until its release. */
  Retiring,
  /** The warm fallback. */
  Standby,
};

/** A channel of one of a leg's paths closed (either end, any reason). */
struct CallMediaChannelCloseInput {
  /** The leg is an inbound placeholder: its control channel never said hello / migrate. */
  bool placeholder = false;
  CallMediaPathSlot slot = CallMediaPathSlot::Active;
  CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
  CallMediaChannelRole role = CallMediaChannelRole::OutboundControl;
  /** This end cancelled the leg. */
  bool local_cancel = false;
  /** The peer closed or reset the channel (`peer_close` / `peer_reset`). */
  bool remote_terminal = false;
};

enum class CallMediaChannelCloseAction {
  /** It carried no call: drop the placeholder. */
  DropPlaceholder = 0,
  /** The migration (or standby add) failed: the call stays where it is. */
  AbandonCandidate,
  /** Expected while (or after) the old path is released — never a failure. */
  DropRetiring,
  /** The call loses its standby only. */
  DropStandby,
  /** K014: a live call's active path closed under it — DecideCallMediaActiveClose says what next. */
  ActivePathClosed,
  /** Dual-dial: the peer's abandoned outbound (our inbound) closed while ours wins — drop that slot only. */
  DropInboundControl,
  /** A leg that is not connecting yet. */
  Ignore,
  /** The connecting leg fails, unreported (this end cancelled, or a local / link-side close). */
  TearDownQuietly,
  /** The connecting leg fails and says so (`on_failed`): the peer closed or reset the channel. */
  TearDownReported,
};

/** Where a closed channel's leg goes: by the path that held it, then the leg's phase. */
CallMediaChannelCloseAction RouteCallMediaChannelClose(const CallMediaChannelCloseInput& in);

/** Map AMP bundle phase → libp2p session phase for transitional Phase() API. */
CallMediaSessionPhase CallMediaBundlePhaseToSessionPhase(CallMediaBundlePhase phase);

CallMediaLegPhase CallMediaBundlePhaseToLegPhase(CallMediaBundlePhase phase);

bool CallMediaBundlePhaseIsActive(CallMediaBundlePhase phase);

// --- The leg's timers -------------------------------------------------------------------------
// Every one is milliseconds on the mesh runtime's clock (amp `Endpoint::GetClock()`): the leg
// coordinator reads no other clock, so a VirtualClock harness drives them all.

/** A leg's connect (control hello → media ready) when its caller sets none; an inbound hello's leg too. */
inline constexpr int64_t kCallMediaConnectTimeoutMs = 15000;
/** An inbound control channel is held (a placeholder) this long for its `hello` / `migrate`. */
inline constexpr int64_t kCallMediaPlaceholderTimeoutMs = 15000;
/** k3: a candidate path must be up and acknowledged within this, or the call stays where it is. */
inline constexpr int64_t kCallMediaMigrateTimeoutMs = 5000;
/** k3: between automatic attempts to move a relayed call onto a direct link. */
inline constexpr int64_t kCallMediaAutoMigrateBackoffMs = 10000;
/** k4 (K008): a call without any path waits this long for a new one before it fails. */
inline constexpr int64_t kCallMediaReconnectWindowMs = 30000;
/** k4 (K008): heartbeat cadence per path role, and when a silent path counts as dead. */
inline constexpr int64_t kCallMediaActiveHeartbeatMs = 500;
inline constexpr int64_t kCallMediaStandbyHeartbeatMs = 10000;
inline constexpr int64_t kCallMediaActiveSilenceFailoverMs = 1500;
/**
 * With no standby to fail over to, a path silent this long while the peer heartbeats is lost
 * (Reconnecting…, then the reconnect window): the peer's app died or its network went, and the
 * link under the path can take most of a minute to notice (its liveness window follows the
 * keepalive cadence).
 */
inline constexpr int64_t kCallMediaActiveSilenceLostMs = 3000;
inline constexpr int64_t kCallMediaStandbyStaleMs = 25000;
/**
 * k7: a call that lost its path while the peer is still connected on another link moves there
 * quietly; the loss is reported (`on_path_lost`, "Reconnecting…") only if that has not landed by then.
 */
inline constexpr int64_t kCallMediaQuietRebindGraceMs = 1000;
/** After a failover, silence alone does not switch again for this long (the peer is still moving). */
inline constexpr int64_t kCallMediaFailoverHoldDownMs = 3000;
/**
 * K014: the peer closed the active path's media channel while the path's control channel is still
 * up. The media channel's CLOSE is best-effort and can overtake the reliable `bye` sent before it,
 * so the path waits this long for that channel's `bye` (the call ended) or its own close (a lost
 * path) before it counts as lost.
 */
inline constexpr int64_t kCallMediaByeAwaitMs = 1500;

/** k3: after a switch, the old path is released once media arrived on the new one and this long passed ... */
inline constexpr int64_t kCallMediaRetireAfterSwitchMs = 1000;
/** ... or after this long regardless (a muted / silent peer). */
inline constexpr int64_t kCallMediaRetireAtMostMs = 5000;
/** A retiring path whose release was never acknowledged is dropped after this. */
inline constexpr int64_t kCallMediaRetireAbandonMs = 10000;

/** K014: one of the active path's channels of a live (MediaReady) call closed — not by this end. */
struct CallMediaActiveCloseInput {
  CallMediaChannelRole role = CallMediaChannelRole::Media;
  /** The peer closed or reset the channel (vs. the link going or a write failing). */
  bool remote_close = false;
  /** The active path still has a bound control channel. */
  bool control_open = false;
  /** The path the call just left is still draining on a live link. */
  bool retiring_alive = false;
  /** A warm standby on a live link. */
  bool standby_alive = false;
};

enum class CallMediaActiveCloseAction {
  /** Drop the media channel and wait (kCallMediaByeAwaitMs) for the control channel's word. */
  AwaitControl,
  /** Back onto the path the call just left. */
  FallBackToRetiring,
  /** TX onto the standby. */
  FailOverToStandby,
  /** No path left: the reconnect window (the offerer re-anchors). */
  PathLost,
};

/**
 * K014: a close the peer did not announce with `bye` is a lost path — never the call's end (that
 * comes with `bye`, or with call_leave from an older peer): the path falls back, fails over, or
 * waits for a new one exactly as for a dead link.
 */
CallMediaActiveCloseAction DecideCallMediaActiveClose(const CallMediaActiveCloseInput& in);

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
  /**
   * K014: the peer closed the active media channel and the path waits (kCallMediaByeAwaitMs) for its control
   * channel's `bye` or close. Silence decides nothing meanwhile — the wait's end does; a lost link still does.
   */
  bool awaiting_bye = false;
};

/**
 * k4 failover: switch TX to the standby path when the active one is lost, or silent 1.5 s while the
 * peer is known to heartbeat (a muted mic still heartbeats and sends silence frames — only a dead
 * path goes quiet) and no `bye` is awaited. Never onto a standby that is itself gone or stale.
 */
bool ShouldFailOverToStandby(const CallMediaFailoverInput& in);

/** k4: the active path is lost and there is no failover — its link went, or it went silent. */
bool ShouldEnterPathLost(const CallMediaFailoverInput& in);

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

struct CallMediaRetiringInput {
  bool link_lost = false;
  int64_t since_switch_ms = 0;
  /** This end drove the switch (it releases the old path; the peer acknowledges). */
  bool drove_switch = false;
  bool release_sent = false;
  bool can_send = false;
  /** Media arrived on the new path since the switch. */
  bool rx_since_switch = false;
};

enum class CallMediaRetiringAction { Keep, Drop, SendRelease };

/** k3 make-before-break: what happens to the path a call just left. */
CallMediaRetiringAction DecideCallMediaRetiring(const CallMediaRetiringInput& in);

} // namespace pbr
