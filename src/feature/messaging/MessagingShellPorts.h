#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Narrow messaging read ports for shell chrome (status bar connection label).
 * Application fills from MessagingHub. Clear via BindShellMessaging({}).
 */
struct MessagingShellPorts {
  std::function<std::string()> statusbar_connection;
};

class MessagingHub;

MessagingShellPorts MakeMessagingShellPorts(MessagingHub& hub);

} // namespace pbr
