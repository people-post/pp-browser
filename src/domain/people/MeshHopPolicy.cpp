#include "domain/people/MeshHopPolicy.h"

#include "foundation/data/MeshRole.h"
#include "common/MeshAlias.h"

#include <algorithm>
#include <unordered_set>

namespace pbr {
namespace {

double MediaHopScore(const mesh::MeshHopCandidate& c, bool prefer_contacts) {
  double score = c.residual_capacity * 100.0;
  if (prefer_contacts) {
    if (c.affinity == mesh::MeshHopAffinity::Contact) {
      score += 25.0;
    } else if (c.affinity == mesh::MeshHopAffinity::DirectoryNode || c.affinity == mesh::MeshHopAffinity::DhtDiscovered ||
               c.affinity == mesh::MeshHopAffinity::OrgSeed) {
      score += 10.0;
    }
  } else {
    if (c.affinity == mesh::MeshHopAffinity::OrgSeed || c.affinity == mesh::MeshHopAffinity::DirectoryNode) {
      score += 25.0;
    } else if (c.affinity == mesh::MeshHopAffinity::DhtDiscovered) {
      score += 15.0;
    } else if (c.affinity == mesh::MeshHopAffinity::Contact) {
      score += 10.0;
    }
  }
  if (c.dialable) {
    score += 5.0;
  }
  return score;
}

const mesh::RelayScope kEscalateOrder[] = {mesh::RelayScope::Link, mesh::RelayScope::Site, mesh::RelayScope::Social, mesh::RelayScope::Org,
                                     mesh::RelayScope::Public};

std::string FirstMultiaddrForPeer(const Contact& contact, const std::string& peer_id) {
  std::vector<std::string> matching;
  for (const std::string& ma : contact.multiaddrs) {
    if (PeerIdFromMultiaddr(ma) == peer_id) {
      matching.push_back(ma);
    }
  }
  if (!matching.empty()) {
    return mesh::PreferredDialMultiaddr(matching);
  }
  return mesh::PreferredDialMultiaddr(contact.multiaddrs);
}

} // namespace

mesh::RelayScopeMask CandidateRelayScopes(const mesh::MeshHopCandidate& candidate,
                                    const std::string& local_listen_multiaddr) {
  mesh::RelayScopeMask mask = 0;
  if (candidate.affinity == mesh::MeshHopAffinity::OrgSeed ||
      candidate.affinity == mesh::MeshHopAffinity::DirectoryNode) {
    return static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Org);
  }
  if (candidate.affinity != mesh::MeshHopAffinity::Contact) {
    return mask;
  }
  mask = static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Social);
  if (candidate.multiaddr.empty()) {
    return mask;
  }
  if (mesh::MultiaddrHasPrivateIpv4Host(candidate.multiaddr)) {
    mask |= static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Site);
  }
  if (!local_listen_multiaddr.empty() && mesh::IsSameIpv4Subnet24(candidate.multiaddr, local_listen_multiaddr)) {
    mask |= static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Link);
  }
  return mask;
}

mesh::RelayScopeMask ProviderServeScopeMask(mesh::MeshReachabilityClass reachability, bool node_enabled) {
  if (!node_enabled) {
    return 0;
  }
  switch (reachability) {
  case mesh::MeshReachabilityClass::Blocked:
  case mesh::MeshReachabilityClass::OutboundOnly:
  case mesh::MeshReachabilityClass::Reachable:
  case mesh::MeshReachabilityClass::Unknown:
  default:
    return mesh::kRelayScopeVolunteerServe;
  }
}

