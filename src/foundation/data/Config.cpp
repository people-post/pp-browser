#include "foundation/data/Config.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "foundation/data/AppPaths.h"
#include "common/AtomicFileWrite.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/Platform.h"
#include "foundation/data/UiDemoConfig.h"
#include "common/ValueJson.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

// Prod / qa (and PP_BROWSER_ORIGIN) replace the saved backend URLs with ones derived from the origin;
// the saved values are kept so a save does not overwrite the custom URLs. A custom LLM endpoint is left alone.
void ApplyBackendOverride(AppConfig& config) {
  config.backend.saved_relay = config.relay;
  config.backend.saved_directory = config.directory;
  config.backend.saved_registration = config.registration;
  config.backend.has_saved_urls = true;
  if (config.llm.preset.empty() || config.llm.preset == "brief") {
    config.llm.base_url = BriefLlmBaseUrl();
  }
  config.promoted_mcp.url = BriefMcpUrl();
  config.relay.base_url = BriefRelayBaseUrl();
  config.directory.base_url = BriefRelayBaseUrl();
  config.registration.base_url = BriefRelayBaseUrl();
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

AppConfig Config::DefaultAppConfig(const bool ui_demo) {
  return DefaultAppConfigFor(BriefOrigin(), ui_demo);
}

AppConfig Config::DefaultAppConfigFor(const std::string& origin, const bool ui_demo) {
  AppConfig config = PlatformDefaults::For(Platform::Detect(), origin);
  NormalizeMeshConfig(config.mesh);
  if (ui_demo) {
    ApplyUiDemoConfig(config);
  }
  return config;
}

std::string Config::DiscoverConfigPath(int argc, char** argv, const Fallback fallback) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }

  if (const char* env = std::getenv("PP_BROWSER_CONFIG")) {
    return env;
  }
  if (fallback == Fallback::None) {
    return {};
  }

  const std::string canonical = AppPaths::ConfigFilePath();
  if (std::filesystem::exists(canonical)) {
    return canonical;
  }

  return {};
}

Roe<AppConfig> Config::Load(int argc, char** argv, const Fallback fallback) {
  auto& logger = Instance().log();
  AppConfig config = DefaultAppConfig();

  const std::string path = DiscoverConfigPath(argc, argv, fallback);
  if (path.empty()) {
    logger.info << "No config file found; using platform defaults (model: " << config.llm.model << ")";
    return config;
  }

  auto loaded = LoadFromFile(path);
  if (!loaded) {
    return loaded.error();
  }

  if (!loaded->llm_api_key_env.empty() && !IsApiKeyEnvName(loaded->llm_api_key_env)) {
    logger.warning << "llm.api_key_env '" << loaded->llm_api_key_env
                   << "' ignored: only an upper-case name ending in _API_KEY is read";
  }
  logger.info << "Loaded config from " << path << " (model: " << loaded->llm.model << ")";
  return loaded.value();
}

Roe<AppConfig> Config::LoadFromFile(const std::string& path, const bool ui_demo) {
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open config file: " + path);
  }

  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return Error(text.error().message + ": " + path);
  }
  auto root = TryParseObject(*text);
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

  AppConfig config = MergeConfig(DefaultAppConfig(ui_demo), *root);
  if (ParseBackendEnv(config.backend.env) == BackendEnv::Qa && !QaBackendAvailable()) {
    config.backend.env = BackendEnvName(BackendEnv::Prod);
  }
  LatchActiveBackend(ParseBackendEnv(config.backend.env));
  if (OriginEnvOverrideActive() || ActiveBackend() != BackendEnv::Custom) {
    ApplyBackendOverride(config);
  }
  const AppConfig defaults = DefaultAppConfig(ui_demo);
  if (config.relay.base_url.empty()) {
    config.relay.base_url = defaults.relay.base_url;
  }
  if (config.directory.base_url.empty()) {
    config.directory.base_url = defaults.directory.base_url;
  }
  NormalizeDirectoryConfig(config.directory);
  if (config.registration.base_url.empty()) {
    config.registration.base_url = defaults.registration.base_url;
  }
  ResolveConfigCredentials(config);
  NormalizeLlmConfig(config);
  NormalizeMeshConfig(config.mesh);
  if (ui_demo) {
    ApplyUiDemoConfig(config);
  }
  return config;
}

Roe<void> Config::SaveToFile(const std::string& path, const AppConfig& config) {
  const Object root = ConfigToObject(config, kConfigVersion);

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

  return AtomicFileWrite::Write(path, DumpJson(root, 2));
}

} // namespace pbr
