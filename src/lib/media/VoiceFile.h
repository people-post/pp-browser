#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/**
 * How a voice message looks on the wire. It is an ordinary attachment (no wire change): an Ogg Opus
 * file named `voice-<duration ms>.ogg` with mime `audio/ogg`. The name is only a display convention,
 * so a bubble is drawn for a file that matches it and is small; anything else is a plain file row.
 */
inline constexpr const char* kVoiceMime = "audio/ogg";
/** A declared size above this (decimal MB, as everywhere on screen) is never drawn as a voice bubble. */
inline constexpr uint64_t kVoiceBubbleMaxBytes = 1000ULL * 1000ULL;

/** `voice-4200.ogg` for 4.2 seconds. */
std::string VoiceFileName(int64_t duration_ms);

/** The duration in a voice file name, or nothing when the name is not exactly `voice-<digits>.ogg`. */
std::optional<int64_t> ParseVoiceFileName(const std::string& filename);

struct VoiceBubbleView {
  bool bubble = false;
  /** Shown length, never above the voice limit whatever the name says. */
  int64_t duration_ms = 0;
};

/** Bubble or file row for a received attachment, from what its sender declared. */
VoiceBubbleView DecideVoiceBubble(const std::string& mime, const std::string& filename, uint64_t declared_bytes,
                                  int voice_max_seconds);

/** "0:07", "1:05": a duration as a person reads it (rounded up to a whole second, at least 0:01). */
std::string FormatVoiceDuration(int64_t duration_ms);

} // namespace pbr
