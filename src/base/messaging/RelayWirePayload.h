#pragma once

#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <string>

namespace pbr {

/** Pre-crypto wire helper: ChatPayload bytes in body.e2e.payload_b64 (D073). */
class RelayWirePayload {
public:
  static Roe<std::string> EncodePlaintextText(const std::string& text);
  static Roe<ThreadMessage> DecodeInboundPayload(const std::string& payload_b64);
  static Roe<std::string> DecodePlaintextText(const std::string& payload_b64);
};

} // namespace pbr
