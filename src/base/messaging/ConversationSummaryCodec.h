#pragma once

#include "base/ai/conversation/ConversationTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>

namespace pbr {

/** JSON codec for thread.db memory key `summary` (D070). */
class ConversationSummaryCodec {
public:
  static constexpr const char* kSummaryKey = "summary";
  static constexpr int kSchemaVersion = 1;

  static Roe<std::string> Encode(const ConversationSummary& summary);
  static Roe<ConversationSummary> Decode(const std::string& json);
};

} // namespace pbr
