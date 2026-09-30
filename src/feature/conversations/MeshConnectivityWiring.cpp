#include "feature/conversations/MeshConnectivityWiring.h"

#include "domain/people/MeshHopPolicy.h"
#include "foundation/data/MeshRole.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::vector<Contact> ListContacts(ContactsStore* contacts) {
  if (contacts) {
    if (auto listed = contacts->List()) {
      return std::move(*listed);
    }
  }
  return {};
}

std::vector<std::string> PeerIds(const std::vector<MeshHopCandidate>& hops) {
  std::vector<std::string> out;
  for (const auto& hop : hops) {
    if (!hop.peer_id.empty()) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

} // namespace

namespace {

MeshConfig MeshConfigOf(const MeshConnectivityWiringInputs& in) {
  auto cfg = in.mesh_config ? in.mesh_config() : nullptr;
  return cfg ? *cfg : MeshConfig{};
}

} // namespace

MeshConnectivityDeps MakeMeshConnectivityDeps(MeshConnectivityWiringInputs in) {
  auto inputs = std::make_shared<MeshConnectivityWiringInputs>(std::move(in));
  const auto directory = [inputs]() {
    return inputs->list_directory_nodes ? inputs->list_directory_nodes() : std::vector<MeshDirectoryNode>{};
  };
  MeshConnectivityDeps deps;
  deps.mesh = inputs->mesh;
  deps.note_lan_peer_id = inputs->note_lan_peer_id;
  deps.register_direct_endpoint = inputs->register_direct_endpoint;
  deps.rendezvous_candidates = [inputs, directory]() {
    MeshConfig mesh_cfg = MeshConfigOf(*inputs);
    NormalizeMeshConfig(mesh_cfg);
    const auto directory_nodes = directory();
    const auto dht_nodes = inputs->list_dht_nodes ? inputs->list_dht_nodes() : std::vector<MeshDirectoryNode>{};
    const bool include_seeds = !inputs->seed_dial_ok || inputs->seed_dial_ok();
    return BuildCircuitHopList(ListContacts(inputs->contacts), directory_nodes, dht_nodes,
                               ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes),
                               mesh_cfg.prefer_contacts_for_routing, include_seeds);
  };
  deps.bootstrap_seeds = [inputs, directory]() {
    return CollectSeedHopCandidates(ResolveEffectiveBootstrapPeers(MeshConfigOf(*inputs), directory()));
  };
  deps.punch_introducers = [inputs]() {
    MeshConfig mesh_cfg = MeshConfigOf(*inputs);
    NormalizeMeshConfig(mesh_cfg);
    MeshPunchIntroducers out;
    out.contact_peer_ids = PeerIds(CollectContactHopCandidates(ListContacts(inputs->contacts)));
    out.seed_peer_ids = PeerIds(CollectSeedHopCandidates(mesh_cfg.bootstrap_peers));
    return out;
  };
  return deps;
}

} // namespace pbr
