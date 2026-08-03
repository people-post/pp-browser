#include "feature/messaging/MessagingShellPorts.h"

#include "feature/messaging/MessagingHub.h"
#include "libp2p/integration/host/Libp2pHost.h"

namespace pbr {

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub) {
  MessagingShellPorts ports;
  ports.statusbar_connection = [&hub]() -> std::string {
    if (!hub.IsMessagingReady()) {
      return {};
    }
    if (Libp2pHost* host = hub.Libp2p(); host && host->IsRunning()) {
      return "Online";
    }
    return "Direct off";
  };
  return ports;
}

} // namespace pbr
