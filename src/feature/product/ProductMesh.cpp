#include "feature/product/ProductMesh.h"

#include "foundation/product/AppVersion.h"

#include "domain/node/NodeMeshServices.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntimeMeshExecutors.h"
#include "common/ProtocolIds.h"
#include "mesh/discovery/AmpDirectoryProtocol.h"
#include "mesh/discovery/NameDirectory.h"
#include "common/runtime/AppRuntime.h"

#include <filesystem>
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/l4/media_relay/serve/MediaRelayServer.h"

#include <unordered_set>
#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

/** A directory provider's Amp key: its PeerId, from a multiaddr or as given. */
std::string AmpDirectoryPeerKeyFromBaseUrl(const std::string& base_url) {
  if (base_url.empty()) {
    return {};
  }
  if (base_url.find('/') != std::string::npos) {
    return PeerIdFromMultiaddr(base_url);
  }
  return base_url;
}

/** The directory's query peers: the Amp providers the config names, then the seeds; deduped. */
std::vector<std::string> DirectoryQueryKeys(const DirectoryConfig& directory, const MeshConfig& mesh_cfg) {
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  DirectoryConfig dir_cfg = directory;
  NormalizeDirectoryConfig(dir_cfg);
  for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(dir_cfg)) {
    const std::string transport = provider.transport.empty() ? "http" : provider.transport;
    if (transport != "amp") {
      continue;
    }
    const std::string key = AmpDirectoryPeerKeyFromBaseUrl(provider.base_url);
    if (!key.empty() && seen.insert(key).second) {
      keys.push_back(key);
    }
  }
  for (const std::string& key : CollectNodeQueryPeerKeys(mesh_cfg.bootstrap_peers, {})) {
    if (!key.empty() && seen.insert(key).second) {
      keys.push_back(key);
    }
  }
  return keys;
}

/** A directory row as the twin lists it (a node it knows of). */
mesh::MeshNodeHit NodeHitFromDirectoryRow(const mesh::MeshDirectoryNode& row) {
  mesh::MeshNodeHit hit;
  hit.relay_user_id = row.account_id.empty() ? row.peer_id : row.account_id;
  if (!row.account_id.empty()) {
    hit.account_id = row.account_id;
  }
  if (!row.nickname.empty()) {
    hit.nickname = row.nickname;
  }
  hit.entity_kind = row.entity_kind.empty() ? "mesh_node" : row.entity_kind;
  hit.seq = row.seq;
  hit.expires_at = row.expires_at;
  hit.capabilities.circuit_relay = row.circuit_relay;
  hit.capabilities.media_relay = row.media_relay;
  hit.capabilities.dht = row.dht;
  hit.capabilities.ledger_gateway = row.ledger_gateway;
  hit.amp_wire = row.amp_wire;
  hit.node_version = row.node_version;
  hit.features = row.features;
  mesh::DirectoryEndpoint ep;
  ep.peer_id = row.peer_id;
  ep.multiaddrs = row.multiaddrs;
  hit.endpoints.push_back(std::move(ep));
  return hit;
}

} // namespace

ProductMesh::ProductMesh()
    : connectivity_(std::make_unique<mesh::MeshConnectivity>()),
      media_relay_(std::make_unique<mesh::MeshMediaRelay>(*connectivity_)),
      hop_feed_(std::make_unique<MeshHopPolicyFeed>(*connectivity_)) {
  redirectLogger("ProductMesh");
  // The one slot, for the owner's lifetime (connectivity keeps it across mesh restarts).
  connectivity_->SetOnRelayChosen([this](const std::string& relay_peer_id) { OnRelayChosen(relay_peer_id); });
}

ProductMesh::~ProductMesh() {
  connectivity_->SetOnRelayChosen({});
  StayReachable(false);
  StopDirectoryCache();
  StopLanDiscovery();
  StopWatchingNetwork();
  Stop();
  Release();
}

