#pragma once

#include <cstddef>

namespace pbr {

/** What happens to the temporary file of a voice recording once its send has finished (or not). */
enum class VoiceFileFate {
  Delete,       // sent, or staged (encrypted, sealed under the profile key): the staged copy is the retry path
  KeepForRetry, // nothing was staged (the send was refused or the file could not be prepared): the file is the only copy
};

/**
 * `staged_before` / `staged_after`: how many unfinished sends the chat held before the attempt and after it.
 * A failed send that left one more behind is staged; any other failure kept nothing but the recording itself.
 */
inline VoiceFileFate DecideVoiceFileFate(const bool sent, const size_t staged_before, const size_t staged_after) {
  if (sent || staged_after > staged_before) {
    return VoiceFileFate::Delete;
  }
  return VoiceFileFate::KeepForRetry;
}

} // namespace pbr
