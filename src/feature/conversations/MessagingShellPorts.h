#pragma once

#include "domain/mesh/reachability/Reachability.h"
#include "domain/mesh/l4/shared/RelayRuntimeStats.h"

#include <cstddef>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Ambient status-bar cluster snapshot (network-status-chrome).
 * Application fills via MakeMessagingShellPorts; ShellHost projects into window.* bindings.
 *
 * Group 1 (Client + Node): Brief (HTTP relay) · Direct (libp2p dial-out)
 * Group 2 (Node / Help on): Help · Inbound (dialable) · Load (when count > 0)
 */
struct StatusbarClusterSnapshot {
  enum class BriefState {
    Hidden,
    Unknown,
    Ok,
    Failed,
  };

  enum class DirectState {
    Hidden,
    Off,
    Checking,
    On,
    Error,
  };

  enum class InboundState {
    Hidden,
    On,
    Off,
  };

  enum class LabelTone {
    Muted,
    Warn,
    Error,
  };

  BriefState brief = BriefState::Hidden;
  DirectState direct = DirectState::Hidden;
  InboundState inbound = InboundState::Hidden;
  /** Node / Help the network visible (desktop Node role). */
  bool help_visible = false;
  /** Sparse word for unhealthy/off states; empty when healthy icons suffice. */
  std::string label;
  LabelTone label_tone = LabelTone::Muted;
  /** Accessible names for icon-only healthy (and other) slot states. */
  std::string brief_title;
  std::string direct_title;
  std::string help_title;
  std::string inbound_title;
  /** Load pills (Help on + count > 0); empty when hidden. */
  bool load_circuit_visible = false;
  bool load_media_visible = false;
  std::string load_circuit_label;
  std::string load_media_label;
  std::string load_circuit_title;
  std::string load_media_title;

  bool operator==(const StatusbarClusterSnapshot& other) const {
    return brief == other.brief && direct == other.direct && inbound == other.inbound &&
           help_visible == other.help_visible && label == other.label && label_tone == other.label_tone &&
           brief_title == other.brief_title && direct_title == other.direct_title &&
           help_title == other.help_title && inbound_title == other.inbound_title &&
           load_circuit_visible == other.load_circuit_visible &&
           load_media_visible == other.load_media_visible &&
           load_circuit_label == other.load_circuit_label && load_media_label == other.load_media_label &&
           load_circuit_title == other.load_circuit_title && load_media_title == other.load_media_title;
  }
  bool operator!=(const StatusbarClusterSnapshot& other) const { return !(*this == other); }
};

/**
 * Hybrid status-bar popover — inspect + Retest; no capability toggles.
 * Strings use settings reachability parity where applicable.
 */
struct StatusbarPopoverSnapshot {
  bool messaging_ready = false;
  std::string brief_label;
  std::string direct_label;
  std::string reachability_status_label;
  std::string reachability_summary;
  bool help_visible = false;
  std::string help_label;
  bool show_upnp = false;
  bool upnp_mapped = false;
  std::string upnp_label;
  std::string last_error;
  bool show_load = false;
  size_t circuit_bridges = 0;
  size_t media_sessions = 0;
  size_t media_participants = 0;
  std::string circuit_load_label;
  std::string media_sessions_label;
  std::string media_participants_label;

  bool operator==(const StatusbarPopoverSnapshot& other) const {
    return messaging_ready == other.messaging_ready && brief_label == other.brief_label &&
           direct_label == other.direct_label &&
           reachability_status_label == other.reachability_status_label &&
           reachability_summary == other.reachability_summary && help_visible == other.help_visible &&
           help_label == other.help_label && show_upnp == other.show_upnp &&
           upnp_mapped == other.upnp_mapped && upnp_label == other.upnp_label &&
           last_error == other.last_error && show_load == other.show_load &&
           circuit_bridges == other.circuit_bridges && media_sessions == other.media_sessions &&
           media_participants == other.media_participants &&
           circuit_load_label == other.circuit_load_label &&
           media_sessions_label == other.media_sessions_label &&
           media_participants_label == other.media_participants_label;
  }
  bool operator!=(const StatusbarPopoverSnapshot& other) const { return !(*this == other); }
};

/** Last known HTTP Brief / relay poll outcome for ambient chrome. */
enum class BriefRelayHealth {
  Unknown,
  Ok,
  Failed,
};

/** Pure mapping for tests and MakeMessagingShellPorts. */
StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, bool has_mesh_error,
                                                       ReachabilityStatus reachability,
                                                       bool help_network_enabled,
                                                       const RelayRuntimeStats& load = {});

StatusbarPopoverSnapshot BuildStatusbarPopoverSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, const std::string& last_error,
                                                       ReachabilityStatus reachability, bool has_global_ipv6,
                                                       bool dial_back_ok, bool upnp_mapped,
                                                       bool help_network_enabled,
                                                       const RelayRuntimeStats& load = {});

/**
 * Narrow messaging read/action ports for shell chrome (status bar cluster + popover).
 * Application fills from ConversationsHub (+ settings deep-link). Clear via BindShellMessaging({}).
 */
struct MessagingShellPorts {
  std::function<StatusbarClusterSnapshot()> statusbar_cluster;
  std::function<StatusbarPopoverSnapshot()> statusbar_popover;
  std::function<void()> retest_reachability;
  /** Deep-link Me → Network (settings owns section open). */
  std::function<void()> open_network_settings;
};

class MeshHost;
class ConversationsHub;

/**
 * Hub-owned bits shell chrome still needs that do not live on MeshHost.
 * Mesh reads (host running, reachability, relay load) come from the MeshHost*
 * accessor passed alongside; everything here is projected off the hub.
 */
struct MessagingShellPortsDeps {
  /** Current mesh host; null before the libp2p stack is up (degrade gracefully). */
  std::function<MeshHost*()> mesh;
  std::function<bool()> messaging_ready;
  std::function<BriefRelayHealth()> brief_health;
  std::function<bool()> help_network_enabled;
  std::function<std::string()> last_mesh_error;
  std::function<void()> retest_reachability;
  std::function<RelayRuntimeStats()> relay_load_stats;
};

/** Collect circuit + media relay serving stats from a (possibly null) mesh host. */
RelayRuntimeStats CollectRelayRuntimeStats(MeshHost* mesh);

/** Core builder: mesh reads via deps.mesh(), hub-owned bits via the deps lambdas. */
MessagingShellPorts MakeMessagingShellPorts(MessagingShellPortsDeps deps);

/** Thin wrapper: passes hub.Mesh() + hub-projected lambdas to the core builder. */
MessagingShellPorts MakeMessagingShellPorts(ConversationsHub& hub);

} // namespace pbr
