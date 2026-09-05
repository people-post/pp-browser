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

/** SDL capture-open attempts (Android OEM AAudio races; 1 elsewhere). */
int CaptureOpenAttemptCount();
/** Delay before attempt `index` (0-based). */
int CaptureOpenRetryDelayMs(int attempt_index);
/** Sleep before reopening capture after a speaker-route change. 0 elsewhere. */
int CaptureReopenSettleDelayMs();
/** Platform SDL audio hints around capture (AAudio voice-communication on Android). */
void ApplyCaptureAudioHints();
void ClearCaptureAudioHints();

} // namespace CallAudioSession

} // namespace pbr
