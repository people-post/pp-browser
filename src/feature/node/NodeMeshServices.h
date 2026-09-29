#pragma once

#include "common/directory/DirectoryTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/people/IdentityStore.h"
#include "foundation/data/Config.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Node-role mesh services, shared by the org seed (`pp-node`) and a desktop in `MeshRole::Node`:
 * which L4 services a node hosts, how its Amp DHT / directory are configured, and the directory
 * row it publishes for itself. Callers own the MeshHost and decide when to (re)apply; policy that
 * differs by deployment (contact-aware admission on desktop, open org-seed admission) stays with
 * the caller or is named for it (`ApplyOrgSeedAdmission`).
 */

/**
 * Host flags for `MeshHost::Start`: a node hosts circuit relay / media relay / DHT per its
 * capabilities and always serves the directory twin (N029 nd4); any other role hosts none.
 * Media relay budget and pricing ride along.
 */
void ApplyNodeHosting(MeshHostConfig& cfg, const MeshConfig& mesh, bool node_role);

/** DHT / directory query peers: directory rows first (N027 L1), then bootstrap seeds; deduped. */
std::vector<std::string> CollectNodeQueryPeerKeys(const std::vector<std::string>& bootstrap_peers,
                                                  const std::vector<MeshDirectoryNode>& directory_nodes = {});

/** Amp DHT: participate (and publish relay records per capability) only as a node with `dht`. */
void ConfigureNodeAmpDht(MeshHost& mesh, IdentityStore& identity, const MeshConfig& config, bool node_role,
                         std::vector<std::string> query_peer_keys);

/** Amp directory: query `query_peer_keys`; host the directory twin only as a node. */
void ConfigureNodeAmpDirectory(MeshHost& mesh, bool node_role, std::vector<std::string> query_peer_keys);

/** This node's directory row: identity, capabilities, advertised (then listen, then configured) addrs. */
MeshNodeHit BuildLocalMeshNodeHit(IdentityStore& identity, MeshHost& mesh, const MeshConfig& config);

/** Org seed: admit strangers for circuit + media relay at short-term + public scope (N018). */
void ApplyOrgSeedAdmission(MeshHost& mesh);

/**
 * Org seed (`pp-node`) after the Amp stack is up: dial endpoints for the bootstrap seeds, DHT and
 * directory against them, a static self row in the directory, and open admission.
 */
void StartOrgSeedServices(MeshHost& mesh, IdentityStore& identity, const MeshConfig& config);

} // namespace pbr
