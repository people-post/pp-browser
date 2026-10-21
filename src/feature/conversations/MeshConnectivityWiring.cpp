#include "feature/conversations/MeshConnectivityWiring.h"

#include "domain/people/MeshHopPolicy.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <memory>
#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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

std::vector<std::string> PeerIds(const std::vector<mesh::MeshHopCandidate>& hops) {
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

mesh::MeshConnectivityDeps MakeMeshConnectivityDeps(const MeshConnectivityWiringInputs& in) {
  mesh::MeshConnectivityDeps deps;
  deps.mesh = in.mesh;
  deps.note_lan_peer_id = in.note_lan_peer_id;
  deps.register_direct_endpoint = in.register_direct_endpoint;
  return deps;
}

mesh::MeshHopPolicy ComputeMeshHopPolicy(const MeshConnectivityWiringInputs& in) {
  MeshConfig mesh_cfg = MeshConfigOf(in);
  NormalizeMeshConfig(mesh_cfg);
  const std::vector<Contact> contacts = ListContacts(in.contacts);
  const auto directory_nodes = in.list_directory_nodes ? in.list_directory_nodes() : std::vector<mesh::MeshDirectoryNode>{};
  const auto dht_nodes = in.list_dht_nodes ? in.list_dht_nodes() : std::vector<mesh::MeshDirectoryNode>{};
  const bool include_seeds = !in.seed_dial_ok || in.seed_dial_ok();

  mesh::MeshHopPolicy policy;
  policy.rendezvous_candidates =
      BuildCircuitHopList(contacts, directory_nodes, dht_nodes, ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes),
                          mesh_cfg.prefer_contacts_for_routing, include_seeds);
  // Trusted relays only: the configured org seeds, not the directory's volunteers merged into them.
  // Seeds known unreachable are no park target either: Accept would wait out the park for nothing.
  policy.bootstrap_seeds = CollectSeedHopCandidates(
      mesh_cfg.trusted_relays_only ? mesh_cfg.bootstrap_peers : ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes));
  if (!include_seeds) {
    const std::vector<std::string> org = PeerIds(CollectSeedHopCandidates(mesh_cfg.bootstrap_peers));
    std::erase_if(policy.bootstrap_seeds, [&org](const mesh::MeshHopCandidate& hop) {
      return std::find(org.begin(), org.end(), hop.peer_id) != org.end();
    });
  }
  policy.punch_introducers.contact_peer_ids = PeerIds(CollectContactHopCandidates(contacts));
  policy.punch_introducers.seed_peer_ids = PeerIds(CollectSeedHopCandidates(mesh_cfg.bootstrap_peers));
  if (mesh_cfg.trusted_relays_only) {  // privacy T4; an introducer sees both ends' addresses
    const auto trusted = TrustedRelayPeerIds(contacts, mesh_cfg.bootstrap_peers);
    policy.rendezvous_candidates = KeepTrustedRelays(std::move(policy.rendezvous_candidates), trusted);
    std::erase_if(policy.punch_introducers.contact_peer_ids,
                  [&trusted](const std::string& id) { return !trusted.contains(id); });
  }
  return policy;
}

MeshHopPolicyFeed::MeshHopPolicyFeed(mesh::MeshConnectivity& connectivity) : connectivity_(connectivity) {}

MeshHopPolicyFeed::~MeshHopPolicyFeed() {
  Stop();
}

void MeshHopPolicyFeed::Start() {
  PushNow();
  if (!started_.exchange(true)) {
    ArmRefresh();
  }
}

void MeshHopPolicyFeed::Stop() {
  started_.store(false);
  if (refresh_timer_ != 0) {
    AppRuntime::CancelCoordinatorTimer(std::exchange(refresh_timer_, 0));
  }
  lifetime_.InvalidateAndWait();
}

void MeshHopPolicyFeed::PushSoon() {
  // Bound: a push still queued when this goes never touches it. The started check is inside the
  // bound call, so Stop either waits for a push or the push sees it stopped.
  lifetime_.Post([](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); },
                 [this]() {
                   if (started_.load()) {
                     PushNow();
                   }
                 });
}

void MeshHopPolicyFeed::SetInputs(MeshConnectivityWiringInputs inputs) {
  std::lock_guard lock(push_mu_);
  inputs_ = std::move(inputs);
}

void MeshHopPolicyFeed::PushNow() {
  std::lock_guard lock(push_mu_);
  connectivity_.SetHopPolicy(ComputeMeshHopPolicy(inputs_));
}

void MeshHopPolicyFeed::ArmRefresh() {
  // The coordinator only dispatches: the push itself runs on a worker.
  refresh_timer_ = AppRuntime::ScheduleCoordinatorRepeating(kRefresh, lifetime_.Bind([this]() { PushSoon(); }));
}

} // namespace pbr
