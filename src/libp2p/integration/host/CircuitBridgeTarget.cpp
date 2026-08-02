#include "libp2p/integration/host/CircuitBridgeTarget.h"

#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <libp2p/host/host.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/peer/peer_id.hpp>

namespace pbr {
namespace {

std::string PeerIdFromMultiaddrString(const std::string& multiaddr) {
  const std::string marker = "/p2p/";
  const auto pos = multiaddr.rfind(marker);
  if (pos == std::string::npos) {
    return {};
  }
  std::string id = multiaddr.substr(pos + marker.size());
  const auto slash = id.find('/');
  if (slash != std::string::npos) {
    id.resize(slash);
  }
  return id;
}

} // namespace

std::optional<std::string> ResolveCircuitTargetMultiaddr(PeerSessionManager& sessions, Libp2pHost& host,
                                                         const std::string& target_peer_id) {
  if (target_peer_id.empty()) {
    return std::nullopt;
  }
  if (auto ma = sessions.PreferredPeerMultiaddr(target_peer_id)) {
    return ma;
  }
  if (auto info = sessions.ResolvePeerInfo(target_peer_id)) {
    if (!info->addresses.empty()) {
      return std::string{info->addresses.front().getStringAddress()};
    }
  }
  if (!host.IsRunning()) {
    return std::nullopt;
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(target_peer_id);
  if (!peer_id) {
    return std::nullopt;
  }
  auto addrs = host.GetHost().getPeerRepository().getAddressRepository().getAddresses(peer_id.value());
  if (!addrs || addrs.value().empty()) {
    return std::nullopt;
  }
  return std::string{addrs.value().front().getStringAddress()};
}

Roe<std::pair<std::string, std::string>> NormalizeCircuitBridgeTarget(PeerSessionManager& sessions,
                                                                      Libp2pHost& host,
                                                                      const CircuitBridgeTarget& target) {
  std::string peer_id = target.target_peer_id;
  std::string multiaddr = target.target_multiaddr;

  if (!multiaddr.empty()) {
    const std::string embedded = PeerIdFromMultiaddrString(multiaddr);
    if (!peer_id.empty() && !embedded.empty() && embedded != peer_id) {
      return Error("circuit target PeerId mismatch");
    }
    if (peer_id.empty()) {
      peer_id = embedded;
    }
  }

  if (multiaddr.empty() && !peer_id.empty()) {
    if (auto resolved = ResolveCircuitTargetMultiaddr(sessions, host, peer_id)) {
      multiaddr = *resolved;
    }
  }

  if (peer_id.empty() && !multiaddr.empty()) {
    peer_id = PeerIdFromMultiaddrString(multiaddr);
  }

  if (peer_id.empty() || multiaddr.empty()) {
    return Error("circuit target not resolved");
  }
  if (!libp2p::peer::PeerId::fromBase58(peer_id)) {
    return Error("invalid circuit target PeerId");
  }
  if (!libp2p::multi::Multiaddress::create(multiaddr)) {
    return Error("invalid circuit target multiaddr");
  }
  return std::pair{peer_id, multiaddr};
}

} // namespace pbr
