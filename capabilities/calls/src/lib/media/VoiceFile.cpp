#include "lib/media/VoiceFile.h"

#include <algorithm>
#include <cctype>

namespace pbr {

std::string VoiceFileName(const int64_t duration_ms) {
  return "voice-" + std::to_string(std::max<int64_t>(duration_ms, 0)) + ".ogg";
}

std::optional<int64_t> ParseVoiceFileName(const std::string& filename) {
  static const std::string kPrefix = "voice-";
  static const std::string kSuffix = ".ogg";
  if (filename.size() <= kPrefix.size() + kSuffix.size() ||
      filename.compare(0, kPrefix.size(), kPrefix) != 0 ||
      filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return std::nullopt;
  }
  const std::string digits = filename.substr(kPrefix.size(), filename.size() - kPrefix.size() - kSuffix.size());
  if (digits.empty() || digits.size() > 9) {
    return std::nullopt;
  }
  int64_t value = 0;
  for (const char c : digits) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

VoiceBubbleView DecideVoiceBubble(const std::string& mime, const std::string& filename, const uint64_t declared_bytes,
                                  const int voice_max_seconds) {
  VoiceBubbleView view;
  std::string lowered = mime;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered != kVoiceMime || declared_bytes == 0 || declared_bytes > kVoiceBubbleMaxBytes) {
    return view;
  }
  const auto named = ParseVoiceFileName(filename);
  if (!named) {
    return view;
  }
  view.bubble = true;
  view.duration_ms = std::min<int64_t>(*named, static_cast<int64_t>(std::max(voice_max_seconds, 0)) * 1000);
  return view;
}

std::string FormatVoiceDuration(const int64_t duration_ms) {
  const int64_t seconds = std::max<int64_t>(1, (std::max<int64_t>(duration_ms, 0) + 999) / 1000);
  return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

} // namespace pbr
