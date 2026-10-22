#pragma once

#include "feature/conversations/MessagingShellPorts.h"

#include <string>

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
  /** node-monitoring M3: mesh traffic (links, rates, round trip, resends) and what we relay. */
  bool show_network = false;
  std::string network_links_label;
  /** Rendezvous parking: how many relays can bridge dialers to us (or that none can). */
  std::string network_parked_label;
  std::string network_rate_label;
  std::string network_rtt_label;
  std::string network_resend_label;
  std::string relay_rate_label;

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
           media_participants_label == other.media_participants_label && show_network == other.show_network &&
           network_links_label == other.network_links_label &&
           network_parked_label == other.network_parked_label && network_rate_label == other.network_rate_label &&
           network_rtt_label == other.network_rtt_label && network_resend_label == other.network_resend_label &&
           relay_rate_label == other.relay_rate_label;
  }
  bool operator!=(const StatusbarPopoverSnapshot& other) const { return !(*this == other); }
};

/** The words and slots the status bar shows for what was read. */
StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, bool has_mesh_error,
                                                       mesh::ReachabilityStatus reachability,
                                                       bool help_network_enabled,
                                                       const mesh::RelayRuntimeStats& load = {});

StatusbarPopoverSnapshot BuildStatusbarPopoverSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, const std::string& last_error,
                                                       mesh::ReachabilityStatus reachability, bool has_global_ipv6,
                                                       bool dial_back_ok, bool upnp_mapped,
                                                       bool help_network_enabled,
                                                       const mesh::RelayRuntimeStats& load = {},
                                                       const MeshTrafficView& traffic = {});

/** The same, from the facts the messaging ports read. */
StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(const StatusbarFacts& facts);
StatusbarPopoverSnapshot BuildStatusbarPopoverSnapshot(const StatusbarFacts& facts);

} // namespace pbr
