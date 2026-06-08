#include "app/Config.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ppbrowser {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

} // namespace

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

std::optional<AppConfig> Config::LoadFromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

  nlohmann::json root;
  try {
    in >> root;
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("Failed to parse config: ") + path + ": " + e.what());
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

  return config;
}

} // namespace ppbrowser
