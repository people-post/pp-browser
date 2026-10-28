#include "lib/ai/ToolPermissionPrompt.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include "lib/ai/LocalizedLabels.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr size_t kMaxArgumentChars = 40;

// A value as the user should see it: long text is cut (never in the middle of a UTF-8 character).
std::string DescribeArgument(const std::string& text) {
  if (text.size() <= kMaxArgumentChars) {
    return text;
  }
  size_t cut = kMaxArgumentChars;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return text.substr(0, cut) + "\xE2\x80\xA6";
}

/** First non-empty string argument among `keys`. */
std::string FirstString(const Object& arguments, std::vector<const char*> keys) {
  for (const char* key : keys) {
    if (auto value = arguments.getString(key); value && !value->empty()) {
      return *value;
    }
  }
  return {};
}

std::optional<bool> FirstBool(const Object& arguments, std::vector<const char*> keys) {
  for (const char* key : keys) {
    if (auto flag = arguments.getIf<bool>(key)) {
      return *flag;
    }
    if (auto text = arguments.getString(key)) {
      if (*text == "true") {
        return true;
      }
      if (*text == "false") {
        return false;
      }
    }
  }
  return std::nullopt;
}

/** "Contacts only" / "contacts-only" -> "contacts_only": the spelling of the ai.option.* keys. */
std::string OptionToken(const std::string& value) {
  std::string out;
  for (const char c : value) {
    out += (c == ' ' || c == '-') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string LanguageLabel(const std::string& value) {
  if (OptionToken(value) == "system") {
    return Tr("ai.option.language.system");
  }
  std::string key = "settings.language.";
  for (const char c : value) {
    key += c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return TrOrDefault(key, value);
}

/** One tool whose card names the value it will set: argument keys, how the value is worded. */
struct ValueTool {
  const char* tool;
  std::vector<const char*> keys;
  const char* option_group; // ai.option.<group>.<value>; "language" = language label; "" = shown as given
};

const std::vector<ValueTool> kValueTools = {
    {"set_appearance", {"appearance", "theme", "mode"}, "appearance"},
    {"set_language", {"language", "locale", "lang"}, "language"},
    {"set_group_invite_policy", {"policy", "group_invite_policy"}, "policy"},
    {"set_call_invite_policy", {"policy", "call_invite_policy", "who"}, "policy"},
    {"update_profile_nickname", {"nickname"}, ""},
};

/** On / off tools: `<key>.on` / `<key>.off` when the call says which. */
struct SwitchTool {
  const char* tool;
  std::vector<const char*> keys;
};

const std::vector<SwitchTool> kSwitchTools = {
    {"set_notifications", {"enabled", "show_notifications", "notifications"}},
    {"set_auto_renew_registration", {"enabled", "auto_renew_registration"}},
    {"set_reduce_transparency", {"enabled", "reduce_transparency"}},
    {"set_call_diagnostics", {"enabled", "call_diagnostics"}},
    {"set_node_enabled", {"enabled", "node_enabled"}},
};

/** The call in plain words (chat.permission.tool.*); never the raw tool name or its argument names. */
std::string DescribeToolCall(const PlannedToolCall& call) {
  const std::string key = "chat.permission.tool." + call.name;
  const std::string base = TrOrDefault(key, std::string());
  if (base.empty()) {
    return Tr("chat.permission.tool.generic");
  }
  for (const ValueTool& tool : kValueTools) {
    if (call.name != tool.tool) {
      continue;
    }
    const std::string value = FirstString(call.arguments, tool.keys);
    if (value.empty()) {
      return base;
    }
    const std::string group = tool.option_group;
    const std::string shown = group.empty()                ? DescribeArgument(value)
                              : group == "language"        ? LanguageLabel(value)
                                                           : TrOrDefault("ai.option." + group + "." + OptionToken(value),
                                                                         DescribeArgument(value));
    return Tr(key + ".value", {{"value", shown}});
  }
  for (const SwitchTool& tool : kSwitchTools) {
    if (call.name != tool.tool) {
      continue;
    }
    const auto enabled = FirstBool(call.arguments, tool.keys);
    return enabled ? Tr(key + (*enabled ? ".on" : ".off")) : base;
  }
  return base;
}

} // namespace

// What the user approves, in plain words: one phrase per call, no tool names or argument names (PB-015). The text
// lands in a paragraph / choice prompt, which the block parser escapes.
std::string DescribeToolCalls(const std::vector<PlannedToolCall>& tools) {
  std::vector<std::string> phrases;
  for (const PlannedToolCall& call : tools) {
    std::string phrase = DescribeToolCall(call);
    if (std::find(phrases.begin(), phrases.end(), phrase) == phrases.end()) {
      phrases.push_back(std::move(phrase));
    }
  }
  std::string out;
  for (const std::string& phrase : phrases) {
    out += (out.empty() ? "" : Tr("ai.card.list_separator")) + phrase;
  }
  return out;
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

std::optional<std::string> RewriteToolPermissionCardBlocks(const std::string& blocks_json,
                                                           const ApprovalOutcome outcome) {
  if (outcome == ApprovalOutcome::Stale) {
    return BuildToolPermissionStaleBlocks("");
  }
  auto root = TryParseObject(blocks_json);
  const Array* blocks = root ? root->getArray("blocks") : nullptr;
  if (!blocks) {
    return std::nullopt;
  }
  // Keep the explanation and the question (it names the action); the buttons give way to one line saying what
  // the user chose.
  std::vector<Value> kept;
  for (const Value& block : blocks->elements) {
    const Object* object = asObject(block);
    if (object && object->getString("type").value_or("") == "choice") {
      if (const auto prompt = object->getString("prompt"); prompt && !prompt->empty()) {
        Object question;
        question.set("type", "paragraph");
        question.set("text", *prompt);
        kept.push_back(ObjectValue(std::move(question)));
      }
      continue;
    }
    kept.push_back(block);
  }
  const char* key = outcome == ApprovalOutcome::AllowOnce     ? "chat.permission.done.allow_once"
                    : outcome == ApprovalOutcome::AllowAlways ? "chat.permission.done.allow_always"
                                                              : "chat.permission.done.deny";
  Object line;
  line.set("type", "paragraph");
  line.set("text", Tr(key));
  kept.push_back(ObjectValue(std::move(line)));
  Object out;
  out.set("blocks", ArrayValue(std::move(kept)));
  return DumpJson(out);
}

} // namespace pbr
