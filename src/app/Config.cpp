#include "app/Config.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

std::string ConfigPath(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }

  if (const char* env = std::getenv("PP_BROWSER_CONFIG")) {
    return env;
  }

  if (std::filesystem::exists("config.json")) {
    return "config.json";
  }

  return {};
}

} // namespace

Config::Config() {
  redirectLogger("Config");
}

Config& Config::Instance() {
  static Config config;
  return config;
}

Roe<AppConfig> Config::Load(int argc, char** argv) {
  auto& logger = Instance().log();
  const std::string path = ConfigPath(argc, argv);
  if (path.empty()) {
    logger.info << "No config.json found; using Ollama defaults (http://localhost:11434/v1). "
                << "Copy config.json.example to config.json to customize the model.";
    return DefaultOllama();
  }

  auto config = LoadFromFile(path);
  if (!config) {
    return Error("Failed to read config: " + path + ": " + config.error().message);
  }

  logger.info << "Loaded config from " << path << " (model: " << config->llm.model << ")";
  return config.value();
}

AppConfig Config::DefaultOllama() {
  AppConfig config;
  config.llm.base_url = "http://localhost:11434/v1";
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "llama3.2";
  }
  config.llm.require_api_key = false;
  return config;
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

  AppConfig config;
  if (root.contains("theme") && root["theme"].is_string()) {
    config.theme = root["theme"].get<std::string>();
  }

  if (!root.contains("llm") || !root["llm"].is_object()) {
    return config;
  }

  const auto& llm = root["llm"];
  if (llm.contains("base_url") && llm["base_url"].is_string()) {
    config.llm.base_url = llm["base_url"].get<std::string>();
  }
  if (llm.contains("model") && llm["model"].is_string()) {
    config.llm.model = llm["model"].get<std::string>();
  }

  if (llm.contains("api_key") && llm["api_key"].is_string()) {
    config.llm.api_key = llm["api_key"].get<std::string>();
  } else if (llm.contains("api_key_env") && llm["api_key_env"].is_string()) {
    config.llm.require_api_key = true;
    config.llm.api_key = ReadEnv(llm["api_key_env"].get<std::string>().c_str());
  }

  if (llm.contains("require_api_key") && llm["require_api_key"].is_boolean()) {
    config.llm.require_api_key = llm["require_api_key"].get<bool>();
  }
  if (llm.contains("num_predict") && llm["num_predict"].is_number_integer()) {
    config.llm.num_predict = llm["num_predict"].get<int>();
  }

  auto load_mcp = [](const nlohmann::json& mcp_json) -> McpConfig {
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
  };

  if (root.contains("mcp") && root["mcp"].is_object()) {
    McpConfig mcp = load_mcp(root["mcp"]);
    if (mcp.IsConfigured()) {
      config.mcp = std::move(mcp);
    }
  } else if (root.contains("mcp_servers") && root["mcp_servers"].is_array() && !root["mcp_servers"].empty()) {
    const auto& first = root["mcp_servers"][0];
    if (first.is_object()) {
      McpConfig mcp = load_mcp(first);
      if (mcp.IsConfigured()) {
        config.mcp = std::move(mcp);
      }
    }
  }

  if (root.contains("context") && root["context"].is_object()) {
    const auto& context = root["context"];
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

  if (root.contains("search") && root["search"].is_object()) {
    const auto& search = root["search"];
    if (search.contains("provider") && search["provider"].is_string()) {
      config.search.provider = search["provider"].get<std::string>();
    }
    if (search.contains("api_key") && search["api_key"].is_string()) {
      config.search.api_key = search["api_key"].get<std::string>();
    } else if (search.contains("api_key_env") && search["api_key_env"].is_string()) {
      config.search.api_key = ReadEnv(search["api_key_env"].get<std::string>().c_str());
    }
  }

  if (root.contains("data_dir") && root["data_dir"].is_string()) {
    config.data_dir = root["data_dir"].get<std::string>();
  }

  auto load_endpoint = [&](const char* key, ServiceEndpointConfig& endpoint) {
    if (root.contains(key) && root[key].is_object()) {
      const auto& obj = root[key];
      if (obj.contains("base_url") && obj["base_url"].is_string()) {
        endpoint.base_url = obj["base_url"].get<std::string>();
      }
    }
  };
  load_endpoint("relay", config.relay);
  load_endpoint("directory", config.directory);
  load_endpoint("registration", config.registration);

  return config;
}

} // namespace pbr
