#pragma once

#include "common/Lifetime.h"
#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/people/ContactsStore.h"
#include "foundation/data/Config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Product inputs for the mesh connectivity's hop candidates (media-client-layers L015). */
struct MeshConnectivityWiringInputs {
  std::function<MeshHost*()> mesh;
  ContactsStore* contacts = nullptr;
  /** Published snapshot: the policy is computed off the hub's thread (MeshHopPolicyFeed). */
  std::function<std::shared_ptr<const MeshConfig>()> mesh_config;
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  /** Reachability seed probe — when false, the rendezvous surface skips org seeds. */
  std::function<bool()> seed_dial_ok;
  std::function<void(const std::string& peer_id)> note_lan_peer_id;
  std::function<void(const std::string& key, const std::string& multiaddr)> register_direct_endpoint;
};

/** The connectivity's ports: the mesh it wires from and its LAN / delivery-plane notices. */
MeshConnectivityDeps MakeMeshConnectivityDeps(const MeshConnectivityWiringInputs& in);

/**
 * The candidate policy as of now: rendezvous surface = contacts ∪ directory ∪ DHT ∪ effective seeds;
 * bootstrap seeds = configured ∪ directory; punch introducers = contacts, then configured seeds;
 * trusted relays only narrows all three (privacy T4). Reads the contacts store, so never on the
 * mesh's threads.
 */
MeshHopPolicy ComputeMeshHopPolicy(const MeshConnectivityWiringInputs& in);

/**
 * Pushes the candidate policy into the mesh (THREADING.md § Mesh threads, rule 1): at Start, on
 * `PushSoon` when an input changed, and every few seconds for the inputs that change without notice
 * (directory, DHT, seed reachability). Computed on a worker, never on the mesh's threads.
 *
 * Start / Stop / PushNow on the hub's thread. Between Stop and the next Start nothing is computed —
 * the inputs read the hub's mesh, which may be going away — so Stop comes before the mesh stops.
 */
class MeshHopPolicyFeed {
public:
  static constexpr std::chrono::milliseconds kRefresh{5000};

  explicit MeshHopPolicyFeed(MeshConnectivity& connectivity);
  ~MeshHopPolicyFeed();
  MeshHopPolicyFeed(const MeshHopPolicyFeed&) = delete;
  MeshHopPolicyFeed& operator=(const MeshHopPolicyFeed&) = delete;

  /** Push now, on the caller, and keep pushing; Start again just pushes. */
  void Start();
  /** No push from now on; waits for one in progress on a worker. Idempotent. */
  void Stop();
  /** Any thread: an input changed — push from a worker (dropped while stopped). */
  void PushSoon();
  /** Where the inputs come from (the hub rebuilds them); the next push reads these. */
  void SetInputs(MeshConnectivityWiringInputs inputs);

private:
  void PushNow();
  void ArmRefresh();

  MeshConnectivity& connectivity_;
  /** Pushes in order: each reads the inputs as of its turn, so an older one never lands last. */
  std::mutex push_mu_;
  MeshConnectivityWiringInputs inputs_;  // push_mu_
  /** Read inside the bound push: one that saw it set is still running when Stop waits. */
  std::atomic<bool> started_{false};
  uint64_t refresh_timer_ = 0;  // hub's thread
  pp::Lifetime lifetime_;
};

} // namespace pbr