Roe<mesh::MeshHost*> ProductMesh::Start(StartRequest request) {
  if (mesh_) {
    return Error("mesh already started");
  }
  if (request.identity == nullptr) {
    return Error("no identity for the mesh");
  }
  MeshConfig product_mesh_cfg = request.mesh;
  NormalizeMeshConfig(product_mesh_cfg);
  const bool node = ResolveMeshRole(product_mesh_cfg) == MeshRole::Node;
  LoadLastGoodRelays(request.profile_data_dir); // the hop policy hands them to the park from its first push

  mesh::MeshHostConfig mesh_cfg;
  if (auto priv = request.identity->GetDeviceMlDsaPrivateKey()) {
    mesh_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = request.identity->GetDeviceMlDsaPublicKey()) {
    mesh_cfg.host.device_ml_dsa_public_key = *pub;
  }
  ApplyNodeHosting(mesh_cfg, request.mesh, node);
  // Skip the blocking UPnP / seed probe when the app is already quitting.
  mesh_cfg.start_reachability_probe = node && !request.stopping;
  if (node) {
    mesh_cfg.try_upnp_first = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
  }
  mesh_cfg.on_reachability_updated = std::move(request.on_reachability_updated);
  mesh_cfg.mesh_enabled = product_mesh_cfg.mesh_enabled && mesh_cfg.host.device_ml_dsa_private_key &&
                          mesh_cfg.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port = product_mesh_cfg.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(product_mesh_cfg.amp_udp_port);
  mesh_cfg.bootstrap_peers = product_mesh_cfg.bootstrap_peers;

  mesh_ = std::make_unique<mesh::MeshHost>(AppRuntimeMeshExecutors(), MetricsRegistry::Global());
  mesh_->SetStopping(request.stopping);
  mesh_->SetAppProtocols(ProductMeshProtocols()); // advertised from the first advertisement on
  mesh_->SetAddressDisclosure(request.address_disclosure);
  mesh_->SetOnUdpSocketCheck(std::move(request.on_udp_socket_check));
  if (auto started = mesh_->Start(mesh_cfg); !started) {
    const std::string reason = mesh_->LastError().empty() ? started.error().message : mesh_->LastError();
    mesh_.reset();
    return Error(reason);
  }
  return mesh_.get();
}

void ProductMesh::ConfigureQueries(const QueryInputs& inputs) {
  if (!mesh_ || !mesh_->Amp() || inputs.identity == nullptr) {
    return;
  }
  const bool node = ResolveMeshRole(inputs.mesh) == MeshRole::Node;
  ConfigureDht(inputs, node);
  ConfigureDirectory(inputs, node);
}

void ProductMesh::ConfigureDht(const QueryInputs& inputs, const bool node_role) {
  if (!mesh_->AmpDht()) {
    return;
  }
  MeshConfig mesh_cfg = inputs.mesh;
  NormalizeMeshConfig(mesh_cfg);
  const std::vector<mesh::MeshDirectoryNode> directory_nodes =
      inputs.directory_nodes ? inputs.directory_nodes() : std::vector<mesh::MeshDirectoryNode>{};
  // The DHT's liveness is the reach record's (pp-cpp-mesh docs/DHT.md D5).
  ConfigureNodeAmpDht(*mesh_, *inputs.identity, inputs.mesh, node_role,
                      CollectNodeQueryPeerKeys(mesh_cfg.bootstrap_peers, directory_nodes), inputs.reach_held_down);
}

void ProductMesh::ConfigureDirectory(const QueryInputs& inputs, const bool node_role) {
  if (!mesh_->AmpDirectory()) {
    return;
  }
  MeshConfig mesh_cfg = inputs.mesh;
  NormalizeMeshConfig(mesh_cfg);
  // A refresh asks a couple of providers that are up or answered lately — not every one, and not
  // one the mesh found gone (pp-cpp-mesh docs/PEER_REACH.md, § 5).
  ConfigureNodeAmpDirectory(*mesh_, node_role, DirectoryQueryKeys(inputs.directory, mesh_cfg),
                            inputs.choose_directory_peers);
  if (!node_role) {
    return;
  }
  // A node tells its peers what it runs (N032); a client keeps its version to itself.
  if (mesh_->Amp()) {
    mesh_->Amp()->Links().SetAdvertisedNodeInfo(AppVersionString(), {});
  }
  // The twin lists this node, then the nodes the directory knows.
  mesh_->AmpDirectory()->SetNodesProvider(
      [mesh = mesh_.get(), identity = inputs.identity, directory_nodes = inputs.directory_nodes, mesh_cfg]() {
        std::vector<mesh::MeshNodeHit> nodes;
        nodes.push_back(BuildLocalMeshNodeHit(*identity, *mesh, mesh_cfg, AppVersionString()));
        if (directory_nodes) {
          for (const mesh::MeshDirectoryNode& row : directory_nodes()) {
            if (!row.peer_id.empty()) {
              nodes.push_back(NodeHitFromDirectoryRow(row));
            }
          }
        }
        return nodes;
      });
}

void ProductMesh::ApplyServing(const MeshConfig& config) {
  if (!mesh_) {
    return;
  }
  const bool node = ResolveMeshRole(config) == MeshRole::Node;
  // Amp L4 inbound hosting is gated per server (no TCP circuit / media relay).
  if (mesh_->AmpCircuitServer()) {
    mesh_->AmpCircuitServer()->SetServeInbound(node && config.capabilities.circuit_relay);
  }
  if (mesh_->AmpMediaRelayServer()) {
    mesh_->AmpMediaRelayServer()->SetServeInbound(node && config.capabilities.media_relay);
  }
}

