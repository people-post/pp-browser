#include "domain/messaging/CallHopPlan.h"

#include "domain/people/MeshHopPolicy.h"

#include <algorithm>

namespace pbr {
namespace {

bool AnySameSubnet24(const std::vector<std::string>& local_mas, const std::string& remote_ma) {
  for (const std::string& local : local_mas) {
    if (!local.empty() && IsSameIpv4Subnet24(local, remote_ma)) {
      return true;
    }
  }
  return false;
}

bool AnyPrivateLocal(const std::vector<std::string>& local_mas) {
  for (const std::string& local : local_mas) {
    if (MultiaddrHasPrivateIpv4Host(local)) {
      return true;
    }
  }
  return false;
}

bool PeerSharesLink(const std::vector<std::string>& local_mas,
                    const std::vector<std::string>& remote_mas) {
  if (remote_mas.empty()) {
    return false;
  }
  for (const std::string& remote : remote_mas) {
    if (remote.empty()) {
      continue;
    }
    if (AnySameSubnet24(local_mas, remote)) {
      return true;
    }
  }
  return false;
}

bool PeerIsPrivateSite(const std::vector<std::string>& remote_mas) {
  if (remote_mas.empty()) {
    return false;
  }
  bool saw = false;
  for (const std::string& remote : remote_mas) {
    if (remote.empty()) {
      continue;
    }
    saw = true;
    if (!MultiaddrHasPrivateIpv4Host(remote)) {
      return false;
    }
  }
  return saw;
}

bool IsOrgOrDirectoryAffinity(MeshHopAffinity affinity) {
  return affinity == MeshHopAffinity::OrgSeed || affinity == MeshHopAffinity::DirectoryNode ||
         affinity == MeshHopAffinity::DhtDiscovered;
}

bool HopHasPublicMa(const MeshHopCandidate& hop) {
  return !hop.multiaddr.empty() && !MultiaddrHasPrivateIpv4Host(hop.multiaddr);
}

} // namespace

CallHopScope InferCallHopScope(
    const std::vector<std::string>& local_mas,
    const std::unordered_map<std::string, std::vector<std::string>>& remote_mas_by_peer) {
  if (remote_mas_by_peer.empty()) {
    return CallHopScope::Wide;
  }

  bool all_link = true;
  bool all_site = true;
  for (const auto& [peer, mas] : remote_mas_by_peer) {
    (void)peer;
    if (mas.empty()) {
      return CallHopScope::Wide;
    }
    if (!PeerSharesLink(local_mas, mas)) {
      all_link = false;
    }
    if (!PeerIsPrivateSite(mas)) {
      all_site = false;
    }
  }

  if (all_link && !local_mas.empty()) {
    return CallHopScope::Link;
  }
  if (all_site && AnyPrivateLocal(local_mas)) {
    return CallHopScope::Site;
  }
  return CallHopScope::Wide;
}

bool LocalAdvertiseHasPublicIpv4(const std::vector<std::string>& local_mas) {
  for (const std::string& ma : local_mas) {
    if (!ma.empty() && !MultiaddrHasPrivateIpv4Host(ma) && ma.find("/ip4/") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool PreferLocalAllowedForScope(CallHopScope scope, bool prefer_local_as_hop,
                                const std::string& local_advertise_ma,
                                bool lan_reachability_confirmed) {
  if (!prefer_local_as_hop || local_advertise_ma.empty()) {
    return false;
  }
  if (scope == CallHopScope::Site) {
    // Different private subnets cannot dial PreferLocal RFC1918 MA.
    return false;
  }
  if (scope == CallHopScope::Link) {
    if (MultiaddrHasPrivateIpv4Host(local_advertise_ma)) {
      // Coincidental same-/24 on different LANs is common — require positive LAN evidence.
      return lan_reachability_confirmed;
    }
    return true;
  }
  // Wide: PreferLocal only when advertise MA is publicly dialable.
  return !MultiaddrHasPrivateIpv4Host(local_advertise_ma);
}

bool GuestMayDialPrivateHopMa(const std::string& hop_multiaddr,
                              const std::vector<std::string>& local_mas) {
  if (hop_multiaddr.empty() || !MultiaddrHasPrivateIpv4Host(hop_multiaddr)) {
    return true;
  }
  // WAN / publicly advertised node must not dial PreferLocal LAN hop.
  if (LocalAdvertiseHasPublicIpv4(local_mas)) {
    return false;
  }
  if (!AnyPrivateLocal(local_mas)) {
    return false;
  }
  return AnySameSubnet24(local_mas, hop_multiaddr);
}

std::vector<MeshHopCandidate> SelectCallMediaHop(std::vector<MeshHopCandidate> ranked,
                                                 CallHopScope scope,
                                                 const std::string& local_peer_id,
                                                 bool prefer_local_as_hop,
                                                 const std::string& local_advertise_ma,
                                                 bool lan_reachability_confirmed) {
  if (PreferLocalAllowedForScope(scope, prefer_local_as_hop, local_advertise_ma,
                                 lan_reachability_confirmed)) {
    return PreferLocalMediaHop(std::move(ranked), local_peer_id, local_advertise_ma);
  }

  // Site / Wide / unconfirmed Link: promote dialable org/directory public MAs first.
  std::vector<MeshHopCandidate> public_org;
  std::vector<MeshHopCandidate> rest;
  public_org.reserve(ranked.size());
  rest.reserve(ranked.size());
  for (MeshHopCandidate& hop : ranked) {
    if (!local_peer_id.empty() && hop.peer_id == local_peer_id) {
      if (MultiaddrHasPrivateIpv4Host(hop.multiaddr) || hop.multiaddr.empty()) {
        continue;
      }
    }
    if (IsOrgOrDirectoryAffinity(hop.affinity) && hop.dialable && HopHasPublicMa(hop)) {
      public_org.push_back(std::move(hop));
    } else {
      rest.push_back(std::move(hop));
    }
  }
  public_org.insert(public_org.end(), std::make_move_iterator(rest.begin()),
                    std::make_move_iterator(rest.end()));
  return public_org;
}

} // namespace pbr
