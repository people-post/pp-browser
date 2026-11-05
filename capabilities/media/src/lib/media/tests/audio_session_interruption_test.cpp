#include "lib/media/AudioSession.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaEngine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

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

} // namespace
} // namespace pbr
