#pragma once

#include "domain/messaging/CallLifecycleTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

/** Named side effects for CallLifecycle::Apply to execute (no I/O in Decide). */
enum class CallLifecycleAction : uint16_t {
  None = 0,
  NoteRing = 1u << 0,
  SetAccepting = 1u << 1,
  ClearAccepting = 1u << 2,
  SetPhase = 1u << 3,
  SetStatus = 1u << 4,
  NotifyChrome = 1u << 5,
  /** Prefer PostUI(NotifyChrome) when already on UI (AcceptClicked). */
  DeferChrome = 1u << 6,
  PostAcceptInvite = 1u << 7,
  PostDeclineInvite = 1u << 8,
  PostLeaveCall = 1u << 9,
  PostRetryMedia = 1u << 10,
  KickAnswererDirectMedia = 1u << 11,
  LogIgnored = 1u << 12,
  LogKeepPhase = 1u << 13,
};

inline CallLifecycleAction operator|(CallLifecycleAction a, CallLifecycleAction b) {
  return static_cast<CallLifecycleAction>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline CallLifecycleAction& operator|=(CallLifecycleAction& a, CallLifecycleAction b) {
  a = a | b;
  return a;
}
inline bool HasAction(CallLifecycleAction set, CallLifecycleAction bit) {
  return (static_cast<uint16_t>(set) & static_cast<uint16_t>(bit)) != 0;
}

struct CallLifecycleTransitionContext {
  CallPhase phase = CallPhase::Idle;
  CallMediaStatus status = CallMediaStatus::None;
  std::string active_call_id;
  std::string accepting_call_id;
  std::string last_ring_call_id;
  /** Event call_id argument (may be empty). */
  std::string event_call_id;
  bool sessions_bound = false;
  bool allows_direct_path = false;
};

struct CallLifecycleTransitionOutcome {
  CallLifecycleAction actions = CallLifecycleAction::None;
  /** Resolved call_id for posts / phase (after last_ring fallback). */
  std::string call_id;
  CallPhase next_phase = CallPhase::Idle;
  CallMediaStatus next_status = CallMediaStatus::None;
  const char* status_reason = nullptr;
  const char* ignore_reason = nullptr;
};

/**
 * Pure chrome State+Status transition table (V037).
 * CallLifecycle::Apply executes actions; planners/CSM stay outside this table.
 */
CallLifecycleTransitionOutcome DecideCallLifecycleTransition(CallLifecycleEvent ev,
                                                             const CallLifecycleTransitionContext& ctx);

} // namespace pbr
