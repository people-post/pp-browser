#pragma once

#include "common/thread/ContextBudget.h"
#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/privacy/AddressDisclosure.h"

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
  std::string api_key;
};

struct MeshCapabilities {
  /**
   * Host circuit-relay bridge for NAT'd peers (n3 / N009).
   * Node + this flag → ServeInbound / publish. Clients consume org/contact
   * circuit for dialability without enabling this (CallStack AmpCircuitHopReach).
   */
  bool circuit_relay = false;
  /** Host blind media forwarder (n4-media / N018). Default on for Node hosts. */
  bool media_relay = true;
  /** Participate in AMP Kademlia peer routing (n2). Default off; Node only. */
  bool dht = false;
  /**
   * Advertise as opaque ledger RPC gateway (N029 Phase C prep).
   * Default off; no chain runtime required to store the flag.
   */
  bool ledger_gateway = false;
};

/** Tunables for mesh DHT (n2). Ignored when capabilities.dht is false. */
struct MeshDhtConfig {
  /** TTL for self peer_routing records (seconds). Re-publish at ttl/2. */
  int record_ttl_seconds = 3600;
  /** FIND_PEER RPC timeout (milliseconds). */
  int find_peer_timeout_ms = 5000;
  /** Max in-flight FIND_PEER lookups per process. */
  int max_concurrent_lookups = 4;
  /** Kademlia k (bucket size target). Wire default 20 — override for lab only. */
  int k_bucket_size = 20;
  /** Inbound FIND_PEER/STORE grants per remote peer per window (n2-hard). */
  int inbound_ops_per_peer_per_window = 60;
  /** Sliding window length for inbound rate limit (seconds). */
  int inbound_rate_window_seconds = 60;
  /** Soft-reputation: skip query peers after this many bad FIND_PEER replies. */
  int soft_reputation_penalty_threshold = 3;
  /** Soft-reputation cooldown after penalty (seconds). */
  int soft_reputation_cooldown_seconds = 300;
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
 * Which video levels this node's media_relay carries per publisher (peer-scoped-broadcast B009).
 * Levels are opaque ordered integers (docs/contracts/MEDIA_CHANNELS.md).
 */
struct MediaRelayVideoConfig {
  /** Levels served; empty = any. */
  std::vector<int> serve_levels;
  /** At most this many levels per publisher; 0 = no limit. */
  int carry_levels = 0;
  /** Refuse a publisher offering none of `serve_levels` (default: carry its closest level). */
  bool strict = false;
};

/**
 * Per-capability relay pricing (N010 / P001).
 * Protocol branches on rate (== 0 free); mode is a UX label only.
 */
struct RelayPricingConfig {
  std::string mode = "volunteer"; // UX label: volunteer | paid
  double rate = 0.0;
};

struct MeshPricingConfig {
  RelayPricingConfig media_relay;
};

/** Cached infra row from GET /v1/mesh/nodes (N027 / n-dir / N029). */
struct MeshDirectoryNode {
  std::string peer_id;
  std::vector<std::string> multiaddrs;
  bool circuit_relay = false;
  bool media_relay = false;
  bool dht = false;
  bool ledger_gateway = false;
  std::string account_id;
  std::string entity_kind;
  int64_t seq = 0;
  std::string expires_at;
  std::string nickname;
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
