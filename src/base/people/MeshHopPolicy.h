#pragma once

#include "base/data/Config.h"
#include "base/people/ContactTypes.h"
#include "common/RelayScope.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Affinity class for hop scoring (N014 / N020). */
enum class MeshHopAffinity {
  Contact = 0,
  DirectoryNode = 1,
  DhtDiscovered = 2,
  OrgSeed = 3,
  Other = 4,
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
  /** From mesh directory capabilities (n-dir); used for media/circuit filters. */
  bool advertises_media_relay = false;
  bool advertises_circuit_relay = false;
};

/**
 * Collect contact PeerIds (+ first multiaddr) for closed-set hops.
 * Skips contacts without a PeerId identity.
 */
std::vector<MeshHopCandidate> CollectContactHopCandidates(const std::vector<Contact>& contacts);

/** Collect org seed peers from configured bootstrap multiaddrs. */
std::vector<MeshHopCandidate> CollectSeedHopCandidates(const std::vector<std::string>& bootstrap_peers);

/** Collect infra mesh_node peers from directory cache (N027 / n-dir). */
std::vector<MeshHopCandidate> CollectDirectoryHopCandidates(const std::vector<MeshDirectoryNode>& nodes);

/**
 * Directory/DHT nodes advertising `ledger_gateway` (N029 Phase C prep).
 * Unused by product dial paths until ledger transport lands.
 */
std::vector<MeshHopCandidate> CollectLedgerGatewayHopCandidates(const std::vector<MeshDirectoryNode>& nodes);

/** Collect DHT-discovered infra peers (n2-caps); same node shape as directory. */
std::vector<MeshHopCandidate> CollectDhtHopCandidates(const std::vector<MeshDirectoryNode>& nodes);

/**
 * Circuit hop order (nf / N014): contacts → directory → seeds when `prefer_contacts`;
 * directory → seeds → contacts when false. Dedupes by peer_id (earlier tier wins).
 */
std::vector<MeshHopCandidate> OrderCircuitHops(std::vector<MeshHopCandidate> contacts,
                                               std::vector<MeshHopCandidate> directory,
                                               std::vector<MeshHopCandidate> dht,
                                               std::vector<MeshHopCandidate> seeds,
                                               bool prefer_contacts = true);

/** Collect + order circuit/media hop candidates (n-dir + n2-caps). Omits seeds when `include_seeds` is false. */
std::vector<MeshHopCandidate> BuildCircuitHopList(const std::vector<Contact>& contacts,
                                                  const std::vector<MeshDirectoryNode>& directory_nodes,
                                                  const std::vector<MeshDirectoryNode>& dht_nodes,
                                                  const std::vector<std::string>& bootstrap_peers,
                                                  bool prefer_contacts, bool include_seeds = true);

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

/**
 * Soft-migrate: prepend local PeerId as hop (AttachAsLocalHop locally).
 * `local_multiaddr` is advertised in CallSfuAttach so remotes can dial this Node — leave empty
 * only in unit tests; production SoftMigrate must pass a LAN/advertise multiaddr.
 * No-op when local_peer_id empty. Dedupes if already present.
 */
std::vector<MeshHopCandidate> PreferLocalMediaHop(std::vector<MeshHopCandidate> ranked,
                                                  const std::string& local_peer_id,
                                                  const std::string& local_multiaddr = {});

/** Preferred PeerId from a contact (newest directory endpoint, else first PeerId / multiaddr). */
std::string PeerIdFromContact(const Contact& contact);

/** All device Peer IDs on a contact (directory endpoints + PeerId ids + multiaddrs). */
std::vector<std::string> PeerIdsFromContact(const Contact& contact);

/** True if peer_id appears as ContactIdKind::PeerId on any contact. */
bool IsContactPeerId(const std::vector<Contact>& contacts, const std::string& peer_id);

/** All PeerId values from contacts (for provider admission). */
std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts);

} // namespace pbr
