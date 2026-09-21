#pragma once

#include "foundation/data/Config.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

enum class MeshRole {
  Client,
  Node,
};

/**
 * L0 Brief seed ADP multiaddrs (N002 / A017 / N027).
 * Org `pp-node` must pin `amp_udp_port=443`. Change rarely when ops cuts long-lived seeds;
 * runtime mesh services prefer HTTP `GET /v1/mesh/nodes` (then this list).
 */
inline constexpr const char* kDefaultMeshBootstrapPeers[] = {
    "/ip4/54.198.185.139/udp/443/adp/1.0.0/p2p/QmbgShE3J3G6fvEHqwTS5ooQiFnYn46rzn6cTexUGWdWaj",
    "/ip4/44.218.209.223/udp/443/adp/1.0.0/p2p/Qmd2m4skBPf7YjPaAKcnhPuhzRv6wEnsf9QheZRANYGuM7",
};
inline constexpr std::size_t kDefaultMeshBootstrapPeerCount =
    sizeof(kDefaultMeshBootstrapPeers) / sizeof(kDefaultMeshBootstrapPeers[0]);

/** First default seed (compat / single-seed call sites). */
inline constexpr const char* kDefaultMeshBootstrapPeer = kDefaultMeshBootstrapPeers[0];

/** Mobile → Client; desktop → Node iff node_enabled (N001). */
MeshRole ResolveMeshRole(const MeshConfig& config);

/** Fill empty bootstrap_peers with project defaults (config file > hardcoded when saving). */
void NormalizeMeshConfig(MeshConfig& config);

/**
 * Effective seed multiaddrs for dial/warm: directory ADP addrs first, then
 * config bootstrap_peers (or hardcoded via Normalize). Dedupes by PeerId.
 * Priority: HTTP directory (live/cache) > config file > hardcoded (N027).
 */
std::vector<std::string> ResolveEffectiveBootstrapPeers(
    const MeshConfig& config, const std::vector<MeshDirectoryNode>& directory_nodes = {});

/** Extract PeerId after `/p2p/` (last component). */
std::string PeerIdFromMultiaddr(const std::string& multiaddr);

} // namespace pbr
