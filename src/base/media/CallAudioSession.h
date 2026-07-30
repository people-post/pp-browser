#pragma once

namespace pbr {

/** Platform audio session for in-call capture/playback (iOS AVAudioSession; no-op elsewhere). */
namespace CallAudioSession {

void ActivateForVoipCall();
void Deactivate();

} // namespace CallAudioSession

} // namespace pbr
