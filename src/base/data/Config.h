#pragma once

#include "base/data/ContextBudget.h"
#include "base/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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

struct Libp2pCapabilities {
  /** Host circuit-relay bridge for NAT'd peers (n3). */
  bool circuit_relay = false;
  /** Host blind media forwarder (n4-media / N018). Default on for Node hosts. */
  bool media_relay = true;
};

/** ↑/↓ ceilings for media_relay (N019). 0 = unbounded / ops default. */
struct MediaRelayBudgetConfig {
  int64_t node_capacity_up_bps = 0;
  int64_t node_capacity_down_bps = 0;
  int64_t max_session_up_bps = 0;
  int64_t max_session_down_bps = 0;
  int64_t default_per_user_up_bps = 0;
  int64_t default_per_user_down_bps = 0;
};

/**
 * Per-capability relay pricing (N010 / P001).
 * Protocol branches on rate (== 0 free); mode is a UX label only.
 */
struct RelayPricingConfig {
  std::string mode = "volunteer"; // UX label: volunteer | paid
  double rate = 0.0;
};

struct Libp2pPricingConfig {
  RelayPricingConfig media_relay;
};

struct Libp2pConfig {
  /**
   * Preferred listen multiaddr when role is Node (N003).
   * May be rewritten after busy-port fallback (N016).
   */
  std::string listen_multiaddr = "/ip4/0.0.0.0/tcp/18517";
  /** Desktop opt-out of Node; ignored on mobile (always Client). */
  bool node_enabled = true;
  /**
   * Seed / bootstrap dial targets (must include /p2p/<PeerId>).
   * L0 cold-start / emergency (N002); mesh services prefer directory (N027).
   */
  std::vector<std::string> bootstrap_peers;
  /**
   * Public multiaddrs published to directory (N027). Never use 0.0.0.0.
   * pp-node mesh_node renew uses these; GUI person register may use listen set.
   */
  std::vector<std::string> advertise_multiaddrs;
  /**
   * When true, register/renew as entity_kind=mesh_node with local capabilities.
   * Default false (pp-browser). pp-node enables via env/config when advertising.
   */
  bool mesh_publish = false;
  size_t max_connections = 48;
  size_t max_concurrent_dials = 6;
  int dial_timeout_ms = 8000;
  int idle_ttl_ms = 180000;
  int dial_failure_backoff_ms = 30000;
  /**
   * Prefer contacts then org seed for circuit/media hop pick (nf / N014).
   * On volunteer desktop Nodes, also prefer serving contacts (limit strangers).
   */
  bool prefer_contacts_for_routing = true;
  /**
   * Parallel AMP UDP stack beside libp2p ([A023]). No product L4 traffic yet.
   * Requires device ML-DSA keys; UDP bind failure is soft (libp2p stays up).
   */
  bool enable_amp_stack = true;
  /** ADP UDP listen port for AmpStack; 0 = ephemeral. */
  int amp_udp_port = 0;
  Libp2pCapabilities capabilities;
  Libp2pPricingConfig pricing;
  MediaRelayBudgetConfig media_relay_budget;
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
