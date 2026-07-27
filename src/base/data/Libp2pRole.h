#pragma once

#include "base/data/Config.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

enum class Libp2pRole {
  Client,
  Node,
};

/** Default Brief seed multiaddr (N002). */
inline constexpr const char* kDefaultLibp2pBootstrapPeer =
    "/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";

/** Preferred desktop Node listen (N003). */
inline constexpr const char* kPreferredLibp2pListenMultiaddr = "/ip4/0.0.0.0/tcp/18517";

inline constexpr int kPreferredLibp2pListenPort = 18517;
inline constexpr int kLibp2pListenFallbackPortEnd = 18526;

/** Mobile → Client; desktop → Node iff node_enabled (N001). */
Libp2pRole ResolveLibp2pRole(const Libp2pConfig& config);

/** Fill empty bootstrap_peers / listen with project defaults. */
void NormalizeLibp2pConfig(Libp2pConfig& config);

/** TCP port from a multiaddr, if present. */
std::optional<int> TcpPortFromMultiaddr(const std::string& multiaddr);

/** Replace `/tcp/<n>` in multiaddr; returns empty on failure. */
std::string ReplaceTcpPortInMultiaddr(const std::string& multiaddr, int port);

/** Extract PeerId after `/p2p/` (last component). */
std::string PeerIdFromMultiaddr(const std::string& multiaddr);

/**
 * Listen candidates for Node (N016): configured addr, then 18517–18526 siblings when
 * applicable, then `/tcp/0` ephemeral on the same IP prefix.
 */
std::vector<std::string> BuildLibp2pListenCandidates(const std::string& preferred_multiaddr);

} // namespace pbr
