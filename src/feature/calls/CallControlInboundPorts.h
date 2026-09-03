#pragma once

#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Call-control hooks conversations needs from CallSessionManager (consumer-declared).
 * Softens conversations→calls concrete type: MeshMessaging / receive pipeline must not
 * include CallSessionManager once filled from CallStack.
 */
struct CallControlInboundPorts {
  std::function<Roe<void>(ThreadMessage& message, const std::string& sender_identity,
                          std::optional<int64_t> relay_created_at_ms,
                          std::optional<int64_t> relay_server_time_ms)>
      apply_inbound_control;
  std::function<bool()> has_active_local_call;

  bool IsBound() const { return static_cast<bool>(apply_inbound_control); }
};

} // namespace pbr
