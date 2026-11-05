#pragma once

#include "feature/conversations/HubMeshOwner.h"
#include "common/Module.h"
#include "foundation/platform/NetworkMonitor.h"
#include "mesh/discovery/MeshDirectoryCache.h"
#include "mesh/media_plane/MeshMediaRelay.h"

#include <memory>
#include <mutex>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** Last-good directory nodes file; per profile, hence per backend. */
std::string MeshDirectoryNodesPath(const std::string& profile_data_dir);
/** `{profile data dir}/mesh_node_reputation.json` (N032). */
std::string MeshNodeReputationPath(const std::string& profile_data_dir);

/**
 * The app's mesh: one `MeshHost`, built for the profile's role and lent to the conversations hub
 * (`HubMeshOwner`). The node role's services — what it hosts and serves, the DHT and directory it
 * runs, its own directory row — come from domain/node, the same ones pp-node runs; messaging never
 * names them.
 */
class ProductMesh : public HubMeshOwner, public Module {
public:
  ProductMesh();
  ~ProductMesh() override;
  ProductMesh(const ProductMesh&) = delete;
  ProductMesh& operator=(const ProductMesh&) = delete;

  Roe<mesh::MeshHost*> Start(StartRequest request) override;
  void ConfigureQueries(const QueryInputs& inputs) override;
  void ApplyServing(const MeshConfig& config) override;
  void Stop() override;
  void Release() override;
  void WatchNetwork(std::function<void(const NetworkChange&)> on_change) override;
  void StopWatchingNetwork() override;
  void StartLanDiscovery(std::function<void(const mesh::LanMdnsDiscoveredPeer&)> on_peer) override;
  void AdvertiseOnLan(const std::string& peer_id, int amp_udp_port) override;
  void StopLanDiscovery() override;
  void StartDirectoryCache(mesh::IDirectoryClient& directory, const std::string& profile_data_dir,
                           std::function<void()> on_updated) override;
  bool HasDirectoryCache() const override { return directory_cache_ != nullptr; }
  std::vector<mesh::MeshDirectoryNode> DirectoryNodesSnapshot() const override;
  void RefreshDirectory() override;
  void MaybeRefreshDirectory() override;
  void StopDirectoryCache() override;
  std::shared_ptr<MeshNodeReputation> NodeReputation() const override;
  mesh::MeshConnectivity& Connectivity() override { return *connectivity_; }
  /** The media relay client (calls and broadcast). */
  mesh::MeshMediaRelay& MediaRelay() { return *media_relay_; }
  void SetMeshInputs(const MeshConnectivityWiringInputs& inputs) override;
  void WireMeshObjects() override;
  void DetachMeshObjects() override;
  void BeginMeshStop() override;
  void DropMediaRelayClient() override;
  void EndMeshStop() override;
  void RefreshMeshView() override;
  void PushHopPolicySoon() override;
  void StayReachable(bool want) override;
  void ClearMeshObjects() override;

private:
  void ConfigureDht(const QueryInputs& inputs, bool node_role);
  void ConfigureDirectory(const QueryInputs& inputs, bool node_role);

  // The neutral mesh objects outlive the mesh they ride (declared before it: they go after it).
  std::unique_ptr<mesh::MeshConnectivity> connectivity_;
  /** The media_relay client, built on connectivity (declared after it: goes first). */
  std::unique_ptr<mesh::MeshMediaRelay> media_relay_;
  /** Pushes the candidate policy into connectivity while the mesh runs (declared after it: goes first). */
  std::unique_ptr<MeshHopPolicyFeed> hop_feed_;
  /** The seed reservation's renewal (coordinator timer); 0 = none. */
  uint64_t stay_reachable_timer_ = 0;
  std::unique_ptr<mesh::MeshHost> mesh_;
  /** The OS network monitor while the hub watches. */
  std::unique_ptr<NetworkMonitor> network_monitor_;
  /** LAN discovery and announcement while the hub's mesh services run. */
  std::unique_ptr<mesh::LanMdnsDiscovery> lan_mdns_;
  /** The directory's mesh nodes, for the profile's lifetime (across mesh restarts). */
  std::unique_ptr<mesh::MeshDirectoryCache> directory_cache_;
  /** N032: this profile's verdicts on mesh nodes; replaced with the directory cache (a profile switch). */
  mutable std::mutex reputation_mu_;
  std::shared_ptr<MeshNodeReputation> node_reputation_ = std::make_shared<MeshNodeReputation>();
  /** A node tries UPnP on its first start only. */
  bool upnp_auto_tried_ = false;
};

} // namespace pbr
