#pragma once

/**
 * Call chrome State + media Status enums (V037).
 * Pure types. The State is projected from the call (LiveCall::Phase); the Status is the call's own.
 */

namespace pbr {

/** Call chrome / shell State. JoinedLocal / MediaPending / MediaConnecting are Calling-like. */
enum class CallPhase {
  Idle = 0,
  Ringing,
  Accepting,
  OutboundCalling,
  JoinedLocal,
  MediaPending,
  MediaConnecting,
  InCall,
  ConnectFailed,
};

/**
 * Why the call's hop path is being recovered — one reason at a time, highest first. The screen's
 * status line reads it; path decisions keep reading the topology's own state.
 */
enum class CallRecovery {
  None,
  /** A SoftMigrate from the 1:1 path onto a hop is in flight ("Setting up group call…"). */
  Migrating,
  /** The call waits for its hop to attach ("Waiting for media path…"). */
  WaitingForHop,
  /** A guest's lost hop transport: a reattach is in flight. */
  Reattaching,
  /** A guest's lost hop transport: attempts made, none in flight, nothing flows. */
  HopLost,
};
/** A recovery is in progress (Migrating / WaitingForHop / Reattaching): not HopLost or None. */
inline bool RecoveryInFlight(const CallRecovery r) {
  return r == CallRecovery::Migrating || r == CallRecovery::WaitingForHop || r == CallRecovery::Reattaching;
}
const char* CallRecoveryName(CallRecovery recovery);

/** Media Status under Calling-like / InCall. Arms at most one planner. */
enum class CallMediaStatus {
  None = 0,
  Deciding,
  DirectConnecting,
  HopWaiting,
  HopAttaching,
  DirectLive,
  HopLive,
  Migrating,
  DegradedTxOnly,
  Failed,
  /** k4: the call lost its path and waits (≤ 30 s) for a new one — UI "Reconnecting…", timer runs on. */
  Reconnecting,
};

enum class CallArmedPlanner {
  None = 0,
  Lifecycle,
  Bridge,
  Topology,
};

/**
 * What happens to the shown call (V037): the user's clicks, and the media path's events. The device
 * shows a projection of its calls (LiveCalls) — these only drive the calls.
 */
enum class CallLifecycleEvent {
  InviteSeen = 0,
  InviteCleared,
  OutboundStarted,
  AcceptClicked,
  DeclineClicked,
  LeaveClicked,
  RetryClicked,
  MediaDeferred,
  MediaKeyReady,
  DirectConnected,
  ConnectFailedEvt,
  /** The peer's connection for a failed, still-open call reached us (its retry): resume media. */
  PeerReconnected,
};

const char* CallPhaseName(CallPhase phase);
const char* CallMediaStatusName(CallMediaStatus status);
const char* CallArmedPlannerName(CallArmedPlanner planner);
const char* CallLifecycleEventName(CallLifecycleEvent ev);

} // namespace pbr
