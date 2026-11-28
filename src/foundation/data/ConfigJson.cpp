#include "foundation/data/ConfigJson.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/ICredentialStore.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::string NormalizeThemePath(std::string theme) {
  if (theme.rfind("assets/", 0) == 0) {
    theme = theme.substr(7);
  }
  return theme;
}

std::optional<int64_t> ReadI64(const Object& object, const char* key) {
  if (auto value = object.getIf<int64_t>(key)) {
    return *value;
  }
  if (auto value = object.getNonNegInt(key)) {
    if (*value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return static_cast<int64_t>(*value);
    }
  }
  return std::nullopt;
}

McpConfig ParseMcpObject(const Object& mcp_object) {
  McpConfig mcp;
  McpConfigFromObject(mcp_object, mcp);
  return mcp;
}

Object WindowPrefsToObject(const WindowPrefs& window) {
  Object object;
  object.set("width", static_cast<int64_t>(window.width));
  object.set("height", static_cast<int64_t>(window.height));
  return object;
}

void WindowPrefsFromObject(const Object& object, WindowPrefs& window) {
  if (auto width = ReadI64(object, "width")) {
    window.width = static_cast<int>(*width);
  }
  if (auto height = ReadI64(object, "height")) {
    window.height = static_cast<int>(*height);
  }
}

Object SafeAreaToObject(const SafeAreaInsets& safe_area) {
  Object object;
  object.set("top", static_cast<int64_t>(safe_area.top));
  object.set("bottom", static_cast<int64_t>(safe_area.bottom));
  object.set("left", static_cast<int64_t>(safe_area.left));
  object.set("right", static_cast<int64_t>(safe_area.right));
  return object;
}

void SafeAreaFromObject(const Object& object, SafeAreaInsets& safe_area) {
  if (auto top = ReadI64(object, "top")) {
    safe_area.top = static_cast<int>(*top);
  }
  if (auto bottom = ReadI64(object, "bottom")) {
    safe_area.bottom = static_cast<int>(*bottom);
  }
  if (auto left = ReadI64(object, "left")) {
    safe_area.left = static_cast<int>(*left);
  }
  if (auto right = ReadI64(object, "right")) {
    safe_area.right = static_cast<int>(*right);
  }
}

Object DisplayPrefsToObject(const DisplayPrefs& display) {
  Object object;
  object.set("fullscreen", display.fullscreen);
  return object;
}

void DisplayPrefsFromObject(const Object& object, DisplayPrefs& display) {
  if (auto fullscreen = object.getIf<bool>("fullscreen")) {
    display.fullscreen = *fullscreen;
  }
}

Object ToolPermissionsToObject(const ToolPermissionsPrefs& perms) {
  Object by_tool;
  for (const auto& [name, entry] : perms.by_tool) {
    Object node;
    node.set("decision", entry.decision);
    by_tool.set(name, node);
  }
  Object by_provider;
  for (const auto& [name, entry] : perms.by_provider) {
    Object node;
    node.set("decision", entry.decision);
    by_provider.set(name, node);
  }
  Object defaults;
  defaults.set("read", perms.default_read);
  defaults.set("write", perms.default_write);
  defaults.set("destructive", perms.default_destructive);

  Object object;
  object.set("schema_version", static_cast<int64_t>(perms.schema_version));
  object.set("defaults", defaults);
  object.set("by_tool", by_tool);
  object.set("by_provider", by_provider);
  return object;
}

ToolPermissionsPrefs ToolPermissionsFromObject(const Object& object) {
  ToolPermissionsPrefs perms;
  if (auto version = ReadI64(object, "schema_version")) {
    perms.schema_version = static_cast<int>(*version);
  }
  if (const Object* defaults = object.getObject("defaults")) {
    if (auto read = defaults->getString("read")) {
      perms.default_read = *read;
    }
    if (auto write = defaults->getString("write")) {
      perms.default_write = *write;
    }
    if (auto destructive = defaults->getString("destructive")) {
      perms.default_destructive = *destructive;
    }
  }
  if (const Object* by_tool = object.getObject("by_tool")) {
    for (const auto& [name, value] : by_tool->fields()) {
      const Object* node = asObject(value);
      if (!node) {
        continue;
      }
      auto decision = node->getString("decision");
      if (!decision || !IsValidToolPermissionDecision(*decision)) {
        continue;
      }
      perms.by_tool[name] = ToolPermissionEntry{.decision = *decision};
    }
  }
  if (const Object* by_provider = object.getObject("by_provider")) {
    for (const auto& [name, value] : by_provider->fields()) {
      const Object* node = asObject(value);
      if (!node) {
        continue;
      }
      auto decision = node->getString("decision");
      if (!decision || !IsValidToolPermissionDecision(*decision)) {
        continue;
      }
      perms.by_provider[name] = ToolPermissionEntry{.decision = *decision};
    }
  }
  return perms;
}

} // namespace

