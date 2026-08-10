#include "feature/ai/ToolPermissionPrompt.h"

#include <nlohmann/json.hpp>
#include <sstream>

namespace pbr {

namespace {

std::string DescribeTools(const std::vector<PlannedToolCall>& tools) {
  std::ostringstream out;
  for (size_t i = 0; i < tools.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << tools[i].name;
  }
  return out.str();
}

nlohmann::json Option(const std::string& label, const std::string& message, const nlohmann::json& payload) {
  return nlohmann::json{{"label", label}, {"message", message}, {"payload", payload}};
}

nlohmann::json DecisionPayload(const std::string& approval_id, const std::string& decision) {
  return nlohmann::json{{"type", "tool_permission"}, {"approval_id", approval_id}, {"decision", decision}};
}

} // namespace

std::string BuildToolPermissionChoiceBlocks(const std::string& approval_id,
                                            const std::vector<PlannedToolCall>& offered_tools) {
  const std::string names = DescribeTools(offered_tools);
  std::string prompt = "Allow the assistant to run ";
  prompt += offered_tools.size() == 1 ? "this action" : "these actions";
  prompt += " that can change your data";
  if (!names.empty()) {
    prompt += " (" + names + ")";
  }
  prompt += "?";

  nlohmann::json blocks = nlohmann::json::array();
  blocks.push_back({{"type", "paragraph"},
                    {"text", "I need your permission before changing contacts, chats, or identity settings."}});
  blocks.push_back({{"type", "choice"},
                    {"prompt", prompt},
                    {"options", nlohmann::json::array({
                                    Option("Allow once", "Allow once", DecisionPayload(approval_id, "allow_once")),
                                    Option("Always allow", "Always allow",
                                           DecisionPayload(approval_id, "allow_always")),
                                    Option("Deny", "Deny", DecisionPayload(approval_id, "deny")),
                                })}});

  return nlohmann::json{{"blocks", blocks}}.dump();
}

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools) {
  const std::string names = DescribeTools(offered_tools);
  std::string text = "Okay — I won't run ";
  text += names.empty() ? "those actions" : names;
  text += ".";
  return nlohmann::json{{"blocks", nlohmann::json::array({{{"type", "paragraph"}, {"text", text}}})}}.dump();
}

std::string BuildToolPermissionStaleBlocks(const std::string& reason_code) {
  std::string text = "That permission prompt is no longer active";
  if (!reason_code.empty()) {
    text += " (" + reason_code + ")";
  }
  text += ". Ask again if you still want this.";
  return nlohmann::json{{"blocks", nlohmann::json::array(
                                       {{{"type", "callout"}, {"variant", "info"}, {"text", text}}})}}
      .dump();
}

} // namespace pbr
