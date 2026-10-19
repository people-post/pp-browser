#pragma once

#include "common/thread/ContextBudget.h"
#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/privacy/AddressDisclosure.h"
#include "foundation/data/MeshConfigTypes.h"

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
  /** http now; amp = MeshHost AmpDirectoryProtocol (N029 nd4; not CreateOrgBackendClients). */
  std::string transport = "http";

  bool operator==(const ServiceEndpointConfig& other) const {
    return base_url == other.base_url && transport == other.transport;
  }
  bool operator!=(const ServiceEndpointConfig& other) const { return !(*this == other); }
};

/**
 * Directory / phone-book endpoints (N029 nd3).
 * Legacy: `base_url` (+ optional `transport`).
 * Preferred: ordered `providers[]` with failover; empty providers → synthesize from base_url.
 */
struct DirectoryConfig {
  std::string base_url;
  std::string transport = "http";
  std::vector<ServiceEndpointConfig> providers;

  bool operator==(const DirectoryConfig& other) const {
    return base_url == other.base_url && transport == other.transport && providers == other.providers;
  }
  bool operator!=(const DirectoryConfig& other) const { return !(*this == other); }
};

/** Effective ordered provider list (providers, else single base_url entry). */
std::vector<ServiceEndpointConfig> EffectiveDirectoryProviders(const DirectoryConfig& config);

/** If base_url empty but providers set, mirror providers[0] into base_url/transport. */
void NormalizeDirectoryConfig(DirectoryConfig& config);

struct SearchConfig {
  std::string provider = "duckduckgo";
};

struct MeshConfig {
  /** Desktop opt-out of Node; ignored on mobile (always Client). */
  bool node_enabled = true;
  /**
   * Seed / bootstrap dial targets (must include /p2p/<PeerId>).
   * Empty → NormalizeMeshConfig fills hardcoded L0 defaults (N002).
   * Config file overrides hardcoded. Runtime hops prefer HTTP directory /
   * last-good disk cache (`mesh_directory_nodes.json`) ahead of this list (N027).
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
  /**
   * Prefer contacts then org seed for circuit/media hop pick (nf / N014).
   * On volunteer desktop Nodes, also prefer serving contacts (limit strangers).
   */
  bool prefer_contacts_for_routing = true;
  /**
   * Who may learn this device's IP address (projects/privacy T1): everyone / contacts / friendly /
   * nobody. Others reach us through a relay. The Node role is always `everyone` (P004).
   */
  DirectAudience direct_connections = DirectAudience::Contacts;
  /**
   * projects/privacy T4: relays see both ends' addresses. When true, only the configured org seeds
   * and Friendly contacts' nodes may relay, park, introduce punches or carry call media for us —
   * not directory volunteers or DHT-discovered nodes.
   */
  bool trusted_relays_only = false;
  /**
   * Peer mesh on/off. When true, MeshHost hard-requires Amp UDP bind (D10).
   * When false, peer mesh underlay stays off. Requires device ML-DSA keys.
   */
  bool mesh_enabled = true;
  /** ADP UDP listen port for AmpStack; 0 = ephemeral. */
  int amp_udp_port = 0;
  /**
   * Call path mobility class (call-path-resilience K004): "auto" (classify from the network) or a
   * pinned "stationary" / "mobile" for dogfood and the hard lab. `--mobility=` overrides it.
   */
  std::string mobility = "auto";
  MeshCapabilities capabilities;
  MeshDhtConfig dht;
  MeshPricingConfig pricing;
  MediaRelayBudgetConfig media_relay_budget;
  MediaRelayVideoConfig media_relay_video;
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
