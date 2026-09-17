#include "feature/calls/CallSessionLifecyclePorts.h"

#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/calls/CallLifecycle.h"

namespace pbr {

CallSessionLifecyclePorts MakeCallSessionLifecyclePorts(CallLifecycle* lifecycle) {
  CallSessionLifecyclePorts ports;
  if (!lifecycle) {
    return ports;
  }
  ports.allows_direct_path = [lifecycle]() { return lifecycle->AllowsDirectPath(); };
  ports.status_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  ports.armed_planner_name = [lifecycle]() {
    return CallArmedPlannerName(lifecycle->ArmedPlanner());
  };
  ports.set_direct_connecting = [lifecycle](const std::string& call_id) {
    lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  };
  ports.accepting_call_id = [lifecycle]() { return lifecycle->AcceptingCallId(); };
  ports.active_call_id = [lifecycle]() { return lifecycle->ActiveCallId(); };
  ports.apply_remote_ended = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::RemoteEnded, call_id);
  };
  ports.is_outbound_calling = [lifecycle]() {
    return lifecycle->Phase() == CallPhase::OutboundCalling;
  };
  return ports;
}

} // namespace pbr
