#pragma once

#include <string>

namespace pbr {

/** Machine keys stored in `preferences.json` → `reachability_nudge_acked_status`. */
constexpr const char* kReachabilityNudgeOutboundOnly = "outbound_only";
constexpr const char* kReachabilityNudgeBlocked = "blocked";

/** Severity for condition-keyed ack: higher means worse inbound (0 = no nudge). */
int ReachabilityNudgeSeverity(const std::string& status_key);

} // namespace pbr
