#pragma once

#include "feature/settings/SettingsPortsViews.h"

#include <string>

namespace pbr {

/** Machine keys stored in `preferences.json` → `reachability_nudge_acked_status`. */
constexpr const char* kReachabilityNudgeOutboundOnly = "outbound_only";
constexpr const char* kReachabilityNudgeBlocked = "blocked";

/** Severity for condition-keyed ack: higher means worse inbound (0 = no nudge). */
int ReachabilityNudgeSeverity(const std::string& status_key);

/** Status key for prefs / comparison (`outbound_only`, `blocked`, …). */
std::string ReachabilityNudgeStatusKey(SettingsReachabilityView::Status status);

/**
 * True when Me / Network attention should show: Node participation is on and current
 * status is worse than the last acked status (empty ack → any outbound-only/blocked).
 */
bool ReachabilityNudgeActive(bool node_participation, const std::string& status_key,
                             const std::string& acked_status_key);

} // namespace pbr
