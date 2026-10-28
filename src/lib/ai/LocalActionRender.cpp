#include "lib/ai/LocalActionRender.h"

#include "common/CivilTime.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/LocalActionWords.h"
#include "lib/ai/LocalizedLabels.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <string_view>
#include <variant>

namespace pbr {

namespace {

using Args = std::map<std::string, std::string>;

constexpr std::string_view kReaders[] = {"get_profile_identity", "get_preferences",   "list_locales",
                                         "get_reachability",     "get_security_status", "get_network_settings",
                                         "get_llm_settings",     "get_integrations"};

/** Boolean writers and the result key that carries the new value. */
struct BoolWriter {
  std::string_view tool;
  std::string_view key;
};
constexpr BoolWriter kBoolWriters[] = {{"set_notifications", "show_notifications"},
                                       {"set_auto_renew_registration", "auto_renew_registration"},
                                       {"set_reduce_transparency", "reduce_transparency"},
                                       {"set_call_diagnostics", "call_diagnostics"},
                                       {"set_node_enabled", "node_enabled"}};

bool IsReader(const std::string& tool) {
  return std::find(std::begin(kReaders), std::end(kReaders), tool) != std::end(kReaders);
}

std::string OnOff(const bool on) {
  return Tr(on ? "ai.card.on" : "ai.card.off");
}

/** A string value that names an option: the option's label when the catalog has one, else the value itself. */
std::string OptionText(const std::string& group, const std::string& value) {
  return TrOrDefault("ai.option." + group + "." + value, value);
}

std::string LanguageText(const std::string& pref) {
  if (pref == "system") {
    return Tr("ai.option.language.system");
  }
  std::string key = "settings.language.";
  for (const char c : pref) {
    key += c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return TrOrDefault(key, pref);
}

std::string StringResult(const Object& result, const std::string& key, const Object& args = {}) {
  if (auto v = result.getString(key); v && !v->empty()) {
    return *v;
  }
  return args.getString(key).value_or("");
}

/** Which option group a string key of a result belongs to ("" = shown as is). */
std::string OptionGroup(const std::string& key) {
  if (key == "group_invite_policy" || key == "call_invite_policy") {
    return "policy";
  }
  static constexpr std::string_view kGroups[] = {"appearance", "status", "pin_status", "direct_connections",
                                                 "preset",     "api_key", "kind"};
  for (const std::string_view g : kGroups) {
    if (key == g) {
      return std::string(g);
    }
  }
  return {};
}

std::string ScalarText(const std::string& key, const Value& value) {
  if (const bool* b = std::get_if<bool>(&value)) {
    return OnOff(*b);
  }
  if (const auto* s = std::get_if<std::string>(&value)) {
    if (s->empty()) {
      return Tr("ai.card.empty_value");
    }
    if (key == "language" || key == "current") {
      return LanguageText(*s);
    }
    const std::string group = OptionGroup(key);
    return group.empty() ? *s : OptionText(group, *s);
  }
  if (const int64_t* i = std::get_if<int64_t>(&value)) {
    return std::to_string(*i);
  }
  if (const uint64_t* u = std::get_if<uint64_t>(&value)) {
    return std::to_string(*u);
  }
  if (const double* d = std::get_if<double>(&value)) {
    return std::to_string(*d);
  }
  return Tr("ai.card.empty_value");
}

std::string ElementText(const Value& element) {
  if (const auto* s = std::get_if<std::string>(&element)) {
    return *s;
  }
  if (const Object* o = asObject(element)) {
    if (auto label = o->getString("label"); label && !label->empty()) {
      return *label;
    }
    std::string id = o->getString("id").value_or("");
    if (const auto enabled = o->getIf<bool>("enabled")) {
      id += " (" + OnOff(*enabled) + ")";
    }
    return id;
  }
  return ScalarText(std::string(), element);
}

std::string ValueText(const std::string& key, const Value& value) {
  if (const Array* array = asArray(value)) {
    if (array->elements.empty()) {
      return Tr("ai.card.empty_value");
    }
    std::string out;
    for (const Value& element : array->elements) {
      if (!out.empty()) {
        out += Tr("ai.card.list_separator");
      }
      out += ElementText(element);
    }
    return out;
  }
  return ScalarText(key, value);
}

std::string RenderCard(const std::string& tool, const Object& result) {
  std::string out;
  for (const auto& [key, value] : result.fields()) {
    const std::string label_key = "ai.card." + tool + "." + key;
    const std::string label = Tr(label_key);
    if (!out.empty()) {
      out += "\n";
    }
    if (label == label_key) {
      out += "??" + key + "??"; // a result key nobody labelled: the render test fails on this
      continue;
    }
    out += Tr("ai.card.line", Args{{"label", label}, {"value", ValueText(key, value)}});
  }
  return out;
}

std::string RenderSentence(const std::string& tool, const Object& result, const LocalRenderContext& ctx) {
  if (tool == "open_conversation" || tool == "start_conversation") {
    if (ctx.name.empty()) {
      return Tr("ai.done.generic");
    }
    return Tr("ai.done." + tool, Args{{"name", ctx.name}});
  }
  if (tool == "update_profile_nickname") {
    return Tr("ai.done.update_profile_nickname", Args{{"nickname", StringResult(result, "nickname", ctx.args)}});
  }
  if (tool == "set_appearance") {
    return Tr("ai.done.set_appearance",
              Args{{"appearance", OptionText("appearance", StringResult(result, "appearance", ctx.args))}});
  }
  if (tool == "set_language") {
    return Tr("ai.done.set_language", Args{{"language", LanguageText(StringResult(result, "language", ctx.args))}});
  }
  if (tool == "set_group_invite_policy" || tool == "set_call_invite_policy") {
    const std::string key = tool == "set_group_invite_policy" ? "group_invite_policy" : "call_invite_policy";
    return Tr("ai.done." + tool, Args{{"policy", OptionText("policy", StringResult(result, key, ctx.args))}});
  }
  if (tool == "probe_reachability") {
    return Tr("ai.done.probe_reachability", Args{{"status", OptionText("status", StringResult(result, "status"))}});
  }
  for (const BoolWriter& w : kBoolWriters) {
    if (tool == w.tool) {
      const bool on = result.getIf<bool>(std::string(w.key)).value_or(false);
      return Tr("ai.done." + tool + (on ? ".on" : ".off"));
    }
  }
  if (tool == "register_user" || tool == "set_mesh_capabilities" || tool == "reset_tool_permissions") {
    return Tr("ai.done." + tool);
  }
  return Tr("ai.done.generic");
}

// --- people-list rows -----------------------------------------------------------------------------------

std::string DateText(const int64_t ms) {
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  if (!pp::civil_time::LocalTime(seconds, &tm)) {
    return {};
  }
  char buf[16];
  if (std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm) == 0) {
    return {};
  }
  return buf;
}

Object LocalActionPayload(const std::string& tool) {
  Object payload;
  payload.set("type", "local_action");
  payload.set("tool", tool);
  return payload;
}

Value ActionValue(const std::string& label, Object payload) {
  Object action;
  action.set("label", label);
  action.set("message", label);
  action.set("payload", ObjectValue(std::move(payload)));
  action.set("style", "primary");
  return ObjectValue(std::move(action));
}

Value ContactRow(const LocalPlanContact& c, const bool identical_rows) {
  const std::string title = !c.remark.empty() ? c.remark : c.nickname;
  Object item;
  item.set("title", title.empty() ? TrOrDefault("people.result.unknown_person", "Unknown person") : title);
  if (!c.remark.empty() && !c.nickname.empty() && c.remark != c.nickname) {
    item.set("subtitle", "~" + c.nickname);
  }
  SetPersonAvatarFields(item, title, c.id);

  std::vector<Value> actions;
  if (identical_rows) {
    item.set("meta", Tr("ai.contact.needs_remark"));
    Object payload = LocalActionPayload("edit_remark");
    payload.set("contact_id", c.id);
    actions.push_back(ActionValue(Tr("ai.contact.add_remark"), std::move(payload)));
  } else {
    if (c.last_contact_ms > 0) {
      item.set("meta", Tr("ai.contact.last_contact", Args{{"date", DateText(c.last_contact_ms)}}));
    }
    Object payload;
    if (c.thread_id.empty()) {
      payload = LocalActionPayload("start_conversation");
    } else {
      payload = LocalActionPayload("open_conversation");
      payload.set("thread_id", c.thread_id);
    }
    payload.set("contact_id", c.id);
    actions.push_back(ActionValue(Tr("ai.contact.open"), std::move(payload)));
  }
  item.set("actions", ArrayValue(std::move(actions)));
  return ObjectValue(std::move(item));
}

Value Paragraph(const std::string& text) {
  Object p;
  p.set("type", "paragraph");
  p.set("text", text);
  return ObjectValue(std::move(p));
}

Value LongList(const std::string& title, std::vector<Value> items) {
  Object block;
  block.set("type", "long_list");
  block.set("title", title);
  block.set("items", ArrayValue(std::move(items)));
  return ObjectValue(std::move(block));
}

} // namespace

std::string RenderLocalAsk(const LocalAsk& ask) {
  if (ask.question_key == "settings.ask.on_or_off") {
    const std::string specific = "settings.ask.on_or_off." + ask.tool;
    const std::string text = Tr(specific);
    if (text != specific) {
      return text;
    }
  }
  return Tr(ask.question_key);
}

std::string RenderLocalResult(const std::string& tool, const Object& result_json, const LocalRenderContext& ctx) {
  std::string text;
  if (result_json.getIf<bool>("success") == std::optional<bool>(false)) {
    text = result_json.getString("message").value_or("");
    if (text.empty()) {
      text = Tr("ai.gave_up");
    }
  } else if (IsReader(tool)) {
    text = RenderCard(tool, result_json);
  } else if (tool == "list_conversations" || tool == "list_contacts" || tool == "search_people") {
    return {};
  } else {
    text = RenderSentence(tool, result_json, ctx);
  }
  if (ctx.synthesis_hints == kHintOnlyFirstAction) {
    text += "\n\n" + Tr("ai.only_first_action");
  }
  return text;
}

std::string RenderLocalAmbiguous(const LocalAmbiguous& ambiguous, const std::string& query) {
  std::vector<Value> blocks;
  if (ambiguous.candidates.empty()) {
    blocks.push_back(Paragraph(Tr("ai.contact.not_found")));
    Object item;
    item.set("title", Tr("ai.contact.search_directory"));
    if (!query.empty()) {
      item.set("subtitle", query);
    }
    Object payload = LocalActionPayload("search_people");
    payload.set("query", query);
    std::vector<Value> actions;
    actions.push_back(ActionValue(Tr("ai.contact.search_directory_action"), std::move(payload)));
    item.set("actions", ArrayValue(std::move(actions)));
    std::vector<Value> items;
    items.push_back(ObjectValue(std::move(item)));
    blocks.push_back(LongList(Tr("ai.contact.list_title"), std::move(items)));
  } else {
    blocks.push_back(Paragraph(Tr(ambiguous.identical_rows ? "ai.contact.needs_remark" : "ai.contact.pick_one")));
    std::vector<Value> items;
    for (const LocalPlanContact& c : ambiguous.candidates) {
      items.push_back(ContactRow(c, ambiguous.identical_rows));
    }
    blocks.push_back(LongList(Tr("ai.contact.list_title"), std::move(items)));
  }
  Object root;
  root.set("blocks", ArrayValue(std::move(blocks)));
  return DumpJson(root);
}

std::string RenderLocalCancelled() { return Tr("ai.cancelled"); }
std::string RenderLocalGaveUp() { return Tr("ai.gave_up"); }
std::string RenderLocalUnsupported() { return Tr("ai.cannot_do_that_yet"); }
std::string RenderLocalNotShareable() { return Tr("ai.action_not_shareable"); }
std::string RenderLocalContactGone() { return Tr("ai.contact.gone"); }

} // namespace pbr
