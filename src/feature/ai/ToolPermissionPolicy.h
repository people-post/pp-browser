#pragma once

#include "foundation/data/ToolPermissions.h"
#include "domain/ai/ToolRegistry.h"

#include <string>
#include <unordered_set>

namespace pbr {

enum class ToolPermissionVerdict {
  Allow,
  Ask,
  Deny,
};

struct ToolPermissionEval {
  ToolPermissionVerdict verdict = ToolPermissionVerdict::Ask;
  std::string risk = "read";
  /** Where the decision came from: tool | provider | default | session | mutating_fallback */
  std::string source;
};

class ToolPermissionPolicy {
public:
  static std::string EffectiveRisk(const ToolMeta& meta);

  static ToolPermissionEval Evaluate(const std::string& tool_name, const ToolMeta& meta,
                                     const ToolPermissionsPrefs& prefs,
                                     const std::unordered_set<std::string>& session_grants = {});

  /** Persist Always allow / Never for a tool name. */
  static void SetToolDecision(ToolPermissionsPrefs& prefs, const std::string& tool_name,
                              const std::string& decision);

  static void ClearAllDecisions(ToolPermissionsPrefs& prefs);
  static void ClearToolDecision(ToolPermissionsPrefs& prefs, const std::string& tool_name);

  static size_t RememberedDecisionCount(const ToolPermissionsPrefs& prefs);
};

} // namespace pbr
