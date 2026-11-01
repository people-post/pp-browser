#pragma once

#include "feature/conversations/HubMeshOwner.h"
#include "common/Module.h"
#include "foundation/platform/NetworkMonitor.h"

#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

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

private:
  void ConfigureDht(const QueryInputs& inputs, bool node_role);
  void ConfigureDirectory(const QueryInputs& inputs, bool node_role);

  std::unique_ptr<mesh::MeshHost> mesh_;
  /** The OS network monitor while the hub watches. */
  std::unique_ptr<NetworkMonitor> network_monitor_;
  /** LAN discovery and announcement while the hub's mesh services run. */
  std::unique_ptr<mesh::LanMdnsDiscovery> lan_mdns_;
  /** A node tries UPnP on its first start only. */
  bool upnp_auto_tried_ = false;
};

} // namespace pbr
