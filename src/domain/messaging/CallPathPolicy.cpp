#include "domain/messaging/CallPathPolicy.h"

#include <algorithm>

namespace pbr {

CallPathPolicy DecideCallPathPolicy(const MobilityClass local, const MobilityClass remote) {
  CallPathPolicy policy;
  const bool any_mobile = local == MobilityClass::Mobile || remote == MobilityClass::Mobile;
  const bool any_unknown = local == MobilityClass::Unknown || remote == MobilityClass::Unknown;
  if (any_mobile) {
    // A mobile end rebinds: a punched path dies with the next NAT mapping. The relay carries the
    // call; direct dialing still goes first (a reachable peer needs no punch).
    policy.punch_at_start = false;
    policy.upgrade_to_direct = false;
    policy.relay_role = CallRelayRole::Anchor;
    policy.standby_priority = CallStandbyPriority::High;
    return policy;
  }
  // Stationary / Unknown pairs punch (Unknown as Stationary); Unknown keeps a high standby priority.
  policy.standby_priority = any_unknown ? CallStandbyPriority::High : CallStandbyPriority::Low;
  return policy;
}

CallStandbyPriority StandbyPriorityFor(const CallPathPolicy& policy, const bool punched) {
  if (!punched) {
    return policy.standby_priority;
  }
  return std::max(policy.standby_priority, CallStandbyPriority::Medium);
}

const char* CallRelayRoleName(const CallRelayRole role) {
  return role == CallRelayRole::Anchor ? "anchor" : "standby";
}

const char* CallStandbyPriorityName(const CallStandbyPriority priority) {
  switch (priority) {
  case CallStandbyPriority::High:
    return "high";
  case CallStandbyPriority::Medium:
    return "medium";
  case CallStandbyPriority::Low:
    break;
  }
  return "low";
}

CallPathPolicy RelayOnlyPolicy(CallPathPolicy policy) {
  policy.relay_only = true;
  policy.punch_at_start = false;
  policy.upgrade_to_direct = false;
  policy.relay_role = CallRelayRole::Anchor;
  policy.want_relay_standby = false;  // the relay is the primary, not a standby
  return policy;
}

} // namespace pbr
