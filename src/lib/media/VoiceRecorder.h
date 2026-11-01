#pragma once

#include "common/Error.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Holder names a voice message takes on the media device arbiter. */
inline constexpr const char* kVoiceRecorderHolder = "voice-recorder";
inline constexpr const char* kVoicePlayerHolder = "voice-player";

/** Where the recorder gets 48 kHz mono s16 PCM. Real code: the microphone lease; tests: a script. */
class IVoiceCaptureSource {
public:
  /** Error::code of an Open() that failed because something else holds the microphone (a call). */
  static constexpr int32_t kOpenBusy = 1;
  /** Error::code of an Open() that found no usable microphone. */
  static constexpr int32_t kOpenNoDevice = 2;

  virtual ~IVoiceCaptureSource() = default;
  /** Blocking (the OS may be asking the person). `still_wanted` turns false when the recording was stopped meanwhile. */
  virtual Roe<void> Open(const std::function<bool()>& still_wanted) = 0;
  /** Copy up to `bytes` of captured PCM; 0 = nothing yet, < 0 = the device failed. */
  virtual int Read(void* dst, int bytes) = 0;
  /** Who now needs the device the recording is using (an incoming call rings), or empty. */
  virtual std::string InterruptedBy() { return {}; }
  virtual void Close() = 0;
};

/** The microphone through the arbiter, exclusive with calls. */
std::unique_ptr<IVoiceCaptureSource> CreateArbiterVoiceCaptureSource(
    MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());

enum class VoiceEnd {
  Sent,        // the person stopped it (release / Send): the file is ready
  LimitReached,// the length limit passed in was reached: the file is ready
  Interrupted, // a call needs the device: what was said so far is ready
  TooShort,    // under the minimum: no file
  Cancelled,   // no file
  Busy,        // the microphone is in use (a call): no file
  NoDevice,    // no microphone could be opened: no file
  Failed,      // the device gave up or the file could not be written: no file
};

/** True for the ends that leave a file to send. */
inline bool VoiceEndHasFile(const VoiceEnd end) {
  return end == VoiceEnd::Sent || end == VoiceEnd::LimitReached || end == VoiceEnd::Interrupted;
}

struct VoiceRecording {
  VoiceEnd end = VoiceEnd::Failed;
  /** The finished file (empty unless VoiceEndHasFile). Named voice-<ms>.ogg. */
  std::string path;
  int64_t duration_ms = 0;
  std::string error;
};

struct VoiceRecorderConfig {
  /** The longest recording; the feature layer passes AttachmentLimits::voice_max_seconds. */
  int max_seconds = 120;
  /** Shorter than this is not sent. */
  int min_ms = 1000;
  /** Where the finished file goes; created when needed. */
  std::string out_dir;
  int bitrate_bps = 24000;
  /** A source that never delivers a sample for this long failed. */
  int no_data_timeout_ms = 4000;
};

/**
 * One voice recording at a time: opens the source on its own thread, encodes 20 ms Opus frames and, when
 * it ends, writes one Ogg file. Nothing is written to disk before the end and a cancelled recording
 * leaves nothing. `on_finished` runs once on the recorder's thread; `on_progress` about ten times a second.
 * Nothing waits for that thread on the caller's: it owns what it needs (so destroying the recorder while
 * the OS is still asking about the microphone is safe) and finishes by itself.
 */
class VoiceRecorder {
public:
  using SourceFactory = std::function<std::unique_ptr<IVoiceCaptureSource>()>;
  struct Callbacks {
    std::function<void(int64_t elapsed_ms)> on_progress;
    std::function<void(VoiceRecording)> on_finished;
  };

  VoiceRecorder(SourceFactory factory, Callbacks callbacks);
  ~VoiceRecorder();
  VoiceRecorder(const VoiceRecorder&) = delete;
  VoiceRecorder& operator=(const VoiceRecorder&) = delete;

  /** Begin; false when one is already running. Returns at once (the device opens on the recorder's thread). */
  bool Start(const VoiceRecorderConfig& config);
  /** End and keep it (release / Send). The result arrives through `on_finished`. */
  void Finish();
  /** End and throw it away. */
  void Cancel();
  bool IsRecording() const { return shared_->running.load(); }

private:
  enum class Request : int { None, Finish, Cancel };
  struct Shared {
    SourceFactory factory;
    Callbacks callbacks;
    std::atomic<bool> running{false};
  };
  static void Run(std::shared_ptr<Shared> shared, VoiceRecorderConfig config, std::shared_ptr<std::atomic<int>> request);

  std::shared_ptr<Shared> shared_;
  std::mutex mutex_;
  std::shared_ptr<std::atomic<int>> request_;
};

} // namespace pbr
