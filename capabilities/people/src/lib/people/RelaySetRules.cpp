#include "lib/people/RelaySetRules.h"

#include "lib/people/MeshHopPolicy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

std::array<int, 3> NumericVersion(const std::string& version) {
  std::array<int, 3> parts{0, 0, 0};
  const char* p = version.data();
  const char* const end = p + version.size();
  for (int& part : parts) {
    const auto [next, ec] = std::from_chars(p, end, part);
    if (ec != std::errc()) {
      break;
    }
    p = next;
    if (p == end || *p != '.') {
      break;
    }
    ++p;
  }
  return parts;
}

bool BelowFloor(const RelaySetContext& context, const std::string& peer_id) {
  if (context.preferred_min_node_version.empty()) {
    return false;
  }
  const auto it = context.claimed_version_of.find(peer_id);
  // A node that says nothing is from before node versions were said: older than any floor.
  return it == context.claimed_version_of.end() || it->second.empty() ||
         CompareNodeVersions(it->second, context.preferred_min_node_version) < 0;
}

std::string OperatorOf(const RelaySetContext& context, const std::string& peer_id) {
  const auto it = context.operator_of.find(peer_id);
  return it == context.operator_of.end() ? std::string() : it->second;
}

} // namespace

int CompareNodeVersions(const std::string& a, const std::string& b) {
  const auto x = NumericVersion(a);
  const auto y = NumericVersion(b);
  return x < y ? -1 : (y < x ? 1 : 0);
}

RelaySetContext MakeRelaySetContext(const std::vector<Contact>& contacts,
                                    const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                                    const std::vector<std::string>& org_seed_multiaddrs,
                                    std::string preferred_min_node_version, const MeshNodeReputation* reputation,
                                    const int64_t now_ms) {
  RelaySetContext context;
  context.anchors = TrustedRelayPeerIds(contacts, org_seed_multiaddrs);
  for (const mesh::MeshDirectoryNode& node : directory_nodes) {
    if (node.peer_id.empty()) {
      continue;
    }
    if (!node.account_id.empty()) {
      context.operator_of.emplace(node.peer_id, node.account_id);
    }
    if (!node.node_version.empty()) {
      context.claimed_version_of.emplace(node.peer_id, node.node_version);
    }
  }
  context.preferred_min_node_version = std::move(preferred_min_node_version);
  context.reputation = reputation;
  context.now_ms = now_ms;
  return context;
}

std::vector<mesh::MeshHopCandidate> ApplyRelaySetRules(std::vector<mesh::MeshHopCandidate> hops,
                                                       const RelaySetContext& context) {
  enum Band : size_t { kFirst = 0, kOperatorRepeat, kBelowFloor, kDemoted, kBands };
  std::array<std::vector<mesh::MeshHopCandidate>, kBands> bands;
  std::unordered_set<std::string> operators_seen;
  for (mesh::MeshHopCandidate& hop : hops) {
    const std::string op = OperatorOf(context, hop.peer_id);
    Band band = kFirst;
    if (context.reputation && context.reputation->IsDemoted(hop.peer_id, op, context.now_ms)) {
      band = kDemoted;
    } else if (BelowFloor(context, hop.peer_id)) {
      band = kBelowFloor;
    } else if (!op.empty() && !context.anchors.contains(hop.peer_id) && !operators_seen.insert(op).second) {
      band = kOperatorRepeat;
    }
    bands[band].push_back(std::move(hop));
  }
  std::vector<mesh::MeshHopCandidate> out;
  out.reserve(hops.size());
  for (auto& band : bands) {
    std::move(band.begin(), band.end(), std::back_inserter(out));
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> AnchorLed(std::vector<mesh::MeshHopCandidate> hops,
                                              const RelaySetContext& context) {
  std::deque<mesh::MeshHopCandidate> anchors;
  std::deque<mesh::MeshHopCandidate> others;
  for (mesh::MeshHopCandidate& hop : hops) {
    (context.anchors.contains(hop.peer_id) ? anchors : others).push_back(std::move(hop));
  }
  std::vector<mesh::MeshHopCandidate> out;
  out.reserve(anchors.size() + others.size());
  while (!anchors.empty()) {
    out.push_back(std::move(anchors.front()));
    anchors.pop_front();
    for (size_t i = 0; i < kMaxUnknownBetweenAnchors && !others.empty(); ++i) {
      out.push_back(std::move(others.front()));
      others.pop_front();
    }
  }
  std::move(others.begin(), others.end(), std::back_inserter(out));
  return out;
}

bool ClaimsContradicted(const mesh::MeshDirectoryNode& row, const ObservedNode& seen, const int our_amp_wire) {
  if (seen.answered_other_wire && row.amp_wire == our_amp_wire) {
    return true; // kept because it said it speaks ours
  }
  if (!seen.told) {
    return false;
  }
  if (row.amp_wire > 0 && seen.amp_wire > 0 && row.amp_wire != seen.amp_wire) {
    return true;
  }
  if (!row.node_version.empty() && !seen.node_version.empty() &&
      CompareNodeVersions(row.node_version, seen.node_version) > 0) {
    return true; // claims newer than it runs; older claims are a node upgraded since it registered
  }
  for (const std::string& feature : row.features) {
    if (std::find(seen.features.begin(), seen.features.end(), feature) == seen.features.end()) {
      return true;
    }
  }
  return false;
}

void AuditNodeClaims(const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                     const std::function<ObservedNode(const std::string& peer_id)>& observe,
                     MeshNodeReputation& reputation, const int our_amp_wire, const int64_t now_ms) {
  if (!observe) {
    return;
  }
  for (const mesh::MeshDirectoryNode& row : directory_nodes) {
    if (row.peer_id.empty() || (row.amp_wire == 0 && row.node_version.empty() && row.features.empty())) {
      continue; // says nothing: nothing to contradict
    }
    if (ClaimsContradicted(row, observe(row.peer_id), our_amp_wire)) {
      reputation.NoteContradiction(row.peer_id, row.account_id, now_ms);
    }
  }
}

} // namespace pbr
