#include "domain/ai/ToolPermissionPrompt.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <map>
#include <sstream>
#include <variant>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr size_t kMaxArgumentChars = 40;

// One argument as the user should see it: scalars as text (long strings cut), containers as a marker.
std::string DescribeArgument(const Value& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    if (text->size() <= kMaxArgumentChars) {
      return *text;
    }
    size_t cut = kMaxArgumentChars;
    while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80) {
      --cut; // do not cut a UTF-8 character in half
    }
    return text->substr(0, cut) + "\xE2\x80\xA6";
  }
  if (const auto* i = std::get_if<int64_t>(&value)) {
    return std::to_string(*i);
  }
  if (const auto* u = std::get_if<uint64_t>(&value)) {
    return std::to_string(*u);
  }
  if (const auto* b = std::get_if<bool>(&value)) {
    return *b ? "true" : "false";
  }
  if (const auto* d = std::get_if<double>(&value)) {
    std::ostringstream out;
    out << *d;
    return out.str();
  }
  if (asObject(value)) {
    return "{\xE2\x80\xA6}";
  }
  if (asArray(value)) {
    return "[\xE2\x80\xA6]";
  }
  return "null";
}

} // namespace

// "name(key=value, …)": the user approves a concrete call, not just a tool name (PB-015). The text lands in a
// paragraph / choice prompt, which the block parser escapes.
std::string DescribeToolCalls(const std::vector<PlannedToolCall>& tools) {
  std::ostringstream out;
  for (size_t i = 0; i < tools.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << tools[i].name;
    if (tools[i].arguments.fields().size() == 0) {
      continue;
    }
    out << "(";
    bool first = true;
    for (const auto& [key, value] : tools[i].arguments.fields()) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << key << "=" << DescribeArgument(value);
    }
    out << ")";
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


std::string BuildToolPermissionChoiceBlocks(const std::string& approval_id,
                                            const std::vector<PlannedToolCall>& offered_tools,
                                            const bool offer_allow_always) {
  const std::string names = DescribeToolCalls(offered_tools);
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
  std::vector<Value> options;
  options.push_back(ObjectValue(Option(allow_once, allow_once, DecisionPayload(approval_id, "allow_once"))));
  if (offer_allow_always) {
    options.push_back(ObjectValue(Option(allow_always, allow_always, DecisionPayload(approval_id, "allow_always"))));
  }
  options.push_back(ObjectValue(Option(deny, deny, DecisionPayload(approval_id, "deny"))));
  choice.set("options", ArrayValue(std::move(options)));

  Object root;
  root.set("blocks", ArrayValue({ObjectValue(std::move(paragraph)), ObjectValue(std::move(choice))}));
  return DumpJson(root);
}

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools) {
  const std::string names = DescribeToolCalls(offered_tools);
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
