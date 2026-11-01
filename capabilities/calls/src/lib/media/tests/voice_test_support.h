#pragma once

#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VoicePlayer.h"
#include "lib/media/VoiceRecorder.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr::voice_test {

using namespace std::chrono_literals;

inline bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

/** A unique empty directory under the system temp area, removed by the destructor. */
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static std::atomic<int> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("pp-voice-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  size_t FileCount() const {
    size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(path, ec); !ec && it != std::filesystem::end(it);
         it.increment(ec)) {
      if (it->is_regular_file()) {
        ++n;
      }
    }
    return n;
  }
};

/** A tone of `ms` milliseconds, 48 kHz mono s16. */
inline std::vector<int16_t> Tone(const int ms, const double hz = 440.0) {
  std::vector<int16_t> pcm(static_cast<size_t>(ms) * 48);
  for (size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / 48000.0));
  }
  return pcm;
}

/** Hands out a scripted amount of PCM and then silence (Read gives 0). Never opens a device. */
class ScriptedSource final : public IVoiceCaptureSource {
public:
  explicit ScriptedSource(std::vector<int16_t> pcm, std::string open_failure = {}, int open_code = 0,
                          std::atomic<bool>* closed_flag = nullptr)
      : pcm_(std::move(pcm)), open_failure_(std::move(open_failure)), open_code_(open_code), closed_flag_(closed_flag) {}

  Roe<void> Open(const std::function<bool()>& /*still_wanted*/) override {
    opened = true;
    if (!open_failure_.empty()) {
      return Error(open_code_, open_failure_);
    }
    return {};
  }
  int Read(void* dst, const int bytes) override {
    const size_t left = (pcm_.size() - pos_) * sizeof(int16_t);
    const int n = static_cast<int>(std::min<size_t>(left, static_cast<size_t>(bytes)));
    if (n > 0) {
      std::memcpy(dst, reinterpret_cast<const uint8_t*>(pcm_.data()) + pos_ * sizeof(int16_t), static_cast<size_t>(n));
      pos_ += static_cast<size_t>(n) / sizeof(int16_t);
    }
    return n;
  }
  std::string InterruptedBy() override { return interrupted_by.load() ? "ringtone" : std::string(); }
  void Close() override {
    if (closed_flag_) {
      *closed_flag_ = true;
    }
  }

  std::atomic<bool> interrupted_by{false};
  bool opened = false;

private:
  std::vector<int16_t> pcm_;
  size_t pos_ = 0;
  std::string open_failure_;
  int open_code_;
  std::atomic<bool>* closed_flag_;
};

/** Collects what a recorder reports. */
struct RecorderProbe {
  std::mutex mutex;
  bool finished = false;
  VoiceRecording recording;
  std::atomic<int64_t> last_progress_ms{0};

  VoiceRecorder::Callbacks Callbacks() {
    VoiceRecorder::Callbacks cb;
    cb.on_progress = [this](int64_t ms) { last_progress_ms = ms; };
    cb.on_finished = [this](VoiceRecording r) {
      std::lock_guard lock(mutex);
      recording = std::move(r);
      finished = true;
    };
    return cb;
  }
  bool Done() {
    std::lock_guard lock(mutex);
    return finished;
  }
  VoiceRecording Get() {
    std::lock_guard lock(mutex);
    return recording;
  }
};

/**
 * A speaker for the player: keeps what was written. Written bytes queue up; with `draining` every Queued()
 * call plays 20 ms of them (about twice real time with the player's polling), otherwise the queue stays full.
 */
class RecordingSink final : public IVoicePlaybackSink {
public:
  struct State {
    std::mutex mutex;
    std::vector<int16_t> written;
    std::atomic<int> queued{0};
    std::atomic<bool> draining{true};
    std::atomic<bool> interrupted{false};
    std::atomic<bool> open_busy{false};
    std::atomic<int> opens{0};
    std::atomic<int> closes{0};
    std::atomic<int> clears{0};
    size_t Written() {
      std::lock_guard lock(mutex);
      return written.size();
    }
  };
  explicit RecordingSink(std::shared_ptr<State> state) : state_(std::move(state)) {}

  Roe<void> Open(const std::function<bool()>&) override {
    if (state_->open_busy.load()) {
      return Error(kOpenBusy, "in use by call");
    }
    ++state_->opens;
    return {};
  }
  int Queued() const override {
    if (state_->draining.load()) {
      const int now = state_->queued.load();
      state_->queued.store(std::max(0, now - 1920));
    }
    return state_->queued.load();
  }
  bool Write(const void* src, const int bytes) override {
    std::lock_guard lock(state_->mutex);
    const auto* p = static_cast<const int16_t*>(src);
    state_->written.insert(state_->written.end(), p, p + bytes / 2);
    state_->queued += bytes;
    return true;
  }
  void Clear() override {
    state_->queued = 0;
    ++state_->clears;
  }
  std::string InterruptedBy() override { return state_->interrupted.load() ? "ringtone" : std::string(); }
  void Close() override { ++state_->closes; }

private:
  std::shared_ptr<State> state_;
};

/** Backend for the arbiter with no hardware: a mic that gives silence and a speaker that swallows. */
class FakeBackend final : public IMediaDeviceBackend {
public:
  class Mic final : public IAudioEndpoint {
  public:
    int Read(void* dst, int bytes) override {
      std::memset(dst, 0, static_cast<size_t>(bytes));
      return bytes;
    }
    bool Write(const void*, int) override { return false; }
    int Queued() const override { return 0; }
    void Clear() override {}
  };
  class Speaker final : public IAudioEndpoint {
  public:
    int Read(void*, int) override { return 0; }
    bool Write(const void*, int) override { return true; }
    int Queued() const override { return 0; }
    void Clear() override {}
  };
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat&,
                                            const std::function<bool()>&, std::string*) override {
    if (kind == MediaDeviceKind::Mic) {
      return std::make_unique<Mic>();
    }
    return std::make_unique<Speaker>();
  }
};

} // namespace pbr::voice_test
