// DEV/PLAN-call-frame-cost.md step 2: the UI reads a video frame only when it is not the one it already has. The
// engine compares the caller's last sequence number under the frame lock and copies nothing when it is the same.
// No real devices: the camera and the decoder are stubs that hand the engine frames on demand.
#include "lib/media/MediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VideoCodecUnavailable.h"
#include "mesh/types/MediaChannel.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

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
using FrameRead = IMediaView::FrameRead;

class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

/** A camera that hands out one frame each time the test allows one. */
class GatedCamera final : public ICameraEndpoint {
public:
  explicit GatedCamera(std::atomic<int>& allowed) : allowed_(allowed) {}
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36, false}; }
  std::optional<VideoFrameRgba> NextFrame() override {
    int n = allowed_.load();
    while (n > 0) {
      if (allowed_.compare_exchange_weak(n, n - 1)) {
        VideoFrameRgba frame;
        frame.width = 64;
        frame.height = 36;
        frame.rgba.assign(64u * 36u * 4u, 0x80);
        return frame;
      }
    }
    return std::nullopt;
  }

private:
  std::atomic<int>& allowed_;
};

class StubBackend final : public IMediaDeviceBackend {
public:
  explicit StubBackend(std::atomic<int>& allowed) : allowed_(allowed) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override {
    return std::make_unique<GatedCamera>(allowed_);
  }

private:
  std::atomic<int>& allowed_;
};

/** Decodes every access unit into a 4x2 frame at once. */
class FrameDecoder final : public IVideoCodec {
public:
  std::string BackendName() const override { return "frame-decoder-stub"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return configured_; }
  Roe<void> ConfigureEncoder(int, int, int) override { return {}; }
  Roe<void> ConfigureDecoder() override {
    configured_ = true;
    return {};
  }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return EncodedAccessUnit{}; }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override {
    VideoFrameRgba frame;
    frame.width = 4;
    frame.height = 2;
    frame.rgba.assign(4u * 2u * 4u, 0x40);
    return std::optional<VideoFrameRgba>{std::move(frame)};
  }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  bool configured_ = false;
};

bool WaitUntil(const std::function<bool()>& done) {
  for (int i = 0; i < 400; ++i) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

/** An untouched output: what "None" and "Same" must leave it as. */
IMediaView::VideoTileFrame Sentinel() {
  IMediaView::VideoTileFrame out;
  out.width = -7;
  out.seq = 4242;
  return out;
}

void ExpectUntouched(const IMediaView::VideoTileFrame& out) {
  EXPECT_EQ(out.width, -7);
  EXPECT_EQ(out.seq, 4242u);
  EXPECT_TRUE(out.rgba.empty());
}

class RemoteFrameReadTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetVideoCodecFactoryForTest([]() -> std::unique_ptr<IVideoCodec> { return std::make_unique<FrameDecoder>(); });
    auto started = engine_.StartDuplex("frame-read-call", [](const MediaEngine::MediaPacket&) {});
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
  }
  void TearDown() override {
    engine_.Stop();
    (void)engine_.WaitForRetiredSessions(3s);
  }
  void Deliver(const uint32_t stream_id) {
    MediaEngine::MediaPacket video;
    video.stream_id = stream_id;
    video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
    video.seq = ++packet_seq_;
    video.payload = {0, 0, 0, 1, 0x65, 0x88};
    engine_.OnInboundPacket(video);
  }
  /** Delivers one frame on `stream_id` and returns its sequence number, read as a new frame. */
  uint64_t DeliverAndRead(const uint32_t stream_id, const uint64_t last_seq) {
    Deliver(stream_id);
    IMediaView::VideoTileFrame out;
    EXPECT_TRUE(WaitUntil([&] { return engine_.CopyRemoteVideoFrameIfNew(last_seq, out) == FrameRead::New; }));
    return out.seq;
  }

  std::atomic<int> allowed_{0};
  MediaDeviceArbiter devices_{std::make_unique<StubBackend>(allowed_)};
  MediaEngine engine_{devices_};
  uint32_t packet_seq_ = 0;
};

TEST_F(RemoteFrameReadTest, NoFrameReadsAsNone) {
  IMediaView::VideoTileFrame out = Sentinel();
  EXPECT_EQ(engine_.CopyRemoteVideoFrameIfNew(0, out), FrameRead::None);
  ExpectUntouched(out);
  EXPECT_EQ(engine_.CopyRemoteVideoFrameForStreamIfNew(7, 0, out), FrameRead::None);
  ExpectUntouched(out);
}

