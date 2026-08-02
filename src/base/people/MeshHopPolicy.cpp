#include "base/people/MeshHopPolicy.h"

#include "base/data/Libp2pRole.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace pbr {
namespace {

double MediaHopScore(const MeshHopCandidate& c, bool prefer_contacts) {
  double score = c.residual_capacity * 100.0;
  if (prefer_contacts) {
    if (c.affinity == MeshHopAffinity::Contact) {
      score += 25.0;
    } else if (c.affinity == MeshHopAffinity::OrgSeed) {
      score += 10.0;
    }
  } else {
    if (c.affinity == MeshHopAffinity::OrgSeed) {
      score += 25.0;
    } else if (c.affinity == MeshHopAffinity::Contact) {
      score += 10.0;
    }
  }
  if (c.dialable) {
    score += 5.0;
  }
  return score;
}

std::string Ip4HostFromMultiaddr(const std::string& multiaddr) {
  const std::string marker = "/ip4/";
  const auto pos = multiaddr.find(marker);
  if (pos == std::string::npos) {
    return {};
  }
  const auto start = pos + marker.size();
  const auto end = multiaddr.find('/', start);
  if (end == std::string::npos) {
    return multiaddr.substr(start);
  }
  return multiaddr.substr(start, end - start);
}

bool ParseIpv4Octets(const std::string& ip, std::array<int, 4>& out) {
  std::istringstream ss(ip);
  std::string part;
  for (int i = 0; i < 4; ++i) {
    if (!std::getline(ss, part, '.') || part.empty()) {
      return false;
    }
    char* end = nullptr;
    const long v = std::strtol(part.c_str(), &end, 10);
    if (end == part.c_str() || v < 0 || v > 255) {
      return false;
    }
    out[static_cast<size_t>(i)] = static_cast<int>(v);
  }
  return !std::getline(ss, part, '.');
}

bool IsPrivateIpv4Host(const std::string& ip) {
  std::array<int, 4> octets{};
  if (!ParseIpv4Octets(ip, octets)) {
    return false;
  }
  if (octets[0] == 10) {
    return true;
  }
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) {
    return true;
  }
  if (octets[0] == 192 && octets[1] == 168) {
    return true;
  }
  return false;
}

const RelayScope kEscalateOrder[] = {RelayScope::Link, RelayScope::Site, RelayScope::Social, RelayScope::Org,
                                     RelayScope::Public};

std::string FirstMultiaddrForPeer(const Contact& contact, const std::string& peer_id) {
  for (const std::string& ma : contact.multiaddrs) {
    if (PeerIdFromMultiaddr(ma) == peer_id) {
      return ma;
    }
  }
  if (!contact.multiaddrs.empty()) {
    return contact.multiaddrs.front();
  }
  return {};
}

} // namespace

