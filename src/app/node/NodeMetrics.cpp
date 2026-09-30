#include "app/node/NodeMetrics.h"

#include "domain/mesh/dht/AmpDhtProtocol.h"
#include "domain/mesh/reachability/Reachability.h"
#include "foundation/platform/os/OsProcessStats.h"
#include "foundation/runtime/AppVersion.h"

#include <chrono>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

double UnixSeconds(const std::chrono::system_clock::time_point at) {
  return std::chrono::duration<double>(at.time_since_epoch()).count();
}

void CollectProcess(MetricsRegistry& r, const std::chrono::steady_clock::time_point started) {
  r.Gauge("pp_process_uptime_seconds", "Seconds since this process started.")
      .Set(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  const OsProcessStats stats = ReadOsProcessStats();
  if (!stats.available) {
    return;
  }
  r.Gauge("pp_process_cpu_seconds", "User + system CPU seconds used (monotonic; use rate()).").Set(stats.cpu_seconds);
  r.Gauge("pp_process_resident_memory_bytes", "Resident memory.").Set(static_cast<double>(stats.resident_bytes));
  r.Gauge("pp_process_threads", "OS threads.").Set(static_cast<double>(stats.threads));
  r.Gauge("pp_process_open_fds", "Open file descriptors.").Set(static_cast<double>(stats.open_fds));
}

void CollectMesh(MetricsRegistry& r, MeshHost& mesh) {
  const char* capability = "Services this node serves (1 = serving).";
  const bool circuit = mesh.AmpCircuitServer() && mesh.AmpCircuitServer()->IsStarted() &&
                       mesh.AmpCircuitServer()->ServeInbound();
  const bool media = mesh.AmpMediaRelayServer() && mesh.AmpMediaRelayServer()->IsStarted() &&
                     mesh.AmpMediaRelayServer()->ServeInbound();
  const bool dht = mesh.AmpDht() && mesh.AmpDht()->IsStarted();
  r.Gauge("pp_node_capability", capability, {{"service", "circuit_relay"}}).Set(circuit ? 1 : 0);
  r.Gauge("pp_node_capability", capability, {{"service", "media_relay"}}).Set(media ? 1 : 0);
  r.Gauge("pp_node_capability", capability, {{"service", "dht"}}).Set(dht ? 1 : 0);

  const ReachabilitySnapshot reach = mesh.Reachability().Snapshot();
  const char* status = "Reachability verdict (1 = current).";
  for (const ReachabilityStatus s : {ReachabilityStatus::Unknown, ReachabilityStatus::Checking,
                                     ReachabilityStatus::Reachable, ReachabilityStatus::OutboundOnly,
                                     ReachabilityStatus::Blocked}) {
    r.Gauge("pp_reachability_status", status, {{"status", ReachabilityStatusKey(s)}}).Set(reach.status == s ? 1 : 0);
  }
  const char* signal = "Reachability probe signals (1 = true).";
  const ReachabilitySignals& sig = reach.signals;
  r.Gauge("pp_reachability_signal", signal, {{"signal", "dial_back_ok"}}).Set(sig.dial_back_ok ? 1 : 0);
  r.Gauge("pp_reachability_signal", signal, {{"signal", "seed_dial_ok"}}).Set(sig.seed_dial_ok ? 1 : 0);
  r.Gauge("pp_reachability_signal", signal, {{"signal", "upnp_mapped"}}).Set(sig.upnp_mapped ? 1 : 0);
  r.Gauge("pp_reachability_signal", signal, {{"signal", "public_ipv4"}}).Set(sig.has_public_listen_ip ? 1 : 0);
  r.Gauge("pp_reachability_signal", signal, {{"signal", "global_ipv6"}}).Set(sig.has_global_ipv6 ? 1 : 0);

  if (auto* amp = mesh.Amp()) {
    size_t links = 0;
    amp->Runtime().WithIoLock([&]() { links = amp->Runtime().Links().CountLinks(); });
    r.Gauge("pp_link_active", "Amp links in the link table.").Set(static_cast<double>(links));
    const pp::adp::EndpointStats traffic = amp->Runtime().GetEndpoint().Stats();  // atomics: any thread
    const char* datagrams = "Amp UDP datagrams, by direction.";
    const char* bytes = "Amp UDP datagram bytes, by direction.";
    const char* reliable = "Amp reliable packets: first sends, retransmits, and given up after the retry cap.";
    r.Counter("pp_amp_datagrams_total", datagrams, {{"direction", "sent"}}).Mirror(traffic.tx_datagrams);
    r.Counter("pp_amp_datagrams_total", datagrams, {{"direction", "received"}}).Mirror(traffic.rx_datagrams);
    r.Counter("pp_amp_bytes_total", bytes, {{"direction", "sent"}}).Mirror(traffic.tx_bytes);
    r.Counter("pp_amp_bytes_total", bytes, {{"direction", "received"}}).Mirror(traffic.rx_bytes);
    r.Counter("pp_amp_datagrams_rejected_total", "Received datagrams no association took (bad HMAC / decode).")
        .Mirror(traffic.rx_rejected);
    r.Counter("pp_amp_reliable_packets_total", reliable, {{"event", "sent"}}).Mirror(traffic.reliable_sent);
    r.Counter("pp_amp_reliable_packets_total", reliable, {{"event", "retransmitted"}}).Mirror(traffic.retransmits);
    r.Counter("pp_amp_reliable_packets_total", reliable, {{"event", "lost"}}).Mirror(traffic.reliable_lost);
  }

  if (CircuitRelayServer* server = mesh.AmpCircuitServer()) {
    const CircuitRelayRuntimeStats load = server->RuntimeStats();
    const char* tunnels = "Circuit relay tunnels open, by state.";
    r.Gauge("pp_circuit_relay_tunnels", tunnels, {{"state", "bridged"}}).Set(static_cast<double>(load.active_bridges));
    r.Gauge("pp_circuit_relay_tunnels", tunnels, {{"state", "setup"}}).Set(static_cast<double>(load.pending_tunnels));
    r.Gauge("pp_circuit_relay_reservations", "Answerers parked on this relay.")
        .Set(static_cast<double>(load.reservations));
    r.Counter("pp_circuit_relay_bytes_total", "Bytes spliced through circuit relay bridges (both directions).")
        .Mirror(load.bytes_relayed);
  }

  if (MediaRelayServer* server = mesh.AmpMediaRelayServer()) {
    const MediaRelayRuntimeStats load = server->RuntimeStats();
    r.Gauge("pp_media_relay_sessions", "Hosted media relay sessions with participants.")
        .Set(static_cast<double>(load.active_sessions));
    r.Gauge("pp_media_relay_participants", "Participants across hosted media relay sessions.")
        .Set(static_cast<double>(load.active_participants));
  }

  if (AmpDhtProtocol* protocol = mesh.AmpDht()) {
    const DhtOpsStats ops = protocol->Stats();
    const char* inbound = "DHT requests served, by op.";
    r.Gauge("pp_dht_records", "DHT records cached.").Set(static_cast<double>(ops.cached_records));
    r.Counter("pp_dht_inbound_requests_total", inbound, {{"op", "find_peer"}}).Mirror(ops.inbound_find_peer);
    r.Counter("pp_dht_inbound_requests_total", inbound, {{"op", "store"}}).Mirror(ops.inbound_store);
    r.Counter("pp_dht_inbound_rate_limited_total", "DHT requests refused by the per-peer rate limit.")
        .Mirror(ops.inbound_rate_limited);
    r.Counter("pp_dht_store_rejected_total", "DHT stores rejected.").Mirror(ops.store_rejected);
    r.Counter("pp_dht_lookups_total", "DHT find_peer lookups issued.").Mirror(ops.find_peer_issued);
  }
}

} // namespace

ScopedMetricsCollector RegisterNodeMetrics(NodeBootstrapResult& boot) {
  MetricsRegistry& registry = MetricsRegistry::Global();
  registry.Gauge("pp_build_info", "This build (value is always 1).", {{"version", AppVersionString()}}).Set(1);
  registry.Gauge("pp_process_start_time_seconds", "Process start, Unix seconds.")
      .Set(UnixSeconds(std::chrono::system_clock::now()));
  const auto started = std::chrono::steady_clock::now();
  NodeBootstrapResult* node = &boot;
  return ScopedMetricsCollector(registry, [node, started](MetricsRegistry& r) {
    CollectProcess(r, started);
    if (node->mesh) {
      CollectMesh(r, *node->mesh);
    }
  });
}

} // namespace pbr
