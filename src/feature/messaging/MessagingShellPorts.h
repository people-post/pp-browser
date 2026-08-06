#pragma once

#include "libp2p/integration/host/Reachability.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Ambient status-bar cluster snapshot (network-status-chrome).
 * Application fills via MakeMessagingShellPorts; ShellHost projects into window.* bindings.
 *
 * Group 1 (Client + Node): Brief (HTTP relay) · Direct (libp2p dial-out)
 * Group 2 (Node / Help on): Help · Inbound (dialable)
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

  bool operator==(const StatusbarClusterSnapshot& other) const {
    return brief == other.brief && direct == other.direct && inbound == other.inbound &&
           help_visible == other.help_visible && label == other.label && label_tone == other.label_tone;
  }
  bool operator!=(const StatusbarClusterSnapshot& other) const { return !(*this == other); }
};

/** Last known HTTP Brief / relay poll outcome for ambient chrome. */
enum class BriefRelayHealth {
  Unknown,
  Ok,
  Failed,
};

/** Pure mapping for tests and MakeMessagingShellPorts. */
StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, bool has_libp2p_error,
                                                       ReachabilityStatus reachability,
                                                       bool help_network_enabled);

/**
 * Narrow messaging read ports for shell chrome (status bar cluster).
 * Application fills from MessagingHub. Clear via BindShellMessaging({}).
 */
struct MessagingShellPorts {
  std::function<StatusbarClusterSnapshot()> statusbar_cluster;
};

class MessagingHub;

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub);

} // namespace pbr
