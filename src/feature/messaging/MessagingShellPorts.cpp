#include "feature/messaging/MessagingShellPorts.h"

#include "base/i18n/LocalizationService.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/messaging/P2pMessagingService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/MediaRelayService.h"

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

std::string BriefLabel(BriefRelayHealth health) {
  switch (health) {
  case BriefRelayHealth::Ok:
    return Tr("shell.statusbar.brief_ok");
  case BriefRelayHealth::Failed:
    return Tr("shell.statusbar.brief_offline");
  case BriefRelayHealth::Unknown:
  default:
    return Tr("shell.statusbar.brief_unknown");
  }
}

std::string DirectLabel(StatusbarClusterSnapshot::DirectState direct) {
  switch (direct) {
  case StatusbarClusterSnapshot::DirectState::On:
    return Tr("shell.statusbar.direct_on");
  case StatusbarClusterSnapshot::DirectState::Off:
    return Tr("shell.statusbar.direct_off");
  case StatusbarClusterSnapshot::DirectState::Checking:
    return Tr("shell.statusbar.direct_checking");
  case StatusbarClusterSnapshot::DirectState::Error:
    return Tr("shell.statusbar.direct_error");
  case StatusbarClusterSnapshot::DirectState::Hidden:
  default:
    return {};
  }
}

std::string ReachabilityStatusLabel(ReachabilityStatus status) {
  switch (status) {
  case ReachabilityStatus::Checking:
    return Tr("settings.network.reachability.checking");
  case ReachabilityStatus::Reachable:
    return Tr("settings.network.reachability.reachable");
  case ReachabilityStatus::OutboundOnly:
    return Tr("settings.network.reachability.outbound_only");
  case ReachabilityStatus::Blocked:
    return Tr("settings.network.reachability.blocked");
  case ReachabilityStatus::Unknown:
  default:
    return Tr("settings.network.reachability.unknown");
  }
}

std::string ReachabilitySummary(ReachabilityStatus status, bool has_global_ipv6, bool dial_back_ok,
                                bool upnp_mapped) {
  switch (status) {
  case ReachabilityStatus::Checking:
    return Tr("settings.network.reachability.summary_checking");
  case ReachabilityStatus::Reachable:
    if (has_global_ipv6 && dial_back_ok) {
      return Tr("settings.network.reachability.summary_reachable_ipv6");
    }
    if (upnp_mapped) {
      return Tr("settings.network.reachability.summary_reachable_upnp");
    }
    return Tr("settings.network.reachability.summary_reachable");
  case ReachabilityStatus::OutboundOnly:
    return Tr("settings.network.reachability.summary_outbound_only");
  case ReachabilityStatus::Blocked:
    return Tr("settings.network.reachability.summary_blocked");
  case ReachabilityStatus::Unknown:
  default:
    return Tr("settings.network.reachability.summary_unknown");
  }
}

std::string CountArg(size_t n) {
  return std::to_string(n);
}

void FillLoadSlots(StatusbarClusterSnapshot& snap, bool help_network_enabled,
                   const RelayRuntimeStats& load) {
  if (!help_network_enabled) {
    return;
  }
  const size_t circuit_n =
      load.circuit_serving ? load.circuit.active_bridges : 0;
  const size_t media_n = load.media_serving ? load.media.active_sessions : 0;
  if (circuit_n > 0) {
    snap.load_circuit_visible = true;
    snap.load_circuit_label =
        Tr("shell.statusbar.load.circuit", {{"count", CountArg(circuit_n)}});
    snap.load_circuit_title =
        Tr("shell.statusbar.a11y.load_circuit", {{"count", CountArg(circuit_n)}});
  }
  if (media_n > 0) {
    snap.load_media_visible = true;
    snap.load_media_label = Tr("shell.statusbar.load.media", {{"count", CountArg(media_n)}});
    snap.load_media_title =
        Tr("shell.statusbar.a11y.load_media", {{"count", CountArg(media_n)}});
  }
}

void FillPopoverLoad(StatusbarPopoverSnapshot& snap, bool help_network_enabled,
                     const RelayRuntimeStats& load) {
  if (!help_network_enabled) {
    return;
  }
  const size_t bridges = load.circuit_serving ? load.circuit.active_bridges : 0;
  const size_t sessions = load.media_serving ? load.media.active_sessions : 0;
  const size_t participants = load.media_serving ? load.media.active_participants : 0;
  if (bridges == 0 && sessions == 0 && participants == 0) {
    return;
  }
  snap.show_load = true;
  snap.circuit_bridges = bridges;
  snap.media_sessions = sessions;
  snap.media_participants = participants;
  snap.circuit_load_label =
      Tr("shell.statusbar.popover.circuit_load", {{"count", CountArg(bridges)}});
  snap.media_sessions_label =
      Tr("shell.statusbar.popover.media_sessions", {{"count", CountArg(sessions)}});
  snap.media_participants_label =
      Tr("shell.statusbar.popover.media_participants", {{"count", CountArg(participants)}});
}

