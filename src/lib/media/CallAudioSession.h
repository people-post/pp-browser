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

/**
 * Voice messages (recording and playing a clip; not calls). They go through this file so the phone's one audio
 * session has one coordinator: nothing here runs while a call (or ringback) holds the session, an activation
 * cancels a pending delayed Deactivate like CancelPendingDeactivate, and the release retries when the system says
 * the session is busy and gives up if a call has taken it meanwhile. Everywhere without a session to manage
 * these do nothing. Call after the SDL device has been opened (SDL rewrites the iOS session when it opens one).
 */
void ActivateForVoicePlayback();
void ActivateForVoiceRecording();
void ReleaseVoice();

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
