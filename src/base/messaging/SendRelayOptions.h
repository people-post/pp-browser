#pragma once

#include "base/messaging/ThreadTypes.h"

#include <optional>
#include <string>

namespace pbr {

/** Options for outbound relay-visible messages (post-v6b shared @ai). */
struct SendRelayOptions {
  std::optional<std::string> sender_contact_id;
  std::optional<std::string> generation;
  std::optional<std::string> ai_invoke_mode;
  std::optional<std::string> seq_owner_contact_id;
  bool update_preview = true;
  /** When set (e.g. System + payload_json), encrypt full ChatPayload instead of text-only. */
  std::optional<ChatContentType> content_type;
  std::optional<std::string> payload_json;
  /** Skip libp2p direct chat (call-control must not block IO on OpenStream). */
  bool prefer_relay = false;
};

} // namespace pbr
