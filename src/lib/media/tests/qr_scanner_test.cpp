#include "lib/media/QrScanner.h"
#include "lib/media/QrDecode.h"
#include "lib/media/CameraCandidates.h"
#include "lib/media/VideoYuv.h"
#include <qrcodegen.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace pbr {
namespace {
VideoFrameRgba QrFrame(const char* text) {
  const auto qr = qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::MEDIUM);
  const int scale = 5;
  VideoFrameRgba frame;
  frame.width = frame.height = (qr.getSize() + 8) * scale;
  frame.rgba.assign(static_cast<size_t>(frame.width * frame.height * 4), 255);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      if (qr.getModule(x / scale - 4, y / scale - 4)) {
        const size_t i = static_cast<size_t>((y * frame.width + x) * 4);
        frame.rgba[i] = frame.rgba[i + 1] = frame.rgba[i + 2] = 0;
      }
    }
  }
  return frame;
}
struct FakeState {
  std::mutex mutex;
  std::condition_variable cv;
  bool block_open = false;
  bool release_open = false;
  std::atomic<int> opens{0};
  std::atomic<int> closes{0};
  std::atomic<bool> rear_requested{false};
  CameraRequestFormat requested_format;
  std::string error;
  bool no_frames = false;
  bool zoom_required = false;
  int candidate_offset_x = 0;
  std::atomic<float> zoom{1.f};
  bool slow_zoom = false;
  bool native_zoom_supported = true;
  std::atomic<int> zoom_requests{0};
  std::chrono::steady_clock::time_point zoom_started{};
  std::atomic<bool> zoom_settled{false};
};
class FakeCamera final : public ICameraEndpoint {
public:
  FakeCamera(FakeState& state, const char* text) : state_(state), frame_(QrFrame(text)) {}
  ~FakeCamera() override { ++state_.closes; }
  CameraGeometry Geometry() const override { return {}; }
  std::string CaptureError() const override { return state_.error; }
  std::optional<VideoFrameRgba> NextFrame() override {
    if (state_.zoom_required && (state_.zoom == 1.f || ZoomPending())) {
      auto damaged = frame_;
      for (int y = damaged.height * 38 / 100; y < damaged.height * 80 / 100; ++y) {
        for (int x = damaged.width * 38 / 100; x < damaged.width * 80 / 100; ++x) {
          const size_t at = static_cast<size_t>(y * damaged.width + x) * 4;
          damaged.rgba[at] = damaged.rgba[at + 1] = damaged.rgba[at + 2] = 255;
        }
      }
      VideoFrameRgba distant;
      distant.width = 640;
      distant.height = 480;
      distant.rgba.assign(640 * 480 * 4, 255);
      for (int y = 0; y < damaged.height; ++y) {
        const size_t at = static_cast<size_t>((y + (480 - damaged.height) / 2) * 640 +
                                            (640 - damaged.width) / 2 + state_.candidate_offset_x) * 4;
        std::copy_n(damaged.rgba.data() + static_cast<size_t>(y * damaged.width) * 4,
                    static_cast<size_t>(damaged.width) * 4, distant.rgba.data() + at);
      }
      return distant;
    }
    return state_.no_frames ? std::nullopt : std::optional<VideoFrameRgba>(frame_);
  }
  bool SetZoom(float factor) override {
    if (!state_.zoom_required || !state_.native_zoom_supported) { return false; }
    if (state_.zoom_requests++ == 0) { state_.zoom_started = std::chrono::steady_clock::now(); }
    state_.zoom = factor;
    return true;
  }
  CameraZoomState ZoomState() const override {
    // A slow ramp settles only when the scanner asks after 600 ms, so a frame fetched before this
    // query is still mid-ramp: judging such a frame against the settled state is the regression.
    if (state_.slow_zoom && state_.zoom_requests > 0 &&
        std::chrono::steady_clock::now() - state_.zoom_started >= std::chrono::milliseconds(600)) {
      state_.zoom_settled = true;
    }
    const bool pending = ZoomPending();
    return {state_.zoom_required && state_.native_zoom_supported, pending ? 1.f : state_.zoom.load(), 3.f, pending};
  }
private:
  bool ZoomPending() const {
    return state_.slow_zoom && state_.zoom_requests > 0 && !state_.zoom_settled;
  }
  FakeState& state_;
  VideoFrameRgba frame_;
};
class FakeBackend final : public IMediaDeviceBackend {
public:
  explicit FakeBackend(FakeState& state) : state_(state) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&,
      const std::function<bool()>&, std::string*) override { return nullptr; }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& format, std::string*) override {
    const int open = ++state_.opens;
    state_.rear_requested = !format.prefer_front_facing;
    state_.requested_format = format;
    std::unique_lock lock(state_.mutex);
    state_.cv.wait(lock, [&] { return !state_.block_open || state_.release_open; });
    return std::make_unique<FakeCamera>(state_, open == 1 ? "old payload" : "new payload");
  }
