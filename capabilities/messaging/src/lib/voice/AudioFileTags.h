#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** What a sound file says about itself in its own tags (ID3 in an MP3, the iTunes atoms of an M4A). */
struct AudioFileTags {
  std::string title;  // "" when the file names none
  std::string artist; // "" when the file names none
  int64_t duration_ms = 0; // 0 when unknown
  /** The embedded cover picture as stored (JPEG or PNG bytes; ID3 APIC), empty when there is none. */
  std::vector<uint8_t> picture;
};

/** A cover larger than this is left out (a card shows it 48dp wide). */
inline constexpr size_t kMaxAudioCoverBytes = 8u * 1024u * 1024u;

/**
 * Reads the tags of the sound file at `path` with the system's own reader: AVAsset on macOS / iOS,
 * MediaMetadataRetriever on Android, nothing elsewhere (AudioFileTags_<platform>). For this device's card only: what
 * it returns is never sent. Blocking (it parses the file): off the UI thread. nullopt when nothing could be read.
 */
std::optional<AudioFileTags> ReadAudioFileTags(const std::string& path);

} // namespace pbr
