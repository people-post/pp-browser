#include "base/data/Config.h"

#include "base/data/AppPaths.h"
#include "base/data/AtomicFileWrite.h"
#include "base/data/ConfigJson.h"
#include "base/data/Libp2pRole.h"
#include "base/data/LlmPreset.h"
#include "base/data/PlatformDefaults.h"
#include "base/platform/DeploymentProfile.h"
#include "base/platform/Platform.h"
#include "common/ValueJson.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

void ApplySandboxBriefUrlRewrites(AppConfig& config) {
  if (!SandboxMode()) {
    return;
  }
  config.llm.base_url = RewriteBriefOriginUrl(config.llm.base_url);
  config.promoted_mcp.url = RewriteBriefOriginUrl(config.promoted_mcp.url);
  config.relay.base_url = RewriteBriefOriginUrl(config.relay.base_url);
  config.directory.base_url = RewriteBriefOriginUrl(config.directory.base_url);
  config.registration.base_url = RewriteBriefOriginUrl(config.registration.base_url);
}

} // namespace

McpConfig ResolvePromotedMcp(const AppConfig& config, const AppConfig& defaults) {
  if (config.promoted_mcp.IsConfigured()) {
    return config.promoted_mcp;
  }
  return defaults.promoted_mcp;
}

Config::Config() {
  redirectLogger("Config");
}

Config& Config::Instance() {
  static Config config;
  return config;
}

AppConfig Config::DefaultAppConfig() {
  AppConfig config = PlatformDefaults::For(Platform::Detect());
  NormalizeLibp2pConfig(config.libp2p);
  return config;
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

  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(text);
  if (!root) {
    return Error("Failed to parse config: " + path);
  }

  if (auto version = root->getIf<int64_t>("config_version")) {
    if (*version > kConfigVersion) {
      return Error("Unsupported config version " + std::to_string(*version) + " in " + path);
    }
  } else if (auto version_u = root->getNonNegInt("config_version")) {
    if (*version_u > static_cast<uint64_t>(kConfigVersion)) {
      return Error("Unsupported config version " + std::to_string(*version_u) + " in " + path);
    }
  }

  AppConfig config = MergeConfig(DefaultAppConfig(), *root);
  ApplySandboxBriefUrlRewrites(config);
  const AppConfig defaults = DefaultAppConfig();
  if (config.relay.base_url.empty()) {
    config.relay.base_url = defaults.relay.base_url;
  }
  if (config.directory.base_url.empty()) {
    config.directory.base_url = defaults.directory.base_url;
  }
  if (config.registration.base_url.empty()) {
    config.registration.base_url = defaults.registration.base_url;
  }
  ResolveConfigCredentials(config);
  NormalizeLlmConfig(config);
  NormalizeLibp2pConfig(config.libp2p);
  return config;
}

Roe<void> Config::SaveToFile(const std::string& path, const AppConfig& config) {
  const Object root = ConfigToObject(config, kConfigVersion);

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

  return AtomicFileWrite::Write(path, DumpJson(root, 2));
}

} // namespace pbr
