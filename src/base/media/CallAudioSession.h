#pragma once

namespace pbr {

/**
 * Platform audio session for in-call capture/playback.
 * Speakerphone toggle is meaningful on phone-like devices (Android / iOS);
 * desktop stubs return SupportsSpeakerToggle() == false.
 * Default route is earpiece; speakerphone is opt-in per call.
 */
namespace CallAudioSession {

void ActivateForVoipCall();
void Deactivate();

/** True when the OS exposes earpiece vs loudspeaker routing. */
bool SupportsSpeakerToggle();
bool IsSpeakerphoneOn();
void SetSpeakerphoneOn(bool on);

} // namespace CallAudioSession

} // namespace pbr
