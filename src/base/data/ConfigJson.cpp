#include "base/data/ConfigJson.h"

#include "base/platform/ICredentialStore.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

void MergeObjectField(nlohmann::json& base, const nlohmann::json& overlay, const char* key) {
  if (!overlay.contains(key)) {
    return;
  }
  if (!overlay[key].is_object()) {
    base[key] = overlay[key];
    return;
  }
  if (!base.contains(key) || !base[key].is_object()) {
    base[key] = nlohmann::json::object();
  }
  DeepMergeJson(base[key], overlay[key]);
}

std::string NormalizeThemePath(std::string theme) {
  if (theme.rfind("assets/", 0) == 0) {
    theme = theme.substr(7);
  }
  return theme;
}

McpConfig ParseMcpJson(const nlohmann::json& mcp_json) {
  McpConfig mcp;
  from_json(mcp_json, mcp);
  return mcp;
}

} // namespace

void to_json(nlohmann::json& j, const LlmConfig& config) {
  j = nlohmann::json{{"base_url", config.base_url},
                     {"model", config.model},
                     {"require_api_key", config.require_api_key},
                     {"num_predict", config.num_predict}};
  if (!config.preset.empty()) {
    j["preset"] = config.preset;
  }
  if (!config.api_key.empty()) {
    j["api_key"] = config.api_key;
  }
}

void from_json(const nlohmann::json& j, LlmConfig& config) {
  if (j.contains("base_url") && j["base_url"].is_string()) {
    config.base_url = j["base_url"].get<std::string>();
  }
  if (j.contains("model") && j["model"].is_string()) {
    config.model = j["model"].get<std::string>();
  }
  if (j.contains("preset") && j["preset"].is_string()) {
    config.preset = j["preset"].get<std::string>();
  }
  if (j.contains("api_key") && j["api_key"].is_string()) {
    config.api_key = j["api_key"].get<std::string>();
  }
  if (j.contains("require_api_key") && j["require_api_key"].is_boolean()) {
    config.require_api_key = j["require_api_key"].get<bool>();
  }
  if (j.contains("num_predict") && j["num_predict"].is_number_integer()) {
    config.num_predict = j["num_predict"].get<int>();
  }
}

void to_json(nlohmann::json& j, const ContextBudget& budget) {
  j = nlohmann::json{{"max_turn_pairs", budget.max_turn_pairs},
                     {"max_recent_chars", budget.max_recent_chars},
                     {"max_input_tokens", budget.max_input_tokens},
                     {"token_estimate_margin", budget.token_estimate_margin},
                     {"max_summary_chars", budget.max_summary_chars}};
}

void from_json(const nlohmann::json& j, ContextBudget& budget) {
  if (j.contains("max_turn_pairs") && j["max_turn_pairs"].is_number_integer()) {
    budget.max_turn_pairs = j["max_turn_pairs"].get<int>();
  }
  if (j.contains("max_recent_chars") && j["max_recent_chars"].is_number_integer()) {
    budget.max_recent_chars = j["max_recent_chars"].get<int>();
  }
  if (j.contains("max_input_tokens") && j["max_input_tokens"].is_number_integer()) {
    budget.max_input_tokens = j["max_input_tokens"].get<int>();
  }
  if (j.contains("token_estimate_margin") && j["token_estimate_margin"].is_number()) {
    budget.token_estimate_margin = j["token_estimate_margin"].get<double>();
  }
  if (j.contains("max_summary_chars") && j["max_summary_chars"].is_number_integer()) {
    budget.max_summary_chars = j["max_summary_chars"].get<int>();
  }
}

void to_json(nlohmann::json& j, const SearchConfig& config) {
  j = nlohmann::json{{"provider", config.provider}};
  if (!config.api_key.empty()) {
    j["api_key"] = config.api_key;
  }
}

void from_json(const nlohmann::json& j, SearchConfig& config) {
  if (j.contains("provider") && j["provider"].is_string()) {
    config.provider = j["provider"].get<std::string>();
  }
  if (j.contains("api_key") && j["api_key"].is_string()) {
    config.api_key = j["api_key"].get<std::string>();
  }
}

