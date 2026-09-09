#pragma once

#include "foundation/data/Config.h"
#include "feature/ai/AgentSession.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IThreadStore;

/** Read-only agent turn chrome for chat UI binding. */
struct AgentView {
  bool configured = false;
  /**
   * Configured and able to run a cloud/mock turn from the agent service's point of view.
   * Chat may still require a Brief guest/registered key — see ChatController::AgentCloudReady().
   */
  bool cloud_ready = false;
  bool turn_active = false;
  std::string visible_tool_name;
  std::string status_hint;
};

/**
 * Agent read snapshot + imperative ops for ChatController.
 * Application fills from AgentSession. Clear via BindAgentPorts({}).
 */
struct AgentUiPorts {
  std::function<AgentView()> snapshot;
  std::function<bool()> has_session;
  std::function<void(const AppConfig& config)> configure;
  std::function<void(ToolRegistrationHook hook)> set_tool_registration_hook;
  std::function<void(const ToolPermissionsPrefs& permissions)> set_tool_permissions;
  std::function<void(ToolPermissionsSaveFn saver)> set_tool_permissions_saver;
  std::function<Roe<void>(const std::string& approval_id, const std::string& decision,
                          const std::string& decision_label)>
      resume_tool_permission;
  std::function<void(IThreadStore* store)> set_thread_store;
  std::function<void(const std::string& text, std::optional<std::string> user_payload)> submit;
  std::function<void()> cancel;
  std::function<void(std::vector<AgentEvent>& out)> poll_events;
  std::function<void()> wait_for_configure_idle;
  std::function<bool()> has_conversation_entries;
  std::function<std::optional<std::string>()> last_conversation_entry_id;
  std::function<void(const std::string& entry_id, const std::string& message)> complete_assistant_message;
  std::function<void(const std::string& entry_id, const std::string& message)> set_assistant_display_plain;
};

AgentUiPorts MakeAgentUiPorts(AgentSession& agent);

} // namespace pbr
