#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/** Direct planner phases (V039) — CallMediaBridge owns Apply. */
enum class CallDirectPlannerPhase : uint8_t {
  Idle = 0,
  Arming,
  KeyWait,
  Connecting,
  Live,
  DegradedTxOnly,
  Stopping,
};

enum class CallDirectPlannerEvent : uint8_t {
  ScheduleOfferer = 0,
  ScheduleAnswerer,
  KeyReady,
  KeyTimeout,
  ConnectSucceeded,
  ConnectFailed,
  TxOnlyGraceExpired,
  CircuitEscalated,
  ReleaseTransport,
  Stop,
};

enum class CallDirectPlannerDecision : uint8_t {
  Transition = 0,
  Keep,
  Ignore,
};

struct CallDirectPlannerApplyContext {
  bool allows_direct_path = false;
  bool media_live_same_call = false;
  bool peer_nonempty = false;
  bool has_media_key = false;
  bool stopping = false;
};

struct CallDirectPlannerPhaseOutcome {
  CallDirectPlannerDecision decision = CallDirectPlannerDecision::Ignore;
  CallDirectPlannerPhase next = CallDirectPlannerPhase::Idle;
};

inline const char* CallDirectPlannerPhaseName(CallDirectPlannerPhase p) {
  switch (p) {
  case CallDirectPlannerPhase::Idle:
    return "Idle";
  case CallDirectPlannerPhase::Arming:
    return "Arming";
  case CallDirectPlannerPhase::KeyWait:
    return "KeyWait";
  case CallDirectPlannerPhase::Connecting:
    return "Connecting";
  case CallDirectPlannerPhase::Live:
    return "Live";
  case CallDirectPlannerPhase::DegradedTxOnly:
    return "DegradedTxOnly";
  case CallDirectPlannerPhase::Stopping:
    return "Stopping";
  }
  return "?";
}

inline const char* CallDirectPlannerEventName(CallDirectPlannerEvent ev) {
  switch (ev) {
  case CallDirectPlannerEvent::ScheduleOfferer:
    return "ScheduleOfferer";
  case CallDirectPlannerEvent::ScheduleAnswerer:
    return "ScheduleAnswerer";
  case CallDirectPlannerEvent::KeyReady:
    return "KeyReady";
  case CallDirectPlannerEvent::KeyTimeout:
    return "KeyTimeout";
  case CallDirectPlannerEvent::ConnectSucceeded:
    return "ConnectSucceeded";
  case CallDirectPlannerEvent::ConnectFailed:
    return "ConnectFailed";
  case CallDirectPlannerEvent::TxOnlyGraceExpired:
    return "TxOnlyGraceExpired";
  case CallDirectPlannerEvent::CircuitEscalated:
    return "CircuitEscalated";
  case CallDirectPlannerEvent::ReleaseTransport:
    return "ReleaseTransport";
  case CallDirectPlannerEvent::Stop:
    return "Stop";
  }
  return "?";
}

/**
 * Pure Direct planner table (V039). Effects stay in CallMediaBridge::Apply.
 * Status Direct* is required for Schedule/Key/Connect progress; Stop/Release always allowed.
 */
inline CallDirectPlannerPhaseOutcome DecideCallDirectPlannerPhase(CallDirectPlannerPhase phase,
                                                                  CallDirectPlannerEvent ev,
                                                                  const CallDirectPlannerApplyContext& ctx) {
  CallDirectPlannerPhaseOutcome out;
  out.next = phase;

  if (ctx.stopping && ev != CallDirectPlannerEvent::Stop &&
      ev != CallDirectPlannerEvent::ReleaseTransport) {
    out.decision = CallDirectPlannerDecision::Ignore;
    return out;
  }

  switch (ev) {
  case CallDirectPlannerEvent::Stop:
    if (phase == CallDirectPlannerPhase::Idle) {
      out.decision = CallDirectPlannerDecision::Keep;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Idle;
    return out;

  case CallDirectPlannerEvent::ReleaseTransport:
    // SoftMigrate: drop stream; capture may stay up — planner returns to Idle for Direct path.
    if (phase == CallDirectPlannerPhase::Idle) {
      out.decision = CallDirectPlannerDecision::Keep;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Idle;
    return out;

  case CallDirectPlannerEvent::ScheduleOfferer:
  case CallDirectPlannerEvent::ScheduleAnswerer:
    if (!ctx.allows_direct_path) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    if (ctx.media_live_same_call &&
        (phase == CallDirectPlannerPhase::Live || phase == CallDirectPlannerPhase::Connecting ||
         phase == CallDirectPlannerPhase::DegradedTxOnly)) {
      out.decision = CallDirectPlannerDecision::Keep;
      return out;
    }
    if (!ctx.peer_nonempty) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Arming;
    return out;

  case CallDirectPlannerEvent::KeyReady:
    if (phase != CallDirectPlannerPhase::KeyWait && phase != CallDirectPlannerPhase::Arming &&
        phase != CallDirectPlannerPhase::Idle) {
      if (phase == CallDirectPlannerPhase::Connecting || phase == CallDirectPlannerPhase::Live) {
        out.decision = CallDirectPlannerDecision::Keep;
        return out;
      }
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    if (!ctx.allows_direct_path) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Connecting;
    return out;

  case CallDirectPlannerEvent::KeyTimeout:
    if (phase != CallDirectPlannerPhase::KeyWait && phase != CallDirectPlannerPhase::Arming) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Idle;
    return out;

  case CallDirectPlannerEvent::ConnectSucceeded:
    if (phase != CallDirectPlannerPhase::Connecting && phase != CallDirectPlannerPhase::Arming &&
        phase != CallDirectPlannerPhase::DegradedTxOnly && phase != CallDirectPlannerPhase::Live) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    if (!ctx.allows_direct_path && phase != CallDirectPlannerPhase::Live) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Live;
    return out;

  case CallDirectPlannerEvent::ConnectFailed:
    if (phase == CallDirectPlannerPhase::Idle || phase == CallDirectPlannerPhase::Stopping) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Idle;
    return out;

  case CallDirectPlannerEvent::TxOnlyGraceExpired:
    if (phase != CallDirectPlannerPhase::Live) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::DegradedTxOnly;
    return out;

  case CallDirectPlannerEvent::CircuitEscalated:
    if (phase != CallDirectPlannerPhase::DegradedTxOnly && phase != CallDirectPlannerPhase::Live) {
      out.decision = CallDirectPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallDirectPlannerDecision::Transition;
    out.next = CallDirectPlannerPhase::Connecting;
    return out;
  }

  out.decision = CallDirectPlannerDecision::Ignore;
  return out;
}

/** After Arming: answerer without key → KeyWait; else Connecting. */
inline CallDirectPlannerPhase DirectPlannerPhaseAfterArm(bool offerer, bool has_media_key) {
  if (!offerer && !has_media_key) {
    return CallDirectPlannerPhase::KeyWait;
  }
  return CallDirectPlannerPhase::Connecting;
}

} // namespace pbr
