#include "lib/media/AudioSession.h"

namespace pbr {
namespace AudioSession {

void ActivateForVoip() {}
void Deactivate() {}

void CancelPendingDeactivate() {}
void SetSystemManaged(bool /*managed*/) {}
void NotifySystemActivation(bool /*active*/) {}

void ActivateForVoicePlayback() {}
void ActivateForVoiceRecording() {}
void ReleaseVoice() {}

bool SupportsSpeakerToggle() {
  return false;
}

bool IsSpeakerphoneOn() {
  return false;
}

void SetSpeakerphoneOn(bool /*on*/) {}

bool SpeakerToggleNeedsDeviceReopen() {
  return false;
}

int OpusEncoderComplexity() {
  return 8;
}

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

} // namespace AudioSession
} // namespace pbr
