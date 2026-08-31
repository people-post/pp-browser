#include "base/data/ConfigJson.h"

#include "base/platform/ICredentialStore.h"
#include "common/ValueJson.h"

#include <limits>
#include "common/PbrCompat.h"

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

Object RelayPricingToObject(const RelayPricingConfig& pricing) {
  Object object;
  object.set("mode", pricing.mode);
  object.set("rate", pricing.rate);
  return object;
}

void RelayPricingFromObject(const Object& object, RelayPricingConfig& pricing) {
  if (auto mode = object.getString("mode")) {
    pricing.mode = *mode;
  }
  if (auto rate = object.getIf<double>("rate")) {
    pricing.rate = *rate;
  } else if (auto rate_i = ReadI64(object, "rate")) {
    pricing.rate = static_cast<double>(*rate_i);
  }
}

Object Libp2pPricingToObject(const Libp2pPricingConfig& pricing) {
  Object object;
  object.set("media_relay", RelayPricingToObject(pricing.media_relay));
  return object;
}

void Libp2pPricingFromObject(const Object& object, Libp2pPricingConfig& pricing) {
  if (const Object* media_relay = object.getObject("media_relay")) {
    RelayPricingFromObject(*media_relay, pricing.media_relay);
  }
}

Object MediaRelayBudgetToObject(const MediaRelayBudgetConfig& budget) {
  auto set_bps = [](Object& object, const char* key, int64_t value) {
    if (value <= 0) {
      object.set(key, Null{});
    } else {
      object.set(key, value);
    }
  };
  Object object;
  set_bps(object, "node_capacity_up_bps", budget.node_capacity_up_bps);
  set_bps(object, "node_capacity_down_bps", budget.node_capacity_down_bps);
  set_bps(object, "max_session_up_bps", budget.max_session_up_bps);
  set_bps(object, "max_session_down_bps", budget.max_session_down_bps);
  set_bps(object, "default_per_user_up_bps", budget.default_per_user_up_bps);
  set_bps(object, "default_per_user_down_bps", budget.default_per_user_down_bps);
  return object;
}

