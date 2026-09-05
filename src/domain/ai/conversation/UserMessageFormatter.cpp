#include "domain/ai/conversation/UserMessageFormatter.h"

namespace pbr {

std::string FormatUserContentForLlm(const TranscriptEntry& entry) {
  if (!entry.user_payload || entry.user_payload->empty()) {
    return entry.user_text;
  }
  return entry.user_text +
         "\n\nStructured action context (supports the user message above; user_text is primary):\n```json\n" +
         *entry.user_payload + "\n```";
}

} // namespace pbr