void to_json(nlohmann::json& j, const ServiceEndpointConfig& endpoint) {
  j = nlohmann::json{{"base_url", endpoint.base_url}, {"transport", endpoint.transport}};
}

void from_json(const nlohmann::json& j, ServiceEndpointConfig& endpoint) {
  if (j.contains("base_url") && j["base_url"].is_string()) {
    endpoint.base_url = j["base_url"].get<std::string>();
  }
  if (j.contains("transport") && j["transport"].is_string()) {
    endpoint.transport = j["transport"].get<std::string>();
  }
}

void to_json(nlohmann::json& j, const Libp2pConfig& config) {
  j = nlohmann::json{{"listen_multiaddr", config.listen_multiaddr},
                     {"max_connections", config.max_connections},
                     {"max_concurrent_dials", config.max_concurrent_dials},
                     {"dial_timeout_ms", config.dial_timeout_ms},
                     {"idle_ttl_ms", config.idle_ttl_ms},
                     {"dial_failure_backoff_ms", config.dial_failure_backoff_ms}};
}

void from_json(const nlohmann::json& j, Libp2pConfig& config) {
  if (j.contains("listen_multiaddr") && j["listen_multiaddr"].is_string()) {
    config.listen_multiaddr = j["listen_multiaddr"].get<std::string>();
  }
  if (j.contains("max_connections") && j["max_connections"].is_number_unsigned()) {
    config.max_connections = j["max_connections"].get<size_t>();
  }
  if (j.contains("max_concurrent_dials") && j["max_concurrent_dials"].is_number_unsigned()) {
    config.max_concurrent_dials = j["max_concurrent_dials"].get<size_t>();
  }
  if (j.contains("dial_timeout_ms") && j["dial_timeout_ms"].is_number_integer()) {
    config.dial_timeout_ms = j["dial_timeout_ms"].get<int>();
  }
  if (j.contains("idle_ttl_ms") && j["idle_ttl_ms"].is_number_integer()) {
    config.idle_ttl_ms = j["idle_ttl_ms"].get<int>();
  }
  if (j.contains("dial_failure_backoff_ms") && j["dial_failure_backoff_ms"].is_number_integer()) {
    config.dial_failure_backoff_ms = j["dial_failure_backoff_ms"].get<int>();
  }
}

void to_json(nlohmann::json& j, const McpConfig& config) {
  j = nlohmann::json::object();
  if (!config.id.empty()) {
    j["id"] = config.id;
  }
  if (!config.url.empty()) {
    j["url"] = config.url;
  }
  if (!config.command.empty()) {
    j["command"] = config.command;
    j["args"] = config.args;
  }
  if (!config.enabled) {
    j["enabled"] = false;
  }
}

void from_json(const nlohmann::json& j, McpConfig& config) {
  if (j.contains("id") && j["id"].is_string()) {
    config.id = j["id"].get<std::string>();
  }
  if (j.contains("command") && j["command"].is_string()) {
    config.command = j["command"].get<std::string>();
  }
  if (j.contains("url") && j["url"].is_string()) {
    config.url = j["url"].get<std::string>();
  }
  if (j.contains("args") && j["args"].is_array()) {
    config.args.clear();
    for (const auto& arg : j["args"]) {
      if (arg.is_string()) {
        config.args.push_back(arg.get<std::string>());
      }
    }
  }
  if (j.contains("enabled") && j["enabled"].is_boolean()) {
    config.enabled = j["enabled"].get<bool>();
  }
}

void to_json(nlohmann::json& j, const AppConfig& config) {
  j = nlohmann::json{{"theme", config.theme},
                     {"llm", config.llm},
                     {"search", config.search},
                     {"context", config.context},
                     {"relay", config.relay},
                     {"directory", config.directory},
                     {"registration", config.registration},
                     {"libp2p", config.libp2p}};
  if (!config.llm_api_key_env.empty()) {
    j["llm"]["api_key_env"] = config.llm_api_key_env;
    j["llm"].erase("api_key");
  }
  if (!config.data_dir.empty()) {
    j["data_dir"] = config.data_dir;
  }
  if (config.promoted_mcp.IsConfigured()) {
    j["promoted_mcp"] = config.promoted_mcp;
  }
  if (!config.mcp_servers.empty()) {
    j["mcp_servers"] = config.mcp_servers;
  }
}

