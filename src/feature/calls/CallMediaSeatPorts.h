#pragma once

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaSeat;
class CallTopologyController;

/**
 * Stack-filled MediaSeat façade for CallSessionManager (V043).
 * CSM must not hold CallMediaSeat* — ops copy these functions.
 */
struct CallMediaSeatPorts {
  std::function<void(const std::string& call_id)> release;
  std::function<void(const std::string& call_id)> bind_hop_for_attach;

  bool IsBound() const { return static_cast<bool>(release); }
};

/**
 * Build ports over topology + seat. Null seat → empty ports.
 * Topology may be null only when seat is null (bind_hop unused).
 */
CallMediaSeatPorts MakeCallMediaSeatPorts(CallTopologyController* topology, CallMediaSeat* seat);

} // namespace pbr
