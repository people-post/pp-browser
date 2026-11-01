#pragma once

#include "common/MeshConfig.h"
#include "common/ServiceEndpoints.h"
#include "lib/people/IdentityStore.h"
#include "mesh/host/MeshHost.h"
#include "mesh/reachability/LanMdnsDiscovery.h"
#include "mesh/types/AddressDisclosure.h"
#include "mesh/types/DirectoryTypes.h"
#include "mesh/types/IDirectoryClient.h"
#include "common/Error.h"
#include "common/NetworkChange.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * Who owns the hub's mesh (feature/product's `ProductMesh`). Messaging runs on the mesh but never
 * builds it: the owner gives it the profile's role — what it hosts and serves, its DHT and
 * directory, the row it publishes for itself — when the hub asks, and frees it when the hub lets it
 * go. The hub keeps the order of the way down (participants, transports) around Stop / Release.
 *
 * Calls come from the hub: Start on a worker (the reachability probe may block), the rest on the
 * hub's (UI) thread.
 */
class HubMeshOwner {
public:
  struct StartRequest {
    /** The mesh slice (normalized; the owner applies the device's role). */
    MeshConfig mesh;
    /** The device's keys (the mesh's identity). */
    IdentityStore* identity = nullptr;
    /** Who may learn our address; outlives the mesh. */
    mesh::AddressDisclosureGate* address_disclosure = nullptr;
    /** Shutdown was requested: start stopping, skip the blocking probe. */
    bool stopping = false;
    /** The reachability probe finished — on the mesh's io lane. */
    std::function<void()> on_reachability_updated;
    /** The UDP socket was checked (a report line) — on the mesh's io lane. */
    std::function<void(const mesh::UdpSocketCheck&)> on_udp_socket_check;
  };

  /** What the DHT and directory ask, as the hub knows it now. */
  struct QueryInputs {
    MeshConfig mesh;
    /** The directory providers (an Amp one is asked through the mesh's directory twin). */
    DirectoryConfig directory;
    IdentityStore* identity = nullptr;
    /** The directory's mesh nodes worth dialling (read again whenever the directory twin lists nodes). */
    std::function<std::vector<mesh::MeshDirectoryNode>()> directory_nodes;
    /** The reach record holds `peer_id` down: the DHT neither dials nor keeps it. */
    std::function<bool(const std::string& peer_id)> reach_held_down;
    /** Which of `keys` a directory list asks (a couple that are up). */
    std::function<std::vector<std::string>(const std::vector<std::string>& keys)> choose_directory_peers;
  };

  virtual ~HubMeshOwner() = default;

  /** Builds and starts the mesh; it is the hub's to use until Stop. The error carries the mesh's reason. */
  virtual Roe<mesh::MeshHost*> Start(StartRequest request) = 0;
  /** The DHT and directory for the role, against these peers: after Start, and when nodes or the role change. */
  virtual void ConfigureQueries(const QueryInputs& inputs) = 0;
  /** The role's inbound serving (circuit, media relay) after a capability change. */
  virtual void ApplyServing(const MeshConfig& config) = 0;
  /** Stops the mesh (its workers joined); the object stays until Release. */
  virtual void Stop() = 0;
  /** Frees the stopped mesh. */
  virtual void Release() = 0;
  /**
   * Watches the device's network (the OS monitor is the app's): each change, the baseline first, reaches
   * `on_change` on a monitor thread until StopWatchingNetwork. The hub watches while its mesh services run.
   */
  virtual void WatchNetwork(std::function<void(const NetworkChange&)> on_change) = 0;
  /** No `on_change` call is running or will run once this returns. Idempotent. */
  virtual void StopWatchingNetwork() = 0;
  /** Listens for peers on the LAN (mDNS); each reaches `on_peer` on the discovery thread until StopLanDiscovery. */
  virtual void StartLanDiscovery(std::function<void(const mesh::LanMdnsDiscoveredPeer&)> on_peer) = 0;
  /** What this device announces on the LAN: its PeerId and Amp UDP port; empty / 0 announces nothing. */
  virtual void AdvertiseOnLan(const std::string& peer_id, int amp_udp_port) = 0;
  /** Stops listening and announcing. Idempotent. */
  virtual void StopLanDiscovery() = 0;

  /**
   * Keeps the directory's mesh nodes fresh and on disk (in `app_data_dir`): the mesh's directory twin first,
   * then `directory`. `on_updated` runs on a worker after a refresh lands. Once per profile; `directory` must
   * outlive StopDirectoryCache.
   */
  virtual void StartDirectoryCache(mesh::IDirectoryClient& directory, const std::string& app_data_dir,
                                   std::function<void()> on_updated) = 0;
  /** Whether the directory cache runs. */
  virtual bool HasDirectoryCache() const = 0;
  /** The cached nodes (every row; empty when not running). Any thread. */
  virtual std::vector<mesh::MeshDirectoryNode> DirectoryNodesSnapshot() const = 0;
  /** Refresh now. */
  virtual void RefreshDirectory() = 0;
  /** Refresh when it is due (the hub's 1 s tick). */
  virtual void MaybeRefreshDirectory() = 0;
  /** Drops the cache (the profile closes). Idempotent. */
  virtual void StopDirectoryCache() = 0;
};

} // namespace pbr
