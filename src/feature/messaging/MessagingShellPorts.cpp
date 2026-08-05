#include "feature/messaging/MessagingShellPorts.h"

#include "base/i18n/LocalizationService.h"
#include "feature/messaging/MessagingHub.h"
#include "libp2p/integration/host/Libp2pHost.h"

namespace pbr {

StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, bool host_running,
                                                       bool has_libp2p_error,
                                                       ReachabilityStatus reachability,
                                                       bool help_network_enabled) {
  StatusbarClusterSnapshot snap;
  if (!messaging_ready) {
    return snap;
  }

  snap.help_visible = help_network_enabled;

  if (host_running) {
    snap.mesh = StatusbarClusterSnapshot::MeshState::On;
    switch (reachability) {
    case ReachabilityStatus::Checking:
      snap.reach = StatusbarClusterSnapshot::ReachState::Checking;
      break;
    case ReachabilityStatus::Reachable:
      snap.reach = StatusbarClusterSnapshot::ReachState::Reachable;
      break;
    case ReachabilityStatus::OutboundOnly:
      snap.reach = StatusbarClusterSnapshot::ReachState::OutboundOnly;
      snap.label = Tr("settings.network.reachability.outbound_only");
      snap.label_tone = StatusbarClusterSnapshot::LabelTone::Warn;
      break;
    case ReachabilityStatus::Blocked:
      snap.reach = StatusbarClusterSnapshot::ReachState::Blocked;
      snap.label = Tr("settings.network.reachability.blocked");
      snap.label_tone = StatusbarClusterSnapshot::LabelTone::Error;
      break;
    case ReachabilityStatus::Unknown:
    default:
      snap.reach = StatusbarClusterSnapshot::ReachState::Unknown;
      break;
    }
    return snap;
  }

  if (has_libp2p_error) {
    snap.mesh = StatusbarClusterSnapshot::MeshState::Error;
    snap.label_tone = StatusbarClusterSnapshot::LabelTone::Error;
    return snap;
  }

  snap.mesh = StatusbarClusterSnapshot::MeshState::Off;
  snap.label = Tr("shell.statusbar.direct_off");
  snap.label_tone = StatusbarClusterSnapshot::LabelTone::Muted;
  return snap;
}

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub) {
  MessagingShellPorts ports;
  ports.statusbar_cluster = [&hub]() -> StatusbarClusterSnapshot {
    const bool ready = hub.IsMessagingReady();
    Libp2pHost* host = hub.Libp2p();
    const bool running = host && host->IsRunning();
    const bool has_error = !hub.LastLibp2pError().empty();
    return BuildStatusbarClusterSnapshot(ready, running, has_error, hub.Reachability().status,
                                         hub.IsHelpNetworkEnabled());
  };
  return ports;
}

} // namespace pbr
