#include "feature/ai/ToolPermissionPrompt.h"

#include "common/ValueJson.h"

#include <sstream>
#include "common/PbrCompat.h"

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

Object Option(const std::string& label, const std::string& message, Object payload) {
  Object option;
  option.set("label", label);
  option.set("message", message);
  option.set("payload", std::move(payload));
  return option;
}

Object DecisionPayload(const std::string& approval_id, const std::string& decision) {
  Object payload;
  payload.set("type", "tool_permission");
  payload.set("approval_id", approval_id);
  payload.set("decision", decision);
  return payload;
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

  Object paragraph;
  paragraph.set("type", "paragraph");
  paragraph.set("text", "I need your permission before changing contacts, chats, or identity settings.");

  Object choice;
  choice.set("type", "choice");
  choice.set("prompt", prompt);
  choice.set("options",
             ArrayValue({ObjectValue(Option("Allow once", "Allow once", DecisionPayload(approval_id, "allow_once"))),
                         ObjectValue(Option("Always allow", "Always allow",
                                            DecisionPayload(approval_id, "allow_always"))),
                         ObjectValue(Option("Deny", "Deny", DecisionPayload(approval_id, "deny")))}));

  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(paragraph)), ObjectValue(std::move(choice))}));
  return DumpJson(root);
}

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools) {
  const std::string names = DescribeTools(offered_tools);
  std::string text = "Okay — I won't run ";
  text += names.empty() ? "those actions" : names;
  text += ".";
  Object paragraph;
  paragraph.set("type", "paragraph");
  paragraph.set("text", text);
  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(paragraph))}));
  return DumpJson(root);
}

std::string BuildToolPermissionStaleBlocks(const std::string& reason_code) {
  std::string text = "That permission prompt is no longer active";
  if (!reason_code.empty()) {
    text += " (" + reason_code + ")";
  }
  text += ". Ask again if you still want this.";
  Object callout;
  callout.set("type", "callout");
  callout.set("variant", "info");
  callout.set("text", text);
  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(callout))}));
  return DumpJson(root);
}

} // namespace pbr
