#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

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
 * A call answered on the phone's call screen (CallKit): the system activates the session itself and tells
 * us (NotifySystemActivation). While managed, ActivateForVoip configures the session but waits for that
 * activation (bounded) instead of activating, and Deactivate leaves the release to the system. No-ops
 * where there is no such call screen.
 */
void SetSystemManaged(bool managed);
void NotifySystemActivation(bool active);

/**
 * The OS took the live session away and gave it back (a call screen, Siri, an alarm; or the media services were
 * reset): every handler added here runs once ActivateForVoip has taken the session again, so each audio user can
 * reopen its devices. There are several (each MediaEngine: a call's, a broadcast's two; one not capturing ignores
 * it), so each adds its own. A single handler that each new engine replaced ended up on a broadcast engine, and the
 * call's audio never came back. Fires only while a live session is active, and only where the OS interrupts the
 * session (iOS's interruptions, Android's loss of audio focus); elsewhere the handlers are kept and never run.
 *
 * Handlers run under the registry's lock: keep them short, and do not add or remove from inside one. Remove waits
 * for a running handler, so its owner may go once Remove returns.
 */
using InterruptionHandlerId = uint64_t;
InterruptionHandlerId AddInterruptionEndedHandler(std::function<void()> fn);
void RemoveInterruptionEndedHandler(InterruptionHandlerId id);
/** The platform's session took the session back after an interruption: runs every handler, in the order added. */
void NotifyInterruptionEnded();

/**
 * The OS took the live session away (a phone call, Siri, an alarm, another app's call): the microphone gives silence
 * or nothing until NotifyInterruptionEnded. Meanwhile a starved capture is not a broken device (MediaEngine does
 * not count it toward giving up on voice processing), and a call tells its peer it is on hold.
 */
void NotifyInterruptionBegan();
/** Between NotifyInterruptionBegan and NotifyInterruptionEnded (or the session's end). Any thread. */
bool IsInterrupted();
/** The live session ended (the platform's Deactivate): an interruption it was under is over with it. */
void ClearInterruption();

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
