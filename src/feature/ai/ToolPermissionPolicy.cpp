#include "feature/ai/ToolPermissionPolicy.h"

namespace pbr {

namespace {

ToolPermissionVerdict ParseDecision(const std::string& decision, ToolPermissionVerdict fallback) {
  if (decision == "allow") {
    return ToolPermissionVerdict::Allow;
  }
  if (decision == "deny") {
    return ToolPermissionVerdict::Deny;
  }
  if (decision == "ask") {
    return ToolPermissionVerdict::Ask;
  }
  return fallback;
}

} // namespace

std::string ToolPermissionPolicy::EffectiveRisk(const ToolMeta& meta) {
  if (meta.risk == "read" || meta.risk == "write" || meta.risk == "destructive") {
    return meta.risk;
  }
  return meta.mutating ? "write" : "read";
}

ToolPermissionEval ToolPermissionPolicy::Evaluate(const std::string& tool_name, const ToolMeta& meta,
                                                  const ToolPermissionsPrefs& prefs,
                                                  const std::unordered_set<std::string>& session_grants) {
  ToolPermissionEval out;
  out.risk = EffectiveRisk(meta);

  if (session_grants.contains(tool_name)) {
    out.verdict = ToolPermissionVerdict::Allow;
    out.source = "session";
    return out;
  }

  if (const auto it = prefs.by_tool.find(tool_name); it != prefs.by_tool.end()) {
    out.verdict = ParseDecision(it->second.decision, ToolPermissionVerdict::Ask);
    out.source = "tool";
    return out;
  }

  if (!meta.provider.empty()) {
    if (const auto it = prefs.by_provider.find(meta.provider); it != prefs.by_provider.end()) {
      out.verdict = ParseDecision(it->second.decision, ToolPermissionVerdict::Ask);
      out.source = "provider";
      return out;
    }
  }

  std::string default_decision = prefs.default_read;
  if (out.risk == "write") {
    default_decision = prefs.default_write;
  } else if (out.risk == "destructive") {
    default_decision = prefs.default_destructive;
  }
  out.verdict = ParseDecision(default_decision, out.risk == "read" ? ToolPermissionVerdict::Allow
                                                                  : ToolPermissionVerdict::Ask);
  out.source = "default";

  // Fail closed for mutating tools with empty/unknown defaults.
  if (out.risk != "read" && out.verdict == ToolPermissionVerdict::Allow && default_decision.empty()) {
    out.verdict = ToolPermissionVerdict::Ask;
    out.source = "mutating_fallback";
  }
  return out;
}

void ToolPermissionPolicy::SetToolDecision(ToolPermissionsPrefs& prefs, const std::string& tool_name,
                                           const std::string& decision) {
  if (tool_name.empty() || !IsValidToolPermissionDecision(decision)) {
    return;
  }
  if (decision == "ask") {
    prefs.by_tool.erase(tool_name);
    return;
  }
  prefs.by_tool[tool_name] = ToolPermissionEntry{.decision = decision};
}

void ToolPermissionPolicy::ClearAllDecisions(ToolPermissionsPrefs& prefs) {
  ClearToolPermissionDecisions(prefs);
}

void ToolPermissionPolicy::ClearToolDecision(ToolPermissionsPrefs& prefs, const std::string& tool_name) {
  prefs.by_tool.erase(tool_name);
}

size_t ToolPermissionPolicy::RememberedDecisionCount(const ToolPermissionsPrefs& prefs) {
  return RememberedToolPermissionCount(prefs);
}

} // namespace pbr
