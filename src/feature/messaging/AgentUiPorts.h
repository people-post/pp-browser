#pragma once

#include <functional>
#include <string>

namespace pbr {

/** Read-only agent turn chrome for chat UI binding (Phase 7 partial). */
struct AgentView {
  bool configured = false;
  bool turn_active = false;
  std::string visible_tool_name;
  std::string status_hint;
};

/**
 * Agent read snapshot for chat presenter. Application fills from AgentSession.
 * Imperative ops remain on AgentSession until AgentFacade (Phase 7).
 */
struct AgentUiPorts {
  std::function<AgentView()> snapshot;
};

} // namespace pbr
