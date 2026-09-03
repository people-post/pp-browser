#pragma once

#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Plaintext envelope for one message row before transcript AEAD (v1). */
struct TranscriptBodyPlaintext {
  std::vector<uint8_t> chat_payload;
  std::string text;
  std::string payload_json;
  std::optional<std::string> content_rml;
  std::vector<TranscriptChatAction> chat_actions;
};

class TranscriptBodyCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const TranscriptBodyPlaintext& body);
  static Roe<TranscriptBodyPlaintext> Decode(const std::vector<uint8_t>& bytes);
  static Roe<TranscriptBodyPlaintext> FromMessage(const ThreadMessage& message);
  static Roe<void> ApplyToMessage(const TranscriptBodyPlaintext& body, ThreadMessage& message);
};

} // namespace pbr
