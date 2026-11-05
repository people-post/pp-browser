#include "lib/media/MediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

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

/** A camera with a frame every time it is asked. */
class FrameCamera final : public ICameraEndpoint {
public:
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36, true}; }
  std::optional<VideoFrameRgba> NextFrame() override {
    VideoFrameRgba frame;
    frame.width = 64;
    frame.height = 36;
    frame.rgba.assign(static_cast<size_t>(64 * 36 * 4), 0x80);
    return frame;
  }
};

class FrameBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override {
    return std::make_unique<FrameCamera>();
  }
};

/** What the encoder was asked, in order: true for a forced keyframe. */
struct EncodeLog {
  std::mutex mu;
  std::vector<bool> keyframes;
  size_t Count() {
    std::lock_guard lock(mu);
    return keyframes.size();
  }
  std::optional<bool> At(size_t i) {
    std::lock_guard lock(mu);
    return i < keyframes.size() ? std::optional<bool>(keyframes[i]) : std::nullopt;
  }
};

class RecordingCodec final : public IVideoCodec {
public:
  explicit RecordingCodec(EncodeLog& log) : log_(log) {}
  std::string BackendName() const override { return "recording-stub"; }
  bool HasEncoder() const override { return true; }
  bool HasDecoder() const override { return false; }
  Roe<void> ConfigureEncoder(int, int, int) override { return {}; }
  Roe<void> ConfigureDecoder() override { return {}; }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, const bool force_keyframe) override {
    {
      std::lock_guard lock(log_.mu);
      log_.keyframes.push_back(force_keyframe);
    }
    return EncodedAccessUnit{.annex_b = {0, 0, 0, 1, 0x65}, .keyframe = force_keyframe};
  }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return std::optional<VideoFrameRgba>{}; }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  EncodeLog& log_;
};

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return done();
}

// #474: a path that cannot carry video pauses the video send; it never turns the camera off, so the
// person's switch and the roster stay as they are, and video comes back by itself — with a keyframe.
TEST(MediaEngineAdaptationTest, APathTooWeakForVideoPausesTheSendAndKeepsTheCamera) {
  EncodeLog encodes;
  MediaDeviceArbiter devices(std::make_unique<FrameBackend>());
  MediaEngine engine(devices);
  engine.SetVideoCodecFactoryForTest(
      [&encodes]() -> std::unique_ptr<IVideoCodec> { return std::make_unique<RecordingCodec>(encodes); });
  auto started = engine.StartDuplex("adaptation-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  ASSERT_TRUE(engine.SetCameraEnabled(true, 0));
  ASSERT_TRUE(WaitFor([&] { return encodes.Count() >= 2; })) << "the camera never sent video";

  MediaTargets weak;
  weak.send_video = false;
  engine.ApplyTargets(weak);
  std::this_thread::sleep_for(150ms); // a frame already past the check may still go
  const size_t paused_at = encodes.Count();
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(encodes.Count(), paused_at) << "video kept going out on a path that cannot carry it";
  EXPECT_TRUE(engine.IsCameraEnabled()) << "adaptation turned the camera off";

  MediaTargets healthy;
  healthy.send_video = true;
  engine.ApplyTargets(healthy);
  ASSERT_TRUE(WaitFor([&] { return encodes.Count() > paused_at; })) << "video did not come back";
  EXPECT_EQ(encodes.At(paused_at), std::optional<bool>(true)) << "video resumes with a keyframe";

  ASSERT_TRUE(engine.SetCameraEnabled(true, 0)) << "the camera can be turned on whatever the path";
  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

// The UI draws a video frame only when told (nothing else wakes its frame loop for one), and a phone saving power
// shows only the sent frames in the self-view: half the camera's, and the others never converted. The sent video
// is the same either way.
TEST(MediaEngineAdaptationTest, EachSelfViewFrameIsAnnouncedAndHalfRateShowsOnlyTheSentOnes) {
  EncodeLog encodes;
  MediaDeviceArbiter devices(std::make_unique<FrameBackend>());
  MediaEngine engine(devices);
  engine.SetVideoCodecFactoryForTest(
      [&encodes]() -> std::unique_ptr<IVideoCodec> { return std::make_unique<RecordingCodec>(encodes); });
  std::atomic<uint64_t> announced{0};
  engine.SetOnVideoFrame([&announced]() { announced.fetch_add(1); });
  auto started = engine.StartDuplex("self-view-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  ASSERT_TRUE(engine.SetCameraEnabled(true, 0));
  ASSERT_TRUE(WaitFor([&] { return encodes.Count() >= 2; })) << "the camera never sent video";

  auto window = [&](std::chrono::milliseconds length) {
    const uint64_t previews = engine.HealthSnapshot().preview_frames;
    const uint64_t calls = announced.load();
    const size_t sent = encodes.Count();
    std::this_thread::sleep_for(length);
    return std::tuple{engine.HealthSnapshot().preview_frames - previews, announced.load() - calls,
                      static_cast<uint64_t>(encodes.Count() - sent)};
  };
  const auto [full_previews, full_calls, full_sent] = window(1000ms);
  EXPECT_GE(full_calls, full_previews) << "a self-view frame the UI was not told of";
  EXPECT_GT(full_previews, full_sent) << "the self-view runs at the camera's rate, above the send rate";

  engine.SetSelfViewHalfRate(true);
  std::this_thread::sleep_for(100ms);
  const auto [half_previews, half_calls, half_sent] = window(1000ms);
  EXPECT_GE(half_calls, half_previews);
  EXPECT_LE(half_previews, half_sent + 1) << "half rate shows only the frames that are sent";
  EXPECT_LT(half_previews * 10, full_previews * 8) << "half rate did not slow the self-view";
  EXPECT_GE(half_sent * 10, full_sent * 7) << "half rate is the self-view's only: the send rate stays";
  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

} // namespace
} // namespace pbr
