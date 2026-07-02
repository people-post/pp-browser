#pragma once

#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <vector>

namespace pbr {

/** Binary ChatPayload v1 codec (D087) — v2a-core supports text + system. */
class ChatPayloadCodec {
public:
  static Roe<std::vector<uint8_t>> EncodeText(const std::string& text);
  static Roe<ThreadMessage> DecodeToMessageFields(const std::vector<uint8_t>& chat_payload);
  static Roe<std::vector<uint8_t>> EncodeToRow(const ThreadMessage& message);
  static Roe<void> ApplyRowToMessage(const std::vector<uint8_t>& chat_payload, ThreadMessage& message);
};

} // namespace pbr
