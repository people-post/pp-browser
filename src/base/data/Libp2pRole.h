#pragma once

#include "base/data/Config.h"

#include <string>

namespace pbr {

enum class Libp2pRole {
  Client,
  Node,
};

/** Default Brief seed ADP multiaddr (N002 / A017). Org `pp-node` must pin `amp_udp_port=443`. */
inline constexpr const char* kDefaultLibp2pBootstrapPeer =
    "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";

/** Mobile → Client; desktop → Node iff node_enabled (N001). */
Libp2pRole ResolveLibp2pRole(const Libp2pConfig& config);

/** Fill empty bootstrap_peers with project defaults. */
void NormalizeLibp2pConfig(Libp2pConfig& config);

/** Extract PeerId after `/p2p/` (last component). */
std::string PeerIdFromMultiaddr(const std::string& multiaddr);

} // namespace pbr
