#include "app/Config.h"

#include "app/AppPaths.h"
#include "platform/ICredentialStore.h"
#include "platform/Platform.h"
#include "platform/PlatformDefaults.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

void MergeLlm(const nlohmann::json& llm, AppConfig& config) {
  if (llm.contains("base_url") && llm["base_url"].is_string()) {
    config.llm.base_url = llm["base_url"].get<std::string>();
  }
  if (llm.contains("model") && llm["model"].is_string()) {
    config.llm.model = llm["model"].get<std::string>();
  }
  if (llm.contains("api_key") && llm["api_key"].is_string()) {
    config.llm.api_key = llm["api_key"].get<std::string>();
  } else if (llm.contains("api_key_env") && llm["api_key_env"].is_string()) {
    config.llm_api_key_env = llm["api_key_env"].get<std::string>();
    config.llm.require_api_key = true;
    config.llm.api_key = ICredentialStore::Instance().Get(config.llm_api_key_env);
  }
  if (llm.contains("require_api_key") && llm["require_api_key"].is_boolean()) {
    config.llm.require_api_key = llm["require_api_key"].get<bool>();
  }
  if (llm.contains("num_predict") && llm["num_predict"].is_number_integer()) {
    config.llm.num_predict = llm["num_predict"].get<int>();
  }
}

McpConfig LoadMcp(const nlohmann::json& mcp_json) {
  McpConfig mcp;
  if (mcp_json.contains("command") && mcp_json["command"].is_string()) {
    mcp.command = mcp_json["command"].get<std::string>();
  }
  if (mcp_json.contains("url") && mcp_json["url"].is_string()) {
    mcp.url = mcp_json["url"].get<std::string>();
  }
  if (mcp_json.contains("args") && mcp_json["args"].is_array()) {
    for (const auto& arg : mcp_json["args"]) {
      if (arg.is_string()) {
        mcp.args.push_back(arg.get<std::string>());
      }
    }
  }
  return mcp;
}

void MergeContext(const nlohmann::json& context, AppConfig& config) {
  if (context.contains("max_turn_pairs") && context["max_turn_pairs"].is_number_integer()) {
    config.context.max_turn_pairs = context["max_turn_pairs"].get<int>();
  }
  if (context.contains("max_recent_chars") && context["max_recent_chars"].is_number_integer()) {
    config.context.max_recent_chars = context["max_recent_chars"].get<int>();
  }
  if (context.contains("max_input_tokens") && context["max_input_tokens"].is_number_integer()) {
    config.context.max_input_tokens = context["max_input_tokens"].get<int>();
  }
  if (context.contains("token_estimate_margin") && context["token_estimate_margin"].is_number()) {
    config.context.token_estimate_margin = context["token_estimate_margin"].get<double>();
  }
  if (context.contains("max_summary_chars") && context["max_summary_chars"].is_number_integer()) {
    config.context.max_summary_chars = context["max_summary_chars"].get<int>();
  }
}

void MergeSearch(const nlohmann::json& search, AppConfig& config) {
  if (search.contains("provider") && search["provider"].is_string()) {
    config.search.provider = search["provider"].get<std::string>();
  }
  if (search.contains("api_key") && search["api_key"].is_string()) {
    config.search.api_key = search["api_key"].get<std::string>();
  } else if (search.contains("api_key_env") && search["api_key_env"].is_string()) {
    config.search.api_key = ICredentialStore::Instance().Get(search["api_key_env"].get<std::string>());
  }
}

void MergeEndpoint(const nlohmann::json& root, const char* key, ServiceEndpointConfig& endpoint) {
  if (root.contains(key) && root[key].is_object()) {
    const auto& obj = root[key];
    if (obj.contains("base_url") && obj["base_url"].is_string()) {
      endpoint.base_url = obj["base_url"].get<std::string>();
    }
  }
}

