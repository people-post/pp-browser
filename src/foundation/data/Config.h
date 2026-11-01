#pragma once

#include "common/ContextBudget.h"
#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "mesh/types/AddressDisclosure.h"
#include "common/MeshConfig.h"
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

struct AppConfig {
  LlmConfig llm;
  std::string llm_api_key_env;
  ContextBudget context = DefaultContextBudget();
  std::string theme = "themes/base.rcss";
  std::string data_dir;
  ServiceEndpointConfig relay;
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
  static Roe<AppConfig> LoadFromFile(const std::string& path);
  static AppConfig DefaultAppConfig();
  static Roe<void> SaveToFile(const std::string& path, const AppConfig& config);
  /** `--config`, else PP_BROWSER_CONFIG, else `fallback`; empty: no file. */
  static std::string DiscoverConfigPath(int argc, char** argv, Fallback fallback = Fallback::AppConfigFile);

  // Backward-compatible alias used by Application.h default arg.
  static AppConfig DefaultOllama() { return DefaultAppConfig(); }

private:
  Config();
};

} // namespace pbr
