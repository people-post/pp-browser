#pragma once

#include "mesh/reachability/Reachability.h"
#include "mesh/l4/shared/RelayRuntimeStats.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"
#include "mesh/host/MeshHost.h"
#include "common/MeshAlias.h"

namespace pbr {

/** Cumulative mesh traffic at one moment (node-monitoring M3: the popover's rates are deltas). */
struct MeshTrafficTotals {
  bool available = false;
  size_t links = 0;
  uint64_t sent_bytes = 0;
  uint64_t received_bytes = 0;
  uint64_t reliable_sent = 0;
  uint64_t retransmits = 0;
  uint64_t rtt_samples = 0;
  uint64_t rtt_sum_ms = 0;
  /** Bytes this node relayed for others (circuit bridges + media relay forwarding). */
  uint64_t relayed_bytes = 0;
  /** Relays holding a reservation for this node (rendezvous parking). */
  size_t parked_relays = 0;
  std::chrono::steady_clock::time_point at{};
};

/** Rates between two samples; a figure with nothing to base it on is negative. */
struct MeshTrafficRates {
  bool valid = false;
  double sent_bps = 0.0;
  double received_bps = 0.0;
  double relayed_bps = 0.0;
  /** Mean round trip of the samples in between; -1 when none. */
  int64_t rtt_ms = -1;
  /** Retransmits per reliable send in between, in percent; -1 when nothing was sent. */
  double resend_pct = -1.0;
};

MeshTrafficRates MeshTrafficRatesBetween(const MeshTrafficTotals& before, const MeshTrafficTotals& now);

/**
 * The popover's rates: deltas between samples at least a second apart (in between, the last rates
 * stand). The popover only samples while open, so a sample after a gap longer than `kMaxGap` (it was
 * closed, the mesh may have restarted) starts over instead of averaging across the gap.
 */
class MeshTrafficSampler {
public:
  static constexpr std::chrono::seconds kMinInterval{1};
  static constexpr std::chrono::seconds kMaxGap{5};
  MeshTrafficRates Sample(const MeshTrafficTotals& now);

private:
  MeshTrafficTotals last_;
  MeshTrafficRates rates_;
};

/** What the popover shows of the mesh traffic (links now, rates since the last sample). */
struct MeshTrafficView {
  bool available = false;
  size_t links = 0;
  size_t parked_relays = 0;
  MeshTrafficRates rates;
};


/** Last known HTTP Brief / relay poll outcome for ambient chrome. */
enum class BriefRelayHealth {
  Unknown,
  Ok,
  Failed,
};

/** What the status bar shows, as read: the GUI words it (gui/shell/StatusbarText). */
struct StatusbarFacts {
  bool messaging_ready = false;
  BriefRelayHealth brief_health = BriefRelayHealth::Unknown;
  bool host_running = false;
  std::string last_error;
  mesh::ReachabilityStatus reachability = mesh::ReachabilityStatus::Unknown;
  bool has_global_ipv6 = false;
  bool dial_back_ok = false;
  bool upnp_mapped = false;
  bool help_network_enabled = false;
  mesh::RelayRuntimeStats load;
  /** Popover only (the cluster does not sample traffic). */
  MeshTrafficView traffic;
};

/**
 * Narrow messaging read/action ports for shell chrome (status bar cluster + popover).
 * Application fills from ConversationsHub (+ settings deep-link). Clear via BindShellMessaging({}).
 */
struct MessagingShellPorts {
  std::function<StatusbarFacts()> statusbar_cluster;
  /** As the cluster, plus the mesh traffic (sampled while the popover is open). */
  std::function<StatusbarFacts()> statusbar_popover;
  std::function<void()> retest_reachability;
  /** Deep-link Me → Network (settings owns section open). */
  std::function<void()> open_network_settings;
};

class ConversationsHub;

/**
 * Hub-owned bits shell chrome still needs that do not live on MeshHost.
 * Mesh reads (host running, reachability, relay load) come from the MeshHost*
 * accessor passed alongside; everything here is projected off the hub.
 */
struct MessagingShellPortsDeps {
  /** Current mesh host; null before the libp2p stack is up (degrade gracefully). */
  std::function<mesh::MeshHost*()> mesh;
  std::function<bool()> messaging_ready;
  std::function<BriefRelayHealth()> brief_health;
  std::function<bool()> help_network_enabled;
  std::function<std::string()> last_mesh_error;
  std::function<void()> retest_reachability;
  std::function<mesh::RelayRuntimeStats()> relay_load_stats;
  /** Cumulative mesh traffic now (the ports turn samples into rates). */
  std::function<MeshTrafficTotals()> traffic_totals;
};

/** Mesh traffic totals from a (possibly null) mesh host. */
MeshTrafficTotals CollectMeshTrafficTotals(mesh::MeshHost* mesh);

/** Collect circuit + media relay serving stats from a (possibly null) mesh host. */
mesh::RelayRuntimeStats CollectRelayRuntimeStats(mesh::MeshHost* mesh);

/** Core builder: mesh reads via deps.mesh(), hub-owned bits via the deps lambdas. */
MessagingShellPorts MakeMessagingShellPorts(MessagingShellPortsDeps deps);

/** Thin wrapper: passes hub.Mesh() + hub-projected lambdas to the core builder. */
MessagingShellPorts MakeMessagingShellPorts(ConversationsHub& hub);

} // namespace pbr
