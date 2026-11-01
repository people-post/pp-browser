#include "lib/media/VoiceAudioSession.h"

#include "lib/media/CallAudioSession.h"

namespace pbr {
namespace VoiceAudioSession {

void ActivateForPlayback() {
  CallAudioSession::ActivateForVoicePlayback();
}

void ActivateForRecording() {
  CallAudioSession::ActivateForVoiceRecording();
}

void RestoreForPlayback() {
  CallAudioSession::ActivateForVoicePlayback();
}

void Release() {
  CallAudioSession::ReleaseVoice();
}

} // namespace VoiceAudioSession
} // namespace pbr
