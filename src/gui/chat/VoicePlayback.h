#pragma once

#include "feature/conversations/InboxController.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/voice/AudioFilePlayer.h"
#include "lib/voice/VoicePlayer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {
class Element;
}

namespace pbr {

/** What the voice bubbles' playback needs from the chat screen. All called on the UI thread. */
struct VoicePlaybackPorts {
  /** The voice limit: a clip longer than it is not played. */
  std::function<int()> max_seconds;
  /** The plaintext file of a voice message (made off the UI thread); `done(nullopt)` when it is not there. */
  std::function<void(const std::string& message_id, std::function<void(std::optional<std::string> path)> done)> resolve;
  /** A bubble part by element id (null when the message is not on screen). */
  std::function<ui::Element*(const std::string& element_id)> element;
  /** A line to tell the person (a toast). */
  std::function<void(const std::string& text)> say;
  /** A call is ringing here or going on: the call stack's own state. Nothing plays then. */
  std::function<bool()> call_live;
  /** Default: the app's UI queue. */
  std::function<void(std::function<void()>)> post_ui;
  /** The system's player for sound files sent as attachments. Default: CreatePlatformAudioFilePlayer (null: none). */
  std::function<std::unique_ptr<IAudioFilePlayer>()> make_file_player;
  /** Puts the audio session in playback before a sound file plays / hands it back after. Default: VoiceAudioSession. */
  std::function<void(bool playing)> file_session;
};

/**
 * Plays the voice bubbles: one at a time, never during a call, stopped by an incoming call. A tap on the
 * playing bubble pauses it, on a paused one resumes it, on another one starts that one. The bubble the clip
 * belongs to is updated in place (fill width, time, play / pause icon), not redrawn.
 */
class VoicePlayback {
public:
  VoicePlayback(VoicePlaybackPorts ports, MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());
  ~VoicePlayback();

  /** The play / pause button of `message_id` was tapped. */
  void Toggle(const std::string& message_id);
  /**
   * The same for a sound file sent as an attachment (an MP3): played by the system's player in place, one at a time
   * with the voice messages, on the loudspeaker. Only when PlaysFiles().
   */
  void ToggleFile(const std::string& message_id);
  /** This platform plays sound files in the app (else they open in the system's app). */
  bool PlaysFiles() const { return file_player_ != nullptr; }
  /** What the bubble of `message_id` shows now (for a redraw of the transcript). */
  VoiceBubbleState State(const std::string& message_id) const;
  /** Stops the clip (a tap that is still waiting for its file is forgotten too). Never waits. */
  void Stop();
  /** Stop, then run `then` once the clip has let go of the speaker (at once when nothing plays). */
  void StopThen(std::function<void()> then);
  /** A call is live: stop at once. */
  void OnCallLive();
  /** A clip plays or a tap is waiting for its file. */
  bool Active() const { return !current_.empty() || !resolving_.empty(); }
  void Shutdown();

private:
  void ToggleImpl(const std::string& message_id, bool file);
  void Start(const std::string& message_id, const std::string& path, bool file);
  /** Ends the sound file playing (no report comes from the system's player after a Stop). */
  void StopFile();
  void OnProgress(const std::string& message_id, uint64_t serial, int64_t position_ms, int64_t duration_ms);
  void OnEnded(const std::string& message_id, uint64_t serial, VoicePlayEnd end);
  /** `idle_text`: what the time line says once the bubble is not playing (its length), when known. */
  void Update(const std::string& message_id, const std::string& idle_text = {});
  void Say(const std::string& text) const;

  VoicePlaybackPorts ports_;
  MediaDeviceArbiter& devices_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<VoicePlayer> player_;
  std::unique_ptr<IAudioFilePlayer> file_player_;
  bool current_is_file_ = false; // `current_` plays through `file_player_`
  uint64_t file_serial_ = 0;     // serials of file plays: kept apart from the voice player's
  std::string current_;   // the clip started or being started
  std::string resolving_; // a tap is waiting for its file
  uint64_t resolve_generation_ = 0;
  uint64_t serial_ = 0;   // the Play() whose events count
  bool stopped_by_user_ = false;
  std::vector<std::function<void()>> after_stop_;
  bool paused_ = false;
  int64_t position_ms_ = 0;
  int64_t duration_ms_ = 0;
};

} // namespace pbr
