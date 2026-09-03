#pragma once

#include "feature/calls/CallFunctionalPorts.h"
#include "feature/conversations/ConversationsHub.h"

namespace pbr {

class CallUiBackend;
class SessionStore;

/** Optional session_store wires profile `call_diagnostics` (CLI `--debug` still ORs in). */
CallFunctionalPorts MakeCallFunctionalPorts(CallUiBackend& backend, ConversationsHub& hub,
                                            SessionStore* session_store = nullptr);

} // namespace pbr
