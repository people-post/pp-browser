#include "feature/settings/SettingsTools.h"

#include "base/data/LlmPreset.h"
#include "base/data/ToolPermissions.h"
#include "base/data/UserPreferences.h"
#include "base/i18n/LocalizationService.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

ToolMeta Meta(std::string domain, std::string risk, const bool mutating) {
  return ToolMeta{"settings", std::move(domain), std::move(risk), mutating};
}

Object MustSchema(std::string_view schema_json) {
  auto parsed = TryParseObject(std::string(schema_json));
  if (!parsed) {
    Object fallback;
    fallback.set("type", "object");
    fallback.set("properties", Object{});
    return fallback;
  }
  return *parsed;
}

const char* ReachabilityStatusName(SettingsReachabilityView::Status status) {
  switch (status) {
  case SettingsReachabilityView::Status::Checking:
    return "checking";
  case SettingsReachabilityView::Status::Reachable:
    return "reachable";
  case SettingsReachabilityView::Status::OutboundOnly:
    return "outbound_only";
  case SettingsReachabilityView::Status::Blocked:
    return "blocked";
  case SettingsReachabilityView::Status::Unknown:
  default:
    return "unknown";
  }
}

Roe<SessionStore*> RequireStore(const SettingsToolPorts& ports) {
  if (!ports.session_store) {
    return Error("Session store unavailable");
  }
  return &ports.session_store();
}

Roe<void> SavePrefs(SessionStore& store, ProfilePreferences prefs) {
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  return store.SaveProfilePrefs(prefs);
}

Roe<void> SaveConfig(SessionStore& store, AppConfig config) {
  return store.SaveConfig(config);
}

std::string TrimAscii(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

/** Safe string read — avoids type errors when LLMs pass non-strings. */
std::string JsonString(const Value& value) {
  if (auto s = asString(value)) {
    return *s;
  }
  if (const bool* b = std::get_if<bool>(&value)) {
    return *b ? "true" : "false";
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
  return {};
}

std::string FirstStringArg(const Object& arguments,
                           std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!arguments.contains(key)) {
      continue;
    }
    auto slot = arguments.fields().tryGet(key);
    if (!slot) {
      continue;
    }
    const std::string value = TrimAscii(JsonString(slot->get()));
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

std::optional<bool> ParseFlexibleBool(const Value& value) {
  if (const bool* b = std::get_if<bool>(&value)) {
    return *b;
  }
  if (auto s = asString(value)) {
    const std::string lower = ToLowerAscii(TrimAscii(*s));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "y" || lower == "on" ||
        lower == "enable" || lower == "enabled") {
      return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "n" || lower == "off" ||
        lower == "disable" || lower == "disabled") {
      return false;
    }
  }
  if (const int64_t* i = std::get_if<int64_t>(&value)) {
    return *i != 0;
  }
  if (const uint64_t* u = std::get_if<uint64_t>(&value)) {
    return *u != 0;
  }
  if (const double* d = std::get_if<double>(&value)) {
    return *d != 0.0;
  }
  return std::nullopt;
}

std::optional<bool> BoolFromArgs(const Object& arguments,
                                 std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (!arguments.contains(key)) {
      continue;
    }
    auto slot = arguments.fields().tryGet(key);
    if (!slot) {
      continue;
    }
    if (auto parsed = ParseFlexibleBool(slot->get())) {
      return parsed;
    }
  }
  return std::nullopt;
}

bool ValidAppearance(const std::string& appearance) {
  return appearance == "system" || appearance == "light" || appearance == "dark";
}

/** Prefer `appearance`; accept LLM aliases (`theme`, `mode`, Title Case, dark_mode bool). */
std::string AppearanceFromArgs(const Object& arguments) {
  std::string value =
      FirstStringArg(arguments, {"appearance", "theme", "mode", "color_scheme", "colorScheme"});
  if (!value.empty()) {
    return ToLowerAscii(std::move(value));
  }
  if (auto dark = BoolFromArgs(arguments, {"dark_mode", "darkMode", "is_dark"})) {
    return *dark ? "dark" : "light";
  }
  if (auto light = BoolFromArgs(arguments, {"light_mode", "lightMode", "is_light"})) {
    return *light ? "light" : "dark";
  }
  return {};
}

