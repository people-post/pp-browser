#include "feature/ai/ToolPermissionPrompt.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <map>
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
  // Each catalog owns its punctuation around the tool names (CJK uses full-width brackets).
  const std::string base = offered_tools.size() == 1 ? "chat.permission.prompt_one" : "chat.permission.prompt_many";
  const std::string prompt = names.empty() ? Tr(base) : Tr(base + "_named", {{"names", names}});

  Object paragraph;
  paragraph.set("type", "paragraph");
  paragraph.set("text", Tr("chat.permission.intro"));

  const std::string allow_once = Tr("chat.permission.allow_once");
  const std::string allow_always = Tr("chat.permission.allow_always");
  const std::string deny = Tr("chat.permission.deny");
  Object choice;
  choice.set("type", "choice");
  choice.set("prompt", prompt);
  choice.set("options",
             ArrayValue({ObjectValue(Option(allow_once, allow_once, DecisionPayload(approval_id, "allow_once"))),
                         ObjectValue(Option(allow_always, allow_always, DecisionPayload(approval_id, "allow_always"))),
                         ObjectValue(Option(deny, deny, DecisionPayload(approval_id, "deny")))}));

  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(paragraph)), ObjectValue(std::move(choice))}));
  return DumpJson(root);
}

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools) {
  const std::string names = DescribeTools(offered_tools);
  const std::string text =
      Tr("chat.permission.denied", {{"names", names.empty() ? Tr("chat.permission.those_actions") : names}});
  Object paragraph;
  paragraph.set("type", "paragraph");
  paragraph.set("text", text);
  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(paragraph))}));
  return DumpJson(root);
}

std::string BuildToolPermissionStaleBlocks(const std::string& reason_code) {
  const std::string text = reason_code.empty() ? Tr("chat.permission.stale")
                                               : Tr("chat.permission.stale_reason", {{"reason", reason_code}});
  Object callout;
  callout.set("type", "callout");
  callout.set("variant", "info");
  callout.set("text", text);
  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(callout))}));
  return DumpJson(root);
}

} // namespace pbr
