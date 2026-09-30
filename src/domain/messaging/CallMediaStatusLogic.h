#pragma once

#include "domain/messaging/CallDirectPlannerLogic.h"
#include "domain/messaging/CallHopPlannerLogic.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include <optional>

namespace pbr {

/**
 * Pure rules between the path planners and the call's media Status (V037 "State + Status"): what a
 * planner's progress shows as, and when a planner may arm. LiveCalls applies them.
 */

/** The media Status a group (hop) planner phase shows as; nullopt: no change (idle / stopping). */
inline std::optional<CallMediaStatus> MediaStatusForHopProgress(const CallHopPlannerPhase phase) {
  switch (phase) {
  case CallHopPlannerPhase::WaitingAttach:
    return CallMediaStatus::HopWaiting;
  case CallHopPlannerPhase::Attaching:
    return CallMediaStatus::HopAttaching;
  case CallHopPlannerPhase::Live:
    return CallMediaStatus::HopLive;
  case CallHopPlannerPhase::Migrating:
    return CallMediaStatus::Migrating;
  case CallHopPlannerPhase::Idle:
  case CallHopPlannerPhase::Stopping:
    return std::nullopt;
  }
  return std::nullopt;
}

/**
 * The media Status a 1:1 (direct) planner phase shows as; nullopt: no change. Live is not reported
 * here — DirectConnected (a lifecycle event) moves the call to DirectLive with its phase.
 */
inline std::optional<CallMediaStatus> MediaStatusForDirectProgress(const CallDirectPlannerPhase phase) {
  switch (phase) {
  case CallDirectPlannerPhase::Arming:
  case CallDirectPlannerPhase::Connecting:
  case CallDirectPlannerPhase::KeyWait:
    return CallMediaStatus::DirectConnecting;
  case CallDirectPlannerPhase::DegradedTxOnly:
    return CallMediaStatus::DegradedTxOnly;
  case CallDirectPlannerPhase::Reconnecting:
    return CallMediaStatus::Reconnecting;
  case CallDirectPlannerPhase::Live:
  case CallDirectPlannerPhase::Idle:
  case CallDirectPlannerPhase::Stopping:
    return std::nullopt;
  }
  return std::nullopt;
}

/** A SoftMigrate to the group path may arm from a 1:1 (or not yet decided) Status only. */
inline bool SoftMigrateMayArm(const CallMediaStatus status) {
  return status == CallMediaStatus::DirectLive || status == CallMediaStatus::DirectConnecting ||
         status == CallMediaStatus::DegradedTxOnly || status == CallMediaStatus::Deciding ||
         status == CallMediaStatus::None;
}

/**
 * The 1:1 planner asks to arm while the Status does not allow the direct path yet: honoured only in
 * the phases where a call is being set up (accept / joined / key or media pending).
 */
inline bool ShouldHonorDirectArmingRequest(const CallPhase phase) {
  return phase == CallPhase::Accepting || phase == CallPhase::JoinedLocal || phase == CallPhase::MediaPending ||
         phase == CallPhase::MediaConnecting;
}

} // namespace pbr
