#include "domain/calls/CallHopPlan.h"

#include "mesh/types/MeshHopDial.h"
#include "common/MeshAlias.h"

#include <algorithm>

namespace pbr {
namespace {

bool AnySameSubnet24(const std::vector<std::string>& local_mas, const std::string& remote_ma) {
  for (const std::string& local : local_mas) {
    if (!local.empty() && mesh::IsSameIpv4Subnet24(local, remote_ma)) {
      return true;
    }
  }
  return false;
}

bool AnyPrivateLocal(const std::vector<std::string>& local_mas) {
  for (const std::string& local : local_mas) {
    if (mesh::MultiaddrHasPrivateIpv4Host(local)) {
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
    if (!mesh::MultiaddrHasPrivateIpv4Host(remote)) {
      return false;
    }
  }
  return saw;
}

bool IsOrgOrDirectoryAffinity(mesh::MeshHopAffinity affinity) {
  return affinity == mesh::MeshHopAffinity::OrgSeed || affinity == mesh::MeshHopAffinity::DirectoryNode ||
         affinity == mesh::MeshHopAffinity::DhtDiscovered;
}

bool HopHasPublicMa(const mesh::MeshHopCandidate& hop) {
  return mesh::MultiaddrHasPublicDialHost(hop.multiaddr);
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
    if (mesh::MultiaddrHasPublicDialHost(ma)) {
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
    if (mesh::MultiaddrHasPrivateIpv4Host(local_advertise_ma)) {
      // Coincidental same-/24 on different LANs is common — require positive LAN evidence.
      return lan_reachability_confirmed;
    }
    // Non-private Link advertise (public /ip4 or global /ip6) may PreferLocal without LAN proof.
    return mesh::MultiaddrHasPublicDialHost(local_advertise_ma);
  }
  // Wide: PreferLocal only when advertise MA is publicly dialable.
  return mesh::MultiaddrHasPublicDialHost(local_advertise_ma);
}

bool GuestMayDialPrivateHopMa(const std::string& hop_multiaddr,
                              const std::vector<std::string>& local_mas) {
  if (hop_multiaddr.empty() || !mesh::MultiaddrHasPrivateIpv4Host(hop_multiaddr)) {
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

std::vector<mesh::MeshHopCandidate> SelectCallMediaHop(std::vector<mesh::MeshHopCandidate> ranked,
                                                 CallHopScope scope,
                                                 const std::string& local_peer_id,
                                                 bool prefer_local_as_hop,
                                                 const std::string& local_advertise_ma,
                                                 bool lan_reachability_confirmed) {
  if (PreferLocalAllowedForScope(scope, prefer_local_as_hop, local_advertise_ma,
                                 lan_reachability_confirmed)) {
    return mesh::PreferLocalMediaHop(std::move(ranked), local_peer_id, local_advertise_ma);
  }

  // Site / Wide / unconfirmed Link: promote dialable org/directory public MAs first.
  std::vector<mesh::MeshHopCandidate> public_org;
  std::vector<mesh::MeshHopCandidate> rest;
  public_org.reserve(ranked.size());
  rest.reserve(ranked.size());
  for (mesh::MeshHopCandidate& hop : ranked) {
    if (!local_peer_id.empty() && hop.peer_id == local_peer_id) {
      if (mesh::MultiaddrHasPrivateIpv4Host(hop.multiaddr) || hop.multiaddr.empty()) {
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

bool HopUsableForAllReporters(const std::string& hop, const std::map<std::string, CallHopReport>& reports) {
  auto listed = [&hop](const std::vector<std::string>& hops) {
    return std::find(hops.begin(), hops.end(), hop) != hops.end();
  };
  for (const auto& [identity, report] : reports) {
    (void)identity;
    if (listed(report.unreachable_hops)) {
      return false;
    }
    const bool refused_plan = report.planned_hop_ok.has_value() && !*report.planned_hop_ok;
    if (refused_plan && !listed(report.reachable_hops)) {
      return false;
    }
  }
  return true;
}

GroupHopJoinDecision DecideGroupHopAtJoin(const GroupHopJoinInput& in) {
  GroupHopJoinDecision out;
  out.hop = in.planned_hop;
  if (in.planned_hop.empty()) {
    return out;
  }
  const bool planned_refused = std::any_of(in.reports.begin(), in.reports.end(), [](const auto& entry) {
    return entry.second.planned_hop_ok.has_value() && !*entry.second.planned_hop_ok;
  });
  if (!planned_refused) {
    return out;
  }
  for (const std::string& hop : in.ranked_hops) {
    if (!hop.empty() && hop != in.planned_hop && HopUsableForAllReporters(hop, in.reports)) {
      out.action = GroupHopAtJoin::UseAlternative;
      out.hop = hop;
      return out;
    }
  }
  out.action = GroupHopAtJoin::RefuseJoiner;
  return out;
}

std::vector<mesh::MeshHopCandidate> PreferNamedHopFirst(std::vector<mesh::MeshHopCandidate> ranked,
                                                        const std::string& hop_peer_id) {
  if (hop_peer_id.empty()) {
    return ranked;
  }
  const auto it = std::find_if(ranked.begin(), ranked.end(),
                               [&](const mesh::MeshHopCandidate& c) { return c.peer_id == hop_peer_id; });
  if (it != ranked.end()) {
    std::rotate(ranked.begin(), it, std::next(it));
  }
  return ranked;
}

std::vector<mesh::MeshHopCandidate> PlannedHopFirst(std::vector<mesh::MeshHopCandidate> ranked,
                                                    const CallPlannedHop& planned) {
  if (planned.peer_id.empty()) {
    return ranked;
  }
  const bool listed = std::any_of(ranked.begin(), ranked.end(),
                                  [&](const mesh::MeshHopCandidate& c) { return c.peer_id == planned.peer_id; });
  if (!listed && !planned.multiaddr.empty()) {
    mesh::MeshHopCandidate hop;
    hop.peer_id = planned.peer_id;
    hop.multiaddr = planned.multiaddr;
    hop.dialable = true;
    ranked.insert(ranked.begin(), std::move(hop));
  }
  return PreferNamedHopFirst(std::move(ranked), planned.peer_id);
}

CallHopReport BuildCallHopReport(const std::map<std::string, std::optional<bool>>& probes,
                                 const std::string& planned_hop) {
  CallHopReport report;
  for (const auto& [hop, ok] : probes) {
    if (ok) {
      (*ok ? report.reachable_hops : report.unreachable_hops).push_back(hop);
    }
    if (hop == planned_hop) {
      report.planned_hop_ok = ok;
    }
  }
  return report;
}

} // namespace pbr