void MergeJsonIntoConfig(const nlohmann::json& root, AppConfig& config) {
  if (root.contains("theme") && root["theme"].is_string()) {
    std::string theme = root["theme"].get<std::string>();
    if (theme.rfind("assets/", 0) == 0) {
      theme = theme.substr(7);
    }
    config.theme = theme;
  }
  if (root.contains("llm") && root["llm"].is_object()) {
    MergeLlm(root["llm"], config);
  }
  if (root.contains("mcp") && root["mcp"].is_object()) {
    McpConfig mcp = LoadMcp(root["mcp"]);
    if (mcp.IsConfigured()) {
      config.mcp = std::move(mcp);
    }
  } else if (root.contains("mcp_servers") && root["mcp_servers"].is_array() && !root["mcp_servers"].empty()) {
    const auto& first = root["mcp_servers"][0];
    if (first.is_object()) {
      McpConfig mcp = LoadMcp(first);
      if (mcp.IsConfigured()) {
        config.mcp = std::move(mcp);
      }
    }
  }
  if (root.contains("context") && root["context"].is_object()) {
    MergeContext(root["context"], config);
  }
  if (root.contains("search") && root["search"].is_object()) {
    MergeSearch(root["search"], config);
  }
  if (root.contains("data_dir") && root["data_dir"].is_string()) {
    config.data_dir = root["data_dir"].get<std::string>();
  }
  MergeEndpoint(root, "relay", config.relay);
  MergeEndpoint(root, "directory", config.directory);
  MergeEndpoint(root, "registration", config.registration);
}

} // namespace

Config::Config() {
  redirectLogger("Config");
}

Config& Config::Instance() {
  static Config config;
  return config;
}

AppConfig Config::DefaultAppConfig() {
  return PlatformDefaults::For(Platform::Detect());
}

std::string Config::DiscoverConfigPath(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }

  if (const char* env = std::getenv("PP_BROWSER_CONFIG")) {
    return env;
  }

  const std::string canonical = AppPaths::ConfigFilePath();
  if (std::filesystem::exists(canonical)) {
    return canonical;
  }

  return {};
}

Roe<AppConfig> Config::Load(int argc, char** argv) {
  auto& logger = Instance().log();
  AppConfig config = DefaultAppConfig();

  const std::string path = DiscoverConfigPath(argc, argv);
  if (path.empty()) {
    logger.info << "No config file found; using platform defaults (model: " << config.llm.model << ")";
    return config;
  }

  auto loaded = LoadFromFile(path);
  if (!loaded) {
    return loaded.error();
  }

  logger.info << "Loaded config from " << path << " (model: " << loaded->llm.model << ")";
  return loaded.value();
}

Roe<AppConfig> Config::LoadFromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open config file: " + path);
  }

  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Failed to parse config: " + path);
  }

  if (root.contains("config_version") && root["config_version"].is_number_integer()) {
    const int version = root["config_version"].get<int>();
    if (version > kConfigVersion) {
      return Error("Unsupported config version " + std::to_string(version) + " in " + path);
    }
  }

  AppConfig config = DefaultAppConfig();
  MergeJsonIntoConfig(root, config);
  return config;
}

Roe<void> Config::SaveToFile(const std::string& path, const AppConfig& config) {
  nlohmann::json root = {
      {"config_version", kConfigVersion},
      {"theme", config.theme},
      {"llm",
       {{"base_url", config.llm.base_url},
        {"model", config.llm.model},
        {"require_api_key", config.llm.require_api_key},
        {"num_predict", config.llm.num_predict}}},
      {"search", {{"provider", config.search.provider}}},
      {"context",
       {{"max_turn_pairs", config.context.max_turn_pairs},
        {"max_recent_chars", config.context.max_recent_chars},
        {"max_input_tokens", config.context.max_input_tokens},
        {"token_estimate_margin", config.context.token_estimate_margin},
        {"max_summary_chars", config.context.max_summary_chars}}},
      {"relay", {{"base_url", config.relay.base_url}}},
      {"directory", {{"base_url", config.directory.base_url}}},
      {"registration", {{"base_url", config.registration.base_url}}},
  };

  if (!config.llm.api_key.empty()) {
    root["llm"]["api_key"] = config.llm.api_key;
  } else if (!config.llm_api_key_env.empty()) {
    root["llm"]["api_key_env"] = config.llm_api_key_env;
  }
  if (!config.search.api_key.empty()) {
    root["search"]["api_key"] = config.search.api_key;
  }
  if (!config.data_dir.empty()) {
    root["data_dir"] = config.data_dir;
  }
  if (config.mcp) {
    nlohmann::json mcp;
    if (!config.mcp->url.empty()) {
      mcp["url"] = config.mcp->url;
    }
    if (!config.mcp->command.empty()) {
      mcp["command"] = config.mcp->command;
      mcp["args"] = config.mcp->args;
    }
    root["mcp"] = std::move(mcp);
  }

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

  std::ofstream out(path);
  if (!out) {
    return Error("Failed to write config file: " + path);
  }
  out << root.dump(2);
  return {};
}

} // namespace pbr