void from_json(const nlohmann::json& j, AppConfig& config) {
  if (j.contains("theme") && j["theme"].is_string()) {
    config.theme = NormalizeThemePath(j["theme"].get<std::string>());
  }
  if (j.contains("llm") && j["llm"].is_object()) {
    from_json(j["llm"], config.llm);
    if (j["llm"].contains("api_key_env") && j["llm"]["api_key_env"].is_string()) {
      config.llm_api_key_env = j["llm"]["api_key_env"].get<std::string>();
      config.llm.api_key.clear();
      config.llm.require_api_key = true;
    } else if (j["llm"].contains("api_key") && j["llm"]["api_key"].is_string()) {
      config.llm.api_key = j["llm"]["api_key"].get<std::string>();
      config.llm_api_key_env.clear();
    }
  }
  if (j.contains("promoted_mcp") && j["promoted_mcp"].is_object()) {
    config.promoted_mcp = ParseMcpJson(j["promoted_mcp"]);
  } else if (j.contains("mcp") && j["mcp"].is_object()) {
    config.promoted_mcp = ParseMcpJson(j["mcp"]);
  }
  if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
    config.mcp_servers.clear();
    for (const auto& item : j["mcp_servers"]) {
      if (!item.is_object()) {
        continue;
      }
      McpConfig mcp = ParseMcpJson(item);
      if (mcp.IsConfigured()) {
        config.mcp_servers.push_back(std::move(mcp));
      }
    }
  }
  if (j.contains("context") && j["context"].is_object()) {
    from_json(j["context"], config.context);
  }
  if (j.contains("search") && j["search"].is_object()) {
    from_json(j["search"], config.search);
    if (j["search"].contains("api_key_env") && j["search"]["api_key_env"].is_string()) {
      config.search.api_key =
          ICredentialStore::Instance().Get(j["search"]["api_key_env"].get<std::string>());
    }
  }
  if (j.contains("data_dir") && j["data_dir"].is_string()) {
    config.data_dir = j["data_dir"].get<std::string>();
  }
  if (j.contains("relay") && j["relay"].is_object()) {
    from_json(j["relay"], config.relay);
  }
  if (j.contains("directory") && j["directory"].is_object()) {
    from_json(j["directory"], config.directory);
  }
  if (j.contains("registration") && j["registration"].is_object()) {
    from_json(j["registration"], config.registration);
  }
  if (j.contains("libp2p") && j["libp2p"].is_object()) {
    from_json(j["libp2p"], config.libp2p);
  }
}

void to_json(nlohmann::json& j, const WindowPrefs& window) {
  j = nlohmann::json{{"width", window.width}, {"height", window.height}};
}

void from_json(const nlohmann::json& j, WindowPrefs& window) {
  if (j.contains("width") && j["width"].is_number_integer()) {
    window.width = j["width"].get<int>();
  }
  if (j.contains("height") && j["height"].is_number_integer()) {
    window.height = j["height"].get<int>();
  }
}

void to_json(nlohmann::json& j, const SafeAreaInsets& safe_area) {
  j = nlohmann::json{{"top", safe_area.top},
                     {"bottom", safe_area.bottom},
                     {"left", safe_area.left},
                     {"right", safe_area.right}};
}

void from_json(const nlohmann::json& j, SafeAreaInsets& safe_area) {
  if (j.contains("top") && j["top"].is_number_integer()) {
    safe_area.top = j["top"].get<int>();
  }
  if (j.contains("bottom") && j["bottom"].is_number_integer()) {
    safe_area.bottom = j["bottom"].get<int>();
  }
  if (j.contains("left") && j["left"].is_number_integer()) {
    safe_area.left = j["left"].get<int>();
  }
  if (j.contains("right") && j["right"].is_number_integer()) {
    safe_area.right = j["right"].get<int>();
  }
}

