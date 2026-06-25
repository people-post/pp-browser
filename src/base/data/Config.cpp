#include "app/Config.h"

#include "app/AppPaths.h"
#include "app/ConfigJson.h"
#include "app/LlmPreset.h"
#include "platform/Platform.h"
#include "platform/PlatformDefaults.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pbr {

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

  AppConfig config = MergeConfig(DefaultAppConfig(), root);
  ResolveConfigCredentials(config);
  ResolveLlmAuthRequirements(config);
  return config;
}

Roe<void> Config::SaveToFile(const std::string& path, const AppConfig& config) {
  const nlohmann::json root = ConfigToJson(config, kConfigVersion);

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
