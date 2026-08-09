#include "feature/settings/SettingsTools.h"

#include "base/data/LlmPreset.h"
#include "base/data/ToolPermissions.h"
#include "base/data/UserPreferences.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

ToolMeta Meta(std::string domain, std::string risk, const bool mutating) {
  return ToolMeta{.provider = "settings",
                  .domain = std::move(domain),
                  .risk = std::move(risk),
                  .mutating = mutating};
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

std::string NormalizeAppearance(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ValidAppearance(const std::string& appearance) {
  return appearance == "system" || appearance == "light" || appearance == "dark";
}

/** Prefer `appearance`; accept common LLM aliases (`theme`, Title Case). */
std::string AppearanceFromArgs(const nlohmann::json& arguments) {
  std::string value = arguments.value("appearance", "");
  if (value.empty()) {
    value = arguments.value("theme", "");
  }
  return NormalizeAppearance(std::move(value));
}

bool ValidGroupInvitePolicy(const std::string& policy) {
  return policy == "everyone" || policy == "contacts_only" || policy == "nobody";
}

bool LanguageAllowed(const SettingsToolPorts& ports, const std::string& language) {
  if (language == "system") {
    return true;
  }
  if (!ports.available_locales) {
    return false;
  }
  for (const LocaleInfo& locale : ports.available_locales()) {
    if (locale.tag == language) {
      return true;
    }
  }
  return false;
}

nlohmann::json OkJson(nlohmann::json extra = nlohmann::json::object()) {
  extra["success"] = true;
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
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("identity", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         if (!ports.load_profile_identity) {
           return Error("Profile identity unavailable");
         }
         const ProfileIdentityView view = ports.load_profile_identity();
         return nlohmann::json{{"ready", view.ready},
                               {"nickname", view.nickname},
                               {"peer_id", view.peer_id},
                               {"relay_id", view.relay_id},
                               {"registered", view.registered},
                               {"registration_status", view.registration_status},
                               {"registration_expires", view.registration_expires},
                               {"show_register", view.show_register}}
             .dump();
       }});

  tools.push_back({.definition = {.name = "get_preferences",
                                  .description = "Read profile preferences: appearance, language, notifications, "
                                                 "group invite policy, transparency, call diagnostics, auto-renew.",
                                  .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                   .meta = Meta("settings", "read", false),
                   .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
                     auto store = RequireStore(ports);
                     if (!store) {
                       return store.error();
                     }
                     const ProfilePreferences& prefs = (*store)->Snapshot().profile_prefs;
                     return nlohmann::json{{"appearance", prefs.appearance},
                                           {"language", prefs.language},
                                           {"show_notifications", prefs.show_notifications},
                                           {"auto_renew_registration", prefs.auto_renew_registration},
                                           {"group_invite_policy", prefs.group_invite_policy},
                                           {"reduce_transparency", prefs.reduce_transparency},
                                           {"call_diagnostics", prefs.call_diagnostics},
                                           {"tool_permissions_remembered",
                                            RememberedToolPermissionCount(prefs.tool_permissions)}}
                         .dump();
                   }});

  tools.push_back(
      {.definition = {.name = "list_locales",
                      .description = "List available UI languages (BCP-47 tags) and the current language preference.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         nlohmann::json locales = nlohmann::json::array();
         if (ports.available_locales) {
           for (const LocaleInfo& locale : ports.available_locales()) {
             std::string label = locale.tag;
             if (ports.language_display_label) {
               label = ports.language_display_label(locale.tag);
             }
             locales.push_back({{"tag", locale.tag}, {"label", label}});
           }
         }
         const std::string current = (*store)->Snapshot().profile_prefs.language;
         return nlohmann::json{{"current", current}, {"locales", locales}}.dump();
       }});

  tools.push_back(
      {.definition = {.name = "get_reachability",
                      .description = "Read network reachability status for this device (reachable / outbound only / "
                                     "blocked) and related signals.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("network", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         return nlohmann::json{{"status", ReachabilityStatusName(view.status)},
                               {"has_global_ipv6", view.has_global_ipv6},
                               {"dial_back_ok", view.dial_back_ok},
                               {"upnp_mapped", view.upnp_mapped},
                               {"help_kind", view.help_kind},
                               {"messaging_ready", ports.messaging_ready ? ports.messaging_ready() : false},
                               {"last_libp2p_error", ports.last_libp2p_error ? ports.last_libp2p_error() : ""}}
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "get_security_status",
                      .description = "Read security status: PIN vault readiness (not the PIN), group invite policy, "
                                     "and how many assistant tool permissions are remembered.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("security", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
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
         return nlohmann::json{{"pin_ready", pin.ready},
                               {"pin_unlocked", pin.unlocked},
                               {"pin_status", pin_status},
                               {"group_invite_policy", prefs.group_invite_policy},
                               {"tool_permissions_remembered",
                                RememberedToolPermissionCount(prefs.tool_permissions)}}
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "get_network_settings",
                      .description = "Read mesh/network participation settings (node, relays, prefer contacts) and "
                                     "listen multiaddr. Does not change endpoints.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("network", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const Libp2pConfig& libp2p = (*store)->Snapshot().config.libp2p;
         return nlohmann::json{{"node_enabled", libp2p.node_enabled},
                               {"listen_multiaddr", libp2p.listen_multiaddr},
                               {"prefer_contacts_for_routing", libp2p.prefer_contacts_for_routing},
                               {"circuit_relay", libp2p.capabilities.circuit_relay},
                               {"media_relay", libp2p.capabilities.media_relay}}
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "get_llm_settings",
                      .description = "Read assistant LLM settings (preset, base URL, model). Never returns API keys.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
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
         return nlohmann::json{{"preset", ResolvePreset(config)},
                               {"base_url", config.llm.base_url},
                               {"model", config.llm.model},
                               {"api_key", key_state}}
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "get_integrations",
                      .description = "Read search provider and MCP server ids/enabled flags. URLs are omitted.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("settings", "read", false),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         const AppConfig& config = (*store)->Snapshot().config;
         nlohmann::json servers = nlohmann::json::array();
         if (config.promoted_mcp.IsConfigured()) {
           servers.push_back({{"id", config.promoted_mcp.id.empty() ? "promoted" : config.promoted_mcp.id},
                              {"enabled", config.promoted_mcp.enabled},
                              {"kind", "promoted"}});
         }
         for (const McpConfig& mcp : config.mcp_servers) {
           servers.push_back({{"id", mcp.id}, {"enabled", mcp.enabled}, {"kind", "custom"}});
         }
         return nlohmann::json{{"search_provider", config.search.provider}, {"mcp_servers", servers}}.dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_appearance",
                      .description = "Set UI appearance theme: system, light, or dark. "
                                     "Pass lowercase appearance (not theme).",
                      .parameters = {{"type", "object"},
                                     {"properties",
                                      {{"appearance",
                                        {{"type", "string"},
                                         {"description", "system | light | dark"}}}}},
                                     {"required", nlohmann::json::array({"appearance"})}}},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
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
         return OkJson({{"appearance", appearance}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_language",
                      .description = "Set UI language preference to system or a shipped BCP-47 tag from list_locales.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"language", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"language"})}}},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string language = arguments.value("language", "");
         if (language.empty() || !LanguageAllowed(ports, language)) {
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
         return OkJson({{"language", language}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_notifications",
                      .description = "Enable or disable OS notification banners (sync continues either way).",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array({"enabled"})}}},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.show_notifications = arguments["enabled"].get<bool>();
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         return OkJson({{"show_notifications", prefs.show_notifications}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_group_invite_policy",
                      .description = "Set who may invite this user to group chats: everyone, contacts_only, or nobody.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"policy", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"policy"})}}},
       .meta = Meta("security", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         const std::string policy = arguments.value("policy", "");
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
         return OkJson({{"group_invite_policy", policy}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_auto_renew_registration",
                      .description = "Enable or disable automatic network registration renewal near expiry.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array({"enabled"})}}},
       .meta = Meta("identity", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.auto_renew_registration = arguments["enabled"].get<bool>();
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         return OkJson({{"auto_renew_registration", prefs.auto_renew_registration}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_reduce_transparency",
                      .description = "When enabled, compact chrome uses opaque surfaces (no backdrop frost).",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array({"enabled"})}}},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.reduce_transparency = arguments["enabled"].get<bool>();
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         return OkJson({{"reduce_transparency", prefs.reduce_transparency}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_call_diagnostics",
                      .description = "Enable or disable call media diagnostics in the UI.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array({"enabled"})}}},
       .meta = Meta("settings", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         prefs.call_diagnostics = arguments["enabled"].get<bool>();
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         return OkJson({{"call_diagnostics", prefs.call_diagnostics}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_node_enabled",
                      .description = "Enable or disable desktop Node participation (help the network). Ignored on mobile.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array({"enabled"})}}},
       .meta = Meta("network", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
           return Error("enabled boolean required");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         config.libp2p.node_enabled = arguments["enabled"].get<bool>();
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         return OkJson({{"node_enabled", config.libp2p.node_enabled}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "set_mesh_capabilities",
                      .description = "Update mesh capability flags: circuit_relay, media_relay, prefer_contacts_for_routing.",
                      .parameters =
                          {{"type", "object"},
                           {"properties",
                            {{"circuit_relay", {{"type", "boolean"}}},
                             {"media_relay", {{"type", "boolean"}}},
                             {"prefer_contacts_for_routing", {{"type", "boolean"}}}}},
                           {"required", nlohmann::json::array()}}},
       .meta = Meta("network", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!arguments.contains("circuit_relay") && !arguments.contains("media_relay") &&
             !arguments.contains("prefer_contacts_for_routing")) {
           return Error("provide at least one of circuit_relay, media_relay, prefer_contacts_for_routing");
         }
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         AppConfig config = (*store)->Snapshot().config;
         if (arguments.contains("circuit_relay") && arguments["circuit_relay"].is_boolean()) {
           config.libp2p.capabilities.circuit_relay = arguments["circuit_relay"].get<bool>();
         }
         if (arguments.contains("media_relay") && arguments["media_relay"].is_boolean()) {
           config.libp2p.capabilities.media_relay = arguments["media_relay"].get<bool>();
         }
         if (arguments.contains("prefer_contacts_for_routing") &&
             arguments["prefer_contacts_for_routing"].is_boolean()) {
           config.libp2p.prefer_contacts_for_routing = arguments["prefer_contacts_for_routing"].get<bool>();
         }
         if (auto saved = SaveConfig(**store, config); !saved) {
           return saved.error();
         }
         return OkJson({{"circuit_relay", config.libp2p.capabilities.circuit_relay},
                        {"media_relay", config.libp2p.capabilities.media_relay},
                        {"prefer_contacts_for_routing", config.libp2p.prefer_contacts_for_routing}})
             .dump();
       }});

  tools.push_back(
      {.definition = {.name = "probe_reachability",
                      .description = "Re-run the network reachability probe. Optionally try UPnP port mapping first.",
                      .parameters = {{"type", "object"},
                                     {"properties", {{"try_upnp", {{"type", "boolean"}}}}},
                                     {"required", nlohmann::json::array()}}},
       .meta = Meta("network", "write", true),
       .execute = [ports](const nlohmann::json& arguments) -> Roe<std::string> {
         if (!ports.run_reachability_probe) {
           return Error("Reachability probe unavailable");
         }
         const bool try_upnp = arguments.value("try_upnp", false);
         ports.run_reachability_probe(try_upnp);
         SettingsReachabilityView view;
         if (ports.load_reachability) {
           view = ports.load_reachability();
         }
         return OkJson({{"status", ReachabilityStatusName(view.status)}, {"try_upnp", try_upnp}}).dump();
       }});

  tools.push_back(
      {.definition = {.name = "reset_tool_permissions",
                      .description = "Clear remembered Always allow / Never decisions for assistant tools so the "
                                     "assistant asks again before changing data.",
                      .parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
       .meta = Meta("security", "write", true),
       .execute = [ports](const nlohmann::json&) -> Roe<std::string> {
         auto store = RequireStore(ports);
         if (!store) {
           return store.error();
         }
         ProfilePreferences prefs = (*store)->Snapshot().profile_prefs;
         ClearToolPermissionDecisions(prefs.tool_permissions);
         if (auto saved = SavePrefs(**store, prefs); !saved) {
           return saved.error();
         }
         return OkJson({{"tool_permissions_remembered", 0}}).dump();
       }});

  return tools;
}

void RegisterSettingsTools(ToolRegistry& registry, SettingsToolPorts ports) {
  SettingsToolProvider provider(std::move(ports));
  registry.RegisterProvider(provider);
}

} // namespace pbr
