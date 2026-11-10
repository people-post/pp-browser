#pragma once

#include <optional>
#include <string>

namespace pbr {

struct TranscriptChatAction {
  std::string label;
  std::string message;
  std::optional<std::string> payload;
};

} // namespace pbr
