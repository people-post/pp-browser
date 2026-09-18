#pragma once

#include "domain/messaging/CallLifecycleTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallLifecycle;

/**
 * Stack-filled Lifecycle façade for CallTopologyController (V046).
 * Topology must not hold CallLifecycle* — ops copy these functions.
 */
struct CallTopologyLifecyclePorts {
  std::function<bool()> allows_hop_path;
  std::function<CallMediaStatus()> status;
  std::function<const char*()> status_name;
  std::function<void(CallMediaStatus status, const std::string& call_id)> set_media_status;
  std::function<uint64_t()> media_cancel_gen;

  bool IsBound() const { return static_cast<bool>(allows_hop_path); }
};

/** Null lifecycle → empty ports (permissive like former null pointer). */
CallTopologyLifecyclePorts MakeCallTopologyLifecyclePorts(CallLifecycle* lifecycle);

} // namespace pbr
