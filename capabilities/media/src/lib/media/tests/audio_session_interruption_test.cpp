#include "lib/media/AudioSession.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using namespace std::chrono_literals;

// Every handler added runs when the session comes back; a removed one does not.
TEST(AudioSessionInterruptionTest, EveryAddedHandlerRunsAndARemovedOneDoesNot) {
  int first = 0;
  int second = 0;
  const auto a = AudioSession::AddInterruptionEndedHandler([&first]() { ++first; });
  const auto b = AudioSession::AddInterruptionEndedHandler([&second]() { ++second; });
  ASSERT_NE(a, b);
  AudioSession::NotifyInterruptionEnded();
  EXPECT_EQ(first, 1);
  EXPECT_EQ(second, 1);
  AudioSession::RemoveInterruptionEndedHandler(a);
  AudioSession::NotifyInterruptionEnded();
  EXPECT_EQ(first, 1) << "removed";
  EXPECT_EQ(second, 2);
  AudioSession::RemoveInterruptionEndedHandler(b);
  EXPECT_EQ(AudioSession::AddInterruptionEndedHandler({}), 0u) << "an empty handler is not added";
}

// Interrupted from the OS's take until its end (which also runs the handlers), or until the session ends.
TEST(AudioSessionInterruptionTest, InterruptedFromBeganUntilEndedOrTheSessionsEnd) {
  int reopened = 0;
  const auto id = AudioSession::AddInterruptionEndedHandler([&reopened]() { ++reopened; });
  EXPECT_FALSE(AudioSession::IsInterrupted());
  AudioSession::NotifyInterruptionBegan();
  EXPECT_TRUE(AudioSession::IsInterrupted());
  EXPECT_EQ(reopened, 0) << "nothing reopens while the OS holds the session";
  AudioSession::NotifyInterruptionEnded();
  EXPECT_FALSE(AudioSession::IsInterrupted());
  EXPECT_EQ(reopened, 1);
  AudioSession::NotifyInterruptionBegan();
  AudioSession::ClearInterruption(); // the call ended during it
  EXPECT_FALSE(AudioSession::IsInterrupted()) << "the next call does not start on hold";
  AudioSession::RemoveInterruptionEndedHandler(id);
}

class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

/** Audio devices that count how often they were opened. */
class CountingBackend final : public IMediaDeviceBackend {
public:
  explicit CountingBackend(std::atomic<int>& opens) : opens_(opens) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    opens_.fetch_add(1);
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override { return nullptr; }

private:
  std::atomic<int>& opens_;
};

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return done();
}

// The call's engine reopens its audio when an interruption ends (a phone call, Siri, an alarm), whatever other
// engines exist. The broadcast hub makes two more after the call's: with one handler that each new engine replaced,
// the reopen went to a broadcast engine that was not capturing, and the call's audio never came back (43e9f65ed).
TEST(AudioSessionInterruptionTest, TheCallsEngineReopensItsAudioWithOtherEnginesAround) {
  std::atomic<int> call_opens{0};
  std::atomic<int> other_opens{0};
  MediaDeviceArbiter call_devices(std::make_unique<CountingBackend>(call_opens));
  MediaDeviceArbiter other_devices(std::make_unique<CountingBackend>(other_opens));
  MediaEngine call(call_devices);
  auto started = call.StartDuplex("interrupted-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  ASSERT_TRUE(WaitFor([&] { return call_opens.load() > 0; })) << "the call never opened its audio";
  std::this_thread::sleep_for(100ms);
  const int opened = call_opens.load();

  {
    MediaEngine broadcast_view(other_devices); // built after the call's, as the broadcast hub's are
    MediaEngine broadcast_capture(other_devices);
    AudioSession::NotifyInterruptionEnded();
    EXPECT_TRUE(WaitFor([&] { return call_opens.load() > opened; })) << "the call's audio was not reopened";
  }
  // The other engines are gone: the call's handler is still there.
  const int reopened = call_opens.load();
  AudioSession::NotifyInterruptionEnded();
  EXPECT_TRUE(WaitFor([&] { return call_opens.load() > reopened; })) << "another engine's teardown took it away";
  EXPECT_EQ(other_opens.load(), 0) << "engines that were not capturing opened nothing";
  call.Stop();
  (void)call.WaitForRetiredSessions(3s);
}

/** A mic that always hears something — on a phone that is the other call it may keep delivering — paced at 20 ms. */
class LoudMic final : public IAudioEndpoint {
public:
  int Read(void* dst, int bytes) override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_read_ < 20ms) {
      return 0;
    }
    last_read_ = now;
    auto* pcm = static_cast<int16_t*>(dst);
    for (int i = 0; i < bytes / static_cast<int>(sizeof(int16_t)); ++i) {
      pcm[i] = (i % 2 == 0) ? int16_t{8000} : int16_t{-8000};
    }
    return bytes;
  }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 0; }
  void Clear() override {}

