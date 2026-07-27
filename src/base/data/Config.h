#pragma once

#include "base/data/ContextBudget.h"
#include "base/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"

#include <string>
#include <vector>

namespace pbr {

struct McpConfig {
  std::string id;
  std::string command;
  std::vector<std::string> args;
  std::string url;
  bool enabled = true;

  bool IsConfigured() const { return !command.empty() || !url.empty(); }
};

struct ServiceEndpointConfig {
  std::string base_url;
  std::string transport = "http";
};

struct SearchConfig {
  std::string provider = "duckduckgo";
  std::string api_key;
};

struct Libp2pConfig {
  /**
   * Preferred listen multiaddr when role is Node (N003).
   * May be rewritten after busy-port fallback (N016).
   */
  std::string listen_multiaddr = "/ip4/0.0.0.0/tcp/18517";
  /** Desktop opt-out of Node; ignored on mobile (always Client). */
  bool node_enabled = true;
  /** Seed / bootstrap dial targets (must include /p2p/<PeerId>). */
  std::vector<std::string> bootstrap_peers;
  size_t max_connections = 48;
  size_t max_concurrent_dials = 6;
  int dial_timeout_ms = 8000;
  int idle_ttl_ms = 180000;
  int dial_failure_backoff_ms = 30000;
};

struct AppConfig {
  LlmConfig llm;
  std::string llm_api_key_env;
  ContextBudget context = DefaultContextBudget();
  std::string theme = "themes/base.rcss";
  std::string data_dir;
  ServiceEndpointConfig relay;
  ServiceEndpointConfig directory;
  ServiceEndpointConfig registration;
  Libp2pConfig libp2p;
  McpConfig promoted_mcp;
  std::vector<McpConfig> mcp_servers;
  SearchConfig search;
};

McpConfig ResolvePromotedMcp(const AppConfig& config, const AppConfig& defaults);

class Config : public Module {
public:
  static Config& Instance();

  static constexpr int kConfigVersion = 1;

  static Roe<AppConfig> Load(int argc, char** argv);
  static Roe<AppConfig> LoadFromFile(const std::string& path);
  static AppConfig DefaultAppConfig();
  static Roe<void> SaveToFile(const std::string& path, const AppConfig& config);
  static std::string DiscoverConfigPath(int argc, char** argv);

  // Backward-compatible alias used by Application.h default arg.
  static AppConfig DefaultOllama() { return DefaultAppConfig(); }

private:
  Config();
};

} // namespace pbr
