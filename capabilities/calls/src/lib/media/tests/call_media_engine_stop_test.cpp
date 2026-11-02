#include "lib/media/CallMediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace pbr {
namespace {

using namespace std::chrono_literals;

constexpr auto kSlowStep = 300ms;

/** Audio endpoint stub; closing is queued on the arbiter's device thread, so it is not the slow part. */
class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

class StubCamera final : public ICameraEndpoint {
public:
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36, true}; }
  std::optional<VideoFrameRgba> NextFrame() override { return std::nullopt; }
};

class StubBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override {
    return std::make_unique<StubCamera>();
  }
};

/** A hardware codec whose teardown (reset / destroy) is slow, like VideoToolbox / MediaCodec on a phone. */
class SlowTeardownCodec final : public IVideoCodec {
public:
  explicit SlowTeardownCodec(std::atomic<int>& made) { made.fetch_add(1); }
  ~SlowTeardownCodec() override { std::this_thread::sleep_for(kSlowStep); }
  std::string BackendName() const override { return "slow-stub"; }
  bool HasEncoder() const override { return true; }
  bool HasDecoder() const override { return false; }
  Roe<void> ConfigureEncoder(int, int, int) override { return {}; }
  Roe<void> ConfigureDecoder() override { return {}; }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return EncodedAccessUnit{}; }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return std::optional<VideoFrameRgba>{}; }
  void ResetEncoder() override {}
  void ResetDecoder() override { std::this_thread::sleep_for(kSlowStep); }
};

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

// B125: Stop() tore the codec down while holding the engine mutex, so a UI getter polled during the
// ~1 s teardown on phones waited for all of it. The teardown now runs after the lock is released.
TEST(CallMediaEngineStopTest, SlowTeardownDoesNotBlockGetters) {
  std::atomic<int> codecs_made{0};
  MediaDeviceArbiter devices(std::make_unique<StubBackend>());
  CallMediaEngine engine(devices);
  engine.SetVideoCodecFactoryForTest(
      [&codecs_made]() -> std::unique_ptr<IVideoCodec> { return std::make_unique<SlowTeardownCodec>(codecs_made); });

  auto started = engine.StartSfu("slow-teardown-call", [](const CallMediaEngine::SfuPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  auto camera = engine.SetCameraEnabled(true, 0);
  ASSERT_TRUE(camera) << (camera ? "" : camera.error().message);
  ASSERT_TRUE(WaitFor([&] { return codecs_made.load() >= 1; })) << "the camera never opened the codec";

  std::atomic<bool> stop_done{false};
  std::thread stopper([&] {
    engine.Stop();
    stop_done = true;
  });

  int64_t worst_ms = 0;
  int polls = 0;
  while (!stop_done.load()) {
    const auto from = std::chrono::steady_clock::now();
    (void)engine.ConnectionState();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
    worst_ms = std::max<int64_t>(worst_ms, ms);
    ++polls;
    std::this_thread::sleep_for(2ms);
  }
  stopper.join();

  EXPECT_GT(polls, 10);
  // Relative bound (CI jitter): a getter that waited for the teardown would take ~kSlowStep.
  EXPECT_LT(worst_ms, std::chrono::duration_cast<std::chrono::milliseconds>(kSlowStep).count() / 3)
      << "ConnectionState waited for the codec teardown";
}

}  // namespace
}  // namespace pbr
