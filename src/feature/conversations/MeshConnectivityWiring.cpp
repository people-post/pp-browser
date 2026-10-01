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
    const std::vector<Contact> contacts = ListContacts(inputs->contacts);
    auto hops = BuildCircuitHopList(contacts, directory_nodes, dht_nodes,
                                    ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes),
                                    mesh_cfg.prefer_contacts_for_routing, include_seeds);
    if (mesh_cfg.trusted_relays_only) {  // privacy T4
      hops = KeepTrustedRelays(std::move(hops), TrustedRelayPeerIds(contacts, mesh_cfg.bootstrap_peers));
    }
    return hops;
  };
  deps.bootstrap_seeds = [inputs, directory]() {
    MeshConfig mesh_cfg = MeshConfigOf(*inputs);
    NormalizeMeshConfig(mesh_cfg);
    // Trusted relays only: the configured org seeds, not the directory's volunteers merged into them.
    return CollectSeedHopCandidates(mesh_cfg.trusted_relays_only ? mesh_cfg.bootstrap_peers
                                                                 : ResolveEffectiveBootstrapPeers(mesh_cfg, directory()));
  };
  deps.punch_introducers = [inputs]() {
    MeshConfig mesh_cfg = MeshConfigOf(*inputs);
    NormalizeMeshConfig(mesh_cfg);
    MeshPunchIntroducers out;
    const std::vector<Contact> contacts = ListContacts(inputs->contacts);
    out.contact_peer_ids = PeerIds(CollectContactHopCandidates(contacts));
    out.seed_peer_ids = PeerIds(CollectSeedHopCandidates(mesh_cfg.bootstrap_peers));
    if (mesh_cfg.trusted_relays_only) {  // an introducer sees both ends' addresses (privacy T4)
      const auto trusted = TrustedRelayPeerIds(contacts, mesh_cfg.bootstrap_peers);
      std::erase_if(out.contact_peer_ids, [&trusted](const std::string& id) { return !trusted.contains(id); });
    }
    return out;
  };
  return deps;
}

} // namespace pbr
