#include "feature/messaging/AgentUiPorts.h"

#include "domain/ai/conversation/Conversation.h"
#include "common/thread/IThreadStore.h"

namespace pbr {

AgentUiPorts MakeAgentUiPorts(AgentSession& agent) {
  AgentUiPorts ports;
  ports.snapshot = [&agent]() {
    AgentView view;
    view.configured = agent.IsConfigured();
    return view;
  };
  ports.has_session = [] { return true; };
  ports.configure = [&agent](const AppConfig& config) { agent.Configure(config); };
  ports.set_tool_registration_hook = [&agent](ToolRegistrationHook hook) {
    agent.SetToolRegistrationHook(std::move(hook));
  };
  ports.set_tool_permissions = [&agent](const ToolPermissionsPrefs& permissions) {
    agent.SetToolPermissions(permissions);
  };
  ports.set_tool_permissions_saver = [&agent](ToolPermissionsSaveFn saver) {
    agent.SetToolPermissionsSaver(std::move(saver));
  };
  ports.resume_tool_permission = [&agent](const std::string& approval_id, const std::string& decision,
                                          const std::string& decision_label) {
    return agent.ResumeToolPermission(approval_id, decision, decision_label);
  };
  ports.set_thread_store = [&agent](IThreadStore* store) { agent.SetThreadStore(store); };
  ports.submit = [&agent](const std::string& text, std::optional<std::string> user_payload) {
    agent.Submit(text, std::move(user_payload));
  };
  ports.cancel = [&agent]() { agent.Cancel(); };
  ports.poll_events = [&agent](std::vector<AgentEvent>& out) { agent.PollEvents(out); };
  ports.wait_for_configure_idle = [&agent]() { agent.WaitForConfigureIdle(); };
  ports.with_session = [&agent](std::function<void(AgentSession&)> callback) { callback(agent); };
  ports.has_conversation_entries = [&agent]() { return !agent.conversation().Entries().empty(); };
  ports.last_conversation_entry_id = [&agent]() -> std::optional<std::string> {
    const auto& entries = agent.conversation().Entries();
    if (entries.empty()) {
      return std::nullopt;
    }
    return entries.back().id;
  };
  ports.complete_assistant_message = [&agent](const std::string& entry_id, const std::string& message) {
    (void)agent.CompleteAssistantMessage(entry_id, message);
  };
  ports.set_assistant_display_plain = [&agent](const std::string& entry_id, const std::string& message) {
    (void)agent.SetAssistantDisplay(entry_id, message, {});
  };
  return ports;
}

} // namespace pbr
