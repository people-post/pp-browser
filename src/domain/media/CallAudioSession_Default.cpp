#include "domain/media/CallAudioSession.h"

#include <SDL3/SDL.h>

namespace pbr {
namespace CallAudioSession {

void ActivateForVoipCall() {}
void Deactivate() {}

bool SupportsSpeakerToggle() {
  return false;
}

bool IsSpeakerphoneOn() {
  return false;
}

void SetSpeakerphoneOn(bool /*on*/) {}

int CaptureOpenAttemptCount() {
  return 1;
}

int CaptureOpenRetryDelayMs(int /*attempt_index*/) {
  return 0;
}

int CaptureReopenSettleDelayMs() {
  return 0;
}

void ApplyCaptureAudioHints() {
#if defined(_WIN32)
  // WASAPI AudioCategory_Communications → the OS voice APO (AEC/NS) when the driver has one.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Communications");
#endif
}
void ClearCaptureAudioHints() {
#if defined(_WIN32)
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "");
#endif
}

} // namespace CallAudioSession
} // namespace pbr