void MediaRelayBudgetFromObject(const Object& object, MediaRelayBudgetConfig& budget) {
  auto read_bps = [&](const char* key, int64_t& out) {
    if (!object.contains(key) || object.isNull(key)) {
      return;
    }
    if (auto value = ReadI64(object, key)) {
      out = *value;
    }
  };
  read_bps("node_capacity_up_bps", budget.node_capacity_up_bps);
  read_bps("node_capacity_down_bps", budget.node_capacity_down_bps);
  read_bps("max_session_up_bps", budget.max_session_up_bps);
  read_bps("max_session_down_bps", budget.max_session_down_bps);
  read_bps("default_per_user_up_bps", budget.default_per_user_up_bps);
  read_bps("default_per_user_down_bps", budget.default_per_user_down_bps);
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

Object LlmConfigToObject(const LlmConfig& config) {
  Object object;
  object.set("base_url", config.base_url);
  object.set("model", config.model);
  object.set("require_api_key", config.require_api_key);
  object.set("num_predict", static_cast<int64_t>(config.num_predict));
  if (!config.preset.empty()) {
    object.set("preset", config.preset);
  }
  if (!config.api_key.empty()) {
    object.set("api_key", config.api_key);
  }
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
  if (auto api_key = object.getString("api_key")) {
    config.api_key = *api_key;
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
  if (!config.api_key.empty()) {
    object.set("api_key", config.api_key);
  }
  return object;
}

void SearchConfigFromObject(const Object& object, SearchConfig& config) {
  if (auto provider = object.getString("provider")) {
    config.provider = *provider;
  }
  if (auto api_key = object.getString("api_key")) {
    config.api_key = *api_key;
  }
}

Object ServiceEndpointToObject(const ServiceEndpointConfig& endpoint) {
  Object object;
  object.set("base_url", endpoint.base_url);
  object.set("transport", endpoint.transport);
  return object;
}

void ServiceEndpointFromObject(const Object& object, ServiceEndpointConfig& endpoint) {
  if (auto base_url = object.getString("base_url")) {
    endpoint.base_url = *base_url;
  }
  if (auto transport = object.getString("transport")) {
    endpoint.transport = *transport;
  }
}

Object Libp2pCapabilitiesToObject(const Libp2pCapabilities& caps) {
  Object object;
  object.set("circuit_relay", caps.circuit_relay);
  object.set("media_relay", caps.media_relay);
  return object;
}

void Libp2pCapabilitiesFromObject(const Object& object, Libp2pCapabilities& caps) {
  if (auto circuit_relay = object.getIf<bool>("circuit_relay")) {
    caps.circuit_relay = *circuit_relay;
  }
  if (auto media_relay = object.getIf<bool>("media_relay")) {
    caps.media_relay = *media_relay;
  }
}

Object Libp2pConfigToObject(const Libp2pConfig& config) {
  std::vector<Value> peers;
  peers.reserve(config.bootstrap_peers.size());
  for (const std::string& peer : config.bootstrap_peers) {
    peers.emplace_back(peer);
  }
  std::vector<Value> advertise;
  advertise.reserve(config.advertise_multiaddrs.size());
  for (const std::string& ma : config.advertise_multiaddrs) {
    advertise.emplace_back(ma);
  }

  Object object;
  object.set("listen_multiaddr", config.listen_multiaddr);
  object.set("node_enabled", config.node_enabled);
  object.set("bootstrap_peers", makeArray(std::move(peers)));
  object.set("advertise_multiaddrs", makeArray(std::move(advertise)));
  object.set("mesh_publish", config.mesh_publish);
  object.setJsonUInt("max_connections", config.max_connections);
  object.setJsonUInt("max_concurrent_dials", config.max_concurrent_dials);
  object.set("dial_timeout_ms", static_cast<int64_t>(config.dial_timeout_ms));
  object.set("idle_ttl_ms", static_cast<int64_t>(config.idle_ttl_ms));
  object.set("dial_failure_backoff_ms", static_cast<int64_t>(config.dial_failure_backoff_ms));
  object.set("prefer_contacts_for_routing", config.prefer_contacts_for_routing);
  object.set("enable_amp_stack", config.enable_amp_stack);
  object.set("amp_udp_port", static_cast<int64_t>(config.amp_udp_port));
  object.set("capabilities", Libp2pCapabilitiesToObject(config.capabilities));
  object.set("pricing", Libp2pPricingToObject(config.pricing));
  object.set("media_relay_budget", MediaRelayBudgetToObject(config.media_relay_budget));
  return object;
}

void Libp2pConfigFromObject(const Object& object, Libp2pConfig& config) {
  if (auto listen_multiaddr = object.getString("listen_multiaddr")) {
    config.listen_multiaddr = *listen_multiaddr;
  }
  if (auto node_enabled = object.getIf<bool>("node_enabled")) {
    config.node_enabled = *node_enabled;
  }
  if (const Array* peers = object.getArray("bootstrap_peers")) {
    config.bootstrap_peers.clear();
    for (const Value& item : peers->elements) {
      if (auto peer = asString(item)) {
        config.bootstrap_peers.push_back(*peer);
      }
    }
  }
  if (const Array* advertise = object.getArray("advertise_multiaddrs")) {
    config.advertise_multiaddrs.clear();
    for (const Value& item : advertise->elements) {
      if (auto ma = asString(item)) {
        config.advertise_multiaddrs.push_back(*ma);
      }
    }
  }
  if (auto mesh_publish = object.getIf<bool>("mesh_publish")) {
    config.mesh_publish = *mesh_publish;
  }
  if (auto max_connections = object.getNonNegInt("max_connections")) {
    config.max_connections = static_cast<size_t>(*max_connections);
  }
  if (auto max_concurrent_dials = object.getNonNegInt("max_concurrent_dials")) {
    config.max_concurrent_dials = static_cast<size_t>(*max_concurrent_dials);
  }
  if (auto dial_timeout_ms = ReadI64(object, "dial_timeout_ms")) {
    config.dial_timeout_ms = static_cast<int>(*dial_timeout_ms);
  }
  if (auto idle_ttl_ms = ReadI64(object, "idle_ttl_ms")) {
    config.idle_ttl_ms = static_cast<int>(*idle_ttl_ms);
  }
  if (auto dial_failure_backoff_ms = ReadI64(object, "dial_failure_backoff_ms")) {
    config.dial_failure_backoff_ms = static_cast<int>(*dial_failure_backoff_ms);
  }
  if (auto prefer = object.getIf<bool>("prefer_contacts_for_routing")) {
    config.prefer_contacts_for_routing = *prefer;
  }
  if (auto enable_amp = object.getIf<bool>("enable_amp_stack")) {
    config.enable_amp_stack = *enable_amp;
  }
  if (auto amp_udp_port = object.getNonNegInt("amp_udp_port")) {
    config.amp_udp_port = static_cast<int>(*amp_udp_port);
  }
  if (const Object* capabilities = object.getObject("capabilities")) {
    Libp2pCapabilitiesFromObject(*capabilities, config.capabilities);
  }
  if (const Object* pricing = object.getObject("pricing")) {
    Libp2pPricingFromObject(*pricing, config.pricing);
  }
  if (const Object* media_relay_budget = object.getObject("media_relay_budget")) {
    MediaRelayBudgetFromObject(*media_relay_budget, config.media_relay_budget);
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
}

Object AppConfigToObject(const AppConfig& config) {
  Object object;
  object.set("theme", config.theme);
  Object llm = LlmConfigToObject(config.llm);
  if (!config.llm_api_key_env.empty()) {
    llm.set("api_key_env", config.llm_api_key_env);
    llm.erase("api_key");
  }
  object.set("llm", llm);
  object.set("search", SearchConfigToObject(config.search));
  object.set("context", ContextBudgetToObject(config.context));
  object.set("relay", ServiceEndpointToObject(config.relay));
  object.set("directory", ServiceEndpointToObject(config.directory));
  object.set("registration", ServiceEndpointToObject(config.registration));
  object.set("libp2p", Libp2pConfigToObject(config.libp2p));
  object.set("initiation_floor", config.initiation_floor);
  if (!config.data_dir.empty()) {
    object.set("data_dir", config.data_dir);
  }
  if (config.promoted_mcp.IsConfigured()) {
    object.set("promoted_mcp", McpConfigToObject(config.promoted_mcp));
  }
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
    } else if (auto api_key = llm->getString("api_key")) {
      config.llm.api_key = *api_key;
      config.llm_api_key_env.clear();
    }
  }
  if (const Object* promoted_mcp = object.getObject("promoted_mcp")) {
    config.promoted_mcp = ParseMcpObject(*promoted_mcp);
  } else if (const Object* mcp = object.getObject("mcp")) {
    config.promoted_mcp = ParseMcpObject(*mcp);
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
    if (auto api_key_env = search->getString("api_key_env")) {
      config.search.api_key = ICredentialStore::Instance().Get(*api_key_env);
    }
  }
  if (auto data_dir = object.getString("data_dir")) {
    config.data_dir = *data_dir;
  }
  if (const Object* relay = object.getObject("relay")) {
    ServiceEndpointFromObject(*relay, config.relay);
  }
  if (const Object* directory = object.getObject("directory")) {
    ServiceEndpointFromObject(*directory, config.directory);
  }
  if (const Object* registration = object.getObject("registration")) {
    ServiceEndpointFromObject(*registration, config.registration);
  }
  if (auto initiation_floor = ReadI64(object, "initiation_floor")) {
    config.initiation_floor = *initiation_floor;
  }
  if (const Object* libp2p = object.getObject("libp2p")) {
    Libp2pConfigFromObject(*libp2p, config.libp2p);
  }
}

Object MachinePrefsToObject(const MachinePreferences& prefs) {
  Object object;
  object.set("schema_version", static_cast<int64_t>(prefs.schema_version));
  object.set("active_profile_id", prefs.active_profile_id);
  object.set("window", WindowPrefsToObject(prefs.window));
  object.set("safe_area", SafeAreaToObject(prefs.safe_area));
  object.set("display", DisplayPrefsToObject(prefs.display));
  return object;
}

void MachinePrefsFromObject(const Object& object, MachinePreferences& prefs) {
  if (auto schema_version = ReadI64(object, "schema_version")) {
    prefs.schema_version = static_cast<int>(*schema_version);
  }
  if (auto active_profile_id = object.getString("active_profile_id")) {
    prefs.active_profile_id = *active_profile_id;
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
  object.set("auto_renew_registration", prefs.auto_renew_registration);
  object.set("show_notifications", prefs.show_notifications);
  object.set("call_diagnostics", prefs.call_diagnostics);
  object.set("group_invite_policy", prefs.group_invite_policy);
  object.set("attachment_download_policy", prefs.attachment_download_policy);
  object.set("reduce_transparency", prefs.reduce_transparency);
  object.set("compact_chrome_frost", prefs.compact_chrome_frost);
  object.set("reachability_nudge_acked_status", prefs.reachability_nudge_acked_status);
  object.set("tool_permissions", ToolPermissionsToObject(prefs.tool_permissions));
  object.set("recent_emojis", makeArray(std::move(recent)));
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
  if (auto auto_renew = object.getIf<bool>("auto_renew_registration")) {
    prefs.auto_renew_registration = *auto_renew;
  } else {
    prefs.auto_renew_registration = true;
  }
  if (auto show_notifications = object.getIf<bool>("show_notifications")) {
    prefs.show_notifications = *show_notifications;
  } else {
    prefs.show_notifications = true;
  }
  if (auto call_diagnostics = object.getIf<bool>("call_diagnostics")) {
    prefs.call_diagnostics = *call_diagnostics;
  } else {
    prefs.call_diagnostics = false;
  }
  if (auto group_invite_policy = object.getString("group_invite_policy")) {
    prefs.group_invite_policy = *group_invite_policy;
  } else {
    prefs.group_invite_policy = "contacts_only";
  }
  if (auto attachment_download_policy = object.getString("attachment_download_policy")) {
    prefs.attachment_download_policy = *attachment_download_policy;
  } else {
    prefs.attachment_download_policy = "smart";
  }
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
}

AppConfig MergeConfig(const AppConfig& defaults, const Object& overlay) {
  Object merged = AppConfigToObject(defaults);
  DeepMergeObject(merged, overlay);
  AppConfig config = defaults;
  AppConfigFromObject(merged, config);
  return config;
}

void ResolveConfigCredentials(AppConfig& config) {
  if (!config.llm_api_key_env.empty() && config.llm.api_key.empty()) {
    config.llm.api_key = ICredentialStore::Instance().Get(config.llm_api_key_env);
    config.llm.require_api_key = true;
  }
}

Object ConfigToObject(const AppConfig& config, int config_version) {
  Object root = AppConfigToObject(config);
  root.set("config_version", static_cast<int64_t>(config_version));
  if (const Object* llm_ptr = root.getObject("llm")) {
    Object llm = *llm_ptr;
    if (!config.llm.api_key.empty()) {
      llm.set("api_key", config.llm.api_key);
      llm.erase("api_key_env");
    } else if (!config.llm_api_key_env.empty()) {
      llm.set("api_key_env", config.llm_api_key_env);
      llm.erase("api_key");
    }
    root.set("llm", llm);
  }
  return root;
}

} // namespace pbr