bool IsApiKeyEnvName(const std::string& name) {
  constexpr std::string_view kSuffix = "_API_KEY";
  if (name.size() <= kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

Object LlmConfigToObject(const LlmConfig& config) {
  Object object;
  object.set("base_url", config.base_url);
  object.set("model", config.model);
  object.set("require_api_key", config.require_api_key);
  object.set("num_predict", static_cast<int64_t>(config.num_predict));
  if (!config.preset.empty()) {
    object.set("preset", config.preset);
  }
  // No api_key: a typed key lives in the profile's secrets.enc, never in config.json.
  return object;
}

void LlmConfigFromObject(const Object& object, LlmConfig& config) {
  if (auto base_url = object.getString("base_url")) {
    config.base_url = *base_url;
  }
  if (auto model = object.getString("model")) {
    config.model = *model;
  }
  if (auto preset = object.getString("preset")) {
    config.preset = *preset;
  }
  if (auto require_api_key = object.getIf<bool>("require_api_key")) {
    config.require_api_key = *require_api_key;
  }
  if (auto num_predict = ReadI64(object, "num_predict")) {
    config.num_predict = static_cast<int>(*num_predict);
  }
}

Object ContextBudgetToObject(const ContextBudget& budget) {
  Object object;
  object.set("max_turn_pairs", static_cast<int64_t>(budget.max_turn_pairs));
  object.set("max_recent_chars", static_cast<int64_t>(budget.max_recent_chars));
  object.set("max_input_tokens", static_cast<int64_t>(budget.max_input_tokens));
  object.set("token_estimate_margin", budget.token_estimate_margin);
  object.set("max_summary_chars", static_cast<int64_t>(budget.max_summary_chars));
  return object;
}

void ContextBudgetFromObject(const Object& object, ContextBudget& budget) {
  if (auto max_turn_pairs = ReadI64(object, "max_turn_pairs")) {
    budget.max_turn_pairs = static_cast<int>(*max_turn_pairs);
  }
  if (auto max_recent_chars = ReadI64(object, "max_recent_chars")) {
    budget.max_recent_chars = static_cast<int>(*max_recent_chars);
  }
  if (auto max_input_tokens = ReadI64(object, "max_input_tokens")) {
    budget.max_input_tokens = static_cast<int>(*max_input_tokens);
  }
  if (auto margin = object.getIf<double>("token_estimate_margin")) {
    budget.token_estimate_margin = *margin;
  } else if (auto margin_i = ReadI64(object, "token_estimate_margin")) {
    budget.token_estimate_margin = static_cast<double>(*margin_i);
  }
  if (auto max_summary_chars = ReadI64(object, "max_summary_chars")) {
    budget.max_summary_chars = static_cast<int>(*max_summary_chars);
  }
}

Object SearchConfigToObject(const SearchConfig& config) {
  Object object;
  object.set("provider", config.provider);
  return object;
}

void SearchConfigFromObject(const Object& object, SearchConfig& config) {
  if (auto provider = object.getString("provider")) {
    config.provider = *provider;
  }
}

Object McpConfigToObject(const McpConfig& config) {
  Object object;
  if (!config.id.empty()) {
    object.set("id", config.id);
  }
  if (!config.url.empty()) {
    object.set("url", config.url);
  }
  if (!config.command.empty()) {
    object.set("command", config.command);
    std::vector<Value> args;
    args.reserve(config.args.size());
    for (const std::string& arg : config.args) {
      args.emplace_back(arg);
    }
    object.set("args", makeArray(std::move(args)));
  }
  if (!config.enabled) {
    object.set("enabled", false);
  }
  if (config.user_set) {
    object.set("set_by_user", true);
  }
  return object;
}

void McpConfigFromObject(const Object& object, McpConfig& config) {
  if (auto id = object.getString("id")) {
    config.id = *id;
  }
  if (auto command = object.getString("command")) {
    config.command = *command;
  }
  if (auto url = object.getString("url")) {
    config.url = *url;
  }
  if (const Array* args = object.getArray("args")) {
    config.args.clear();
    for (const Value& arg : args->elements) {
      if (auto value = asString(arg)) {
        config.args.push_back(*value);
      }
    }
  }
  if (auto enabled = object.getIf<bool>("enabled")) {
    config.enabled = *enabled;
  }
  if (auto user_set = object.getIf<bool>("set_by_user")) {
    config.user_set = *user_set;
  }
}

Object AppConfigToObject(const AppConfig& config) {
  Object object;
  object.set("theme", config.theme);
  Object llm = LlmConfigToObject(config.llm);
  if (!config.llm_api_key_env.empty()) {
    llm.set("api_key_env", config.llm_api_key_env);
  }
  object.set("llm", llm);
  object.set("search", SearchConfigToObject(config.search));
  object.set("context", ContextBudgetToObject(config.context));
  Object backend;
  // The saved choice as the file had it when this build runs another (BackendConfig::unavailable_env).
  backend.set("env", config.backend.unavailable_env.empty() ? config.backend.env : config.backend.unavailable_env);
  if (Object services = BackendServicesToObject(config.backend.services); !services.empty()) {
    backend.set("services", services);
  }
  object.set("backend", backend);
  const bool saved = config.backend.has_saved_urls;
  Object relay = ServiceEndpointToObject(saved ? config.backend.saved_relay : config.relay);
  if (config.relay_content_sha256) {
    relay.set("content_sha256", true);
  }
  if (!config.relay_extra_base_urls.empty()) {
    std::vector<Value> extras;
    for (const std::string& url : config.relay_extra_base_urls) {
      extras.emplace_back(url);
    }
    relay.set("extra_base_urls", makeArray(std::move(extras)));
  }
  object.set("relay", relay);
  object.set("directory", DirectoryConfigToObject(saved ? config.backend.saved_directory : config.directory));
  object.set("registration", ServiceEndpointToObject(saved ? config.backend.saved_registration : config.registration));
  object.set("mesh", MeshConfigToObject(config.mesh));
  object.set("initiation_floor", config.initiation_floor);
  if (!config.node.empty()) {
    object.set("node", config.node);
  }
  if (!config.data_dir.empty()) {
    object.set("data_dir", config.data_dir);
  }
  // `promoted_mcp` is not written: the user's own is the profile's (services.json, profile-model PM011; SessionStore
  // keeps it there), and the backend's seed is never written back (#744). One a file still holds is read (and moved).
  if (!config.mcp_servers.empty()) {
    std::vector<Value> servers;
    servers.reserve(config.mcp_servers.size());
    for (const McpConfig& mcp : config.mcp_servers) {
      servers.push_back(ObjectValue(McpConfigToObject(mcp)));
    }
    object.set("mcp_servers", makeArray(std::move(servers)));
  }
  return object;
}

void AppConfigFromObject(const Object& object, AppConfig& config) {
  if (auto theme = object.getString("theme")) {
    config.theme = NormalizeThemePath(*theme);
  }
  if (const Object* llm = object.getObject("llm")) {
    LlmConfigFromObject(*llm, config.llm);
    if (auto api_key_env = llm->getString("api_key_env")) {
      config.llm_api_key_env = *api_key_env;
      config.llm.api_key.clear();
      config.llm.require_api_key = true;
    }
  }
  if (const Object* promoted_mcp = object.getObject("promoted_mcp")) {
    config.promoted_mcp = ParseMcpObject(*promoted_mcp);
  }
  if (const Array* mcp_servers = object.getArray("mcp_servers")) {
    config.mcp_servers.clear();
    for (const Value& item : mcp_servers->elements) {
      const Object* mcp_object = asObject(item);
      if (!mcp_object) {
        continue;
      }
      McpConfig mcp = ParseMcpObject(*mcp_object);
      if (mcp.IsConfigured()) {
        config.mcp_servers.push_back(std::move(mcp));
      }
    }
  }
  if (const Object* context = object.getObject("context")) {
    ContextBudgetFromObject(*context, config.context);
  }
  if (const Object* search = object.getObject("search")) {
    SearchConfigFromObject(*search, config.search);
  }
  if (auto data_dir = object.getString("data_dir")) {
    config.data_dir = *data_dir;
  }
  if (const Object* backend = object.getObject("backend")) {
    if (auto env = backend->getString("env")) {
      config.backend.env = BackendEnvName(ParseBackendEnv(*env));
    }
    if (const Object* services = backend->getObject("services")) {
      BackendServicesFromObject(*services, config.backend.services);
    }
  }
  if (const Object* relay = object.getObject("relay")) {
    ServiceEndpointFromObject(*relay, config.relay);
    if (auto content_sha256 = relay->getIf<bool>("content_sha256")) {
      config.relay_content_sha256 = *content_sha256;
    }
    if (const Array* extras = relay->getArray("extra_base_urls")) {
      config.relay_extra_base_urls.clear();
      for (const Value& item : extras->elements) {
        if (auto url = asString(item); url && !url->empty()) {
          config.relay_extra_base_urls.push_back(*url);
        }
      }
    }
  }
  if (const Object* directory = object.getObject("directory")) {
    DirectoryConfigFromObject(*directory, config.directory);
  }
  if (const Object* registration = object.getObject("registration")) {
    ServiceEndpointFromObject(*registration, config.registration);
  }
  if (auto initiation_floor = ReadI64(object, "initiation_floor")) {
    config.initiation_floor = *initiation_floor;
  }
  if (const Object* mesh = object.getObject("mesh")) {
    MeshConfigFromObject(*mesh, config.mesh);
  }
  if (const Object* node = object.getObject("node")) {
    config.node = *node;
  }
}

Object MachinePrefsToObject(const MachinePreferences& prefs) {
  Object object;
  object.set("schema_version", static_cast<int64_t>(prefs.schema_version));
  object.set("window", WindowPrefsToObject(prefs.window));
  object.set("safe_area", SafeAreaToObject(prefs.safe_area));
  object.set("display", DisplayPrefsToObject(prefs.display));
  object.set("collect_run_logs", prefs.collect_run_logs);
  object.set("collect_crash_logs", prefs.collect_crash_logs);
  if (prefs.show_tester_options) {
    object.set("show_tester_options", true);
  }
  if (prefs.update_card_dismissals > 0 && !prefs.update_card_version.empty()) {
    object.set("update_card_version", prefs.update_card_version);
    object.set("update_card_dismissals", static_cast<int64_t>(prefs.update_card_dismissals));
    object.set("update_card_dismissed_ms", prefs.update_card_dismissed_ms);
  }
  return object;
}

void MachinePrefsFromObject(const Object& object, MachinePreferences& prefs) {
  if (auto schema_version = ReadI64(object, "schema_version")) {
    prefs.schema_version = static_cast<int>(*schema_version);
  }
  if (const Object* window = object.getObject("window")) {
    WindowPrefsFromObject(*window, prefs.window);
  }
  if (const Object* safe_area = object.getObject("safe_area")) {
    SafeAreaFromObject(*safe_area, prefs.safe_area);
  }
  if (const Object* display = object.getObject("display")) {
    DisplayPrefsFromObject(*display, prefs.display);
  }
  prefs.show_tester_options = object.getIf<bool>("show_tester_options").value_or(false);
  if (auto collect = object.getIf<bool>("collect_run_logs")) {
    prefs.collect_run_logs = *collect;
  }
  if (auto collect = object.getIf<bool>("collect_crash_logs")) {
    prefs.collect_crash_logs = *collect;
  }
  if (auto version = object.getString("update_card_version")) {
    prefs.update_card_version = *version;
  }
  if (auto dismissals = ReadI64(object, "update_card_dismissals")) {
    prefs.update_card_dismissals = static_cast<int>(std::clamp<int64_t>(*dismissals, 0, 2));
  }
  if (auto dismissed_ms = ReadI64(object, "update_card_dismissed_ms")) {
    prefs.update_card_dismissed_ms = std::max<int64_t>(0, *dismissed_ms);
  }
}

Object ProfilePrefsToObject(const ProfilePreferences& prefs) {
  std::vector<Value> recent;
  recent.reserve(prefs.recent_emojis.size());
  for (const std::string& emoji : prefs.recent_emojis) {
    recent.emplace_back(emoji);
  }

  Object object;
  object.set("schema_version", static_cast<int64_t>(prefs.schema_version));
  object.set("theme", prefs.theme);
  object.set("appearance", prefs.appearance);
  object.set("language", prefs.language);
  object.set("pin_is_default", prefs.pin_is_default);
  if (prefs.pin_digits) {
    object.set("pin_digits", static_cast<int64_t>(*prefs.pin_digits));
  }
  object.set("recovery_phrase_seen", prefs.recovery_phrase_seen);
  object.set("recovery_reminder_dismissals", static_cast<int64_t>(prefs.recovery_reminder_dismissals));
  object.set("recovery_reminder_dismissed_ms", prefs.recovery_reminder_dismissed_ms);
  object.set("composer_voice_mode_last", prefs.composer_voice_mode_last);
  object.set("account_sync_pending", prefs.account_sync_pending);
  object.set("unlock_grace_minutes", static_cast<int64_t>(prefs.unlock_grace_minutes));
  object.set("auto_renew_registration", prefs.auto_renew_registration);
  object.set("show_notifications", prefs.show_notifications);
  object.set("show_notification_preview", prefs.show_notification_preview);
  object.set("stay_connected", prefs.stay_connected);
  object.set("call_diagnostics", prefs.call_diagnostics);
  object.set("crash_reports_enabled", prefs.crash_reports_enabled);
  object.set("group_invite_policy", prefs.group_invite_policy);
  object.set("call_invite_policy", prefs.call_invite_policy);
  object.set("attachment_download_policy", prefs.attachment_download_policy);
  object.set("attachment_auto_on_wifi", prefs.attachment_auto_on_wifi);
  object.set("attachment_auto_on_cellular", prefs.attachment_auto_on_cellular);
  object.set("show_advanced_settings", prefs.show_advanced_settings);
  object.set("onboarding_completed", prefs.onboarding_completed);
  object.set("ai_panel_intro_seen", prefs.ai_panel_intro_seen);
  object.set("ai_dot_shown", prefs.ai_dot_shown);
  object.set("ai_dot_look", prefs.ai_dot_look);
  object.set("ai_dot_size", prefs.ai_dot_size);
  object.set("ai_dot_left", prefs.ai_dot_left);
  object.set("ai_dot_y_permille", static_cast<int64_t>(prefs.ai_dot_y_permille));
  object.set("ai_dot_tucked", prefs.ai_dot_tucked);
  object.set("reduce_transparency", prefs.reduce_transparency);
  object.set("compact_chrome_frost", prefs.compact_chrome_frost);
  object.set("sidebar_width_dp", static_cast<int64_t>(prefs.sidebar_width_dp));
  object.set("sidebar_collapsed", prefs.sidebar_collapsed);
  object.set("auxiliary_width_dp", static_cast<int64_t>(prefs.auxiliary_width_dp));
  object.set("reachability_nudge_acked_status", prefs.reachability_nudge_acked_status);
  object.set("tool_permissions", ToolPermissionsToObject(prefs.tool_permissions));
  object.set("recent_emojis", makeArray(std::move(recent)));
  std::vector<Value> hidden_widgets;
  for (const std::string& id : prefs.home_widgets_hidden) {
    hidden_widgets.emplace_back(id);
  }
  object.set("home_widgets_hidden", makeArray(std::move(hidden_widgets)));
  std::vector<Value> widget_order;
  for (const std::string& id : prefs.home_widgets_order) {
    widget_order.emplace_back(id);
  }
  object.set("home_widgets_order", makeArray(std::move(widget_order)));
  object.set("home_show_dynamic", prefs.home_show_dynamic);
  object.set("home_animated_logo", prefs.home_animated_logo);
  std::vector<Value> starred;
  for (const std::string& id : prefs.starred_contacts) {
    starred.emplace_back(id);
  }
  object.set("starred_contacts", makeArray(std::move(starred)));
  return object;
}

void ProfilePrefsFromObject(const Object& object, ProfilePreferences& prefs) {
  if (auto schema_version = ReadI64(object, "schema_version")) {
    prefs.schema_version = static_cast<int>(*schema_version);
  }
  if (auto theme = object.getString("theme")) {
    prefs.theme = NormalizeThemePath(*theme);
  }
  if (auto appearance = object.getString("appearance")) {
    prefs.appearance = *appearance;
  }
  if (auto language = object.getString("language")) {
    prefs.language = *language;
  } else {
    prefs.language = "system";
  }
  if (auto pin_is_default = object.getIf<bool>("pin_is_default")) {
    prefs.pin_is_default = *pin_is_default;
  }
  // Absent or not 0 / 4..8: unknown (the next PIN unlock writes it).
  const std::optional<int64_t> pin_digits = ReadI64(object, "pin_digits");
  prefs.pin_digits = (pin_digits && (*pin_digits == 0 || (*pin_digits >= 4 && *pin_digits <= 8)))
                         ? std::optional<int>(static_cast<int>(*pin_digits))
                         : std::nullopt;
  prefs.recovery_phrase_seen = object.getIf<bool>("recovery_phrase_seen").value_or(false);
  prefs.recovery_reminder_dismissals =
      static_cast<int>(std::clamp<int64_t>(ReadI64(object, "recovery_reminder_dismissals").value_or(0), 0, 2));
  prefs.recovery_reminder_dismissed_ms =
      std::max<int64_t>(0, ReadI64(object, "recovery_reminder_dismissed_ms").value_or(0));
  prefs.composer_voice_mode_last = object.getIf<bool>("composer_voice_mode_last").value_or(false);
  prefs.account_sync_pending = object.getIf<bool>("account_sync_pending").value_or(false);
  const int64_t grace_minutes = ReadI64(object, "unlock_grace_minutes").value_or(15);
  prefs.unlock_grace_minutes = (grace_minutes == 0 || grace_minutes == 5 || grace_minutes == 15 ||
                                grace_minutes == 60 || grace_minutes == kUnlockGraceAlways)
                                   ? static_cast<int>(grace_minutes)
                                   : 15;
  if (auto auto_renew = object.getIf<bool>("auto_renew_registration")) {
    prefs.auto_renew_registration = *auto_renew;
  } else {
    prefs.auto_renew_registration = true;
  }
  prefs.stay_connected = object.getIf<bool>("stay_connected").value_or(false);
  if (auto show_notifications = object.getIf<bool>("show_notifications")) {
    prefs.show_notifications = *show_notifications;
  } else {
    prefs.show_notifications = true;
  }
  prefs.show_notification_preview = object.getIf<bool>("show_notification_preview").value_or(true);
  if (auto call_diagnostics = object.getIf<bool>("call_diagnostics")) {
    prefs.call_diagnostics = *call_diagnostics;
  } else {
    prefs.call_diagnostics = false;
  }
  if (auto crash_reports_enabled = object.getIf<bool>("crash_reports_enabled")) {
    prefs.crash_reports_enabled = *crash_reports_enabled;
  } else {
    prefs.crash_reports_enabled = false;
  }
  // Product decision (Kenneth, 2026-11-11): both policies default to everyone.
  prefs.group_invite_policy = object.getString("group_invite_policy").value_or("everyone");
  prefs.call_invite_policy = object.getString("call_invite_policy").value_or("everyone");
  if (auto attachment_download_policy = object.getString("attachment_download_policy")) {
    prefs.attachment_download_policy = *attachment_download_policy;
  } else {
    prefs.attachment_download_policy = "smart";
  }
  prefs.attachment_auto_on_wifi = object.getIf<bool>("attachment_auto_on_wifi").value_or(true);
  prefs.attachment_auto_on_cellular = object.getIf<bool>("attachment_auto_on_cellular").value_or(false);
  if (prefs.attachment_download_policy == "always_auto" && !prefs.attachment_auto_on_wifi &&
      !prefs.attachment_auto_on_cellular) {
    prefs.attachment_download_policy = "smart";
  }
  prefs.show_advanced_settings = object.getIf<bool>("show_advanced_settings").value_or(false);
  prefs.onboarding_completed = object.getIf<bool>("onboarding_completed").value_or(false);
  prefs.ai_panel_intro_seen = object.getIf<bool>("ai_panel_intro_seen").value_or(false);
  prefs.ai_dot_shown = object.getIf<bool>("ai_dot_shown").value_or(true);
  if (auto look = object.getString("ai_dot_look")) {
    prefs.ai_dot_look = *look;
  }
  if (prefs.ai_dot_look != "auto" &&
      !(prefs.ai_dot_look.size() == 1 && prefs.ai_dot_look[0] >= 'a' && prefs.ai_dot_look[0] <= 'g')) {
    prefs.ai_dot_look = "auto";
  }
  if (auto size = object.getString("ai_dot_size")) {
    prefs.ai_dot_size = *size;
  }
  if (prefs.ai_dot_size != "s" && prefs.ai_dot_size != "m" && prefs.ai_dot_size != "l") {
    prefs.ai_dot_size = "m";
  }
  prefs.ai_dot_left = object.getIf<bool>("ai_dot_left").value_or(false);
  prefs.ai_dot_y_permille =
      static_cast<int>(std::clamp<int64_t>(ReadI64(object, "ai_dot_y_permille").value_or(600), 0, 1000));
  prefs.ai_dot_tucked = object.getIf<bool>("ai_dot_tucked").value_or(false);
  if (auto reduce_transparency = object.getIf<bool>("reduce_transparency")) {
    prefs.reduce_transparency = *reduce_transparency;
  } else {
    prefs.reduce_transparency = false;
  }
  if (auto compact_chrome_frost = object.getIf<bool>("compact_chrome_frost")) {
    prefs.compact_chrome_frost = *compact_chrome_frost;
  } else {
    prefs.compact_chrome_frost = true;
  }
  if (auto sidebar_width = ReadI64(object, "sidebar_width_dp")) {
    prefs.sidebar_width_dp = static_cast<int>(*sidebar_width);
  } else {
    prefs.sidebar_width_dp = 320;
  }
  if (auto sidebar_collapsed = object.getIf<bool>("sidebar_collapsed")) {
    prefs.sidebar_collapsed = *sidebar_collapsed;
  } else {
    prefs.sidebar_collapsed = false;
  }
  if (auto auxiliary_width = ReadI64(object, "auxiliary_width_dp")) {
    prefs.auxiliary_width_dp = static_cast<int>(*auxiliary_width);
  } else {
    prefs.auxiliary_width_dp = 320;
  }
  if (auto acked = object.getString("reachability_nudge_acked_status")) {
    prefs.reachability_nudge_acked_status = *acked;
  } else {
    prefs.reachability_nudge_acked_status.clear();
  }
  if (const Object* tool_permissions = object.getObject("tool_permissions")) {
    prefs.tool_permissions = ToolPermissionsFromObject(*tool_permissions);
  } else {
    prefs.tool_permissions = ToolPermissionsPrefs{};
  }
  prefs.recent_emojis.clear();
  if (const Array* recent = object.getArray("recent_emojis")) {
    for (const Value& item : recent->elements) {
      if (auto emoji = asString(item)) {
        prefs.recent_emojis.push_back(*emoji);
      }
    }
  }
  prefs.home_widgets_hidden.clear();
  if (const Array* hidden = object.getArray("home_widgets_hidden")) {
    for (const Value& item : hidden->elements) {
      if (auto id = asString(item)) {
        prefs.home_widgets_hidden.push_back(*id);
      }
    }
  }
  prefs.home_widgets_order.clear();
  if (const Array* order = object.getArray("home_widgets_order")) {
    for (const Value& item : order->elements) {
      if (auto id = asString(item)) {
        prefs.home_widgets_order.push_back(*id);
      }
    }
  }
  prefs.home_show_dynamic = object.getIf<bool>("home_show_dynamic").value_or(false);
  prefs.home_animated_logo = object.getIf<bool>("home_animated_logo").value_or(true);
  prefs.starred_contacts.clear();
  if (const Array* starred = object.getArray("starred_contacts")) {
    for (const Value& item : starred->elements) {
      if (auto id = asString(item); id && !id->empty() &&
                                    std::find(prefs.starred_contacts.begin(), prefs.starred_contacts.end(), *id) ==
                                        prefs.starred_contacts.end()) {
        prefs.starred_contacts.push_back(*id);
      }
    }
  }
}

AppConfig MergeConfig(const AppConfig& defaults, const Object& overlay) {
  Object merged = AppConfigToObject(defaults);
  DeepMergeObject(merged, overlay);
  AppConfig config = defaults;
  AppConfigFromObject(merged, config);
  return config;
}

void ResolveConfigCredentials(AppConfig& config) {
  if (!config.llm_api_key_env.empty() && config.llm.api_key.empty() && IsApiKeyEnvName(config.llm_api_key_env)) {
    config.llm.api_key = ICredentialStore::Instance().Get(config.llm_api_key_env);
    config.llm.require_api_key = true;
  }
}

Object ConfigToObject(const AppConfig& config, int config_version) {
  Object root = AppConfigToObject(config);
  root.set("config_version", static_cast<int64_t>(config_version));
  return root;
}

} // namespace pbr