TEST_F(RemoteFrameReadTest, TheSameFrameIsNotCopiedANewOneIs) {
  const uint64_t first = DeliverAndRead(7, 0);
  ASSERT_NE(first, 0u);
  IMediaView::VideoTileFrame out = Sentinel();
  EXPECT_EQ(engine_.CopyRemoteVideoFrameIfNew(first, out), FrameRead::Same);
  ExpectUntouched(out);
  EXPECT_EQ(engine_.CopyRemoteVideoFrameForStreamIfNew(7, first, out), FrameRead::Same);
  ExpectUntouched(out);

  Deliver(7);
  ASSERT_TRUE(WaitUntil([&] { return engine_.CopyRemoteVideoFrameIfNew(first, out) == FrameRead::New; }));
  EXPECT_NE(out.seq, first);
  EXPECT_EQ(out.width, 4);
  EXPECT_EQ(out.rgba.size(), 4u * 2u * 4u);
  IMediaView::VideoTileFrame by_stream = Sentinel();
  EXPECT_EQ(engine_.CopyRemoteVideoFrameForStreamIfNew(7, first, by_stream), FrameRead::New);
  EXPECT_EQ(by_stream.seq, out.seq);
  EXPECT_EQ(by_stream.rgba.size(), 4u * 2u * 4u);
  EXPECT_EQ(engine_.CopyRemoteVideoFrameForStreamIfNew(8, 0, by_stream), FrameRead::None) << "another stream";
}

TEST_F(RemoteFrameReadTest, ASmallerSequenceNumberIsANewFrameToo) {
  // Frames up to a high number, then the remote video is cleared (a stall, the stream rebuilt): numbering starts over.
  uint64_t last = 0;
  for (int i = 0; i < 5; ++i) {
    last = DeliverAndRead(7, last);
  }
  ASSERT_GE(last, 5u);
  engine_.ClearRemoteVideo();
  IMediaView::VideoTileFrame out = Sentinel();
  EXPECT_EQ(engine_.CopyRemoteVideoFrameIfNew(last, out), FrameRead::None);
  ExpectUntouched(out);
  Deliver(7);
  ASSERT_TRUE(WaitUntil([&] { return engine_.CopyRemoteVideoFrameForStreamIfNew(7, last, out) == FrameRead::New; }));
  EXPECT_LT(out.seq, last) << "compared by inequality, not by size";
  IMediaView::VideoTileFrame primary;
  EXPECT_EQ(engine_.CopyRemoteVideoFrameIfNew(last, primary), FrameRead::New);
}

TEST_F(RemoteFrameReadTest, TheOldCopiesStillCopyEveryTime) {
  const uint64_t first = DeliverAndRead(7, 0);
  IMediaView::VideoTileFrame out;
  ASSERT_TRUE(engine_.CopyRemoteVideoFrame(out));
  EXPECT_EQ(out.seq, first);
  IMediaView::VideoTileFrame again;
  ASSERT_TRUE(engine_.CopyRemoteVideoFrameForStream(7, again));
  EXPECT_EQ(again.seq, first);
  EXPECT_EQ(again.rgba.size(), 4u * 2u * 4u);
}

TEST(LocalFrameReadTest, TheSelfViewIsCopiedOnlyWhenNew) {
  std::atomic<int> allowed{0};
  MediaDeviceArbiter devices(std::make_unique<StubBackend>(allowed));
  MediaEngine engine(devices);
  engine.SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test stub"); });
  ASSERT_TRUE(engine.StartDuplex("frame-read-local", [](const MediaEngine::MediaPacket&) {}));
  IMediaView::VideoTileFrame out = Sentinel();
  EXPECT_EQ(engine.CopyLocalVideoFrameIfNew(0, out), FrameRead::None) << "camera off";
  ExpectUntouched(out);

  ASSERT_TRUE(engine.SetCameraEnabled(true));
  allowed.store(1);
  ASSERT_TRUE(WaitUntil([&] { return engine.CopyLocalVideoFrameIfNew(0, out) == FrameRead::New; }));
  const uint64_t first = out.seq;
  EXPECT_EQ(out.width, 64);
  ASSERT_TRUE(WaitUntil([&] { return allowed.load() == 0; }));
  std::this_thread::sleep_for(50ms); // the camera has nothing more to give
  IMediaView::VideoTileFrame same = Sentinel();
  EXPECT_EQ(engine.CopyLocalVideoFrameIfNew(first, same), FrameRead::Same);
  ExpectUntouched(same);

  allowed.store(1);
  ASSERT_TRUE(WaitUntil([&] { return engine.CopyLocalVideoFrameIfNew(first, out) == FrameRead::New; }));
  EXPECT_NE(out.seq, first);
  const uint64_t second = out.seq;

  ASSERT_TRUE(engine.SetCameraEnabled(false));
  IMediaView::VideoTileFrame gone = Sentinel();
  EXPECT_TRUE(WaitUntil([&] { return engine.CopyLocalVideoFrameIfNew(second, gone) == FrameRead::None; }))
      << "the preview is cleared with the camera";
  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

} // namespace
} // namespace pbr
