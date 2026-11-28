#include "feature/settings/ReachabilityNudge.h"

namespace pbr {

int ReachabilityNudgeSeverity(const std::string& status_key) {
  if (status_key == kReachabilityNudgeOutboundOnly) {
    return 1;
  }
  if (status_key == kReachabilityNudgeBlocked) {
    return 2;
  }
  return 0;
}

} // namespace pbr
