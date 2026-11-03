#include "lib/media/MediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace pbr {
namespace {

using namespace std::chrono_literals;

class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

/** A camera that opens but gives no frames: the OS has refused it, or the prompt is still up. */
class SilentCamera final : public ICameraEndpoint {
public:
  explicit SilentCamera(bool denied) : denied_(denied) {}
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36, true}; }
  std::optional<VideoFrameRgba> NextFrame() override { return std::nullopt; }
  bool PermissionPending() const override { return !denied_; }
  bool PermissionDenied() const override { return denied_; }

private:
  bool denied_;
};

class SilentBackend final : public IMediaDeviceBackend {
public:
  explicit SilentBackend(bool denied) : denied_(denied) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override {
    return std::make_unique<SilentCamera>(denied_);
  }

private:
  bool denied_;
};

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return done();
}

// B147: a phone whose camera permission was off showed no prompt and no hint — the camera stayed
// "on" with nothing behind it. A refusal now turns video off and says why.
TEST(MediaEngineCameraPermissionTest, ACameraTheOsRefusedTurnsVideoOffAndSaysWhy) {
  MediaDeviceArbiter devices(std::make_unique<SilentBackend>(/*denied=*/true));
  MediaEngine engine(devices);
  auto started = engine.StartDuplex("permission-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  ASSERT_TRUE(engine.SetCameraEnabled(true, 0));

  ASSERT_TRUE(WaitFor([&] { return !engine.IsCameraEnabled(); })) << "the refused camera stayed on";
  const auto failure = engine.TakeCameraFailure();
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->kind, CameraFailure::Kind::PermissionDenied);
  EXPECT_FALSE(engine.TakeCameraFailure().has_value()) << "told once";

  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

TEST(MediaEngineCameraPermissionTest, ACameraWaitingForThePromptStaysOn) {
  MediaDeviceArbiter devices(std::make_unique<SilentBackend>(/*denied=*/false));
  MediaEngine engine(devices);
  auto started = engine.StartDuplex("prompt-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  ASSERT_TRUE(engine.SetCameraEnabled(true, 0));

  std::this_thread::sleep_for(300ms);
  EXPECT_TRUE(engine.IsCameraEnabled());
  EXPECT_FALSE(engine.TakeCameraFailure().has_value());

  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

} // namespace
} // namespace pbr
