#pragma once

#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/people/ContactsStore.h"
#include "foundation/data/Config.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Product inputs for the mesh connectivity's hop candidates (media-client-layers L015). */
struct MeshConnectivityWiringInputs {
  std::function<MeshHost*()> mesh;
  ContactsStore* contacts = nullptr;
  /** Published snapshot: the providers run on the Connectivity owner. */
  std::function<std::shared_ptr<const MeshConfig>()> mesh_config;
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  /** Reachability seed probe — when false, the rendezvous surface skips org seeds. */
  std::function<bool()> seed_dial_ok;
  std::function<void(const std::string& peer_id)> note_lan_peer_id;
  std::function<void(const std::string& key, const std::string& multiaddr)> register_direct_endpoint;
};

/**
 * Candidate policy (MeshHopPolicy) as MeshConnectivity ports: rendezvous surface = contacts ∪
 * directory ∪ DHT ∪ effective seeds; bootstrap seeds = configured ∪ directory; punch introducers
 * = contacts, then configured seeds.
 */
MeshConnectivityDeps MakeMeshConnectivityDeps(MeshConnectivityWiringInputs in);

} // namespace pbr