std::string NormalizePolicyToken(std::string value) {
  value = ToLowerAscii(TrimAscii(std::move(value)));
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      out.push_back(ch);
    } else if (ch == '-' || ch == ' ' || ch == '/') {
      if (!out.empty() && out.back() != '_') {
        out.push_back('_');
      }
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out == "contacts" || out == "contact_only" || out == "only_contacts" ||
      out == "contacts_only") {
    return "contacts_only";
  }
  if (out == "all" || out == "anybody" || out == "anyone" || out == "everyone") {
    return "everyone";
  }
  if (out == "none" || out == "no_one" || out == "noone" || out == "nobody") {
    return "nobody";
  }
  return out;
}

bool ValidGroupInvitePolicy(const std::string& policy) {
  return policy == "everyone" || policy == "contacts_only" || policy == "nobody";
}

std::string PolicyFromArgs(const Object& arguments) {
  return NormalizePolicyToken(
      FirstStringArg(arguments, {"policy", "group_invite_policy", "invite_policy", "who"}));
}

/**
 * Map free-form LLM language input to a storable pref (`system` or shipped tag).
 * Accepts aliases (`locale`/`lang`), tag variants (`zh`, `zh-CN`, `zh_Hans`), and labels.
 */
std::string CanonicalLanguagePref(const SettingsToolPorts& ports, std::string raw) {
  raw = TrimAscii(std::move(raw));
  if (raw.empty()) {
    return {};
  }
  const std::string lower = ToLowerAscii(raw);
  if (lower == "system" || lower == "auto" || lower == "default" || lower == "os") {
    return "system";
  }

  std::vector<LocaleInfo> locales;
  if (ports.available_locales) {
    locales = ports.available_locales();
  }

  const std::string normalized = LocalizationService::NormalizeTag(raw);
  for (const LocaleInfo& locale : locales) {
    if (locale.tag == raw || locale.tag == normalized) {
      return locale.tag;
    }
  }

  const std::string primary = LocalizationService::PrimaryLanguage(normalized);
  for (const LocaleInfo& locale : locales) {
    if (LocalizationService::PrimaryLanguage(locale.tag) == primary) {
      return locale.tag;
    }
  }

  for (const LocaleInfo& locale : locales) {
    std::string label = locale.tag;
    if (ports.language_display_label) {
      label = ports.language_display_label(locale.tag);
    }
    if (EqualsIgnoreCaseAscii(label, raw) || label == raw) {
      return locale.tag;
    }
  }

  // Common English / Chinese display aliases when catalogs are thin.
  if (lower == "english" || lower == "en-us" || lower == "en_us") {
    for (const LocaleInfo& locale : locales) {
      if (locale.tag == "en") {
        return "en";
      }
    }
  }
  if (lower == "chinese" || lower == "simplified chinese" || lower == "zh-cn" || lower == "zh_cn" ||
      raw == "中文" || raw == "简体中文" || raw == "汉语" || raw == "普通话") {
    for (const LocaleInfo& locale : locales) {
      if (locale.tag == "zh-Hans" || LocalizationService::PrimaryLanguage(locale.tag) == "zh") {
        return locale.tag;
      }
    }
  }

  return {};
}

std::string LanguageFromArgs(const SettingsToolPorts& ports, const Object& arguments) {
  return CanonicalLanguagePref(
      ports, FirstStringArg(arguments, {"language", "locale", "lang", "language_code", "languageCode"}));
}

Object OkJson(Object extra = {}) {
  extra.set("success", true);
  return extra;
}

} // namespace

SettingsToolPorts SettingsToolPortsFromCommands(const SettingsCommands& commands) {
  return SettingsToolPorts{
      .load_profile_identity = commands.load_profile_identity,
      .language_display_label = commands.language_display_label,
      .available_locales = commands.available_locales,
      .apply_appearance = commands.apply_appearance,
      .session_store = commands.session_store,
      .messaging_ready = commands.messaging_ready,
      .last_mesh_error = commands.last_mesh_error,
      .load_reachability = commands.load_reachability,
      .load_pin_protection = commands.load_pin_protection,
      .run_reachability_probe = commands.run_reachability_probe,
  };
}

SettingsToolProvider::SettingsToolProvider(SettingsToolPorts ports) : ports_(std::move(ports)) {}

std::string SettingsToolProvider::Id() const {
  return "settings";
}

