#pragma once

/**
 * Call chrome State + media Status enums (V037).
 * Pure types — no Module / AppRuntime. CallLifecycle owns Apply side effects.
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
};

enum class CallArmedPlanner {
  None = 0,
  Lifecycle,
  Bridge,
  Topology,
};

enum class CallLifecycleEvent {
  InviteSeen = 0,
  InviteCleared,
  OutboundStarted,
  AcceptClicked,
  DeclineClicked,
  LeaveClicked,
  RetryClicked,
  AcceptSucceeded,
  AcceptFailed,
  DeclineDone,
  LeaveDone,
  MediaDeferred,
  MediaKeyReady,
  DirectConnected,
  ConnectFailedEvt,
  RemoteEnded,
};

const char* CallPhaseName(CallPhase phase);
const char* CallMediaStatusName(CallMediaStatus status);
const char* CallArmedPlannerName(CallArmedPlanner planner);
const char* CallLifecycleEventName(CallLifecycleEvent ev);

} // namespace pbr
