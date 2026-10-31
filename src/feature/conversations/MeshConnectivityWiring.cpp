#include "feature/conversations/MeshConnectivityWiring.h"

#include "lib/people/MeshHopPolicy.h"
#include "foundation/data/MeshRole.h"
#include "common/runtime/AppRuntime.h"

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

  // One list (pp-cpp-mesh docs/PEER_REACH.md § 3.3): the rendezvous surface in its order, then
  // anyone allowed another role only; each candidate carries the roles its sources allow.
  mesh::MeshHopPolicy policy;
  auto grant = [&policy](const mesh::MeshHopCandidate& hop, const uint32_t role) {
    if (hop.peer_id.empty()) {
      return;
    }
    auto it = std::find_if(policy.candidates.begin(), policy.candidates.end(),
                           [&hop](const mesh::MeshHopCandidate& c) { return c.peer_id == hop.peer_id; });
    if (it == policy.candidates.end()) {
      policy.candidates.push_back(hop);
      policy.candidates.back().roles = role;
    } else {
      it->roles |= role;
    }
  };

  std::vector<mesh::MeshHopCandidate> rendezvous =
      BuildCircuitHopList(contacts, directory_nodes, dht_nodes, ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes),
                          mesh_cfg.prefer_contacts_for_routing, include_seeds);
  rendezvous = KeepCircuitRelays(std::move(rendezvous), directory_nodes, dht_nodes, mesh_cfg.bootstrap_peers);
  // Trusted relays only: the configured org seeds, not the directory's volunteers merged into them.
  // Seeds known unreachable are no park target either: Accept would wait out the park for nothing.
  std::vector<mesh::MeshHopCandidate> seeds = CollectSeedHopCandidates(
      mesh_cfg.trusted_relays_only ? mesh_cfg.bootstrap_peers : ResolveEffectiveBootstrapPeers(mesh_cfg, directory_nodes));
  if (!include_seeds) {
    const std::vector<std::string> org = PeerIds(CollectSeedHopCandidates(mesh_cfg.bootstrap_peers));
    std::erase_if(seeds, [&org](const mesh::MeshHopCandidate& hop) {
      return std::find(org.begin(), org.end(), hop.peer_id) != org.end();
    });
  }
  std::vector<mesh::MeshHopCandidate> contact_introducers = CollectContactHopCandidates(contacts);
  if (mesh_cfg.trusted_relays_only) {  // privacy T4; an introducer sees both ends' addresses
    const auto trusted = TrustedRelayPeerIds(contacts, mesh_cfg.bootstrap_peers);
    rendezvous = KeepTrustedRelays(std::move(rendezvous), trusted);
    std::erase_if(contact_introducers, [&trusted](const mesh::MeshHopCandidate& hop) { return !trusted.contains(hop.peer_id); });
  }

  for (const auto& hop : rendezvous) {
    grant(hop, mesh::kReachRoleRendezvous);
  }
  for (const auto& hop : seeds) {
    grant(hop, mesh::kReachRoleSeed);
  }
  for (auto hop : contact_introducers) {
    hop.affinity = mesh::MeshHopAffinity::Contact;  // asked before the seeds
    grant(hop, mesh::kReachRoleIntroducer);
  }
  for (const auto& hop : CollectSeedHopCandidates(mesh_cfg.bootstrap_peers)) {
    grant(hop, mesh::kReachRoleIntroducer);
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
