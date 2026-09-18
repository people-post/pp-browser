#include "feature/calls/CallDirectArmingPorts.h"

#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/calls/CallLifecycle.h"

namespace pbr {
namespace {

CallMediaStatus CallMediaStatusForDirectPhase(CallDirectPlannerPhase phase) {
  switch (phase) {
  case CallDirectPlannerPhase::Arming:
  case CallDirectPlannerPhase::Connecting:
  case CallDirectPlannerPhase::KeyWait:
    return CallMediaStatus::DirectConnecting;
  case CallDirectPlannerPhase::Live:
    return CallMediaStatus::DirectLive;
  case CallDirectPlannerPhase::DegradedTxOnly:
    return CallMediaStatus::DegradedTxOnly;
  case CallDirectPlannerPhase::Idle:
  case CallDirectPlannerPhase::Stopping:
    return CallMediaStatus::None;
  }
  return CallMediaStatus::None;
}

} // namespace

CallDirectArmingPorts MakeCallDirectArmingPorts(CallLifecycle* lifecycle) {
  CallDirectArmingPorts ports;
  if (!lifecycle) {
    return ports;
  }
  ports.direct_ops_allowed = [lifecycle]() { return lifecycle->AllowsDirectPath(); };
  ports.request_direct_arming = [lifecycle](const std::string& call_id) {
    if (lifecycle->AllowsDirectPath()) {
      return;
    }
    const CallPhase phase = lifecycle->Phase();
    if (phase == CallPhase::Accepting || phase == CallPhase::JoinedLocal ||
        phase == CallPhase::MediaPending || phase == CallPhase::MediaConnecting) {
      lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
    }
  };
  ports.report_progress = [lifecycle](CallDirectPlannerPhase phase, const std::string& call_id) {
    const CallMediaStatus mapped = CallMediaStatusForDirectPhase(phase);
    if (mapped == CallMediaStatus::None) {
      return;
    }
    // Live chrome goes through on_connected → DirectConnected Apply (phase + Status).
    if (mapped == CallMediaStatus::DirectLive) {
      return;
    }
    lifecycle->SetMediaStatus(mapped, call_id);
  };
  ports.on_connected = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::DirectConnected, call_id);
  };
  ports.on_connect_failed = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  };
  ports.on_media_deferred = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaDeferred, call_id);
  };
  ports.on_media_key_ready = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaKeyReady, call_id);
  };
  ports.arming_debug_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  return ports;
}

} // namespace pbr
