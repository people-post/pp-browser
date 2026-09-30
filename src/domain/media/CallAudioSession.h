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
/**
 * An audio user is about to start I/O without ActivateForVoipCall (ringtone / ringback open their
 * speaker first): drop a pending delayed Deactivate retry so it cannot stop that I/O. No-op where
 * Deactivate does not retry.
 */
void CancelPendingDeactivate();

/** True when the OS exposes earpiece vs loudspeaker routing. */
bool SupportsSpeakerToggle();
bool IsSpeakerphoneOn();
void SetSpeakerphoneOn(bool on);
/** True when a speaker-route change needs the SDL devices closed and reopened (Android AudioRecord goes silent otherwise). */
bool SpeakerToggleNeedsDeviceReopen();

/** Opus encoder complexity (0–10): 5 on phones (CPU / battery), 8 on desktop. */
int OpusEncoderComplexity();

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
