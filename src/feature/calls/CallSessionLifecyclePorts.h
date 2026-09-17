#pragma once

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallLifecycle;

/**
 * Stack-filled Lifecycle façade for CallSessionManager (V043).
 * CSM must not hold CallLifecycle* — ops copy these functions.
 */
struct CallSessionLifecyclePorts {
  std::function<bool()> allows_direct_path;
  std::function<const char*()> status_name;
  std::function<const char*()> armed_planner_name;
  std::function<void(const std::string& call_id)> set_direct_connecting;
  std::function<std::string()> accepting_call_id;
  std::function<std::string()> active_call_id;
  std::function<void(const std::string& call_id)> apply_remote_ended;
  std::function<bool()> is_outbound_calling;

  bool IsBound() const { return static_cast<bool>(allows_direct_path); }
};

/** Null lifecycle → empty ports. */
CallSessionLifecyclePorts MakeCallSessionLifecyclePorts(CallLifecycle* lifecycle);

} // namespace pbr
