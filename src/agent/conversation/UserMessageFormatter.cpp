#include "agent/conversation/UserMessageFormatter.h"

namespace pbr {

std::string FormatUserContentForLlm(const TranscriptEntry& entry) {
  if (!entry.user_payload || entry.user_payload->empty()) {
    return entry.user_text;
  }
  return entry.user_text + "\n\n```json\n" + *entry.user_payload + "\n```";
}

} // namespace pbr
