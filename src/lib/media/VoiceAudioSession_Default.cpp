#include "lib/media/VoiceAudioSession.h"

// Desktop and Android: SDL already opens playback on the speaker with the media role, and capture needs no
// session switch. (iOS has VoiceAudioSession_Ios.mm.)

namespace pbr {
namespace VoiceAudioSession {

void ActivateForPlayback() {}
void ActivateForRecording() {}
void RestoreForPlayback() {}
void Release() {}

} // namespace VoiceAudioSession
} // namespace pbr
