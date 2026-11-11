#include "lib/media/SdlAudioReset.h"

namespace pbr {

bool SdlAudioResetSupported() {
  return false;
}

AudioResetResult ResetSdlAudioOnMainThread(std::chrono::milliseconds /*budget*/, std::function<bool()> /*safe_now*/,
                                           std::string* error) {
  if (error) {
    *error = "unsupported";
  }
  return AudioResetResult::Unsupported;
}

} // namespace pbr
