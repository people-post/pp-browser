#pragma once

#include "feature/calls/CallMediaPlannerSelectLogic.h"

#include <cstdint>
#include <string>

namespace pbr {

/** Hop planner phases (V039) — CallTopologyController owns Apply. */
enum class CallHopPlannerPhase : uint8_t {
  Idle = 0,
  WaitingAttach,
  Migrating,
  Attaching,
  Live,
  Stopping,
};

enum class CallHopPlannerEvent : uint8_t {
  LocalAcceptN3 = 0,
  RemoteAcceptN3,
  SfuAttachInbound,
  SoftMigrateRequested,
  AttachSucceeded,
  AttachFailed,
  AttachWaitExpired,
  HopRefuse,
  Stop,
};

enum class CallHopPlannerDecision : uint8_t {
  Transition = 0,
  Keep,
  Ignore,
};

struct CallHopPlannerApplyContext {
  bool allows_hop_path = false;
  bool should_arm_hop = false; // N≥3 / effective planner N
  bool has_sfu_hint = false;
  bool soft_migrate_in_flight = false;
  bool sfu_attached = false;
};

struct CallHopPlannerPhaseOutcome {
  CallHopPlannerDecision decision = CallHopPlannerDecision::Ignore;
  CallHopPlannerPhase next = CallHopPlannerPhase::Idle;
};

inline const char* CallHopPlannerPhaseName(CallHopPlannerPhase p) {
  switch (p) {
  case CallHopPlannerPhase::Idle:
    return "Idle";
  case CallHopPlannerPhase::WaitingAttach:
    return "WaitingAttach";
  case CallHopPlannerPhase::Migrating:
    return "Migrating";
  case CallHopPlannerPhase::Attaching:
    return "Attaching";
  case CallHopPlannerPhase::Live:
    return "Live";
  case CallHopPlannerPhase::Stopping:
    return "Stopping";
  }
  return "?";
}

inline const char* CallHopPlannerEventName(CallHopPlannerEvent ev) {
  switch (ev) {
  case CallHopPlannerEvent::LocalAcceptN3:
    return "LocalAcceptN3";
  case CallHopPlannerEvent::RemoteAcceptN3:
    return "RemoteAcceptN3";
  case CallHopPlannerEvent::SfuAttachInbound:
    return "SfuAttachInbound";
  case CallHopPlannerEvent::SoftMigrateRequested:
    return "SoftMigrateRequested";
  case CallHopPlannerEvent::AttachSucceeded:
    return "AttachSucceeded";
  case CallHopPlannerEvent::AttachFailed:
    return "AttachFailed";
  case CallHopPlannerEvent::AttachWaitExpired:
    return "AttachWaitExpired";
  case CallHopPlannerEvent::HopRefuse:
    return "HopRefuse";
  case CallHopPlannerEvent::Stop:
    return "Stop";
  }
  return "?";
}

inline CallHopPlannerPhaseOutcome DecideCallHopPlannerPhase(CallHopPlannerPhase phase,
                                                            CallHopPlannerEvent ev,
                                                            const CallHopPlannerApplyContext& ctx) {
  CallHopPlannerPhaseOutcome out;
  out.next = phase;

  switch (ev) {
  case CallHopPlannerEvent::Stop:
    if (phase == CallHopPlannerPhase::Idle) {
      out.decision = CallHopPlannerDecision::Keep;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    out.next = CallHopPlannerPhase::Idle;
    return out;

  case CallHopPlannerEvent::LocalAcceptN3:
  case CallHopPlannerEvent::RemoteAcceptN3:
    if (!ctx.should_arm_hop && !ctx.has_sfu_hint) {
      out.decision = CallHopPlannerDecision::Ignore;
      return out;
    }
    if (ctx.sfu_attached && phase == CallHopPlannerPhase::Live) {
      out.decision = CallHopPlannerDecision::Keep;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    if (ctx.has_sfu_hint) {
      out.next = CallHopPlannerPhase::Attaching;
    } else {
      out.next = CallHopPlannerPhase::WaitingAttach;
    }
    return out;

  case CallHopPlannerEvent::SoftMigrateRequested:
    if (!ctx.should_arm_hop && !ctx.allows_hop_path && phase == CallHopPlannerPhase::Idle) {
      out.decision = CallHopPlannerDecision::Ignore;
      return out;
    }
    if (ctx.soft_migrate_in_flight && phase == CallHopPlannerPhase::Migrating) {
      out.decision = CallHopPlannerDecision::Keep;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    out.next = CallHopPlannerPhase::Migrating;
    return out;

  case CallHopPlannerEvent::SfuAttachInbound:
    if (!ctx.allows_hop_path) {
      out.decision = CallHopPlannerDecision::Ignore;
      return out;
    }
    if (phase == CallHopPlannerPhase::Live && ctx.sfu_attached) {
      out.decision = CallHopPlannerDecision::Keep;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    out.next = CallHopPlannerPhase::Attaching;
    return out;

  case CallHopPlannerEvent::AttachSucceeded:
    if (phase == CallHopPlannerPhase::Idle || phase == CallHopPlannerPhase::Stopping) {
      out.decision = CallHopPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    out.next = CallHopPlannerPhase::Live;
    return out;

  case CallHopPlannerEvent::AttachFailed:
  case CallHopPlannerEvent::AttachWaitExpired:
  case CallHopPlannerEvent::HopRefuse:
    if (phase == CallHopPlannerPhase::Idle) {
      out.decision = CallHopPlannerDecision::Ignore;
      return out;
    }
    out.decision = CallHopPlannerDecision::Transition;
    out.next = CallHopPlannerPhase::Idle;
    return out;
  }

  out.decision = CallHopPlannerDecision::Ignore;
  return out;
}

} // namespace pbr
