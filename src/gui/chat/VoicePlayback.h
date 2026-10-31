#pragma once

#include "feature/conversations/InboxController.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VoicePlayer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
  /** What the bubble of `message_id` shows now (for a redraw of the transcript). */
  VoiceBubbleState State(const std::string& message_id) const;
  void Stop();
  void Shutdown();

private:
  void Start(const std::string& message_id, const std::string& path);
  void OnProgress(const std::string& message_id, int64_t position_ms, int64_t duration_ms);
  void OnEnded(const std::string& message_id, VoicePlayEnd end);
  /** `idle_text`: what the time line says once the bubble is not playing (its length), when known. */
  void Update(const std::string& message_id, const std::string& idle_text = {});
  void Say(const std::string& text) const;

  VoicePlaybackPorts ports_;
  MediaDeviceArbiter& devices_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<VoicePlayer> player_;
  std::string current_;   // the clip started or being started
  std::string resolving_; // a tap is waiting for its file
  bool paused_ = false;
  int64_t position_ms_ = 0;
  int64_t duration_ms_ = 0;
};

} // namespace pbr
