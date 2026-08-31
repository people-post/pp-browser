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
  /**
   * Route outbound send onto Critical workers (call-control / charge_required).
   * Avoids Accept/MediaKey sitting behind PollInbox. Transport is still Amp-first when dialable,
   * then Brief when a `relay:` route is known.
   */
  bool critical_lane = false;
  /**
   * When set, attach this `key_init_b64` and encrypt with the **current** stored PSK
   * (E027 `psk_rotate` — do not first-message encapsulate).
   */
  std::optional<std::string> key_init_b64;
};

} // namespace pbr
