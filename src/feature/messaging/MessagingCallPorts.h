#pragma once

#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

class CallLifecycle;
class CallSessionManager;

/**
 * Call session / lifecycle ports for CallController.
 * Application fills from MessagingHub. Clear via BindCallPorts({}).
 */
struct MessagingCallPorts {
  std::function<bool()> initialized;
  std::function<CallSessionManager*()> calls;
  std::function<CallLifecycle*()> lifecycle;
  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::optional<Contact>>(const std::string& identity, ContactIdKind kind)>
      find_contact_by_identity;
  std::function<std::optional<std::string>()> local_relay_identity;
};

class MessagingHub;

MessagingCallPorts MakeMessagingCallPorts(MessagingHub& hub);

} // namespace pbr