void ProductMesh::Stop() {
  if (mesh_) {
    mesh_->Stop();
  }
}

void ProductMesh::Release() {
  mesh_.reset();
}

void ProductMesh::WatchNetwork(std::function<void(const NetworkChange&)> on_change) {
  if (network_monitor_) {
    return;
  }
  network_monitor_ = std::make_unique<NetworkMonitor>();
  if (!network_monitor_->Start(std::move(on_change))) {
    log().info << "network monitor unavailable on this platform";
  }
}

void ProductMesh::StopWatchingNetwork() {
  if (network_monitor_) {
    network_monitor_->Stop();
    network_monitor_.reset();
  }
}

void ProductMesh::StartLanDiscovery(std::function<void(const mesh::LanMdnsDiscoveredPeer&)> on_peer) {
  StopLanDiscovery();
  lan_mdns_ = std::make_unique<mesh::LanMdnsDiscovery>();
  lan_mdns_->SetOnDiscovered(std::move(on_peer));
  if (auto started = lan_mdns_->Start(); !started) {
    log().warning << "LAN mDNS discovery unavailable: " << started.error().message;
  }
}

void ProductMesh::AdvertiseOnLan(const std::string& peer_id, const int amp_udp_port) {
  if (!lan_mdns_ || !lan_mdns_->IsRunning()) {
    return;
  }
  // Empty host_ips → LanMdnsDiscovery enumerates LAN IFs (Amp typically binds 0.0.0.0).
  lan_mdns_->SetAdvertisement(peer_id, amp_udp_port, {});
}

std::string MeshDirectoryNodesPath(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "mesh_directory_nodes.json").string();
}

std::string MeshNodeReputationPath(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "mesh_node_reputation.json").string();
}

std::string MeshLastGoodRelaysPath(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "mesh_last_good_relays.json").string();
}

void ProductMesh::LoadLastGoodRelays(const std::string& profile_data_dir) {
  const std::string path = profile_data_dir.empty() ? std::string{} : MeshLastGoodRelaysPath(profile_data_dir);
  {
    std::lock_guard lock(relay_memory_mu_);
    if (last_good_relays_->Path() == path) {
      return; // a mesh restart in one profile: what this run noted (a save may still be queued) stands
    }
  }
  auto store = std::make_shared<LastGoodRelayStore>(path);
  store->Load();
  log().info << "last good relays loaded count=" << store->Relays().size();
  std::lock_guard lock(relay_memory_mu_);
  last_good_relays_ = std::move(store);
}

std::vector<std::string> ProductMesh::LastGoodRelays() const {
  std::lock_guard lock(relay_memory_mu_);
  return last_good_relays_->Relays();
}

void ProductMesh::SetOnRelayChosen(std::function<void(const std::string& relay_peer_id)> hook) {
  std::lock_guard lock(relay_memory_mu_);
  on_relay_chosen_ = std::move(hook);
}

void ProductMesh::OnRelayChosen(const std::string& relay_peer_id) {
  std::lock_guard lock(relay_memory_mu_);
  if (last_good_relays_->Note(relay_peer_id)) {
    // A file write: never on connectivity's lane.
    AppRuntime::PostWorkerNormal([store = last_good_relays_]() { store->Save(); });
  }
  if (on_relay_chosen_) {
    on_relay_chosen_(relay_peer_id); // under the lock: a hook replaced or cleared never runs after
  }
}

std::shared_ptr<MeshNodeReputation> ProductMesh::NodeReputation() const {
  std::lock_guard lock(reputation_mu_);
  return node_reputation_;
}