private:
  FakeState& state_;
};
bool WaitFor(const std::function<bool()>& predicate, std::chrono::seconds timeout = std::chrono::seconds(3)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
TEST(QrDecodeTest, DecodesRawPayloadAndRotatedFrame) {
  const auto frame = QrFrame("https://example.test/invite?peer=abc");
  EXPECT_EQ(DecodeQr(frame), "https://example.test/invite?peer=abc");
  VideoFrameRgba rotated;
  ASSERT_TRUE(RotateRgba90Cw(frame, rotated));
  EXPECT_EQ(DecodeQr(rotated), "https://example.test/invite?peer=abc");
  EXPECT_FALSE(DecodeQr({}));
  auto truncated = frame;
  truncated.rgba.resize(5);
  EXPECT_FALSE(DecodeQr(truncated));
}
TEST(QrScannerTest, DeliversPreviewAndPayloadThenReleasesCamera) {
  FakeState state;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return result.text.has_value(); }));
  EXPECT_EQ(result.text, "old payload");
  ASSERT_TRUE(result.preview);
  EXPECT_TRUE(state.rear_requested);
  EXPECT_EQ(state.requested_format.capture_width, 1280);
  EXPECT_EQ(state.requested_format.capture_height, 720);
  EXPECT_EQ(state.requested_format.fps, 30);
  EXPECT_TRUE(WaitFor([&] { return state.closes == 1; }));
  EXPECT_TRUE(devices.Holders(MediaDeviceKind::Camera).empty());
}
TEST(QrScannerTest, StopAndRestartDiscardBlockedOldOpenWithoutBlockingUi) {
  FakeState state;
  state.block_open = true;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  const bool opening = WaitFor([&] { return state.opens == 1; });
  const auto start = std::chrono::steady_clock::now();
  scanner.Stop();
  scanner.Start();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
  EXPECT_FALSE(scanner.Poll().text);
  {
    std::lock_guard lock(state.mutex);
    state.release_open = true;
    state.cv.notify_all();
  }
  ASSERT_TRUE(opening);
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return result.text.has_value(); }));
  EXPECT_EQ(result.text, "new payload");
  scanner.Shutdown();
  EXPECT_TRUE(WaitFor([&] { return state.closes == 2; }));
}
TEST(QrScannerTest, ReportsPermissionDenialAndDeviceOpenFailure) {
  FakeState state;
  state.error = "Camera permission denied";
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return !result.error.empty(); }));
  EXPECT_EQ(result.error, state.error);
  EXPECT_FALSE(result.text);
  MediaDeviceArbiter no_camera(CreateNullMediaDeviceBackend());
  QrScanner unavailable(no_camera);
  unavailable.Start();
  EXPECT_TRUE(WaitFor([&] { return !unavailable.Poll().error.empty(); }));
}
TEST(QrScannerTest, MissingFramesBecomeVisibleErrorAndReleaseCamera) {
  FakeState state;
  state.no_frames = true;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return !result.error.empty(); }, std::chrono::seconds(18)));
  EXPECT_NE(result.error.find("no frames"), std::string::npos);
  EXPECT_TRUE(WaitFor([&] { return state.closes == 1; }));
}
TEST(QrScannerTest, StopDiscardsAlreadyCompletedUnconsumedPayload) {
  FakeState state;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  ASSERT_TRUE(WaitFor([&] { return state.closes == 1; }));
  scanner.Stop();
  auto update = scanner.Poll();
  EXPECT_FALSE(update.text);
  EXPECT_FALSE(update.preview);
  EXPECT_TRUE(update.error.empty());
}
TEST(QrScannerTest, ReportsCameraAlreadyHeldWithoutOpeningAnother) {
  FakeState state;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  auto held = devices.AcquireCamera({"call", {}});
  ASSERT_TRUE(held);
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return !result.error.empty(); }));
  EXPECT_NE(result.error.find("held by call"), std::string::npos);
  EXPECT_EQ(state.opens, 1);
}
TEST(QrScannerTest, RearPreferenceKeepsBorrowedCamerasLast) {
  EXPECT_EQ(OrderCameraCandidates({{1, true, false}, {2, false, true}, {3, false, false}}, false),
            (std::vector<uint32_t>{3, 1, 2}));
}
TEST(QrScannerTest, DetectedUnreadableDistantCodeRequestsZoomThenReadsPayload) {
  FakeState state;
  state.zoom_required = true;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return result.text.has_value(); }));
  EXPECT_GT(state.zoom.load(), 1.f);
  EXPECT_LE(state.zoom.load(), 3.f);
  EXPECT_EQ(result.text, "old payload");
}

