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

bool PreferLocalAllowedForScope(CallHopScope scope, bool prefer_local_as_hop,
                                const std::string& local_advertise_ma) {
  if (!prefer_local_as_hop || local_advertise_ma.empty()) {
    return false;
  }
  if (scope == CallHopScope::Link || scope == CallHopScope::Site) {
    return true;
  }
  // Wide: PreferLocal only when advertise MA is publicly dialable.
  return !MultiaddrHasPrivateIpv4Host(local_advertise_ma);
}

std::vector<MeshHopCandidate> SelectCallMediaHop(std::vector<MeshHopCandidate> ranked,
                                                 CallHopScope scope,
                                                 const std::string& local_peer_id,
                                                 bool prefer_local_as_hop,
                                                 const std::string& local_advertise_ma) {
  if (PreferLocalAllowedForScope(scope, prefer_local_as_hop, local_advertise_ma)) {
    return PreferLocalMediaHop(std::move(ranked), local_peer_id, local_advertise_ma);
  }

  // Wide (or PreferLocal unavailable): promote dialable org/directory public MAs first.
  std::vector<MeshHopCandidate> public_org;
  std::vector<MeshHopCandidate> rest;
  public_org.reserve(ranked.size());
  rest.reserve(ranked.size());
  for (MeshHopCandidate& hop : ranked) {
    if (!local_peer_id.empty() && hop.peer_id == local_peer_id) {
      // Drop private PreferLocal self on Wide — never fan-out RFC1918 self as hop.
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