bool IsSameIpv4Subnet24(const std::string& multiaddr_a, const std::string& multiaddr_b) {
  const std::string ip_a = Ip4HostFromMultiaddr(multiaddr_a);
  const std::string ip_b = Ip4HostFromMultiaddr(multiaddr_b);
  if (ip_a.empty() || ip_b.empty()) {
    return false;
  }
  std::array<int, 4> a{};
  std::array<int, 4> b{};
  if (!ParseIpv4Octets(ip_a, a) || !ParseIpv4Octets(ip_b, b)) {
    return false;
  }
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

RelayScopeMask CandidateRelayScopes(const MeshHopCandidate& candidate,
                                    const std::string& local_listen_multiaddr) {
  RelayScopeMask mask = 0;
  if (candidate.affinity == MeshHopAffinity::OrgSeed) {
    return static_cast<RelayScopeMask>(RelayScope::Org);
  }
  if (candidate.affinity != MeshHopAffinity::Contact) {
    return mask;
  }
  mask = static_cast<RelayScopeMask>(RelayScope::Social);
  if (candidate.multiaddr.empty()) {
    return mask;
  }
  const std::string hop_ip = Ip4HostFromMultiaddr(candidate.multiaddr);
  if (IsPrivateIpv4Host(hop_ip)) {
    mask |= static_cast<RelayScopeMask>(RelayScope::Site);
  }
  if (!local_listen_multiaddr.empty() && IsSameIpv4Subnet24(candidate.multiaddr, local_listen_multiaddr)) {
    mask |= static_cast<RelayScopeMask>(RelayScope::Link);
  }
  return mask;
}

RelayScopeMask ProviderServeScopeMask(MeshReachabilityClass reachability, bool node_enabled) {
  if (!node_enabled) {
    return 0;
  }
  switch (reachability) {
  case MeshReachabilityClass::Blocked:
  case MeshReachabilityClass::OutboundOnly:
  case MeshReachabilityClass::Reachable:
  case MeshReachabilityClass::Unknown:
  default:
    return kRelayScopeVolunteerServe;
  }
}

std::vector<MeshHopCandidate> CollectContactHopCandidates(const std::vector<Contact>& contacts) {
  std::vector<MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  for (const Contact& contact : contacts) {
    if (contact.trust == TrustLevel::Blocked) {
      continue;
    }
    const std::string peer_id = PeerIdFromContact(contact);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    MeshHopCandidate c;
    c.peer_id = peer_id;
    c.multiaddr = FirstMultiaddrForPeer(contact, peer_id);
    c.affinity = MeshHopAffinity::Contact;
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<MeshHopCandidate> CollectSeedHopCandidates(const std::vector<std::string>& bootstrap_peers) {
  std::vector<MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  for (const std::string& ma : bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    MeshHopCandidate c;
    c.peer_id = peer_id;
    c.multiaddr = ma;
    c.affinity = MeshHopAffinity::OrgSeed;
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<MeshHopCandidate> OrderCircuitHops(std::vector<MeshHopCandidate> contacts,
                                               std::vector<MeshHopCandidate> seeds,
                                               bool prefer_contacts) {
  std::vector<MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  auto append = [&](std::vector<MeshHopCandidate>& list) {
    for (MeshHopCandidate& c : list) {
      if (c.peer_id.empty() || !seen.insert(c.peer_id).second) {
        continue;
      }
      out.push_back(std::move(c));
    }
  };
  if (prefer_contacts) {
    append(contacts);
    append(seeds);
  } else {
    append(seeds);
    append(contacts);
  }
  return out;
}

std::vector<MeshHopCandidate> RankMediaHops(std::vector<MeshHopCandidate> candidates,
                                            bool prefer_contacts) {
  std::vector<MeshHopCandidate> filtered;
  filtered.reserve(candidates.size());
  for (MeshHopCandidate& c : candidates) {
    if (c.affinity == MeshHopAffinity::Other) {
      continue;
    }
    if (!c.dialable || c.recently_failed) {
      continue;
    }
    if (c.residual_capacity <= 0.0) {
      continue;
    }
    filtered.push_back(std::move(c));
  }
  std::stable_sort(filtered.begin(), filtered.end(),
                   [prefer_contacts](const MeshHopCandidate& a, const MeshHopCandidate& b) {
                     return MediaHopScore(a, prefer_contacts) > MediaHopScore(b, prefer_contacts);
                   });
  return filtered;
}

std::vector<MeshHopCandidate> RankMediaHopsEscalating(
    std::vector<MeshHopCandidate> candidates, bool prefer_contacts,
    const std::string& local_listen_multiaddr, RelayScopeMask consumer_scopes) {
  std::vector<MeshHopCandidate> result;
  std::unordered_set<std::string> picked;
  for (RelayScope band : kEscalateOrder) {
    if (!RelayScopeMaskHas(consumer_scopes, band)) {
      continue;
    }
    std::vector<MeshHopCandidate> band_candidates;
    band_candidates.reserve(candidates.size());
    for (const MeshHopCandidate& c : candidates) {
      if (picked.count(c.peer_id) > 0) {
        continue;
      }
      const RelayScopeMask scopes = CandidateRelayScopes(c, local_listen_multiaddr);
      if (!RelayScopeMaskHas(scopes, band)) {
        continue;
      }
      band_candidates.push_back(c);
    }
    for (MeshHopCandidate& ranked : RankMediaHops(std::move(band_candidates), prefer_contacts)) {
      if (picked.insert(ranked.peer_id).second) {
        result.push_back(std::move(ranked));
      }
    }
  }
  return result;
}

std::string PeerIdFromContact(const Contact& contact) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::PeerId && !id.value.empty()) {
      return id.value;
    }
  }
  for (const std::string& ma : contact.multiaddrs) {
    const std::string pid = PeerIdFromMultiaddr(ma);
    if (!pid.empty()) {
      return pid;
    }
  }
  return {};
}

std::vector<MeshHopCandidate> ExcludeSelfHop(std::vector<MeshHopCandidate> candidates,
                                             const std::string& local_peer_id) {
  if (local_peer_id.empty()) {
    return candidates;
  }
  return ExcludePeerIds(std::move(candidates), {local_peer_id});
}

std::vector<MeshHopCandidate> ExcludePeerIds(std::vector<MeshHopCandidate> candidates,
                                             const std::unordered_set<std::string>& excluded) {
  if (excluded.empty()) {
    return candidates;
  }
  std::vector<MeshHopCandidate> out;
  out.reserve(candidates.size());
  for (MeshHopCandidate& c : candidates) {
    if (excluded.count(c.peer_id) > 0) {
      continue;
    }
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<MeshHopCandidate> PreferInCallMediaHops(
    std::vector<MeshHopCandidate> ranked, const std::unordered_set<std::string>& in_call_peer_ids) {
  if (in_call_peer_ids.empty() || ranked.empty()) {
    return ranked;
  }
  std::vector<MeshHopCandidate> in_call;
  std::vector<MeshHopCandidate> rest;
  in_call.reserve(ranked.size());
  rest.reserve(ranked.size());
  for (MeshHopCandidate& c : ranked) {
    if (!c.multiaddr.empty() && in_call_peer_ids.count(c.peer_id) > 0) {
      in_call.push_back(std::move(c));
    } else {
      rest.push_back(std::move(c));
    }
  }
  in_call.insert(in_call.end(), std::make_move_iterator(rest.begin()),
                 std::make_move_iterator(rest.end()));
  return in_call;
}

bool IsContactPeerId(const std::vector<Contact>& contacts, const std::string& peer_id) {
  if (peer_id.empty()) {
    return false;
  }
  for (const Contact& contact : contacts) {
    if (PeerIdFromContact(contact) == peer_id) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const Contact& contact : contacts) {
    const std::string peer_id = PeerIdFromContact(contact);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    out.push_back(peer_id);
  }
  return out;
}

} // namespace pbr
