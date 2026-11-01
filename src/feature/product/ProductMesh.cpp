#include "feature/product/ProductMesh.h"

#include "domain/node/NodeMeshServices.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntimeMeshExecutors.h"
#include "common/ProtocolIds.h"
#include "mesh/discovery/AmpDirectoryProtocol.h"
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
std::vector<std::string> DirectoryQueryKeys(const AppConfig& config, const MeshConfig& mesh_cfg) {
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  DirectoryConfig dir_cfg = config.directory;
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
  mesh::DirectoryEndpoint ep;
  ep.peer_id = row.peer_id;
  ep.multiaddrs = row.multiaddrs;
  hit.endpoints.push_back(std::move(ep));
  return hit;
}

} // namespace

ProductMesh::ProductMesh() {
  redirectLogger("ProductMesh");
}

ProductMesh::~ProductMesh() {
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
  MeshConfig product_mesh_cfg = request.config.mesh;
  NormalizeMeshConfig(product_mesh_cfg);
  const bool node = ResolveMeshRole(product_mesh_cfg) == MeshRole::Node;

  mesh::MeshHostConfig mesh_cfg;
  if (auto priv = request.identity->GetDeviceMlDsaPrivateKey()) {
    mesh_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = request.identity->GetDeviceMlDsaPublicKey()) {
    mesh_cfg.host.device_ml_dsa_public_key = *pub;
  }
  ApplyNodeHosting(mesh_cfg, request.config.mesh, node);
  // Skip the blocking UPnP / seed probe when the app is already quitting.
  mesh_cfg.start_reachability_probe = node && !request.stopping;
  if (node) {
    mesh_cfg.try_upnp_first = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
  }
  mesh_cfg.on_reachability_updated = std::move(request.on_reachability_updated);
  mesh_cfg.mesh_enabled = product_mesh_cfg.mesh_enabled && mesh_cfg.host.device_ml_dsa_private_key &&
                          mesh_cfg.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port =
      product_mesh_cfg.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(product_mesh_cfg.amp_udp_port);
  mesh_cfg.bootstrap_peers = product_mesh_cfg.bootstrap_peers;

  mesh_ = std::make_unique<mesh::MeshHost>(AppRuntimeMeshExecutors(), MetricsRegistry::Global());
  mesh_->SetStopping(request.stopping);
  mesh_->SetAppProtocols(ProductMeshProtocols());  // advertised from the first advertisement on
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
  const bool node = ResolveMeshRole(inputs.config.mesh) == MeshRole::Node;
  ConfigureDht(inputs, node);
  ConfigureDirectory(inputs, node);
}

void ProductMesh::ConfigureDht(const QueryInputs& inputs, const bool node_role) {
  if (!mesh_->AmpDht()) {
    return;
  }
  MeshConfig mesh_cfg = inputs.config.mesh;
  NormalizeMeshConfig(mesh_cfg);
  const std::vector<mesh::MeshDirectoryNode> directory_nodes =
      inputs.directory_nodes ? inputs.directory_nodes() : std::vector<mesh::MeshDirectoryNode>{};
  // The DHT's liveness is the reach record's (pp-cpp-mesh docs/DHT.md D5).
  ConfigureNodeAmpDht(*mesh_, *inputs.identity, inputs.config.mesh, node_role,
                      CollectNodeQueryPeerKeys(mesh_cfg.bootstrap_peers, directory_nodes), inputs.reach_held_down);
}

void ProductMesh::ConfigureDirectory(const QueryInputs& inputs, const bool node_role) {
  if (!mesh_->AmpDirectory()) {
    return;
  }
  MeshConfig mesh_cfg = inputs.config.mesh;
  NormalizeMeshConfig(mesh_cfg);
  // A refresh asks a couple of providers that are up or answered lately — not every one, and not
  // one the mesh found gone (pp-cpp-mesh docs/PEER_REACH.md, § 5).
  ConfigureNodeAmpDirectory(*mesh_, node_role, DirectoryQueryKeys(inputs.config, mesh_cfg),
                            inputs.choose_directory_peers);
  if (!node_role) {
    return;
  }
  // The twin lists this node, then the nodes the directory knows.
  mesh_->AmpDirectory()->SetNodesProvider(
      [mesh = mesh_.get(), identity = inputs.identity, directory_nodes = inputs.directory_nodes, mesh_cfg]() {
        std::vector<mesh::MeshNodeHit> nodes;
        nodes.push_back(BuildLocalMeshNodeHit(*identity, *mesh, mesh_cfg));
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

} // namespace pbr
