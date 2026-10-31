#pragma once

#include "common/Error.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/OggOpus.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

/** Where decoded 48 kHz mono s16 PCM goes. Real code: the speaker lease; tests: a recording sink. */
class IVoicePlaybackSink {
public:
  static constexpr int32_t kOpenBusy = 1;     // a call (or a recording) has the device
  static constexpr int32_t kOpenNoDevice = 2;

  virtual ~IVoicePlaybackSink() = default;
  virtual Roe<void> Open(const std::function<bool()>& still_wanted) = 0;
  /** Bytes written and not yet played. */
  virtual int Queued() const = 0;
  virtual bool Write(const void* src, int bytes) = 0;
  virtual void Clear() = 0;
  /** Who needs the device now (an incoming call rings), or empty. */
  virtual std::string InterruptedBy() { return {}; }
  virtual void Close() = 0;
};

/** The speaker through the arbiter. Refused while a call, a ringtone or a recording has the device. */
std::unique_ptr<IVoicePlaybackSink> CreateArbiterVoicePlaybackSink(
    MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());

enum class VoicePlayEnd {
  Finished,
  Stopped,     // Stop() or another clip started
  Unplayable,  // not a valid voice file (parsing failed): "cannot play"
  Busy,        // a call has the device: not played
  NoDevice,
  Interrupted, // a call started ringing or began
};

struct VoicePlayerCallbacks {
  /** About ten times a second while playing. */
  std::function<void(const std::string& id, int64_t position_ms, int64_t duration_ms)> on_progress;
  /** Once per Play(), on the player's thread. */
  std::function<void(const std::string& id, VoicePlayEnd end)> on_ended;
};

/**
 * Plays one voice file at a time. The file is read, parsed and decoded only when Play() is called
 * (it came from a stranger and is treated as such: see ParseOggOpus), on the player's own thread.
 * Starting a clip stops the one playing.
 */
class VoicePlayer {
public:
  using SinkFactory = std::function<std::unique_ptr<IVoicePlaybackSink>()>;

  VoicePlayer(SinkFactory factory, VoicePlayerCallbacks callbacks);
  ~VoicePlayer();
  VoicePlayer(const VoicePlayer&) = delete;
  VoicePlayer& operator=(const VoicePlayer&) = delete;

  /** `max_seconds`: the voice limit; a file longer than it (plus a second) is refused as unplayable. */
  void Play(const std::string& id, const std::string& path, int max_seconds);
  void Pause();
  void Resume();
  /** Ends the clip that is playing (its `on_ended` says Stopped); does not wait. */
  void Stop();

  /** The id being played or paused ("" when none). */
  std::string CurrentId() const;
  bool IsPaused() const { return paused_.load(); }

private:
  struct Job {
    std::string id;
    std::string path;
    int max_seconds = 120;
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
  };
  void Run(std::shared_ptr<Job> job);

  SinkFactory factory_;
  VoicePlayerCallbacks callbacks_;
  mutable std::mutex mutex_;
  std::thread thread_;
  std::shared_ptr<Job> job_;
  std::atomic<bool> paused_{false};
};

} // namespace pbr
