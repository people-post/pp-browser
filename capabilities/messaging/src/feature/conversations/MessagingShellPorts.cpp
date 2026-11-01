#include "feature/conversations/MessagingShellPorts.h"

#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/host/MeshHost.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

#include <cstdio>
#include <memory>

namespace pbr {
namespace {

BriefRelayHealth MapBriefHealth(MeshDeliveryOrchestrator::BriefRelayHealthState health) {
  switch (health) {
  case MeshDeliveryOrchestrator::BriefRelayHealthState::Ok:
    return BriefRelayHealth::Ok;
  case MeshDeliveryOrchestrator::BriefRelayHealthState::Failed:
    return BriefRelayHealth::Failed;
  case MeshDeliveryOrchestrator::BriefRelayHealthState::Unknown:
  default:
    return BriefRelayHealth::Unknown;
  }
}

} // namespace

mesh::RelayRuntimeStats CollectRelayRuntimeStats(mesh::MeshHost* mesh) {
  mesh::RelayRuntimeStats stats;
  if (!mesh) {
    return stats;
  }
  if (mesh::CircuitRelayServer* amp_circuit = mesh->AmpCircuitServer()) {
    stats.circuit_serving = amp_circuit->IsStarted() && amp_circuit->ServeInbound();
    stats.circuit = amp_circuit->RuntimeStats();
  }
  if (mesh::MediaRelayServer* amp_media = mesh->AmpMediaRelayServer()) {
    stats.media_serving = amp_media->IsStarted() && amp_media->ServeInbound();
    stats.media = amp_media->RuntimeStats();
  }
  return stats;
}

MeshTrafficRates MeshTrafficRatesBetween(const MeshTrafficTotals& before, const MeshTrafficTotals& now) {
  MeshTrafficRates rates;
  const double seconds = std::chrono::duration<double>(now.at - before.at).count();
  if (!before.available || !now.available || seconds <= 0.0) {
    return rates;
  }
  const auto delta = [](const uint64_t later, const uint64_t earlier) {
    return later >= earlier ? later - earlier : uint64_t{0};  // a restarted mesh counts from zero
  };
  rates.valid = true;
  rates.sent_bps = static_cast<double>(delta(now.sent_bytes, before.sent_bytes)) / seconds;
  rates.received_bps = static_cast<double>(delta(now.received_bytes, before.received_bytes)) / seconds;
  rates.relayed_bps = static_cast<double>(delta(now.relayed_bytes, before.relayed_bytes)) / seconds;
  if (const uint64_t samples = delta(now.rtt_samples, before.rtt_samples); samples > 0) {
    rates.rtt_ms = static_cast<int64_t>(delta(now.rtt_sum_ms, before.rtt_sum_ms) / samples);
  }
  if (const uint64_t sent = delta(now.reliable_sent, before.reliable_sent); sent > 0) {
    rates.resend_pct =
        100.0 * static_cast<double>(delta(now.retransmits, before.retransmits)) / static_cast<double>(sent);
  }
  return rates;
}

MeshTrafficRates MeshTrafficSampler::Sample(const MeshTrafficTotals& now) {
  if (!now.available || !last_.available || now.at - last_.at > kMaxGap) {
    last_ = now;
    rates_ = {};
  } else if (now.at - last_.at >= kMinInterval) {
    rates_ = MeshTrafficRatesBetween(last_, now);
    last_ = now;
  }
  return rates_;
}

MeshTrafficTotals CollectMeshTrafficTotals(mesh::MeshHost* mesh) {
  MeshTrafficTotals totals;
  totals.at = std::chrono::steady_clock::now();
  if (!mesh || !mesh->IsRunning() || !mesh->Amp()) {
    return totals;
  }
  auto& runtime = mesh->Amp()->Runtime();
  totals.available = true;
  runtime.WithIoLock([&]() { totals.links = runtime.Links().CountLinks(); });
  const pp::adp::EndpointStats traffic = runtime.GetEndpoint().Stats();
  totals.sent_bytes = traffic.tx_bytes;
  totals.received_bytes = traffic.rx_bytes;
  totals.reliable_sent = traffic.reliable_sent;
  totals.retransmits = traffic.retransmits;
  totals.rtt_samples = traffic.rtt_samples;
  totals.rtt_sum_ms = traffic.rtt_sum_ms;
  const mesh::RelayRuntimeStats load = CollectRelayRuntimeStats(mesh);
  totals.relayed_bytes = load.circuit.bytes_relayed + load.media.bytes_relayed;
  if (mesh::CircuitClientCoordinator* circuit = mesh->AmpCircuitClient()) {
    totals.parked_relays = circuit->ParkedRelayCount();
  }
  return totals;
}

MessagingShellPorts MakeMessagingShellPorts(MessagingShellPortsDeps deps) {
  MessagingShellPorts ports;
  ports.statusbar_cluster = [deps]() -> StatusbarFacts {
    mesh::MeshHost* mesh = deps.mesh ? deps.mesh() : nullptr;
    StatusbarFacts facts;
    facts.messaging_ready = deps.messaging_ready && deps.messaging_ready();
    facts.brief_health = deps.brief_health ? deps.brief_health() : BriefRelayHealth::Unknown;
    facts.host_running = mesh && mesh->IsRunning();
    facts.last_error = deps.last_mesh_error ? deps.last_mesh_error() : std::string{};
    if (mesh) {
      const mesh::ReachabilitySnapshot reach = mesh->Reachability().Snapshot();
      facts.reachability = reach.status;
      facts.has_global_ipv6 = reach.signals.has_global_ipv6;
      facts.dial_back_ok = reach.signals.dial_back_ok;
      facts.upnp_mapped = reach.signals.upnp_mapped;
    }
    facts.help_network_enabled = deps.help_network_enabled && deps.help_network_enabled();
    facts.load = deps.relay_load_stats ? deps.relay_load_stats() : mesh::RelayRuntimeStats{};
    return facts;
  };
  auto sampler = std::make_shared<MeshTrafficSampler>();
  ports.statusbar_popover = [cluster = ports.statusbar_cluster, deps, sampler]() -> StatusbarFacts {
    StatusbarFacts facts = cluster();
    if (deps.traffic_totals) {
      const MeshTrafficTotals now = deps.traffic_totals();
      facts.traffic.available = now.available;
      facts.traffic.links = now.links;
      facts.traffic.parked_relays = now.parked_relays;
      facts.traffic.rates = sampler->Sample(now);
    }
    return facts;
  };
  ports.retest_reachability = std::move(deps.retest_reachability);
  return ports;
}

MessagingShellPorts MakeMessagingShellPorts(ConversationsHub& hub) {
  MessagingShellPortsDeps deps;
  deps.mesh = [&hub]() -> mesh::MeshHost* { return hub.Mesh(); };
  deps.messaging_ready = [&hub]() { return hub.IsMessagingReady(); };
  deps.brief_health = [&hub]() { return MapBriefHealth(hub.MeshMessaging().BriefRelayHealth()); };
  deps.help_network_enabled = [&hub]() { return hub.IsHelpNetworkEnabled(); };
  deps.last_mesh_error = [&hub]() { return hub.LastMeshError(); };
  deps.retest_reachability = [&hub]() { hub.RunReachabilityProbe(false); };
  deps.relay_load_stats = [&hub]() { return CollectRelayRuntimeStats(hub.Mesh()); };
  deps.traffic_totals = [&hub]() { return CollectMeshTrafficTotals(hub.Mesh()); };
  return MakeMessagingShellPorts(std::move(deps));
}

} // namespace pbr
