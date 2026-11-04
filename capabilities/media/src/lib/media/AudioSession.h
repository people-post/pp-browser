#pragma once

#include <chrono>

namespace pbr {

/**
 * The platform's one audio session (the phone's): live two-way (VoIP) audio — a call — takes it with
 * ActivateForVoip; short holders (a ringback tone) and voice clips share it under its rules.
 * Speakerphone toggle is meaningful on phone-like devices (Android / iOS); desktop stubs return
 * SupportsSpeakerToggle() == false. Default route is earpiece; speakerphone is opt-in per live session,
 * chosen any time it is on screen.
 */
namespace AudioSession {

/** Takes the session for live VoIP audio and applies the speaker choice (IsSpeakerphoneOn). */
void ActivateForVoip();
/**
 * Releases the session and puts the OS route back on the earpiece. The speaker choice survives: a media
 * rebuild mid-session (failover, retry) activates again with it. Whoever ends the live session clears the
 * choice (SetSpeakerphoneOn(false)).
 */
void Deactivate();
/**
 * An audio user is about to start I/O without ActivateForVoip (ringtone / ringback open their
 * speaker first): drop a pending delayed Deactivate retry so it cannot stop that I/O. No-op where
 * Deactivate does not retry.
 */
void CancelPendingDeactivate();

/**
 * A short holder of the session that is not live media — a ringback tone — holds it while it has it activated.
 * Live media waits for those holds to end before activating its own (WaitUntilToneHoldsReleased), so a tone
 * releasing late cannot deactivate a session that just started.
 */
void BeginToneHold();
void EndToneHold();
bool ToneHoldsSession();
/** Wait until no tone holds the session, at most `timeout`. */
void WaitUntilToneHoldsReleased(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

/** True when the OS exposes earpiece vs loudspeaker routing. */
bool SupportsSpeakerToggle();
/** The live session's speaker choice. */
bool IsSpeakerphoneOn();
/**
 * Sets the speaker choice: routed now when the session is active (a tone or live media), else when
 * ActivateForVoip next takes it — so it may be set before live media starts (a call ringing out).
 */
void SetSpeakerphoneOn(bool on);
/** True when a speaker-route change needs the SDL devices closed and reopened (Android AudioRecord goes silent otherwise). */
bool SpeakerToggleNeedsDeviceReopen();

/**
 * Voice clips (recording and playing one; not live media). They go through this file so the phone's one audio
 * session has one coordinator: nothing here runs while live media (or a tone) holds the session, an activation
 * cancels a pending delayed Deactivate like CancelPendingDeactivate, and the release retries when the system says
 * the session is busy and gives up if live media has taken it meanwhile. Everywhere without a session to manage
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

} // namespace AudioSession

} // namespace pbr