private:
  std::chrono::steady_clock::time_point last_read_{};
};

class LoudMicBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<LoudMic>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override { return nullptr; }
};

// While the OS has the audio session (another phone call), every outgoing audio frame is silence — from the first
// frame after the interruption began, whatever the mute flag says (the UI's mute can be seconds late) — and packets
// keep their cadence; once it ends the mic is heard again.
TEST(AudioSessionInterruptionTest, OutgoingAudioIsSilenceWhileTheOsHasTheSession) {
  AudioSession::ClearInterruption();
  struct Frame {
    bool silent;
    bool interrupted;
  };
  std::mutex mu;
  std::vector<Frame> frames;
  std::atomic<int> sent_while_interrupted{0};
  MediaDeviceArbiter devices(std::make_unique<LoudMicBackend>());
  MediaEngine engine(devices);
  int loud_before = 0;
  // Capture thread. The interruption begins right after the third loud frame, on that thread: the next frame is the
  // first the engine builds after it began, so it shows whether there is any window.
  engine.SetEncodeInputObserverForTest([&](const int16_t* pcm, size_t samples) {
    const bool silent = std::all_of(pcm, pcm + samples, [](int16_t s) { return s == 0; });
    const bool interrupted = AudioSession::IsInterrupted(); // only this thread sets it: as the engine saw it
    {
      std::lock_guard lock(mu);
      frames.push_back({silent, interrupted});
    }
    if (!interrupted && !silent && ++loud_before == 3) {
      AudioSession::NotifyInterruptionBegan();
    }
  });
  auto started = engine.StartDuplex("held-call", [&](const MediaEngine::MediaPacket&) {
    if (AudioSession::IsInterrupted()) {
      sent_while_interrupted.fetch_add(1);
    }
  });
  ASSERT_TRUE(started) << (started ? "" : started.error().message);

  const auto interrupted_frames = [&] {
    std::lock_guard lock(mu);
    return std::count_if(frames.begin(), frames.end(), [](const Frame& f) { return f.interrupted; });
  };
  ASSERT_TRUE(WaitFor([&] { return interrupted_frames() >= 10; })) << "no frames went out while interrupted";
  EXPECT_FALSE(engine.IsMuted()) << "the guarantee does not lean on the mute flag";
  EXPECT_GE(sent_while_interrupted.load(), 9) << "packets keep flowing while interrupted (the peer sees no stall)";
  AudioSession::NotifyInterruptionEnded();
  const auto loud_after_end = [&] {
    std::lock_guard lock(mu);
    const auto last_held = std::find_if(frames.rbegin(), frames.rend(), [](const Frame& f) { return f.interrupted; });
    return std::any_of(frames.rbegin(), last_held, [](const Frame& f) { return !f.silent; });
  };
  EXPECT_TRUE(WaitFor(loud_after_end)) << "the mic is heard again once the interruption ends";
  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
  AudioSession::ClearInterruption();

  std::lock_guard lock(mu);
  const auto first_held = std::find_if(frames.begin(), frames.end(), [](const Frame& f) { return f.interrupted; });
  ASSERT_NE(first_held, frames.end());
  ASSERT_NE(first_held, frames.begin());
  EXPECT_FALSE(std::prev(first_held)->silent) << "the mic was heard before the interruption";
  EXPECT_TRUE(first_held->silent) << "the first frame after the interruption began is already silence";
  for (const Frame& f : frames) {
    if (f.interrupted) {
      EXPECT_TRUE(f.silent) << "a frame went out with mic audio while the OS had the session";
    }
  }
}

} // namespace
} // namespace pbr
