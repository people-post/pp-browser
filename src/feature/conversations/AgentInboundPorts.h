#pragma once

#include "domain/messaging/AtAiParser.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

/**
 * Inbound agent ops for MessageRouter (consumer-declared; app fills from AgentSession).
 * Softens conversations→ai: messaging must not include feature/ai/.
 */
struct AgentInboundPorts {
  std::function<void(const std::string& thread_id, const std::string& prompt,
                     std::optional<std::string> user_payload, AtAiMode mode)>
      submit_scoped_assist;
  std::function<void(const std::string& thread_id, const std::string& user_text,
                     std::optional<std::string> user_payload)>
      submit_to_thread;

  bool IsBound() const {
    return static_cast<bool>(submit_scoped_assist) && static_cast<bool>(submit_to_thread);
  }
};

} // namespace pbr