void FillSlotTitles(StatusbarClusterSnapshot& snap) {
  if (snap.brief != StatusbarClusterSnapshot::BriefState::Hidden) {
    if (snap.brief == StatusbarClusterSnapshot::BriefState::Ok) {
      snap.brief_title = Tr("shell.statusbar.a11y.brief_ok");
    } else if (snap.brief == StatusbarClusterSnapshot::BriefState::Failed) {
      snap.brief_title = Tr("shell.statusbar.a11y.brief_failed");
    } else {
      snap.brief_title = Tr("shell.statusbar.a11y.brief_unknown");
    }
  }
  if (snap.direct != StatusbarClusterSnapshot::DirectState::Hidden) {
    switch (snap.direct) {
    case StatusbarClusterSnapshot::DirectState::On:
      snap.direct_title = Tr("shell.statusbar.a11y.direct_on");
      break;
    case StatusbarClusterSnapshot::DirectState::Off:
      snap.direct_title = Tr("shell.statusbar.a11y.direct_off");
      break;
    case StatusbarClusterSnapshot::DirectState::Checking:
      snap.direct_title = Tr("shell.statusbar.a11y.direct_checking");
      break;
    case StatusbarClusterSnapshot::DirectState::Error:
      snap.direct_title = Tr("shell.statusbar.a11y.direct_error");
      break;
    default:
      break;
    }
  }
  if (snap.help_visible) {
    snap.help_title = Tr("shell.statusbar.a11y.help_on");
  }
  if (snap.inbound == StatusbarClusterSnapshot::InboundState::On) {
    snap.inbound_title = Tr("shell.statusbar.a11y.inbound_on");
  } else if (snap.inbound == StatusbarClusterSnapshot::InboundState::Off) {
    snap.inbound_title = Tr("shell.statusbar.a11y.inbound_off");
  }
}

RelayRuntimeStats CollectRelayRuntimeStats(MessagingHub& hub) {
  RelayRuntimeStats stats;
  if (CircuitRelayService* circuit = hub.CircuitRelay()) {
    stats.circuit_serving = circuit->IsStarted();
    if (stats.circuit_serving) {
      stats.circuit = circuit->RuntimeStats();
    }
  }
  if (MediaRelayService* media = hub.MediaRelay()) {
    stats.media_serving = media->IsStarted();
    if (stats.media_serving) {
      stats.media = media->RuntimeStats();
    }
  }
  return stats;
}

} // namespace

StatusbarClusterSnapshot BuildStatusbarClusterSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, bool has_libp2p_error,
                                                       ReachabilityStatus reachability,
                                                       bool help_network_enabled,
                                                       const RelayRuntimeStats& load) {
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
    FillSlotTitles(snap);
    FillLoadSlots(snap, help_network_enabled, load);
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
  FillSlotTitles(snap);
  FillLoadSlots(snap, help_network_enabled, load);
  return snap;
}

StatusbarPopoverSnapshot BuildStatusbarPopoverSnapshot(bool messaging_ready, BriefRelayHealth brief_health,
                                                       bool host_running, const std::string& last_error,
                                                       ReachabilityStatus reachability, bool has_global_ipv6,
                                                       bool dial_back_ok, bool upnp_mapped,
                                                       bool help_network_enabled,
                                                       const RelayRuntimeStats& load) {
  StatusbarPopoverSnapshot snap;
  snap.messaging_ready = messaging_ready;
  if (!messaging_ready) {
    return snap;
  }

  snap.brief_label = BriefLabel(brief_health);
  snap.help_visible = help_network_enabled;
  if (help_network_enabled) {
    snap.help_label = Tr("settings.network.help_network");
  }

  const StatusbarClusterSnapshot cluster =
      BuildStatusbarClusterSnapshot(messaging_ready, brief_health, host_running, !last_error.empty(),
                                    reachability, help_network_enabled, load);
  snap.direct_label = DirectLabel(cluster.direct);

  if (host_running) {
    snap.reachability_status_label = ReachabilityStatusLabel(reachability);
    snap.reachability_summary =
        ReachabilitySummary(reachability, has_global_ipv6, dial_back_ok, upnp_mapped);
    snap.show_upnp = help_network_enabled;
    snap.upnp_mapped = upnp_mapped;
    if (snap.show_upnp) {
      snap.upnp_label =
          upnp_mapped ? Tr("shell.statusbar.upnp_mapped") : Tr("shell.statusbar.upnp_not_mapped");
    }
  } else {
    snap.reachability_status_label = Tr("settings.network.reachability.unknown");
    snap.reachability_summary = Tr("settings.network.reachability.summary_unknown");
  }

  snap.last_error = last_error;
  FillPopoverLoad(snap, help_network_enabled, load);
  return snap;
}

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub) {
  MessagingShellPorts ports;
  ports.statusbar_cluster = [&hub]() -> StatusbarClusterSnapshot {
    const bool ready = hub.IsMessagingReady();
    Libp2pHost* host = hub.Libp2p();
    const bool running = host && host->IsRunning();
    const bool has_error = !hub.LastLibp2pError().empty();
    const RelayRuntimeStats load = CollectRelayRuntimeStats(hub);
    return BuildStatusbarClusterSnapshot(ready, MapBriefHealth(hub.P2p().BriefRelayHealth()), running,
                                         has_error, hub.Reachability().status,
                                         hub.IsHelpNetworkEnabled(), load);
  };
  ports.statusbar_popover = [&hub]() -> StatusbarPopoverSnapshot {
    const bool ready = hub.IsMessagingReady();
    Libp2pHost* host = hub.Libp2p();
    const bool running = host && host->IsRunning();
    const ReachabilitySnapshot reach = hub.Reachability();
    const RelayRuntimeStats load = CollectRelayRuntimeStats(hub);
    return BuildStatusbarPopoverSnapshot(ready, MapBriefHealth(hub.P2p().BriefRelayHealth()), running,
                                         hub.LastLibp2pError(), reach.status, reach.signals.has_global_ipv6,
                                         reach.signals.dial_back_ok, reach.signals.upnp_mapped,
                                         hub.IsHelpNetworkEnabled(), load);
  };
  ports.retest_reachability = [&hub]() { hub.RunReachabilityProbe(false); };
  return ports;
}

} // namespace pbr
