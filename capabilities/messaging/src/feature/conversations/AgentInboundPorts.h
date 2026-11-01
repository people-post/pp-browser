#pragma once

#include "lib/messaging/AtAiParser.h"

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
  /**
   * A button of the on-device app-action flow ({"type":"local_action","tool":…}): `payload` is that JSON, `text` its
   * label. `in_peer_chat`: the button sits in a person-to-person chat. Optional (unset: such buttons do nothing).
   */
  std::function<void(const std::string& thread_id, const std::string& text, const std::string& payload,
                     bool in_peer_chat)>
      submit_local_action;
  /** Brief preset: the text asks to operate the app (a shared "@ai+"/"@ai++" then shares nothing). Optional. */
  std::function<bool(const std::string& text)> is_app_action_request;
  /** A turn is running. A send that needs the agent then fails with `MessageRouter::kAgentBusyError` before any
   * side effect (nothing stored, nothing sent to a peer), so the UI can queue it (B112). Optional. */
  std::function<bool()> is_busy;

  bool IsBound() const {
    return static_cast<bool>(submit_scoped_assist) && static_cast<bool>(submit_to_thread);
  }
};

} // namespace pbr
