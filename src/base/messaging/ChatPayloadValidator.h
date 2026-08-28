#pragma once

#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** v4 ChatPayload ingest/send validation (D026, D050, D029, D030). */
class ChatPayloadValidator {
public:
  static Roe<ThreadMessage> DecodeValidated(const std::vector<uint8_t>& chat_payload);
  static Roe<void> ValidateOutboundText(const std::string& text);
  static void SanitizeInboundFields(ThreadMessage& message);
};

} // namespace pbr
