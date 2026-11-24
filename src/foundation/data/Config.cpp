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

// Prod / staging / dev (and PP_BROWSER_ORIGIN) replace the saved backend URLs with the active table's; the saved
// values are kept so a save does not overwrite the custom URLs. A custom LLM endpoint is left alone. The promoted MCP is
// the user's: it is never replaced here (#744), the table only seeds it (LoadFromFile).
void ApplyBackendOverride(AppConfig& config, const BackendTable& table) {
  config.backend.saved_relay = config.relay;
  config.backend.saved_directory = config.directory;
  config.backend.saved_registration = config.registration;
  config.backend.has_saved_urls = true;
  if (config.llm.preset.empty() || config.llm.preset == "brief") {
    config.llm.base_url = table.llm_gateway_url;
  }
  config.relay.base_url = table.org_relay_url;
  // A built-in backend's directory is its table's: a `directory.providers` list left in the file does not steer it (#743).
  config.directory.providers.clear();
  config.directory.base_url = table.DirectoryUrl();
  config.registration.base_url = table.registration_url;
  // The org relay's address carries its relay id (multi-relay MR008, SIGNED_FORMATS § 3.5): the table's for this
  // backend (`pending` while its operator set is not made: not used).
  config.relay.relay_id = table.org_relay_id;
  config.registration.relay_id = table.registration_relay_id;
}

} // namespace

bool IsPromotedMcpSeedEntry(const McpConfig& mcp, const CompiledOrigins& compiled) {
  return mcp.id.empty() && mcp.command.empty() && mcp.args.empty() && mcp.enabled &&
         IsBuiltInPromotedMcpSeed(mcp.url, compiled);
}

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

namespace {

/** What every default config shares, whichever table its endpoints came from. */
AppConfig FinishDefaults(AppConfig config, const bool ui_demo) {
  // With no saved choice a build runs its fallback: prod in a store build, never prod in a tester build.
  config.backend.env = BackendEnvName(FallbackBackendIn(BuildCompiledOrigins()));
  NormalizeMeshConfig(config.mesh);
  if (ui_demo) {
    ApplyUiDemoConfig(config);
  }
  return config;
}

} // namespace

AppConfig Config::DefaultAppConfig(const bool ui_demo) {
  return FinishDefaults(PlatformDefaults::For(Platform::Detect()), ui_demo);
}

AppConfig Config::DefaultAppConfigFor(const std::string& origin, const bool ui_demo) {
  return FinishDefaults(PlatformDefaults::For(Platform::Detect(), origin), ui_demo);
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

  // The run's backend is one this build can run; the saved choice stays as the file has it (unavailable_env). It is
  // settled, with a custom backend's own services, before any default is made: a custom backend's defaults are empty,
  // never the fallback environment's (#741, #742).
  BackendEnv saved_env = FallbackBackendIn(BuildCompiledOrigins());
  if (const Object* backend = root->getObject("backend")) {
    if (auto env = backend->getString("env")) {
      saved_env = ParseBackendEnv(*env);
    }
  }
  const BackendEnv runs_env = ResolveLatchedBackend(saved_env, BuildCompiledOrigins());
  LatchActiveBackend(runs_env, CustomServicesFromObject(*root));
  AppConfig config = MergeConfig(DefaultAppConfig(ui_demo), *root);
  config.backend.unavailable_env = runs_env == saved_env ? std::string() : std::string(BackendEnvName(saved_env));
  config.backend.env = BackendEnvName(runs_env);
  const BackendTable table = ActiveBackendTable();
  if (OriginEnvOverrideActive() || ActiveBackend() != BackendEnv::Custom) {
    ApplyBackendOverride(config, table);
  }
  // The promoted MCP the profile starts with is the table's seed; the user's own value (the file's) wins. A file's value
  // that is any built-in backend's seed (the old app wrote the running backend's into every save) was never the user's:
  // it is replaced by this backend's seed — none on a custom backend that names none.
  if (IsPromotedMcpSeedEntry(config.promoted_mcp, BuildCompiledOrigins())) {
    config.promoted_mcp = McpConfig{};
  }
  if (!config.promoted_mcp.IsConfigured()) {
    config.promoted_mcp.url = table.promoted_mcp_url;
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
