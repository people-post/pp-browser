#pragma once

#include <string>
#include <unordered_map>

namespace pbr {

/** Persisted trust for agent tools: allow | ask | deny. */
struct ToolPermissionEntry {
  std::string decision = "ask";
};

/**
 * Profile-scoped tool permission preferences (preferences.json → tool_permissions).
 * Resolution: by_tool → by_provider → risk defaults → ask if mutating.
 */
struct ToolPermissionsPrefs {
  static constexpr int kSchemaVersion = 1;

  int schema_version = kSchemaVersion;
  std::string default_read = "allow";
  std::string default_write = "ask";
  std::string default_destructive = "ask";
  std::unordered_map<std::string, ToolPermissionEntry> by_tool;
  std::unordered_map<std::string, ToolPermissionEntry> by_provider;
};

inline bool IsValidToolPermissionDecision(const std::string& decision) {
  return decision == "allow" || decision == "ask" || decision == "deny";
}

inline size_t RememberedToolPermissionCount(const ToolPermissionsPrefs& prefs) {
  return prefs.by_tool.size() + prefs.by_provider.size();
}

inline void ClearToolPermissionDecisions(ToolPermissionsPrefs& prefs) {
  prefs.by_tool.clear();
  prefs.by_provider.clear();
}

} // namespace pbr