void to_json(nlohmann::json& j, const DisplayPrefs& display) {
  j = nlohmann::json{{"fullscreen", display.fullscreen}};
}

void from_json(const nlohmann::json& j, DisplayPrefs& display) {
  if (j.contains("fullscreen") && j["fullscreen"].is_boolean()) {
    display.fullscreen = j["fullscreen"].get<bool>();
  }
}

void to_json(nlohmann::json& j, const MachinePreferences& prefs) {
  j = nlohmann::json{{"schema_version", prefs.schema_version},
                     {"active_profile_id", prefs.active_profile_id},
                     {"window", prefs.window},
                     {"safe_area", prefs.safe_area},
                     {"display", prefs.display}};
}

void from_json(const nlohmann::json& j, MachinePreferences& prefs) {
  if (j.contains("schema_version") && j["schema_version"].is_number_integer()) {
    prefs.schema_version = j["schema_version"].get<int>();
  }
  if (j.contains("active_profile_id") && j["active_profile_id"].is_string()) {
    prefs.active_profile_id = j["active_profile_id"].get<std::string>();
  }
  if (j.contains("window") && j["window"].is_object()) {
    from_json(j["window"], prefs.window);
  }
  if (j.contains("safe_area") && j["safe_area"].is_object()) {
    from_json(j["safe_area"], prefs.safe_area);
  }
  if (j.contains("display") && j["display"].is_object()) {
    from_json(j["display"], prefs.display);
  }
}

void to_json(nlohmann::json& j, const ProfilePreferences& prefs) {
  j = nlohmann::json{{"schema_version", prefs.schema_version},
                     {"theme", prefs.theme},
                     {"appearance", prefs.appearance},
                     {"pin_is_default", prefs.pin_is_default}};
}

void from_json(const nlohmann::json& j, ProfilePreferences& prefs) {
  if (j.contains("schema_version") && j["schema_version"].is_number_integer()) {
    prefs.schema_version = j["schema_version"].get<int>();
  }
  if (j.contains("theme") && j["theme"].is_string()) {
    prefs.theme = NormalizeThemePath(j["theme"].get<std::string>());
  }
  if (j.contains("appearance") && j["appearance"].is_string()) {
    prefs.appearance = j["appearance"].get<std::string>();
  }
  if (j.contains("pin_is_default") && j["pin_is_default"].is_boolean()) {
    prefs.pin_is_default = j["pin_is_default"].get<bool>();
  }
}

void DeepMergeJson(nlohmann::json& base, const nlohmann::json& overlay) {
  if (!overlay.is_object()) {
    base = overlay;
    return;
  }
  if (!base.is_object()) {
    base = nlohmann::json::object();
  }
  for (const auto& [key, value] : overlay.items()) {
    if (value.is_object() && base.contains(key) && base[key].is_object()) {
      DeepMergeJson(base[key], value);
    } else {
      base[key] = value;
    }
  }
}

AppConfig MergeConfig(const AppConfig& defaults, const nlohmann::json& overlay) {
  nlohmann::json merged = defaults;
  DeepMergeJson(merged, overlay);
  AppConfig config = defaults;
  from_json(merged, config);
  return config;
}

void ResolveConfigCredentials(AppConfig& config) {
  if (!config.llm_api_key_env.empty() && config.llm.api_key.empty()) {
    config.llm.api_key = ICredentialStore::Instance().Get(config.llm_api_key_env);
    config.llm.require_api_key = true;
  }
}

nlohmann::json ConfigToJson(const AppConfig& config, int config_version) {
  nlohmann::json root = config;
  root["config_version"] = config_version;
  if (!config.llm.api_key.empty()) {
    root["llm"]["api_key"] = config.llm.api_key;
    root["llm"].erase("api_key_env");
  } else if (!config.llm_api_key_env.empty()) {
    root["llm"]["api_key_env"] = config.llm_api_key_env;
    root["llm"].erase("api_key");
  }
  return root;
}

nlohmann::json MachinePrefsToJson(const MachinePreferences& prefs) {
  return prefs;
}

nlohmann::json ProfilePrefsToJson(const ProfilePreferences& prefs) {
  return prefs;
}

} // namespace pbr
