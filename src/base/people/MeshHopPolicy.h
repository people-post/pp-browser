#pragma once

#include "base/people/ContactTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** Affinity class for hop scoring (N014 / N020). */
enum class MeshHopAffinity {
  Contact = 0,
  OrgSeed = 1,
  Other = 2,
};

struct MeshHopCandidate {
  std::string peer_id;
  /** Dial multiaddr including `/p2p/<PeerId>` when known. */
  std::string multiaddr;
  MeshHopAffinity affinity = MeshHopAffinity::Other;
  bool dialable = true;
  bool recently_failed = false;
  /** 0..1 residual capacity estimate; 1 = fully free. */
  double residual_capacity = 1.0;
};

/**
 * Collect contact PeerIds (+ first multiaddr) for closed-set hops.
 * Skips contacts without a PeerId identity.
 */
std::vector<MeshHopCandidate> CollectContactHopCandidates(const std::vector<Contact>& contacts);

/** Collect org seed peers from configured bootstrap multiaddrs. */
std::vector<MeshHopCandidate> CollectSeedHopCandidates(const std::vector<std::string>& bootstrap_peers);

/**
 * Circuit hop order (nf / N014): contacts first when `prefer_contacts`, then seeds.
 * Dedupes by peer_id (contact wins over seed).
 */
std::vector<MeshHopCandidate> OrderCircuitHops(std::vector<MeshHopCandidate> contacts,
                                               std::vector<MeshHopCandidate> seeds,
                                               bool prefer_contacts = true);

/**
 * Media hop rank (n4 / N020 short-term): filter → score over contacts ∪ seeds only.
 * Drops Other affinity, undialable, and recently_failed below quality floor.
 * Higher score first. Affinity is a bonus; residual capacity matters.
 */
std::vector<MeshHopCandidate> RankMediaHops(std::vector<MeshHopCandidate> candidates);

/** True if peer_id appears as ContactIdKind::PeerId on any contact. */
bool IsContactPeerId(const std::vector<Contact>& contacts, const std::string& peer_id);

/** All PeerId values from contacts (for provider admission). */
std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts);

} // namespace pbr
