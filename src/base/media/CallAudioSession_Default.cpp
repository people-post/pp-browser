#include "base/media/CallAudioSession.h"

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

void ApplyCaptureAudioHints() {}
void ClearCaptureAudioHints() {}

} // namespace CallAudioSession
} // namespace pbr