TEST(QrDecodeTest, DenseContactPayloadRetainsIdentityAndDetectedBounds) {
  const std::string payload = "pp-browser-contact-v1:" + std::string(700, 'A');
  const auto detected = InspectQr(QrFrame(payload.c_str()));
  ASSERT_EQ(detected.text, payload);
  ASSERT_TRUE(detected.region);
  EXPECT_NEAR(detected.region->center_x, .5f, .03f);
  EXPECT_NEAR(detected.region->center_y, .5f, .03f);
}

TEST(QrScannerTest, OffCenterCandidateLimitsZoomToKeepTheCodeVisible) {
  FakeState state;
  state.zoom_required = true;
  state.candidate_offset_x = -128;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  ASSERT_TRUE(WaitFor([&] { return scanner.Poll().text.has_value(); }));
  EXPECT_GT(state.zoom.load(), 1.f);
  EXPECT_LT(state.zoom.load(), 1.7f);
}

TEST(QrScannerTest, PendingNativeRampDoesNotCompoundZoomFromOldFrames) {
  FakeState state;
  state.zoom_required = true;
  state.slow_zoom = true;
  state.candidate_offset_x = -128;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  ASSERT_TRUE(WaitFor([&] { return scanner.Poll().text.has_value(); }));
  EXPECT_EQ(state.zoom_requests, 1);
  EXPECT_LT(state.zoom.load(), 1.7f);
}

TEST(QrScannerTest, CameraWithoutNativeZoomUsesBoundedSoftwarePreview) {
  FakeState state;
  state.zoom_required = true;
  state.native_zoom_supported = false;
  MediaDeviceArbiter devices(std::make_unique<FakeBackend>(state));
  QrScanner scanner(devices);
  scanner.Start();
  QrScanner::Update result;
  ASSERT_TRUE(WaitFor([&] { result = scanner.Poll(); return result.preview.has_value(); }));
  EXPECT_LT(result.preview->width, 640);
  EXPECT_LT(result.preview->height, 480);
  EXPECT_EQ(state.zoom_requests, 0);
  EXPECT_FALSE(result.text);
  scanner.Stop();
  EXPECT_TRUE(WaitFor([&] { return state.closes == 1; }));
}
} // namespace
} // namespace pbr
