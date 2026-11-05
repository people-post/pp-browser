#pragma once

#include "lib/people/ContactTypes.h"
#include "lib/people/MeshNodeReputation.h"

#include "mesh/types/MeshConfigTypes.h"
#include "mesh/types/MeshHopTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pbr {

/**
 * The rules a relay list is composed by (N032 rules 5–8), against nodes that may lie about themselves. A node's own
 * claims (its directory row) only ever demote it; what this profile verified and remembers decides the rest. Nothing
 * here drops a candidate: an untrusted relay is only tried later, so the mesh still has it when nothing else works.
 */
struct RelaySetContext {
  /** Org seeds and Friendly contacts' nodes (TrustedRelayPeerIds): every set should keep one. */
  std::unordered_set<std::string> anchors;
  /** A directory node's operator (its account id), by PeerId. */
  std::unordered_map<std::string, std::string> operator_of;
  /** A directory node's claimed version, by PeerId; a node not in it said nothing. */
  std::unordered_map<std::string, std::string> claimed_version_of;
  /** Nodes said to run older than this (or nothing) are tried later; empty = no floor. */
  std::string preferred_min_node_version;
  const MeshNodeReputation* reputation = nullptr;
  int64_t now_ms = 0;
};

RelaySetContext MakeRelaySetContext(const std::vector<Contact>& contacts,
                                    const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                                    const std::vector<std::string>& org_seed_multiaddrs,
                                    std::string preferred_min_node_version, const MeshNodeReputation* reputation,
                                    int64_t now_ms);

/**
 * Order kept within each band, bands in this order: the rest; a second (or later) non-anchor relay of an operator
 * already in the list (rule 7); a node below the version floor; a node or operator this profile demoted (rules 6, 8).
 */
std::vector<mesh::MeshHopCandidate> ApplyRelaySetRules(std::vector<mesh::MeshHopCandidate> hops,
                                                       const RelaySetContext& context);

/** Between two anchors, at most this many other relays while anchors remain (rule 7). */
inline constexpr size_t kMaxUnknownBetweenAnchors = 2;

/**
 * Rule 7 for a set the mesh picks from the front (park spots, circuit relays): an anchor first, then at most
 * kMaxUnknownBetweenAnchors others before the next anchor, as long as anchors remain; the rest after. Order is
 * otherwise kept. Without anchors the list is unchanged. Apply before ApplyRelaySetRules, whose bands then only take
 * nodes from between anchors and put the distrusted behind all of them.
 */
std::vector<mesh::MeshHopCandidate> AnchorLed(std::vector<mesh::MeshHopCandidate> hops, const RelaySetContext& context);

/** -1, 0, 1 comparing the numeric `MAJOR.MINOR.PATCH` of two versions (a pre-release suffix is ignored). */
int CompareNodeVersions(const std::string& a, const std::string& b);

/** What this profile saw of a node itself, to check its directory row against (rules 5, 6). */
struct ObservedNode {
  /** It was reached, and told its own over channel 0 (the fields below are then set). */
  bool told = false;
  int amp_wire = 0;
  std::string node_version;
  std::vector<std::string> features;
  /** A dial failed because it answered in another wire version. */
  bool answered_other_wire = false;
};

/**
 * Whether `row`'s claims are contradicted by what was observed — only in the direction a liar gains: claiming our
 * wire version and answering in another, claiming a newer version than it runs, or a feature it lacks. A node that
 * upgraded since it registered, or says nothing, is never contradicted.
 */
bool ClaimsContradicted(const mesh::MeshDirectoryNode& row, const ObservedNode& seen, int our_amp_wire);

/**
 * Rules 5, 6: check each directory node's claims against what this profile saw of it (`observe`), and demote in
 * `reputation` the node, and its operator, whose claims were contradicted. Runs with each relay list built.
 */
void AuditNodeClaims(const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                     const std::function<ObservedNode(const std::string& peer_id)>& observe,
                     MeshNodeReputation& reputation, int our_amp_wire, int64_t now_ms);

} // namespace pbr
