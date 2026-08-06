#include "feature/messaging/MessagingShellPorts.h"

#include "base/i18n/LocalizationService.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/messaging/P2pMessagingService.h"
#include "libp2p/integration/host/Libp2pHost.h"

namespace pbr {
namespace {

BriefRelayHealth MapBriefHealth(P2pMessagingService::BriefRelayHealthState health) {
  switch (health) {
  case P2pMessagingService::BriefRelayHealthState::Ok:
    return BriefRelayHealth::Ok;
  case P2pMessagingService::BriefRelayHealthState::Failed:
    return BriefRelayHealth::Failed;
  case P2pMessagingService::BriefRelayHealthState::Unknown:
  default:
    return BriefRelayHealth::Unknown;
  }
}

} // namespace

StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, bool has_libp2p_error,
                                                       ReachabilityStatus reachability,
                                                       bool help_network_enabled) {
  StatusbarClusterSnapshot snap;
  if (!messaging_ready) {
    return snap;
  }

  snap.help_visible = help_network_enabled;

  switch (brief_health) {
  case BriefRelayHealth::Ok:
    snap.brief = StatusbarClusterSnapshot::BriefState::Ok;
    break;
  case BriefRelayHealth::Failed:
    snap.brief = StatusbarClusterSnapshot::BriefState::Failed;
    snap.label = Tr("shell.statusbar.brief_offline");
    snap.label_tone = StatusbarClusterSnapshot::LabelTone::Error;
    break;
  case BriefRelayHealth::Unknown:
  default:
    snap.brief = StatusbarClusterSnapshot::BriefState::Unknown;
    break;
  }

  if (!host_running) {
    if (has_libp2p_error) {
      snap.direct = StatusbarClusterSnapshot::DirectState::Error;
      if (snap.label.empty()) {
        snap.label_tone = StatusbarClusterSnapshot::LabelTone::Error;
      }
    } else {
      snap.direct = StatusbarClusterSnapshot::DirectState::Off;
      if (snap.label.empty()) {
        snap.label = Tr("shell.statusbar.direct_off");
        snap.label_tone = StatusbarClusterSnapshot::LabelTone::Warn;
      }
    }
    return snap;
  }

  switch (reachability) {
  case ReachabilityStatus::Checking:
    snap.direct = StatusbarClusterSnapshot::DirectState::Checking;
    break;
  case ReachabilityStatus::Reachable:
    snap.direct = StatusbarClusterSnapshot::DirectState::On;
    if (help_network_enabled) {
      snap.inbound = StatusbarClusterSnapshot::InboundState::On;
    }
    break;
  case ReachabilityStatus::OutboundOnly:
    snap.direct = StatusbarClusterSnapshot::DirectState::On;
    if (help_network_enabled) {
      snap.inbound = StatusbarClusterSnapshot::InboundState::Off;
      if (snap.label.empty()) {
        snap.label = Tr("settings.network.reachability.outbound_only");
        snap.label_tone = StatusbarClusterSnapshot::LabelTone::Warn;
      }
    }
    break;
  case ReachabilityStatus::Blocked:
    snap.direct = StatusbarClusterSnapshot::DirectState::Error;
    if (help_network_enabled) {
      snap.inbound = StatusbarClusterSnapshot::InboundState::Off;
    }
    if (snap.label.empty()) {
      snap.label = Tr("settings.network.reachability.blocked");
      snap.label_tone = StatusbarClusterSnapshot::LabelTone::Error;
    }
    break;
  case ReachabilityStatus::Unknown:
  default:
    snap.direct = StatusbarClusterSnapshot::DirectState::Checking;
    break;
  }
  return snap;
}

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub) {
  MessagingShellPorts ports;
  ports.statusbar_cluster = [&hub]() -> StatusbarClusterSnapshot {
    const bool ready = hub.IsMessagingReady();
    Libp2pHost* host = hub.Libp2p();
    const bool running = host && host->IsRunning();
    const bool has_error = !hub.LastLibp2pError().empty();
    return BuildStatusbarClusterSnapshot(ready, MapBriefHealth(hub.P2p().BriefRelayHealth()), running,
                                         has_error, hub.Reachability().status,
                                         hub.IsHelpNetworkEnabled());
  };
  return ports;
}

} // namespace pbr
