#pragma once

#include "foundation/data/Config.h"
#include "foundation/identity/MultiaddrPeerId.h"
#include "common/MeshAlias.h"

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
    // The org relays the directory listed on 2026-10-31 (after the 2026-10-30 fleet move, #416).
    "/ip4/34.233.23.235/udp/443/adp/1.0.0/p2p/QmZyhSEd95N31Az5Fvr9ZJ9Ugarzy5B1pbFfuUXtxEC9Cv",
    "/ip4/50.18.237.202/udp/443/adp/1.0.0/p2p/QmdyKbzCcjUe89Go4oB8nb2gRFCjDCP3bZCRTLkMUdkFkw",
    "/ip4/54.217.120.211/udp/443/adp/1.0.0/p2p/QmRmTERfW4NbMf8Seu6WyNg68j6sfXWtsKoDdLHVoMVhnv",
    "/ip4/35.158.70.252/udp/443/adp/1.0.0/p2p/Qmc1TDTb6K4ymGNKMBHG6c45kxu7qA7TtCXdDfZxupStTj",
    "/ip4/57.181.254.92/udp/443/adp/1.0.0/p2p/QmXYY8waMbbuh4cpUXtKiwum8G6no8BKuEmCxFhS1hGwS1",
    "/ip4/15.164.114.146/udp/443/adp/1.0.0/p2p/QmWpQr3JEyeEVbePLmvRe1onbuM4dwhPG1P5Lo8Su3r97y",
    "/ip4/3.1.86.213/udp/443/adp/1.0.0/p2p/QmW9utyUR2S9N8Avite7aiS4LCcb2w6fJcd3PzyM2ozL5j",
};
inline constexpr std::size_t kDefaultMeshBootstrapPeerCount =
    sizeof(kDefaultMeshBootstrapPeers) / sizeof(kDefaultMeshBootstrapPeers[0]);

/** First default seed (compat / single-seed call sites). */
inline constexpr const char* kDefaultMeshBootstrapPeer = kDefaultMeshBootstrapPeers[0];

/**
 * Retired L0 seed PeerIds (ops cut). NormalizeMeshConfig strips matching bootstrap MAs so
 * upgrades do not keep dialing a dead door from an old config.json.
 */
inline constexpr const char* kRetiredMeshBootstrapPeerIds[] = {
    "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR", // was 3.208.41.58
    "QmbgShE3J3G6fvEHqwTS5ooQiFnYn46rzn6cTexUGWdWaj",       // was 54.198.185.139 (destroyed 2026-10-30)
    "Qmd2m4skBPf7YjPaAKcnhPuhzRv6wEnsf9QheZRANYGuM7",       // was 44.218.209.223 (destroyed 2026-10-30)
};
inline constexpr std::size_t kRetiredMeshBootstrapPeerIdCount =
    sizeof(kRetiredMeshBootstrapPeerIds) / sizeof(kRetiredMeshBootstrapPeerIds[0]);

/** Mobile → Client; desktop → Node iff node_enabled (N001). */
MeshRole ResolveMeshRole(const MeshConfig& config);

/** Strip retired seeds; fill empty bootstrap_peers with project defaults. */
void NormalizeMeshConfig(MeshConfig& config);

/**
 * The addresses worth publishing or dialling from off the node's own network: private-range and
 * local-scope IP addresses (a container's, a LAN's) are dropped when any other address is listed —
 * a dial that falls back to one only times out (#379). A list of nothing else is kept as it is.
 */
std::vector<std::string> DialWorthyMultiaddrs(std::vector<std::string> multiaddrs);

/**
 * Effective seed multiaddrs for dial/warm: directory ADP addrs first, then
 * config bootstrap_peers (or hardcoded via Normalize). Dedupes by PeerId.
 * Priority: HTTP directory (live/cache) > config file > hardcoded (N027).
 */
std::vector<std::string> ResolveEffectiveBootstrapPeers(
    const MeshConfig& config, const std::vector<mesh::MeshDirectoryNode>& directory_nodes = {});


} // namespace pbr
