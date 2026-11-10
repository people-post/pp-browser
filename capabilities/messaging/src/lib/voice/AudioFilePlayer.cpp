#include "lib/voice/AudioFilePlayer.h"

#include <algorithm>
#include <cctype>

namespace pbr {

namespace {

class ExclusiveAudioFilePlayer;
ExclusiveAudioFilePlayer* g_playing = nullptr; // UI thread only

/** The one-at-a-time rule across every sound-file player in the app (see CreatePlatformAudioFilePlayer). */
class ExclusiveAudioFilePlayer final : public IAudioFilePlayer {
public:
  explicit ExclusiveAudioFilePlayer(std::unique_ptr<IAudioFilePlayer> inner) : inner_(std::move(inner)) {}
  ~ExclusiveAudioFilePlayer() override { Stop(); }

  bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) override {
    if (g_playing != nullptr && g_playing != this) {
      g_playing->TakenOver();
    }
    // Kept to tell this owner when another player takes over (a stale copy after a natural end is harmless: the
    // owner ignores an end for a play it no longer has).
    on_ended_ = callbacks.on_ended;
    if (!inner_->Play(path, std::move(callbacks))) {
      on_ended_ = nullptr;
      return false;
    }
    g_playing = this;
    return true;
  }
  // Only the one playing reaches the system's player: on Android that player is one for the whole app, so an idle
  // player's Stop (the storage page closing, the chat shutting down) must not end another one's file.
  void Pause() override {
    if (g_playing == this) {
      inner_->Pause();
    }
  }
  void Resume() override {
    if (g_playing == this) {
      inner_->Resume();
    }
  }
  Roe<void> Seek(double seconds) override {
    if (g_playing != this) {
      return Error("no sound file playing here");
    }
    return inner_->Seek(seconds);
  }
  void Stop() override {
    on_ended_ = nullptr;
    if (g_playing == this) {
      inner_->Stop();
      g_playing = nullptr;
    }
  }

private:
  void TakenOver() {
    auto on_ended = std::move(on_ended_);
    Stop();
    if (on_ended) {
      on_ended(VoicePlayEnd::Interrupted);
    }
  }

  std::unique_ptr<IAudioFilePlayer> inner_;
  std::function<void(VoicePlayEnd)> on_ended_;
};

} // namespace

std::unique_ptr<IAudioFilePlayer> MakeExclusiveAudioFilePlayer(std::unique_ptr<IAudioFilePlayer> inner) {
  if (!inner) {
    return nullptr;
  }
  return std::make_unique<ExclusiveAudioFilePlayer>(std::move(inner));
}

std::unique_ptr<IAudioFilePlayer> CreatePlatformAudioFilePlayer() {
  return MakeExclusiveAudioFilePlayer(CreateSystemAudioFilePlayer());
}

bool IsAudioFileAttachment(const std::string& mime, const std::string& filename) {
  if (mime.rfind("audio/", 0) == 0) {
    return true;
  }
  // Not std::filesystem::path: on Windows a UTF-8 name outside the ANSI code page throws there.
  const size_t dot = filename.rfind('.');
  std::string ext = dot == std::string::npos ? std::string() : filename.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".mp3" || ext == ".m4a" || ext == ".wav" || ext == ".aac" || ext == ".ogg";
}

} // namespace pbr
