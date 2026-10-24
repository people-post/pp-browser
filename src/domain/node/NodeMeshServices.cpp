#include "domain/node/NodeMeshServices.h"

#include "mesh/types/RelayScope.h"
#include "mesh/types/MediaChannel.h"
#include "mesh/dht/DhtTypes.h"
#include "mesh/discovery/AmpDirectoryProtocol.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "mesh/reachability/AmpObservedAddrs.h"
#include "foundation/data/MeshRole.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

void RegisterSeedEndpoints(mesh::MeshHost& mesh, const std::vector<std::string>& bootstrap_peers) {
  for (const std::string& ma : bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || ma.empty()) {
      continue;
    }
    (void)mesh.Amp()->Links().RegisterEndpoint(peer_id, ma);
  }
}

} // namespace

void ApplyNodeHosting(mesh::MeshHostConfig& cfg, const MeshConfig& mesh, bool node_role) {
  cfg.host_circuit_relay = node_role && mesh.capabilities.circuit_relay;
  cfg.host_media_relay = node_role && mesh.capabilities.media_relay;
  cfg.host_dht = node_role && mesh.capabilities.dht;
  cfg.host_directory = node_role;
  cfg.media_relay_budget = mesh.media_relay_budget;
  cfg.media_relay_pricing = mesh.pricing.media_relay;
  cfg.media_relay_video.serve_levels.clear();
  for (const int level : mesh.media_relay_video.serve_levels) {
    if (mesh::IsVideoLevel(level)) {
      cfg.media_relay_video.serve_levels.push_back(static_cast<uint8_t>(level));
    }
  }
  cfg.media_relay_video.carry_levels = mesh.media_relay_video.carry_levels;
  cfg.media_relay_video.strict = mesh.media_relay_video.strict;
}

std::vector<std::string> CollectNodeQueryPeerKeys(const std::vector<std::string>& bootstrap_peers,
                                                  const std::vector<mesh::MeshDirectoryNode>& directory_nodes) {
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  for (const mesh::MeshDirectoryNode& node : directory_nodes) {
    if (node.peer_id.empty() || !seen.insert(node.peer_id).second) {
      continue;
    }
    keys.push_back(node.peer_id);
  }
  for (const std::string& ma : bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    keys.push_back(peer_id);
  }
  return keys;
}

void ConfigureNodeAmpDht(mesh::MeshHost& mesh, IdentityStore& identity, const MeshConfig& config, bool node_role,
                         std::vector<std::string> query_peer_keys,
                         std::function<bool(const std::string& peer_id)> reach_held_down) {
  if (!mesh.Amp() || !mesh.AmpDht()) {
    return;
  }
  const bool participate = node_role && config.capabilities.dht;

  mesh::AmpDhtProtocolConfig cfg;
  cfg.local_peer_id = mesh.Amp()->LocalPeerId();
  cfg.listen_multiaddrs = DialWorthyMultiaddrs(mesh.AdvertisedListenMultiaddrs());
  if (cfg.listen_multiaddrs.empty() && mesh::IsUsableAdpListen(mesh.AmpListenMultiaddr())) {
    cfg.listen_multiaddrs = {mesh.AmpListenMultiaddr()};
  }
  if (auto priv = identity.GetDeviceMlDsaPrivateKey()) {
    cfg.device_signing_secret = *priv;
  }
  if (auto pub = identity.GetDeviceMlDsaPublicKey()) {
    cfg.device_signing_public = *pub;
  }
  cfg.tunables = config.dht;
  cfg.query_peer_keys = std::move(query_peer_keys);
  cfg.participate = participate;
  cfg.publish_circuit_relay = participate && config.capabilities.circuit_relay;
  cfg.publish_media_relay = participate && config.capabilities.media_relay;
  cfg.reach_held_down = std::move(reach_held_down);
  mesh.ConfigureAmpDht(std::move(cfg));
  mesh.RefreshAmpDhtHosting(participate);
}

void ConfigureNodeAmpDirectory(
    mesh::MeshHost& mesh, bool node_role, std::vector<std::string> query_peer_keys,
    std::function<std::vector<std::string>(const std::vector<std::string>&)> choose_query_peers) {
  if (!mesh.Amp() || !mesh.AmpDirectory()) {
    return;
  }
  mesh::AmpDirectoryProtocolConfig cfg;
  cfg.local_peer_id = mesh.Amp()->LocalPeerId();
  cfg.query_peer_keys = std::move(query_peer_keys);
  cfg.choose_query_peers = std::move(choose_query_peers);
  mesh.ConfigureAmpDirectory(std::move(cfg));
  mesh.RefreshAmpDirectoryHosting(node_role);
}

