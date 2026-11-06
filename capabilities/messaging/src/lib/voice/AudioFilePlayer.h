#pragma once

#include "common/Error.h"
#include "lib/voice/VoicePlayer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pbr {

/** What a sound file's playback reports. Called on any thread; the caller moves them to its own. */
struct AudioFilePlayerCallbacks {
  /** About ten times a second while it plays. */
  std::function<void(int64_t position_ms, int64_t duration_ms)> on_progress;
  /** Once, when the file played to its end (Finished) or the system could not decode it (Unplayable). Not after Stop. */
  std::function<void(VoicePlayEnd end)> on_ended;
};

/**
 * A sound file sent as an attachment (MP3, M4A, WAV, AAC ...), played by the system's own player in the app, on the
 * loudspeaker like a voice message (the caller sets the audio session). One file at a time: Play replaces the one
 * playing, which then reports nothing. Called on the UI thread.
 */
class IAudioFilePlayer {
public:
  virtual ~IAudioFilePlayer() = default;
  /** Starts `path` from the beginning; false when it cannot be played (no such file, a type the system cannot read). */
  virtual bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) = 0;
  virtual void Pause() = 0;
  virtual void Resume() = 0;
  /** Jumps to `seconds` from the start (clamped to the file), playing or paused; progress carries on from there. An
   *  error when nothing plays or the system refuses. */
  virtual Roe<void> Seek(double seconds) = 0;
  /** Ends it at once; no `on_ended` follows. */
  virtual void Stop() = 0;
};

/**
 * Apple: AVAudioPlayer; Android: MediaPlayer. Null where the app has none (the file opens in the system's app).
 * One sound file plays in the whole app: a Play on one player (the chat's, the storage page's) stops the one playing
 * on another, which reports `on_ended(Interrupted)` (its owner resets its controls but leaves the audio session).
 */
std::unique_ptr<IAudioFilePlayer> CreatePlatformAudioFilePlayer();
/** The platform's player itself (AudioFilePlayer_<platform>); CreatePlatformAudioFilePlayer wraps it. */
std::unique_ptr<IAudioFilePlayer> CreateSystemAudioFilePlayer();
/** Wraps `inner` (a test double, the system's player) in the one-at-a-time rule above; null stays null. */
std::unique_ptr<IAudioFilePlayer> MakeExclusiveAudioFilePlayer(std::unique_ptr<IAudioFilePlayer> inner);

/** A sound file by its type or name: an audio mime type, or an mp3 / m4a / wav / aac / ogg name (a voice message
 *  has its own bubble, decided first). */
bool IsAudioFileAttachment(const std::string& mime, const std::string& filename);

} // namespace pbr