void ProductMesh::StartDirectoryCache(mesh::IDirectoryClient& directory, const std::string& profile_data_dir,
                                      std::function<void()> on_updated) {
  StopDirectoryCache();
  {
    auto reputation = std::make_shared<MeshNodeReputation>();
    reputation->SetPersistPath(MeshNodeReputationPath(profile_data_dir));
    reputation->Load();
    std::lock_guard lock(reputation_mu_);
    node_reputation_ = std::move(reputation);
  }
  mesh::IDirectoryClient* client = &directory;
  directory_cache_ = std::make_unique<mesh::MeshDirectoryCache>(
      [client]() -> Roe<std::vector<mesh::MeshDirectoryNode>> {
        // Sync fetcher: HTTP INameDirectory (tests / async-unset path).
        mesh::DirectoryClientNameDirectory names(*client);
        auto records = names.ListService("mesh_node");
        if (!records) {
          return records.error();
        }
        return mesh::MeshDirectoryNodesFromNameRecords(*records);
      },
      AppRuntimeMeshExecutors().blocking);
  directory_cache_->SetPersistPath(MeshDirectoryNodesPath(profile_data_dir));
  directory_cache_->LoadPersisted();
  // N029 nd4: the Amp directory twin first (no worker park), then HTTP on Normal.
  directory_cache_->SetAsyncFetcher(
      [this, client](std::function<void(Roe<std::vector<mesh::MeshDirectoryNode>>)> done) {
        auto fetch_http = [client, done]() {
          AppRuntime::PostWorkerNormal([client, done]() {
            mesh::DirectoryClientNameDirectory names(*client);
            auto records = names.ListService("mesh_node");
            if (!records) {
              done(records.error());
              return;
            }
            done(mesh::MeshDirectoryNodesFromNameRecords(*records));
          });
        };
        if (mesh_ && mesh_->AmpDirectory() && mesh_->AmpDirectory()->IsStarted()) {
          mesh_->AmpDirectory()->ListMeshNodesAsync([done, fetch_http](mesh::AmpDirectoryProtocol::ListRoe amp_nodes) {
            if (amp_nodes) {
              auto rows = mesh::MeshDirectoryNodesFromHits(*amp_nodes);
              if (!rows.empty()) {
                done(std::move(rows));
                return;
              }
            }
            fetch_http();
          });
          return;
        }
        fetch_http();
      });
  directory_cache_->SetOnUpdated(std::move(on_updated));
  directory_cache_->RequestRefresh();
}

std::vector<mesh::MeshDirectoryNode> ProductMesh::DirectoryNodesSnapshot() const {
  return directory_cache_ ? directory_cache_->Snapshot() : std::vector<mesh::MeshDirectoryNode>{};
}

void ProductMesh::RefreshDirectory() {
  if (directory_cache_) {
    directory_cache_->RequestRefresh();
  }
}

void ProductMesh::MaybeRefreshDirectory() {
  if (directory_cache_) {
    directory_cache_->MaybeRefresh();
  }
}

void ProductMesh::StopDirectoryCache() {
  directory_cache_.reset();
  std::lock_guard lock(reputation_mu_);
  node_reputation_ = std::make_shared<MeshNodeReputation>(); // the closed profile's stays on its disk
}

void ProductMesh::StopLanDiscovery() {
  if (lan_mdns_) {
    lan_mdns_->Stop();
    lan_mdns_.reset();
  }
}

void ProductMesh::SetMeshInputs(const MeshConnectivityWiringInputs& inputs) {
  connectivity_->SetDeps(MakeMeshConnectivityDeps(inputs));
  MeshConnectivityWiringInputs with_memory = inputs;
  with_memory.last_good_relays = [this]() { return LastGoodRelays(); }; // K015: the park's first dials
  hop_feed_->SetInputs(std::move(with_memory));
}

void ProductMesh::WireMeshObjects() {
  hop_feed_->Start(); // the policy is in before connectivity wires
  connectivity_->Wire();
  media_relay_->Wire();
}

void ProductMesh::DetachMeshObjects() {
  media_relay_->ResetRelayClient();
  connectivity_->ResetDialRegistry();
}

void ProductMesh::BeginMeshStop() {
  hop_feed_->Stop(); // its inputs read the mesh
  connectivity_->InvalidateAsyncOps();
}

void ProductMesh::DropMediaRelayClient() {
  media_relay_->ResetRelayClient();
}

void ProductMesh::EndMeshStop() {
  connectivity_->ResetAfterMeshStop();
}

void ProductMesh::RefreshMeshView() {
  connectivity_->RefreshLocalView(); // advertised addrs follow the probe (media consumers' view)
  hop_feed_->PushSoon();             // seed reachability gates the rendezvous surface
}

void ProductMesh::PushHopPolicySoon() {
  hop_feed_->PushSoon();
}

void ProductMesh::StayReachable(const bool want) {
  if (!want) {
    if (stay_reachable_timer_ != 0) {
      AppRuntime::CancelCoordinatorTimer(stay_reachable_timer_);
      stay_reachable_timer_ = 0;
    }
    return;
  }
  if (stay_reachable_timer_ != 0) {
    return;
  }
  // A seed reservation lasts about 30 s; renewing every 20 s keeps contacts able to reach this device by its
  // PeerId through a seed, with no relay (docs/contracts/ACCOUNT_CARD.md).
  const auto renew = [this]() {
    AppRuntime::PostUI([this]() {
      if (stay_reachable_timer_ != 0 && mesh_) {
        connectivity_->Rendezvous().ReserveOnBootstrapSeeds();
      }
    });
  };
  renew();
  stay_reachable_timer_ = AppRuntime::ScheduleCoordinatorRepeating(std::chrono::seconds(20), renew);
}

void ProductMesh::ClearMeshObjects() {
  StayReachable(false);
  media_relay_->Clear();
  hop_feed_->Stop();
  connectivity_->Clear();
}

} // namespace pbr
