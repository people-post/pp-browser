// Desktop Windows / Linux: no tag reader; a sound file's card shows its name and a music-note placeholder.
#include "lib/voice/AudioFileTags.h"

namespace pbr {

std::optional<AudioFileTags> ReadAudioFileTags(const std::string& /*path*/) {
  return std::nullopt;
}

} // namespace pbr
