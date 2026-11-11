#pragma once

#include "common/ContextBudget.h"
#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "mesh/types/AddressDisclosure.h"
#include "common/MeshConfig.h"
#include "foundation/product/DevTools.h"
#include "common/ServiceEndpoints.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

struct McpConfig {
  std::string id;
  std::string command;
  std::vector<std::string> args;
  std::string url;
  bool enabled = true;

  bool IsConfigured() const { return !command.empty() || !url.empty(); }
};

struct SearchConfig {
  std::string provider = "duckduckgo";
};

/**
 * Which backend the app talks to (`backend.env`: prod | qa | custom). `prod` / `qa` derive every
 * backend URL from the compiled origin; `custom` uses the relay / directory / registration URLs
 * saved in the config file. A change applies on the next start.
 */
struct BackendConfig {
  std::string env = "prod";
  /**
   * The relay / directory / registration URLs as saved in the file, kept when the loaded values were
   * replaced by an origin (prod / qa / PP_BROWSER_ORIGIN) so a save does not overwrite the custom URLs.
   */
  bool has_saved_urls = false;
  ServiceEndpointConfig saved_relay;
  DirectoryConfig saved_directory;
  ServiceEndpointConfig saved_registration;
};

struct AppConfig {
  BackendConfig backend;
  LlmConfig llm;
  std::string llm_api_key_env;
  ContextBudget context = DefaultContextBudget();
  std::string theme = "themes/base.rcss";
  std::string data_dir;
  ServiceEndpointConfig relay;
  /**
   * `relay.content_sha256`: send `x-amz-content-sha256` on relay POST/PUT (needed behind CloudFront origin access
   * control). Deploy-provided, no settings UI; written to the file only when true so a build-time default of true
   * is not shadowed by a saved false.
   */
  bool relay_content_sha256 = false;
  DirectoryConfig directory;
  ServiceEndpointConfig registration;
  MeshConfig mesh;
  McpConfig promoted_mcp;
  std::vector<McpConfig> mcp_servers;
  SearchConfig search;
  /**
   * Dogfood: own initiation floor in pp_credit minor units (P001).
   * No Me UI yet — set via config.json. Seeded into LocalIdentity on hub start.
   * Default 0 (free).
   */
  int64_t initiation_floor = 0;
};

McpConfig ResolvePromotedMcp(const AppConfig& config, const AppConfig& defaults);

class Config : public Module {
public:
  static Config& Instance();

  static constexpr int kConfigVersion = 1;

  /** The config file when neither `--config` nor PP_BROWSER_CONFIG names one. */
  enum class Fallback {
    /** The app's own file (AppPaths::ConfigFilePath) when it exists: the desktop app. */
    AppConfigFile,
    /** None — defaults: pp-node, which must not take the desktop app's settings of the same user. */
    None,
  };

  static Roe<AppConfig> Load(int argc, char** argv, Fallback fallback = Fallback::AppConfigFile);
  /** `ui_demo`: the UI demo app has no backend or mesh (UiDemoConfig.h); every Config comes out with those applied. */
  static Roe<AppConfig> LoadFromFile(const std::string& path, bool ui_demo = kUiDemoBuild);
  static AppConfig DefaultAppConfig(bool ui_demo = kUiDemoBuild);
  /** DefaultAppConfig() against backend `origin` instead of this run's ("" leaves the backend URLs empty). */
  static AppConfig DefaultAppConfigFor(const std::string& origin, bool ui_demo = kUiDemoBuild);
  static Roe<void> SaveToFile(const std::string& path, const AppConfig& config);
  /** `--config`, else PP_BROWSER_CONFIG, else `fallback`; empty: no file. */
  static std::string DiscoverConfigPath(int argc, char** argv, Fallback fallback = Fallback::AppConfigFile);

  // Backward-compatible alias used by Application.h default arg.
  static AppConfig DefaultOllama() { return DefaultAppConfig(); }

private:
  Config();
};

} // namespace pbr
