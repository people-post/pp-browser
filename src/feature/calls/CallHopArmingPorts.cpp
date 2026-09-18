#include "feature/calls/CallHopArmingPorts.h"

#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/calls/CallLifecycle.h"

namespace pbr {
namespace {

CallMediaStatus CallMediaStatusForHopPhase(CallHopPlannerPhase phase) {
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
    return CallMediaStatus::None;
  }
  return CallMediaStatus::None;
}

bool SoftMigrateMayArmFromStatus(CallMediaStatus st) {
  return st == CallMediaStatus::DirectLive || st == CallMediaStatus::DirectConnecting ||
         st == CallMediaStatus::DegradedTxOnly || st == CallMediaStatus::Deciding ||
         st == CallMediaStatus::None;
}

} // namespace

CallHopArmingPorts MakeCallHopArmingPorts(CallLifecycle* lifecycle) {
  CallHopArmingPorts ports;
  if (!lifecycle) {
    return ports;
  }
  ports.hop_ops_allowed = [lifecycle]() { return lifecycle->AllowsHopPath(); };
  ports.soft_migrate_may_arm = [lifecycle]() {
    return SoftMigrateMayArmFromStatus(lifecycle->Status());
  };
  ports.media_cancel_gen = [lifecycle]() { return lifecycle->MediaCancelGen(); };
  ports.report_progress = [lifecycle](CallHopPlannerPhase phase, const std::string& call_id) {
    const CallMediaStatus mapped = CallMediaStatusForHopPhase(phase);
    if (mapped == CallMediaStatus::None &&
        (phase == CallHopPlannerPhase::Idle || phase == CallHopPlannerPhase::Stopping)) {
      // Topology Stop / Idle must not clear product Status via None — Leave owns that.
      return;
    }
    lifecycle->SetMediaStatus(mapped, call_id);
  };
  ports.arming_debug_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  return ports;
}

} // namespace pbr