mesh::MeshNodeHit BuildLocalMeshNodeHit(IdentityStore& identity, mesh::MeshHost& mesh, const MeshConfig& config) {
  mesh::MeshNodeHit hit;
  hit.entity_kind = "mesh_node";
  if (auto loaded = identity.Get()) {
    hit.relay_user_id = loaded->relay_user_id.empty() ? loaded->peer_id : loaded->relay_user_id;
    if (!loaded->account_id.empty()) {
      hit.account_id = loaded->account_id;
    }
    if (!loaded->nickname.empty()) {
      hit.nickname = loaded->nickname;
    }
    if (!loaded->public_key_b64.empty()) {
      hit.signing_public_key_b64 = loaded->public_key_b64;
    }
    if (!loaded->kem_public_key_b64.empty()) {
      hit.kem_public_key_b64 = loaded->kem_public_key_b64;
    }
  }
  if (hit.relay_user_id.empty() && mesh.Amp()) {
    hit.relay_user_id = mesh.Amp()->LocalPeerId();
  }
  hit.capabilities.circuit_relay = config.capabilities.circuit_relay;
  hit.capabilities.media_relay = config.capabilities.media_relay;
  hit.capabilities.dht = config.capabilities.dht;
  hit.capabilities.ledger_gateway = config.capabilities.ledger_gateway;
  mesh::DirectoryEndpoint ep;
  if (mesh.Amp()) {
    ep.peer_id = mesh.Amp()->LocalPeerId();
  }
  for (const std::string& ma : mesh.AdvertisedListenMultiaddrs()) {
    if (!ma.empty()) {
      ep.multiaddrs.push_back(ma);
    }
  }
  if (ep.multiaddrs.empty() && mesh::IsUsableAdpListen(mesh.AmpListenMultiaddr())) {
    ep.multiaddrs.push_back(mesh.AmpListenMultiaddr());
  }
  for (const std::string& ma : config.advertise_multiaddrs) {
    if (!ma.empty() && std::find(ep.multiaddrs.begin(), ep.multiaddrs.end(), ma) == ep.multiaddrs.end()) {
      ep.multiaddrs.push_back(ma);
    }
  }
  // Published for readers anywhere: not the container's or the LAN's address beside a public one (#379).
  ep.multiaddrs = DialWorthyMultiaddrs(std::move(ep.multiaddrs));
  if (!ep.peer_id.empty()) {
    hit.endpoints.push_back(std::move(ep));
  }
  return hit;
}

void ApplyOrgSeedAdmission(mesh::MeshHost& mesh) {
  // Do not rely on an empty contact set to mean "serve everyone".
  const mesh::RelayScopeMask org_serve = mesh::kRelayScopeShortTerm | static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Public);
  if (auto* circuit = mesh.AmpCircuitServer()) {
    mesh::CircuitRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = false;
    policy.serve_scope_mask = org_serve;
    circuit->SetAdmissionPolicy(std::move(policy));
  }
  if (auto* media = mesh.AmpMediaRelayServer()) {
    mesh::MediaRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = false;
    policy.serve_scope_mask = org_serve;
    media->SetAdmissionPolicy(std::move(policy));
  }
}

void StartOrgSeedServices(mesh::MeshHost& mesh, IdentityStore& identity, const MeshConfig& config) {
  if (!mesh.Amp()) {
    return;
  }
  const bool node_role = ResolveMeshRole(config) == MeshRole::Node;
  RegisterSeedEndpoints(mesh, config.bootstrap_peers);
  ConfigureNodeAmpDht(mesh, identity, config, node_role, CollectNodeQueryPeerKeys(config.bootstrap_peers));
  ConfigureNodeAmpDirectory(mesh, node_role, CollectNodeQueryPeerKeys(config.bootstrap_peers));
  if (node_role && mesh.AmpDirectory()) {
    mesh.AmpDirectory()->SetNodesSnapshot({BuildLocalMeshNodeHit(identity, mesh, config)});
  }
  ApplyOrgSeedAdmission(mesh);
}

} // namespace pbr
