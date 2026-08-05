#pragma once

#include "libp2p/integration/host/Reachability.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Ambient status-bar cluster snapshot (network-status-chrome s1).
 * Application fills via MakeMessagingShellPorts; ShellHost projects into window.* bindings.
 */
struct StatusbarClusterSnapshot {
  enum class MeshState {
    Hidden,
    Off,
    On,
    Error,
  };

  enum class ReachState {
    Hidden,
    Unknown,
    Checking,
    Reachable,
    OutboundOnly,
    Blocked,
  };

  MeshState mesh = MeshState::Hidden;
  ReachState reach = ReachState::Hidden;
  /** Node / Help the network visible (desktop Node role). */
  bool help_visible = false;
  /** Sparse word for unhealthy/off states; empty when healthy icons suffice. */
  std::string label;
  /** Label tone: muted (default), warn, or error. */
  enum class LabelTone {
    Muted,
    Warn,
    Error,
  };
  LabelTone label_tone = LabelTone::Muted;

  bool operator==(const StatusbarClusterSnapshot& other) const {
    return mesh == other.mesh && reach == other.reach && help_visible == other.help_visible &&
           label == other.label && label_tone == other.label_tone;
  }
  bool operator!=(const StatusbarClusterSnapshot& other) const { return !(*this == other); }
};

/** Pure mapping for tests and MakeMessagingShellPorts. */
StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, bool host_running,
                                                       bool has_libp2p_error,
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
