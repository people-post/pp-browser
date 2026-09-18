#include "feature/calls/CallTopologyLifecyclePorts.h"

#include "feature/calls/CallLifecycle.h"

namespace pbr {

CallTopologyLifecyclePorts MakeCallTopologyLifecyclePorts(CallLifecycle* lifecycle) {
  CallTopologyLifecyclePorts ports;
  if (!lifecycle) {
    return ports;
  }
  ports.allows_hop_path = [lifecycle]() { return lifecycle->AllowsHopPath(); };
  ports.status = [lifecycle]() { return lifecycle->Status(); };
  ports.status_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  ports.set_media_status = [lifecycle](CallMediaStatus status, const std::string& call_id) {
    lifecycle->SetMediaStatus(status, call_id);
  };
  ports.media_cancel_gen = [lifecycle]() { return lifecycle->MediaCancelGen(); };
  return ports;
}

} // namespace pbr
