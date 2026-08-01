#pragma once

#include "base/people/ContactTypes.h"
#include "base/people/RelayScope.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Affinity class for hop scoring (N014 / N020). */
enum class MeshHopAffinity {
  Contact = 0,
  OrgSeed = 1,
  Other = 2,
};

/** Mirrors ReachabilityStatus for provider caps without requiring libp2p headers in callers. */
enum class MeshReachabilityClass {
  Unknown,
  Reachable,
  OutboundOnly,
  Blocked,
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
 * When `prefer_contacts` is false, OrgSeed outranks Contact at equal capacity (seeds first).
 */
std::vector<MeshHopCandidate> RankMediaHops(std::vector<MeshHopCandidate> candidates,
                                            bool prefer_contacts = true);

/**
 * N023 ns1: escalate scope bands (link→site→social→org), rank within each via RankMediaHops.
 * `local_listen_multiaddr` enables same-/24 link boost among eligible contacts only.
 * `consumer_scopes` defaults to short-term mask (no public).
 */
std::vector<MeshHopCandidate> RankMediaHopsEscalating(
    std::vector<MeshHopCandidate> candidates, bool prefer_contacts,
    const std::string& local_listen_multiaddr = {},
    RelayScopeMask consumer_scopes = kRelayScopeShortTerm);

/** Scope tags for a candidate (eligible closed-set peers only). */
RelayScopeMask CandidateRelayScopes(const MeshHopCandidate& candidate,
                                    const std::string& local_listen_multiaddr = {});

/** Provider advertisement cap from reachability (N023). */
RelayScopeMask ProviderServeScopeMask(MeshReachabilityClass reachability, bool node_enabled);

/** True when both multiaddrs share the same IPv4 /24 (v1 link scope inference). */
bool IsSameIpv4Subnet24(const std::string& multiaddr_a, const std::string& multiaddr_b);

/** Drop hop whose peer_id equals `local_peer_id` (never dial self as media_relay). */
std::vector<MeshHopCandidate> ExcludeSelfHop(std::vector<MeshHopCandidate> candidates,
                                             const std::string& local_peer_id);

/** Drop hops whose peer_id is in `excluded`. */
std::vector<MeshHopCandidate> ExcludePeerIds(std::vector<MeshHopCandidate> candidates,
                                             const std::unordered_set<std::string>& excluded);

/**
 * Soft-migrate preference: dialable in-call peers first (e.g. Windows already on the call
 * with Media relay), then remaining ranked hops (out-of-call Linux, org seed, …).
 * Peers without multiaddr stay in place but SoftMigrate will skip them quickly.
 */
std::vector<MeshHopCandidate> PreferInCallMediaHops(std::vector<MeshHopCandidate> ranked,
                                                   const std::unordered_set<std::string>& in_call_peer_ids);

/** PeerId from a contact row (ContactIdKind::PeerId or multiaddr /p2p/). */
std::string PeerIdFromContact(const Contact& contact);

/** True if peer_id appears as ContactIdKind::PeerId on any contact. */
bool IsContactPeerId(const std::vector<Contact>& contacts, const std::string& peer_id);

/** All PeerId values from contacts (for provider admission). */
std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts);

} // namespace pbr
