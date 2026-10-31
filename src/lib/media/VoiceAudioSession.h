#pragma once

namespace pbr {

/**
 * The phone's audio session for voice messages, kept apart from `CallAudioSession` (which is for
 * calls and is not touched here). Call each function AFTER the SDL device has been opened: SDL
 * rewrites the iOS audio session when it opens a device.
 *
 *  - Playback: the `playback` category on the loudspeaker, so a voice message is heard with the
 *    mute switch on. Android needs nothing: SDL opens playback with the media role, so it plays on
 *    the speaker at media volume.
 *  - Recording: a record-capable category; the OS asks for the microphone as usual.
 *
 * `Release` hands the session back (other apps' audio resumes). Desktop builds do nothing.
 */
namespace VoiceAudioSession {

void ActivateForPlayback();
void ActivateForRecording();
/** After recording the session goes back to the playback setup, so a message just recorded can be played at once. */
void RestoreForPlayback();
void Release();

} // namespace VoiceAudioSession

} // namespace pbr
