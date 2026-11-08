#pragma once

#include "mesh/types/AddressDisclosure.h"
#include "mesh/types/MeshConfigTypes.h"

#include "mesh/types/MeshConfigTypes.h"

#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The mesh slice of the app's config, as values (projects/capability-repos CR015): the app reads it from its
 * config file (foundation/data) and passes it to what runs the mesh (domain/node, the product's mesh).
 */
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
   * With mesh_publish: also send a signed liveness heartbeat to the directory every 5 minutes (pp-node). Off by
   * default — it tells the directory, and through its listing anyone, when this node is up and from which
   * address; meant for org-run seeds. Privacy P011.
   */
  bool mesh_heartbeat = false;
  /**
   * Prefer contacts then org seed for circuit/media hop pick (nf / N014).
   * On volunteer desktop Nodes, also prefer serving contacts (limit strangers).
   */
  bool prefer_contacts_for_routing = true;
  /**
   * Who may learn this device's IP address (projects/privacy T1): everyone / contacts / friendly /
   * nobody. Others reach us through a relay. The Node role is always `everyone` (P004).
   */
  mesh::DirectAudience direct_connections = mesh::DirectAudience::Contacts;
  /**
   * projects/privacy T4: relays see both ends' addresses. When true, only the configured org seeds
   * and Friendly contacts' nodes may relay, park, introduce punches or carry call media for us —
   * not directory volunteers or DHT-discovered nodes.
   */
  bool trusted_relays_only = false;
  /**
   * N032: relays that say they run an older pp-node than this (or say nothing) are tried after the others, never
   * dropped. Raised as a node rollout progresses; empty = no floor.
   */
  std::string preferred_min_node_version;
  /**
   * Desktop: keep a reservation on the mesh seeds so contacts can reach this device by its PeerId with no
   * relay (docs/contracts/ACCOUNT_CARD.md). Costs a little traffic and tells the seeds when it is online;
   * phones are woken by push instead.
   */
  bool reachable_without_relay = false;
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
  mesh::MeshCapabilities capabilities;
  mesh::MeshDhtConfig dht;
  mesh::MeshPricingConfig pricing;
  mesh::MediaRelayBudgetConfig media_relay_budget;
  /** What this node takes on when it serves others (`mesh.capacity`; 0 = the default). */
  mesh::MeshCapacityConfig capacity;
  mesh::MediaRelayVideoConfig media_relay_video;
};

enum class MeshRole {
  Client,
  Node,
};

/** `config` as it applies on this device: a phone is always a client (N001), whatever `node_enabled` says. */
inline MeshConfig EffectiveMeshConfig(MeshConfig config, const bool mobile) {
  if (mobile) {
    config.node_enabled = false;
  }
  return config;
}

/** The role an effective config says (EffectiveMeshConfig): a node when `node_enabled`. */
inline MeshRole MeshRoleOf(const MeshConfig& effective) {
  return effective.node_enabled ? MeshRole::Node : MeshRole::Client;
}

/**
 * Seed multiaddrs to dial / warm: the directory's mesh nodes first, then the config's seeds (a normalized config
 * — the app fills its defaults), deduped by PeerId (N027).
 */
std::vector<std::string> EffectiveBootstrapPeers(const MeshConfig& config,
                                                 const std::vector<mesh::MeshDirectoryNode>& directory_nodes = {});

} // namespace pbr
