#include "lib/voice/VoiceAudioSession.h"

#include "lib/media/AudioSession.h"

namespace pbr {
namespace VoiceAudioSession {

void ActivateForPlayback() {
  AudioSession::ActivateForVoicePlayback();
}

void ActivateForRecording() {
  AudioSession::ActivateForVoiceRecording();
}

void RestoreForPlayback() {
  AudioSession::ActivateForVoicePlayback();
}

void Release() {
  AudioSession::ReleaseVoice();
}

} // namespace VoiceAudioSession
} // namespace pbr