std::vector<ToolDescriptor> SettingsToolProvider::ListTools() {
  // Capture ports by value so lambdas outlive this temporary provider.
  SettingsToolPorts ports = ports_;
  std::vector<ToolDescriptor> tools;

  tools.push_back(MakeTool(
      ToolDefinition{"get_profile_identity", "Read this device's profile identity (nickname, peer id, registration status). "
                                     "Does not return secrets or API keys.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("identity", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         if (!ports.load_profile_identity) {
           return Error("Profile identity unavailable");
         }
         const ProfileIdentityView view = ports.load_profile_identity();
         Object out;
         out.set("ready", view.ready);
         out.set("nickname", view.nickname);
         out.set("peer_id", view.peer_id);
         out.set("relay_id", view.relay_id);
         out.set("registered", view.registered);
         out.set("registration_status", view.registration_status);
         out.set("registration_expires", view.registration_expires);
         out.set("show_register", view.show_register);
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_preferences", "Read profile preferences: appearance, language, notifications, "
                                                 "group invite policy, transparency, call diagnostics, auto-renew.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("settings", "read", false),
      [ports](const Object&) -> Roe<std::string> {
                     auto store = RequireStore(ports);
                     if (!store) {
                       return store.error();
                     }
                     const ProfilePreferences& prefs = (*store)->Snapshot().profile_prefs;
                     Object out;
         out.set("appearance", prefs.appearance);
         out.set("language", prefs.language);
         out.set("show_notifications", prefs.show_notifications);
         out.set("auto_renew_registration", prefs.auto_renew_registration);
         out.set("group_invite_policy", prefs.group_invite_policy);
         out.set("reduce_transparency", prefs.reduce_transparency);
         out.set("call_diagnostics", prefs.call_diagnostics);
         out.set("tool_permissions_remembered",
                   static_cast<int64_t>(RememberedToolPermissionCount(prefs.tool_permissions)));
         return DumpJson(out);
                   }));

  tools.push_back(MakeTool(
      ToolDefinition{"list_locales", "List available UI languages (BCP-47 tags) and the current language preference.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("settings", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         std::vector<Value> locales;
         if (ports.available_locales) {
           for (const LocaleInfo& locale : ports.available_locales()) {
             std::string label = locale.tag;
             if (ports.language_display_label) {
               label = ports.language_display_label(locale.tag);
             }
             Object entry;
             entry.set("tag", locale.tag);
             entry.set("label", label);
             locales.push_back(ObjectValue(std::move(entry)));
           }
         }
         const std::string current = (*store)->Snapshot().profile_prefs.language;
         Object out;
         out.set("current", current);
         out.set("locales", ArrayValue(std::move(locales)));
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_reachability", "Read network reachability status for this device (reachable / outbound only / "
                                     "blocked) and related signals.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("network", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         Object out;
         out.set("status", ReachabilityStatusName(view.status));
         out.set("has_global_ipv6", view.has_global_ipv6);
         out.set("dial_back_ok", view.dial_back_ok);
         out.set("upnp_mapped", view.upnp_mapped);
         out.set("help_kind", view.help_kind);
         out.set("messaging_ready", ports.messaging_ready ? ports.messaging_ready() : false);
         out.set("last_mesh_error", ports.last_mesh_error ? ports.last_mesh_error() : "");
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_security_status", "Read security status: PIN vault readiness (not the PIN), group invite policy, "
                                     "and how many assistant tool permissions are remembered.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("security", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         PinProtectionView pin;
         if (ports.load_pin_protection) {
           pin = ports.load_pin_protection();
         }
         const ProfilePreferences& prefs = (*store)->Snapshot().profile_prefs;
         std::string pin_status = "not_setup";
         if (pin.ready) {
           pin_status = prefs.pin_is_default ? "app_default" : "custom";
         }
         Object out;
         out.set("pin_ready", pin.ready);
         out.set("pin_unlocked", pin.unlocked);
         out.set("pin_status", pin_status);
         out.set("group_invite_policy", prefs.group_invite_policy);
         out.set("tool_permissions_remembered",
                   static_cast<int64_t>(RememberedToolPermissionCount(prefs.tool_permissions)));
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_network_settings", "Read mesh/network participation settings (node, relays, prefer contacts, "
                                     "mesh_enabled, amp_udp_port). Does not change endpoints.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("network", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const MeshConfig& mesh_cfg = (*store)->Snapshot().config.mesh;
         Object out;
         out.set("node_enabled", mesh_cfg.node_enabled);
         out.set("amp_udp_port", static_cast<int64_t>(mesh_cfg.amp_udp_port));
         out.set("mesh_enabled", mesh_cfg.mesh_enabled);
         out.set("prefer_contacts_for_routing", mesh_cfg.prefer_contacts_for_routing);
         out.set("circuit_relay", mesh_cfg.capabilities.circuit_relay);
         out.set("media_relay", mesh_cfg.capabilities.media_relay);
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_llm_settings", "Read assistant LLM settings (preset, base URL, model). Never returns API keys.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("settings", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const AppConfig& config = (*store)->Snapshot().config;
         std::string key_state = "unset";
         if (!config.llm.api_key.empty()) {
           key_state = "set";
         } else if (!config.llm_api_key_env.empty()) {
           key_state = "env";
         }
         Object out;
         out.set("preset", ResolvePreset(config));
         out.set("base_url", config.llm.base_url);
         out.set("model", config.llm.model);
         out.set("api_key", key_state);
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"get_integrations", "Read search provider and MCP server ids/enabled flags. URLs are omitted.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("settings", "read", false),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const AppConfig& config = (*store)->Snapshot().config;
         std::vector<Value> servers;
         if (config.promoted_mcp.IsConfigured()) {
           Object entry;
           entry.set("id", config.promoted_mcp.id.empty() ? "promoted" : config.promoted_mcp.id);
           entry.set("enabled", config.promoted_mcp.enabled);
           entry.set("kind", "promoted");
           servers.push_back(ObjectValue(std::move(entry)));
         }
         for (const McpConfig& mcp : config.mcp_servers) {
           Object entry;
           entry.set("id", mcp.id);
           entry.set("enabled", mcp.enabled);
           entry.set("kind", "custom");
           servers.push_back(ObjectValue(std::move(entry)));
         }
         Object out;
         out.set("search_provider", config.search.provider);
         out.set("mcp_servers", ArrayValue(std::move(servers)));
         return DumpJson(out);
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_appearance", "Set UI appearance: system, light, or dark. "
                                     "Args: appearance (preferred) or theme/mode; case-insensitive.", MustSchema(R"json({"type":"object","properties":{"appearance":{"type":"string","description":"system | light | dark"},"theme":{"type":"string","description":"Alias for appearance"}},"required":[]})json")},
      Meta("settings", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const std::string appearance = AppearanceFromArgs(arguments);
         if (!ValidAppearance(appearance)) {
           return Error("appearance must be system, light, or dark");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.appearance = appearance;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         if (ports.apply_appearance) {
           ports.apply_appearance(appearance);
         }
         Object ok;
         ok.set("appearance", appearance);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_language", "Set UI language to system or a shipped BCP-47 tag from list_locales "
                          "(en, zh-Hans). Accepts locale/lang aliases and common labels "
                          "(Chinese, 简体中文, zh, zh-CN).", MustSchema(R"json({"type":"object","properties":{"language":{"type":"string","description":"system | en | zh-Hans (or alias/label)"},"locale":{"type":"string","description":"Alias for language"}},"required":[]})json")},
      Meta("settings", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const std::string language = LanguageFromArgs(ports, arguments);
         if (language.empty()) {
           return Error("language must be system or a tag from list_locales");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.language = language;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("language", language);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_notifications", "Enable or disable OS notification banners (sync continues either way).", MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"show_notifications":{"type":"boolean","description":"Alias for enabled"}},"required":[]})json")},
      Meta("settings", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled =
             BoolFromArgs(arguments, {"enabled", "show_notifications", "notifications"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.show_notifications = *enabled;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("show_notifications", prefs.show_notifications);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_group_invite_policy", "Set who may invite this user to group chats: everyone, "
                                     "contacts_only, or nobody.", MustSchema(R"json({"type":"object","properties":{"policy":{"type":"string"},"group_invite_policy":{"type":"string","description":"Alias for policy"}},"required":[]})json")},
      Meta("security", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const std::string policy = PolicyFromArgs(arguments);
         if (!ValidGroupInvitePolicy(policy)) {
           return Error("policy must be everyone, contacts_only, or nobody");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.group_invite_policy = policy;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("group_invite_policy", policy);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_auto_renew_registration", "Enable or disable automatic network registration renewal near expiry.", MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
      Meta("identity", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled = BoolFromArgs(arguments, {"enabled", "auto_renew_registration"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.auto_renew_registration = *enabled;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("auto_renew_registration", prefs.auto_renew_registration);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_reduce_transparency", "When enabled, compact chrome uses opaque surfaces (no backdrop frost).", MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
      Meta("settings", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled = BoolFromArgs(arguments, {"enabled", "reduce_transparency"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.reduce_transparency = *enabled;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("reduce_transparency", prefs.reduce_transparency);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_call_diagnostics", "Enable or disable call media diagnostics in the UI.", MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
      Meta("settings", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled = BoolFromArgs(arguments, {"enabled", "call_diagnostics"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.call_diagnostics = *enabled;
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("call_diagnostics", prefs.call_diagnostics);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_node_enabled", "Enable or disable desktop Node participation (help the network). Ignored on mobile.", MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
      Meta("network", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled = BoolFromArgs(arguments, {"enabled", "node_enabled"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         config.mesh.node_enabled = *enabled;
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("node_enabled", config.mesh.node_enabled);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"set_mesh_capabilities", "Update mesh capability flags: circuit_relay, media_relay, dht, prefer_contacts_for_routing.", MustSchema(R"json({"type":"object","properties":{"circuit_relay":{"type":"boolean"},"media_relay":{"type":"boolean"},"dht":{"type":"boolean"},"prefer_contacts_for_routing":{"type":"boolean"}},"required":[]})json")},
      Meta("network", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         const auto circuit = BoolFromArgs(arguments, {"circuit_relay"});
         const auto media = BoolFromArgs(arguments, {"media_relay"});
         const auto dht = BoolFromArgs(arguments, {"dht"});
         const auto prefer = BoolFromArgs(arguments, {"prefer_contacts_for_routing"});
         if (!circuit && !media && !dht && !prefer) {
           return Error("provide at least one of circuit_relay, media_relay, dht, prefer_contacts_for_routing");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         if (circuit) {
           config.mesh.capabilities.circuit_relay = *circuit;
         }
         if (media) {
           config.mesh.capabilities.media_relay = *media;
         }
         if (dht) {
           config.mesh.capabilities.dht = *dht;
         }
         if (prefer) {
           config.mesh.prefer_contacts_for_routing = *prefer;
         }
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("circuit_relay", config.mesh.capabilities.circuit_relay);
         ok.set("media_relay", config.mesh.capabilities.media_relay);
         ok.set("dht", config.mesh.capabilities.dht);
         ok.set("prefer_contacts_for_routing", config.mesh.prefer_contacts_for_routing);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"probe_reachability", "Re-run the network reachability probe. Optionally try UPnP port mapping first.", MustSchema(R"json({"type":"object","properties":{"try_upnp":{"type":"boolean"}},"required":[]})json")},
      Meta("network", "write", true),
      [ports](const Object& arguments) -> Roe<std::string> {
         if (!ports.run_reachability_probe) {
           return Error("Reachability probe unavailable");
         }
         const bool try_upnp = BoolFromArgs(arguments, {"try_upnp"}).value_or(false);
         ports.run_reachability_probe(try_upnp);
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         Object ok;
         ok.set("status", ReachabilityStatusName(view.status));
         ok.set("try_upnp", try_upnp);
         return DumpJson(OkJson(std::move(ok)));
       }));

  tools.push_back(MakeTool(
      ToolDefinition{"reset_tool_permissions", "Clear remembered Always allow / Never decisions for assistant tools so the "
                                     "assistant asks again before changing data.", MustSchema(R"json({"type":"object","properties":{}})json")},
      Meta("security", "write", true),
      [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         ClearToolPermissionDecisions(prefs.tool_permissions);
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object ok;
         ok.set("tool_permissions_remembered", static_cast<int64_t>(0));
         return DumpJson(OkJson(std::move(ok)));
       }));

  return tools;
}

void RegisterSettingsTools(ToolRegistry& registry, SettingsToolPorts ports) {
  SettingsToolProvider provider(std::move(ports));
  registry.RegisterProvider(provider);
}

} // namespace pbr
