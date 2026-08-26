#pragma once

#include "base/messaging/EmojiKey.h"

#include <string>

namespace pbr {

inline constexpr const char* kAnnotationTypeReaction = "reaction";
inline constexpr const char* kAnnotationTypeReactionClear = "reaction_clear";

inline std::string BuildReactionPayloadJson(const std::string& annotation_type,
                                            const std::string& target_message_id, const std::string& emoji) {
  // Keep JSON minimal and stable for ChatPayloadCodec::DecodeAnnotationJson.
  std::string escaped;
  escaped.reserve(emoji.size());
  for (char ch : emoji) {
    if (ch == '\\' || ch == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return std::string("{\"annotation_type\":\"") + annotation_type + "\",\"target_message_id\":\"" +
         target_message_id + "\",\"value\":\"" + escaped + "\"}";
}

inline bool IsReactionAnnotationType(const std::string& annotation_type) {
  return annotation_type == kAnnotationTypeReaction || annotation_type == kAnnotationTypeReactionClear;
}

} // namespace pbr
