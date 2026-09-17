#include "feature/calls/CallMediaSeatPorts.h"

#include "feature/calls/CallMediaPaths.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallTopologyController.h"

namespace pbr {

CallMediaSeatPorts MakeCallMediaSeatPorts(CallTopologyController* topology, CallMediaSeat* seat) {
  CallMediaSeatPorts ports;
  if (!seat) {
    return ports;
  }
  ports.release = [seat](const std::string& call_id) { seat->Release(call_id); };
  ports.bind_hop_for_attach = [topology, seat](const std::string& call_id) {
    if (!topology || call_id.empty()) {
      return;
    }
    (void)CallHopPath(topology, seat).BindForAttach(call_id);
  };
  return ports;
}

} // namespace pbr
