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

std::string ReachabilityNudgeStatusKey(SettingsReachabilityView::Status status) {
  switch (status) {
  case SettingsReachabilityView::Status::OutboundOnly:
    return kReachabilityNudgeOutboundOnly;
  case SettingsReachabilityView::Status::Blocked:
    return kReachabilityNudgeBlocked;
  case SettingsReachabilityView::Status::Unknown:
  case SettingsReachabilityView::Status::Checking:
  case SettingsReachabilityView::Status::Reachable:
    break;
  }
  return {};
}

bool ReachabilityNudgeActive(bool node_participation, const std::string& status_key,
                             const std::string& acked_status_key) {
  if (!node_participation) {
    return false;
  }
  const int severity = ReachabilityNudgeSeverity(status_key);
  if (severity <= 0) {
    return false;
  }
  return severity > ReachabilityNudgeSeverity(acked_status_key);
}

} // namespace pbr
