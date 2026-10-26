#pragma once

#include "foundation/data/Config.h"
#include "lib/people/ContactTypes.h"
#include "mesh/types/MeshHopDial.h"
#include "mesh/types/MeshHopTypes.h"
#include "mesh/types/RelayScope.h"
#include "common/MeshAlias.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/**
 * Collect contact PeerIds (+ first multiaddr) for closed-set hops.
 * Skips contacts without a PeerId identity.
 */
std::vector<mesh::MeshHopCandidate> CollectContactHopCandidates(const std::vector<Contact>& contacts);

/** Collect org seed peers from configured bootstrap multiaddrs. */
std::vector<mesh::MeshHopCandidate> CollectSeedHopCandidates(const std::vector<std::string>& bootstrap_peers);

/** Collect infra mesh_node peers from directory cache (N027 / n-dir). */
std::vector<mesh::MeshHopCandidate> CollectDirectoryHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes);

/**
 * Directory/DHT nodes advertising `ledger_gateway` (N029 Phase C prep).
 * Unused by product dial paths until ledger transport lands.
 */
std::vector<mesh::MeshHopCandidate> CollectLedgerGatewayHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes);

/** Collect DHT-discovered infra peers (n2-caps); same node shape as directory. */
std::vector<mesh::MeshHopCandidate> CollectDhtHopCandidates(const std::vector<mesh::MeshDirectoryNode>& nodes);

/**
 * Circuit hop order (nf / N014): contacts → directory → seeds when `prefer_contacts`;
 * directory → seeds → contacts when false. Dedupes by peer_id (earlier tier wins).
 */
std::vector<mesh::MeshHopCandidate> OrderCircuitHops(std::vector<mesh::MeshHopCandidate> contacts,
                                               std::vector<mesh::MeshHopCandidate> directory,
                                               std::vector<mesh::MeshHopCandidate> dht,
                                               std::vector<mesh::MeshHopCandidate> seeds,
                                               bool prefer_contacts = true);

/** Each node's addresses as DialWorthyMultiaddrs keeps them: a directory is read off the node's LAN. */
std::vector<mesh::MeshDirectoryNode> DirectoryNodesForDial(std::vector<mesh::MeshDirectoryNode> nodes);

/**
 * Circuit relays among `hops`: the configured org seeds, and peers the directory or the DHT lists with
 * `circuit_relay`. A contact's phone or a peer the DHT merely knows is no relay — it refuses a circuit
 * (#379) — so it is not offered as one.
 */
std::vector<mesh::MeshHopCandidate> KeepCircuitRelays(std::vector<mesh::MeshHopCandidate> hops,
                                                     const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                                                     const std::vector<mesh::MeshDirectoryNode>& dht_nodes,
                                                     const std::vector<std::string>& org_seeds);

/** Collect + order circuit/media hop candidates (n-dir + n2-caps). Omits seeds when `include_seeds` is false. */
std::vector<mesh::MeshHopCandidate> BuildCircuitHopList(const std::vector<Contact>& contacts,
                                                  const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
                                                  const std::vector<mesh::MeshDirectoryNode>& dht_nodes,
                                                  const std::vector<std::string>& bootstrap_peers,
                                                  bool prefer_contacts, bool include_seeds = true);

/**
 * Media hop rank (n4 / N020 short-term): filter → score over contacts ∪ seeds only.
 * Drops Other affinity, undialable, and recently_failed below quality floor.
 * Higher score first. Affinity is a bonus; residual capacity matters.
 * When `prefer_contacts` is false, OrgSeed outranks Contact at equal capacity (seeds first).
 */
std::vector<mesh::MeshHopCandidate> RankMediaHops(std::vector<mesh::MeshHopCandidate> candidates,
                                            bool prefer_contacts = true);

/**
 * N023 ns1: escalate scope bands (link→site→social→org), rank within each via RankMediaHops.
 * `local_listen_multiaddr` enables same-/24 link boost among eligible contacts only.
 * `consumer_scopes` defaults to short-term mask (no public).
 */
std::vector<mesh::MeshHopCandidate> RankMediaHopsEscalating(
    std::vector<mesh::MeshHopCandidate> candidates, bool prefer_contacts,
    const std::string& local_listen_multiaddr = {},
    mesh::RelayScopeMask consumer_scopes = mesh::kRelayScopeShortTerm);

/**
 * A group call's SFU hop: durable relays (org seeds, directory and DHT nodes) before contacts' nodes,
 * each part in its order (#385). An SFU's upload grows with the square of the members, which a home
 * node runs out of first; a contact's node stays a fallback.
 */
std::vector<mesh::MeshHopCandidate> DurableRelaysFirst(std::vector<mesh::MeshHopCandidate> hops);

/** Scope tags for a candidate (eligible closed-set peers only). */
mesh::RelayScopeMask CandidateRelayScopes(const mesh::MeshHopCandidate& candidate,
                                    const std::string& local_listen_multiaddr = {});

/** Provider advertisement cap from reachability (N023). */
mesh::RelayScopeMask ProviderServeScopeMask(mesh::MeshReachabilityClass reachability, bool node_enabled);

/** Drop hop whose peer_id equals `local_peer_id` (never dial self as media_relay). */
std::vector<mesh::MeshHopCandidate> ExcludeSelfHop(std::vector<mesh::MeshHopCandidate> candidates,
                                             const std::string& local_peer_id);

/** Drop hops whose peer_id is in `excluded`. */
std::vector<mesh::MeshHopCandidate> ExcludePeerIds(std::vector<mesh::MeshHopCandidate> candidates,
                                             const std::unordered_set<std::string>& excluded);

/**
 * Soft-migrate preference: dialable in-call peers first (e.g. Windows already on the call
 * with Media relay), then remaining ranked hops (out-of-call Linux, org seed, …).
 * Peers without multiaddr stay in place but SoftMigrate will skip them quickly.
 */
std::vector<mesh::MeshHopCandidate> PreferInCallMediaHops(std::vector<mesh::MeshHopCandidate> ranked,
                                                   const std::unordered_set<std::string>& in_call_peer_ids);

/** Preferred PeerId from a contact (newest directory endpoint, else first PeerId / multiaddr). */
std::string PeerIdFromContact(const Contact& contact);

/** All device Peer IDs on a contact (directory endpoints + PeerId ids + multiaddrs). */
std::vector<std::string> PeerIdsFromContact(const Contact& contact);

/** True if peer_id appears as ContactIdKind::PeerId on any contact. */
bool IsContactPeerId(const std::vector<Contact>& contacts, const std::string& peer_id);

/** All PeerId values from contacts (for provider admission). */
std::vector<std::string> ContactPeerIds(const std::vector<Contact>& contacts);

/**
 * projects/privacy T4 — relays trusted with both ends' addresses: the configured org seeds
 * (`org_seed_multiaddrs`, not directory-merged ones) and Friendly contacts' nodes.
 */
std::unordered_set<std::string> TrustedRelayPeerIds(const std::vector<Contact>& contacts,
                                                    const std::vector<std::string>& org_seed_multiaddrs);
/** `hops` without the ones outside `trusted` (order kept). */
std::vector<mesh::MeshHopCandidate> KeepTrustedRelays(std::vector<mesh::MeshHopCandidate> hops,
                                                const std::unordered_set<std::string>& trusted);

} // namespace pbr
