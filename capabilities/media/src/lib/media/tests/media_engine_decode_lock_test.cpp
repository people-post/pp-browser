#include "lib/media/MediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"
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

constexpr auto kSlowDecode = 400ms;

class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

class AudioOnlyBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    return std::make_unique<QuietEndpoint>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override { return nullptr; }
};

/** A decoder that takes a long time per access unit, as a wedged hardware codec does before L016 gives up on it. */
class SlowDecoder final : public IVideoCodec {
public:
  explicit SlowDecoder(std::atomic<bool>& decoding) : decoding_(decoding) {}
  std::string BackendName() const override { return "slow-decode-stub"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return configured_; }
  Roe<void> ConfigureEncoder(int, int, int) override { return {}; }
  Roe<void> ConfigureDecoder() override {
    configured_ = true;
    return {};
  }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return EncodedAccessUnit{}; }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override {
    decoding_.store(true);
    std::this_thread::sleep_for(kSlowDecode);
    decoding_.store(false);
    return std::optional<VideoFrameRgba>{};
  }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  std::atomic<bool>& decoding_;
  bool configured_ = false;
};

// #474: remote video is decoded outside the media lock, so a slow decode holds neither the getters
// nor the audio path (playout and capture take the same lock).
TEST(MediaEngineDecodeLockTest, ASlowDecodeHoldsNeitherGettersNorAudio) {
  std::atomic<bool> decoding{false};
  MediaDeviceArbiter devices(std::make_unique<AudioOnlyBackend>());
  MediaEngine engine(devices);
  engine.SetVideoCodecFactoryForTest(
      [&decoding]() -> std::unique_ptr<IVideoCodec> { return std::make_unique<SlowDecoder>(decoding); });
  auto started = engine.StartDuplex("decode-lock-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);

  MediaEngine::MediaPacket video;
  video.stream_id = 7;
  video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
  video.seq = 1;
  video.payload = {0, 0, 0, 1, 0x65, 0x88};
  std::thread reader([&engine, video]() { engine.OnInboundPacket(video); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!decoding.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(decoding.load()) << "the decoder never ran";

  const auto from = std::chrono::steady_clock::now();
  (void)engine.ActiveSessionId(); // takes the media lock
  MediaEngine::MediaPacket audio;
  audio.stream_id = 7;
  audio.channel_id = mesh::kMediaChannelAudio;
  audio.seq = 1;
  audio.payload = {0xf8, 0xff, 0xfe};
  engine.OnInboundPacket(audio); // the audio path takes it too
  const auto waited = std::chrono::steady_clock::now() - from;
  EXPECT_TRUE(decoding.load()) << "the decode finished first: the test proved nothing";
  EXPECT_LT(waited, kSlowDecode / 4) << "the media lock was held across the decode";

  reader.join();
  engine.Stop();
  (void)engine.WaitForRetiredSessions(3s);
}

} // namespace
} // namespace pbr