std::vector<mesh::MeshHopCandidate> CollectContactHopCandidates(const std::vector<Contact>& contacts) {
  std::vector<mesh::MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  for (const Contact& contact : contacts) {
    if (contact.trust == TrustLevel::Blocked) {
      continue;
    }
    const std::string peer_id = PeerIdFromContact(contact);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    mesh::MeshHopCandidate c;
    c.peer_id = peer_id;
    c.multiaddr = FirstMultiaddrForPeer(contact, peer_id);
    c.affinity = mesh::MeshHopAffinity::Contact;
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> CollectSeedHopCandidates(const std::vector<std::string>& bootstrap_peers) {
  std::vector<mesh::MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  for (const std::string& ma : bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    mesh::MeshHopCandidate c;
    c.peer_id = peer_id;
    c.multiaddr = ma;
    c.affinity = mesh::MeshHopAffinity::OrgSeed;
    out.push_back(std::move(c));
  }
  return out;
}

std::string PreferredDirectoryMultiaddr(const std::vector<std::string>& multiaddrs) {
  return mesh::PreferredDialMultiaddr(multiaddrs);
}

std::vector<mesh::MeshHopCandidate> CollectDirectoryHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes) {
  std::vector<mesh::MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  for (const mesh::MeshDirectoryNode& node : nodes) {
    if (node.peer_id.empty() || !seen.insert(node.peer_id).second) {
      continue;
    }
    mesh::MeshHopCandidate c;
    c.peer_id = node.peer_id;
    c.multiaddr = PreferredDirectoryMultiaddr(node.multiaddrs);
    c.affinity = mesh::MeshHopAffinity::DirectoryNode;
    c.advertises_media_relay = node.media_relay;
    c.advertises_circuit_relay = node.circuit_relay;
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> CollectLedgerGatewayHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes) {
  std::vector<mesh::MeshDirectoryNode> filtered;
  filtered.reserve(nodes.size());
  for (const mesh::MeshDirectoryNode& node : nodes) {
    if (node.ledger_gateway) {
      filtered.push_back(node);
    }
  }
  return CollectDirectoryHopCandidates(filtered);
}

std::vector<mesh::MeshHopCandidate> CollectDhtHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes) {
  auto out = CollectDirectoryHopCandidates(nodes);
  for (mesh::MeshHopCandidate& c : out) {
    c.affinity = mesh::MeshHopAffinity::DhtDiscovered;
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> OrderCircuitHops(std::vector<mesh::MeshHopCandidate> contacts,
                                               std::vector<mesh::MeshHopCandidate> directory,
                                               std::vector<mesh::MeshHopCandidate> dht,
                                               std::vector<mesh::MeshHopCandidate> seeds,
                                               bool prefer_contacts) {
  std::vector<mesh::MeshHopCandidate> out;
  std::unordered_set<std::string> seen;
  auto append = [&](std::vector<mesh::MeshHopCandidate>& list) {
    for (mesh::MeshHopCandidate& c : list) {
      if (c.peer_id.empty() || !seen.insert(c.peer_id).second) {
        continue;
      }
      out.push_back(std::move(c));
    }
  };
  if (prefer_contacts) {
    append(contacts);
    append(directory);
    append(dht);
    append(seeds);
  } else {
    append(directory);
    append(dht);
    append(seeds);
    append(contacts);
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> BuildCircuitHopList(const std::vector<Contact>& contacts,
                                                  const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                                                  const std::vector<mesh::MeshDirectoryNode>& dht_nodes,
                                                  const std::vector<std::string>& bootstrap_peers,
                                                  const bool prefer_contacts, const bool include_seeds) {
  auto contact_hops = CollectContactHopCandidates(contacts);
  auto directory_hops = CollectDirectoryHopCandidates(directory_nodes);
  auto dht_hops = CollectDhtHopCandidates(dht_nodes);
  auto seed_hops = include_seeds ? CollectSeedHopCandidates(bootstrap_peers) : std::vector<mesh::MeshHopCandidate>{};
  return OrderCircuitHops(std::move(contact_hops), std::move(directory_hops), std::move(dht_hops),
                          std::move(seed_hops), prefer_contacts);
}

std::vector<mesh::MeshHopCandidate> RankMediaHops(std::vector<mesh::MeshHopCandidate> candidates,
                                            bool prefer_contacts) {
  std::vector<mesh::MeshHopCandidate> filtered;
  filtered.reserve(candidates.size());
  for (mesh::MeshHopCandidate& c : candidates) {
    if (c.affinity == mesh::MeshHopAffinity::Other) {
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
                   [prefer_contacts](const mesh::MeshHopCandidate& a, const mesh::MeshHopCandidate& b) {
                     return MediaHopScore(a, prefer_contacts) > MediaHopScore(b, prefer_contacts);
                   });
  return filtered;
}

std::vector<mesh::MeshHopCandidate> RankMediaHopsEscalating(
    std::vector<mesh::MeshHopCandidate> candidates, bool prefer_contacts,
    const std::string& local_listen_multiaddr, mesh::RelayScopeMask consumer_scopes) {
  std::vector<mesh::MeshHopCandidate> result;
  std::unordered_set<std::string> picked;
  for (mesh::RelayScope band : kEscalateOrder) {
    if (!mesh::RelayScopeMaskHas(consumer_scopes, band)) {
      continue;
    }
    std::vector<mesh::MeshHopCandidate> band_candidates;
    band_candidates.reserve(candidates.size());
    for (const mesh::MeshHopCandidate& c : candidates) {
      if (picked.count(c.peer_id) > 0) {
        continue;
      }
      const mesh::RelayScopeMask scopes = CandidateRelayScopes(c, local_listen_multiaddr);
      if (!mesh::RelayScopeMaskHas(scopes, band)) {
        continue;
      }
      band_candidates.push_back(c);
    }
    for (mesh::MeshHopCandidate& ranked : RankMediaHops(std::move(band_candidates), prefer_contacts)) {
      if (picked.insert(ranked.peer_id).second) {
        result.push_back(std::move(ranked));
      }
    }
  }
  return result;
}

std::vector<std::string> PeerIdsFromContact(const Contact& contact) {
  std::vector<std::string> out;
  const auto add = [&](const std::string& peer_id) {
    if (peer_id.empty() || std::find(out.begin(), out.end(), peer_id) != out.end()) {
      return;
    }
    out.push_back(peer_id);
  };
  if (const mesh::DirectoryEndpoint* preferred = mesh::PreferredDirectoryEndpoint(contact.remote.endpoints)) {
    add(preferred->peer_id);
  }
  for (const mesh::DirectoryEndpoint& endpoint : contact.remote.endpoints) {
    add(endpoint.peer_id);
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId) {
      add(id.value);
    }
  }
  for (const std::string& ma : contact.multiaddrs) {
    add(PeerIdFromMultiaddr(ma));
  }
  return out;
}

std::string PeerIdFromContact(const Contact& contact) {
  const std::vector<std::string> ids = PeerIdsFromContact(contact);
  if (ids.empty()) {
    return {};
  }
  return ids.front();
}

std::vector<mesh::MeshHopCandidate> ExcludeSelfHop(std::vector<mesh::MeshHopCandidate> candidates,
                                             const std::string& local_peer_id) {
  if (local_peer_id.empty()) {
    return candidates;
  }
  return ExcludePeerIds(std::move(candidates), {local_peer_id});
}

std::vector<mesh::MeshHopCandidate> ExcludePeerIds(std::vector<mesh::MeshHopCandidate> candidates,
                                             const std::unordered_set<std::string>& excluded) {
  if (excluded.empty()) {
    return candidates;
  }
  std::vector<mesh::MeshHopCandidate> out;
  out.reserve(candidates.size());
  for (mesh::MeshHopCandidate& c : candidates) {
    if (excluded.count(c.peer_id) > 0) {
      continue;
    }
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> PreferInCallMediaHops(
    std::vector<mesh::MeshHopCandidate> ranked, const std::unordered_set<std::string>& in_call_peer_ids) {
  if (in_call_peer_ids.empty() || ranked.empty()) {
    return ranked;
  }
  std::vector<mesh::MeshHopCandidate> in_call;
  std::vector<mesh::MeshHopCandidate> rest;
  in_call.reserve(ranked.size());
  rest.reserve(ranked.size());
  for (mesh::MeshHopCandidate& c : ranked) {
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
    const std::vector<std::string> ids = PeerIdsFromContact(contact);
    if (std::find(ids.begin(), ids.end(), peer_id) != ids.end()) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const Contact& contact : contacts) {
    for (const std::string& peer_id : PeerIdsFromContact(contact)) {
      if (peer_id.empty() || !seen.insert(peer_id).second) {
        continue;
      }
      out.push_back(peer_id);
    }
  }
  return out;
}

std::unordered_set<std::string> TrustedRelayPeerIds(const std::vector<Contact>& contacts,
                                                    const std::vector<std::string>& org_seed_multiaddrs) {
  std::unordered_set<std::string> trusted;
  for (const mesh::MeshHopCandidate& seed : CollectSeedHopCandidates(org_seed_multiaddrs)) {
    trusted.insert(seed.peer_id);
  }
  for (const Contact& contact : contacts) {
    if (contact.trust != TrustLevel::Friendly) {
      continue;
    }
    for (const std::string& peer_id : PeerIdsFromContact(contact)) {
      trusted.insert(peer_id);
    }
  }
  trusted.erase("");
  return trusted;
}

std::vector<mesh::MeshHopCandidate> KeepTrustedRelays(std::vector<mesh::MeshHopCandidate> hops,
                                                const std::unordered_set<std::string>& trusted) {
  std::erase_if(hops, [&trusted](const mesh::MeshHopCandidate& hop) { return !trusted.contains(hop.peer_id); });
  return hops;
}

} // namespace pbr
