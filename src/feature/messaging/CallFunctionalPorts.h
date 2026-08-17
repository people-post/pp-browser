#pragma once

#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

class CallUiBackend;

/**
 * Call functional ports for CallController.
 * Application fills from owned CallUiBackend + MessagingHub lookups.
 * Clear via BindCallPorts({}).
 */
struct CallFunctionalPorts {
  std::function<bool()> initialized;
  std::function<CallUiBackend*()> backend;
  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::optional<Contact>>(const std::string& identity, ContactIdKind kind)>
      find_contact_by_identity;
  std::function<std::optional<std::string>()> local_relay_identity;
  /** Profile pref OR `--debug` CLI override (V032 call diagnostics). */
  std::function<bool()> call_diagnostics_enabled;
};

class MessagingHub;
class SessionStore;

/** Optional session_store wires profile `call_diagnostics` (CLI `--debug` still ORs in). */
CallFunctionalPorts MakeCallFunctionalPorts(CallUiBackend& backend, MessagingHub& hub,
                                            SessionStore* session_store = nullptr);

} // namespace pbr
