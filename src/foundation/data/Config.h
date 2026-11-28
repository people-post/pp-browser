#pragma once

#include "common/ContextBudget.h"
#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "mesh/types/AddressDisclosure.h"
#include "common/MeshConfig.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/product/DevTools.h"
#include "common/ServiceEndpoints.h"
#include "common/Value.h"

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
  /**
   * `set_by_user` (optional key): the promoted MCP's address is the user's own, typed in Integrations. Only an entry that
   * carries it, or one with a `command` (a stdio server the user configured), is the user's: any other `promoted_mcp` in
   * a file is a leftover of the old app, which wrote the running backend's derived address into every save, and is replaced
   * by the active backend's seed on load (profile-model P2a, #741, #744). Read for `promoted_mcp` only.
   */
  bool user_set = false;

  bool IsConfigured() const { return !command.empty() || !url.empty(); }
  bool operator==(const McpConfig&) const = default;
};

struct SearchConfig {
  std::string provider = "duckduckgo";
};

/**
 * Which backend the app talks to (`backend.env`: prod | staging | dev | custom). `prod` / `staging` / `dev` derive every
 * backend URL from the compiled origin; `custom` uses the relay / directory / registration URLs
 * saved in the config file. A change applies on the next start.
 */
struct BackendConfig {
  /** The backend this config runs (ResolveLatchedBackend of the saved choice): one this build can run. */
  std::string env = "prod";
  /**
   * `backend.env` as the file names it when this build cannot run it ("" otherwise): a tester build reading `prod`, a
   * store build reading `staging`. A save writes it back, so the saved choice is never rewritten just because this build
   * runs another (a store build after a tester build still finds `prod`, release-compatibility RC022); only a user's
   * switch (ApplyNetworkSettingsDraft) replaces it.
   */
  std::string unavailable_env;
  /**
   * The relay / directory / registration URLs as saved in the file, kept when the loaded values were
   * replaced by an origin (prod / staging / dev / PP_BROWSER_ORIGIN) so a save does not overwrite the custom URLs.
   */
  bool has_saved_urls = false;
  ServiceEndpointConfig saved_relay;
  DirectoryConfig saved_directory;
  ServiceEndpointConfig saved_registration;
  /**
   * `backend.services` (optional keys, `config_version` unchanged): the other services a **custom** backend names
   * (LLM gateway, LLM token, account locker, client-compat, crash intake, promoted MCP seed, mesh nodes); the org relay,
   * registration and directory are the `relay`, `registration` and `directory` keys. A service not named is absent: what
   * needs it is off, with no fallback to a built-in backend's address (profile-model PM002). Read for a custom backend
   * only; prod / staging / dev take their table from the build and leave these as the file has them.
   */
  CustomServices services;
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
  /**
   * `relay.extra_base_urls`: our other relays, after the backend's (multi-relay R2): the app registers, receives and
   * pushes on each, and lists the `https://` ones in its account card. At most 3 (4 relays in all, MR004). No settings
   * UI yet (R3); kept whatever `backend.env` says, written to the file only when set.
   */
  std::vector<std::string> relay_extra_base_urls;
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
  /**
   * `node`: the settings of a node's optional services (projects/node-relay NR001), `node.<service>.…`, kept as the
   * file has them. A build with the service reads its own section; pp-node itself reads only each `enabled` (an
   * explicit ask for a service the build lacks fails its start). Tier-free; kept and written back as read.
   */
  Object node;
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

private:
  Config();
};

} // namespace pbr
