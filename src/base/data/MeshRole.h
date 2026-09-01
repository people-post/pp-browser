#pragma once

#include "base/data/Config.h"

#include <string>

namespace pbr {

enum class MeshRole {
  Client,
  Node,
};

/** Default Brief seed ADP multiaddr (N002 / A017). Org `pp-node` must pin `amp_udp_port=443`. */
inline constexpr const char* kDefaultMeshBootstrapPeer =
    "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";

/** Mobile → Client; desktop → Node iff node_enabled (N001). */
MeshRole ResolveMeshRole(const MeshConfig& config);

/** Fill empty bootstrap_peers with project defaults. */
void NormalizeMeshConfig(MeshConfig& config);

/** Extract PeerId after `/p2p/` (last component). */
std::string PeerIdFromMultiaddr(const std::string& multiaddr);

} // namespace pbr
