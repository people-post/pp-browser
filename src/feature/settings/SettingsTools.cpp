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
  return ToolMeta{.provider = "settings",
                  .domain = std::move(domain),
                  .risk = std::move(risk),
                  .mutating = mutating};
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
      .last_libp2p_error = commands.last_libp2p_error,
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

  tools.push_back(
      {.definition = {.name = "get_profile_identity",
                      .description = "Read this device's profile identity (nickname, peer id, registration status). "
                                     "Does not return secrets or API keys.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("identity", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
         if (!ports.load_profile_identity) {
           return Error("Profile identity unavailable");
         }
         const ProfileIdentityView view = ports.load_profile_identity();
         Object __out;
         __out.set("ready", view.ready);
         __out.set("nickname", view.nickname);
         __out.set("peer_id", view.peer_id);
         __out.set("relay_id", view.relay_id);
         __out.set("registered", view.registered);
         __out.set("registration_status", view.registration_status);
         __out.set("registration_expires", view.registration_expires);
         __out.set("show_register", view.show_register);
         return DumpJson(__out);
       }});

  tools.push_back({.definition = {.name = "get_preferences",
                                  .description = "Read profile preferences: appearance, language, notifications, "
                                                 "group invite policy, transparency, call diagnostics, auto-renew.",
                                  .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
                   .meta = Meta("settings", "read", false),
                   .execute = [ports](const Object&) -> Roe<std::string> {
                     auto store = RequireStore(ports);
                     if (!store) {
                       return store.error();
                     }
                     const ProfilePreferences& prefs = (*store)->Snapshot().profile_prefs;
                     Object __out;
         __out.set("appearance", prefs.appearance);
         __out.set("language", prefs.language);
         __out.set("show_notifications", prefs.show_notifications);
         __out.set("auto_renew_registration", prefs.auto_renew_registration);
         __out.set("group_invite_policy", prefs.group_invite_policy);
         __out.set("reduce_transparency", prefs.reduce_transparency);
         __out.set("call_diagnostics", prefs.call_diagnostics);
         __out.set("tool_permissions_remembered",
                   static_cast<int64_t>(RememberedToolPermissionCount(prefs.tool_permissions)));
         return DumpJson(__out);
                   }});

  tools.push_back(
      {.definition = {.name = "list_locales",
                      .description = "List available UI languages (BCP-47 tags) and the current language preference.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
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
         Object __out;
         __out.set("current", current);
         __out.set("locales", ArrayValue(std::move(locales)));
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "get_reachability",
                      .description = "Read network reachability status for this device (reachable / outbound only / "
                                     "blocked) and related signals.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("network", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         Object __out;
         __out.set("status", ReachabilityStatusName(view.status));
         __out.set("has_global_ipv6", view.has_global_ipv6);
         __out.set("dial_back_ok", view.dial_back_ok);
         __out.set("upnp_mapped", view.upnp_mapped);
         __out.set("help_kind", view.help_kind);
         __out.set("messaging_ready", ports.messaging_ready ? ports.messaging_ready() : false);
         __out.set("last_libp2p_error", ports.last_libp2p_error ? ports.last_libp2p_error() : "");
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "get_security_status",
                      .description = "Read security status: PIN vault readiness (not the PIN), group invite policy, "
                                     "and how many assistant tool permissions are remembered.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("security", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
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
         Object __out;
         __out.set("pin_ready", pin.ready);
         __out.set("pin_unlocked", pin.unlocked);
         __out.set("pin_status", pin_status);
         __out.set("group_invite_policy", prefs.group_invite_policy);
         __out.set("tool_permissions_remembered",
                   static_cast<int64_t>(RememberedToolPermissionCount(prefs.tool_permissions)));
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "get_network_settings",
                      .description = "Read mesh/network participation settings (node, relays, prefer contacts) and "
                                     "listen multiaddr. Does not change endpoints.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("network", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const Libp2pConfig& libp2p = (*store)->Snapshot().config.libp2p;
         Object __out;
         __out.set("node_enabled", libp2p.node_enabled);
         __out.set("listen_multiaddr", libp2p.listen_multiaddr);
         __out.set("prefer_contacts_for_routing", libp2p.prefer_contacts_for_routing);
         __out.set("circuit_relay", libp2p.capabilities.circuit_relay);
         __out.set("media_relay", libp2p.capabilities.media_relay);
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "get_llm_settings",
                      .description = "Read assistant LLM settings (preset, base URL, model). Never returns API keys.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
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
         Object __out;
         __out.set("preset", ResolvePreset(config));
         __out.set("base_url", config.llm.base_url);
         __out.set("model", config.llm.model);
         __out.set("api_key", key_state);
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "get_integrations",
                      .description = "Read search provider and MCP server ids/enabled flags. URLs are omitted.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const Object&) -> Roe<std::string> {
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
         Object __out;
         __out.set("search_provider", config.search.provider);
         __out.set("mcp_servers", ArrayValue(std::move(servers)));
         return DumpJson(__out);
       }});

  tools.push_back(
      {.definition = {.name = "set_appearance",
                      .description = "Set UI appearance: system, light, or dark. "
                                     "Args: appearance (preferred) or theme/mode; case-insensitive.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"appearance":{"type":"string","description":"system | light | dark"},"theme":{"type":"string","description":"Alias for appearance"}},"required":[]})json")},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("appearance", appearance);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_language",
                      .description =
                          "Set UI language to system or a shipped BCP-47 tag from list_locales "
                          "(en, zh-Hans). Accepts locale/lang aliases and common labels "
                          "(Chinese, 简体中文, zh, zh-CN).",
                      .parameters =
                          MustSchema(R"json({"type":"object","properties":{"language":{"type":"string","description":"system | en | zh-Hans (or alias/label)"},"locale":{"type":"string","description":"Alias for language"}},"required":[]})json")},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("language", language);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_notifications",
                      .description = "Enable or disable OS notification banners (sync continues either way).",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"show_notifications":{"type":"boolean","description":"Alias for enabled"}},"required":[]})json")},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("show_notifications", prefs.show_notifications);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_group_invite_policy",
                      .description = "Set who may invite this user to group chats: everyone, "
                                     "contacts_only, or nobody.",
                      .parameters =
                          MustSchema(R"json({"type":"object","properties":{"policy":{"type":"string"},"group_invite_policy":{"type":"string","description":"Alias for policy"}},"required":[]})json")},
       .meta = Meta("security", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("group_invite_policy", policy);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_auto_renew_registration",
                      .description = "Enable or disable automatic network registration renewal near expiry.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("identity", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("auto_renew_registration", prefs.auto_renew_registration);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_reduce_transparency",
                      .description = "When enabled, compact chrome uses opaque surfaces (no backdrop frost).",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("reduce_transparency", prefs.reduce_transparency);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_call_diagnostics",
                      .description = "Enable or disable call media diagnostics in the UI.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
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
         Object __ok;
         __ok.set("call_diagnostics", prefs.call_diagnostics);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_node_enabled",
                      .description = "Enable or disable desktop Node participation (help the network). Ignored on mobile.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("network", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
         const auto enabled = BoolFromArgs(arguments, {"enabled", "node_enabled"});
         if (!enabled) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         config.libp2p.node_enabled = *enabled;
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         Object __ok;
         __ok.set("node_enabled", config.libp2p.node_enabled);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "set_mesh_capabilities",
                      .description = "Update mesh capability flags: circuit_relay, media_relay, prefer_contacts_for_routing.",
                      .parameters =
                          MustSchema(R"json({"type":"object","properties":{"circuit_relay":{"type":"boolean"},"media_relay":{"type":"boolean"},"prefer_contacts_for_routing":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("network", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
         const auto circuit = BoolFromArgs(arguments, {"circuit_relay"});
         const auto media = BoolFromArgs(arguments, {"media_relay"});
         const auto prefer = BoolFromArgs(arguments, {"prefer_contacts_for_routing"});
         if (!circuit && !media && !prefer) {
           return Error("provide at least one of circuit_relay, media_relay, prefer_contacts_for_routing");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         if (circuit) {
           config.libp2p.capabilities.circuit_relay = *circuit;
         }
         if (media) {
           config.libp2p.capabilities.media_relay = *media;
         }
         if (prefer) {
           config.libp2p.prefer_contacts_for_routing = *prefer;
         }
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         Object __ok;
         __ok.set("circuit_relay", config.libp2p.capabilities.circuit_relay);
         __ok.set("media_relay", config.libp2p.capabilities.media_relay);
         __ok.set("prefer_contacts_for_routing", config.libp2p.prefer_contacts_for_routing);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "probe_reachability",
                      .description = "Re-run the network reachability probe. Optionally try UPnP port mapping first.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{"try_upnp":{"type":"boolean"}},"required":[]})json")},
       .meta = Meta("network", "write", true),
       .execute = [ports](const Object& arguments) -> Roe<std::string> {
         if (!ports.run_reachability_probe) {
           return Error("Reachability probe unavailable");
         }
         const bool try_upnp = BoolFromArgs(arguments, {"try_upnp"}).value_or(false);
         ports.run_reachability_probe(try_upnp);
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         Object __ok;
         __ok.set("status", ReachabilityStatusName(view.status));
         __ok.set("try_upnp", try_upnp);
         return DumpJson(OkJson(std::move(__ok)));
       }});

  tools.push_back(
      {.definition = {.name = "reset_tool_permissions",
                      .description = "Clear remembered Always allow / Never decisions for assistant tools so the "
                                     "assistant asks again before changing data.",
                      .parameters = MustSchema(R"json({"type":"object","properties":{}})json")},
       .meta = Meta("security", "write", true),
       .execute = [ports](const Object&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         ClearToolPermissionDecisions(prefs.tool_permissions);
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         Object __ok;
         __ok.set("tool_permissions_remembered", static_cast<int64_t>(0));
         return DumpJson(OkJson(std::move(__ok)));
       }});

  return tools;
}

void RegisterSettingsTools(ToolRegistry& registry, SettingsToolPorts ports) {
  SettingsToolProvider provider(std::move(ports));
  registry.RegisterProvider(provider);
}

} // namespace pbr
