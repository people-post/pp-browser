#include "lib/media/MediaEngine.h"
#include "mesh/types/MediaChannel.h"
#include "lib/media/DecodeStallPolicy.h"
#include "lib/media/IVideoCodec.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <opus.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using Spec = MediaEngine::SessionSpec;

/** One real 20 ms Opus frame (silence) so inbound decode paths run. */
std::vector<uint8_t> OpusSilenceFrame() {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  EXPECT_EQ(err, OPUS_OK);
  std::vector<int16_t> pcm(960, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 960, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  EXPECT_GT(n, 0);
  return {out.begin(), out.begin() + std::max(n, 0)};
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

class MediaSessionSpecTest : public ::testing::Test {
protected:
  void SetUp() override { engine_.SetSkipDeviceOpenForTest(true); }
  void TearDown() override { engine_.Stop(); }

  MediaEngine::PacketSendFn CountingSend() {
    return [this](const MediaEngine::MediaPacket&) { sent_.fetch_add(1); };
  }
  void Deliver(uint32_t stream_id, uint32_t seq) {
    MediaEngine::MediaPacket pkt;
    pkt.stream_id = stream_id;
    pkt.channel_id = mesh::kMediaChannelAudio;
    pkt.seq = seq;
    pkt.payload = frame_;
    engine_.OnInboundPacket(pkt);
  }

  MediaEngine engine_;
  std::atomic<int> sent_{0};
  std::vector<uint8_t> frame_ = OpusSilenceFrame();
};

// Viewer shape: receive → decode → mix; the mic is never opened and nothing is sent.
TEST_F(MediaSessionSpecTest, PlaybackOnlyReceivesAndNeverSends) {
  ASSERT_TRUE(engine_.Start("session:view", Spec::PlaybackOnly(), {}));
  EXPECT_TRUE(engine_.IsActive());
  EXPECT_EQ(engine_.ActiveSpec(), Spec::PlaybackOnly());
  for (uint32_t seq = 1; seq <= 5; ++seq) {
    Deliver(42, seq);
  }
  const auto health = engine_.HealthSnapshot();
  EXPECT_EQ(health.stream_count, 1u);
  EXPECT_GE(health.rx_audio_frames, 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sent_.load(), 0);
  EXPECT_EQ(engine_.HealthSnapshot().tx_audio_frames, 0u);
  EXPECT_FALSE(engine_.HasLocalCapture());
  EXPECT_FALSE(engine_.SetCameraEnabled(true)) << "camera needs a capturing session";
}

// PB-051: a remote that fans out packets under ever-new stream ids must not make us allocate an
// Opus decoder and jitter buffer per id without end. Tracks beyond the cap are dropped; the ones we
// already have keep receiving.
TEST_F(MediaSessionSpecTest, RemoteAudioTracksAreCapped) {
  ASSERT_TRUE(engine_.Start("session:view", Spec::PlaybackOnly(), {}));
  // stream 1 is the id-less 1:1 path and always dropped; start at 2.
  for (uint32_t stream = 2; stream <= MediaEngine::kMaxRemoteAudioTracks + 13; ++stream) {
    Deliver(stream, 1);
  }
  EXPECT_EQ(engine_.HealthSnapshot().stream_count, MediaEngine::kMaxRemoteAudioTracks);
  const uint64_t before = engine_.HealthSnapshot().rx_audio_frames;
  Deliver(2, 2);
  EXPECT_EQ(engine_.HealthSnapshot().rx_audio_frames, before + 1) << "an existing track still receives";
  Deliver(MediaEngine::kMaxRemoteAudioTracks + 100, 1);
  EXPECT_EQ(engine_.HealthSnapshot().rx_audio_frames, before + 1) << "a dropped stream is not counted";
}

// A029: a sent audio packet holds 40 ms — half the packets, and half the per-packet headers, of 20 ms.
TEST_F(MediaSessionSpecTest, SentAudioPacketsHoldFortyMs) {
  std::mutex mu;
  std::vector<int> samples;
  ASSERT_TRUE(engine_.Start("session:cast", Spec::CaptureOnly(), [&](const MediaEngine::MediaPacket& pkt) {
    std::lock_guard lock(mu);
    samples.push_back(
        opus_packet_get_nb_samples(pkt.payload.data(), static_cast<opus_int32>(pkt.payload.size()), 48000));
  }));
  EXPECT_TRUE(WaitFor(
      [&] {
        std::lock_guard lock(mu);
        return samples.size() >= 3;
      },
      std::chrono::seconds(2)));
  engine_.Stop();
  std::lock_guard lock(mu);
  for (const int n : samples) {
    EXPECT_EQ(n, 48000 * 40 / 1000);
  }
}

// A peer on a 20 ms build and one on 40 ms both play: the receiver reads each packet's length.
TEST_F(MediaSessionSpecTest, ReceivesTwentyAndFortyMsPackets) {
  ASSERT_TRUE(engine_.Start("session:view", Spec::PlaybackOnly(), {}));
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  std::vector<int16_t> pcm(1920, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 1920, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  ASSERT_GT(n, 0);
  const std::vector<uint8_t> forty(out.begin(), out.begin() + n);
  for (uint32_t seq = 1; seq <= 5; ++seq) {
    Deliver(42, seq); // 20 ms
    MediaEngine::MediaPacket pkt;
    pkt.stream_id = 43;
    pkt.channel_id = mesh::kMediaChannelAudio;
    pkt.seq = seq;
    pkt.payload = forty;
    engine_.OnInboundPacket(pkt);
  }
  const auto health = engine_.HealthSnapshot();
  EXPECT_EQ(health.stream_count, 2u);
  EXPECT_EQ(health.rx_audio_frames, 10u);
}

// Broadcaster shape: capture → encode → send; inbound packets are not decoded.
TEST_F(MediaSessionSpecTest, CaptureOnlySendsAndIgnoresInbound) {
  ASSERT_TRUE(engine_.Start("session:cast", Spec::CaptureOnly(), CountingSend()));
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 3; }, std::chrono::seconds(2)))
      << "silence frames (no device in tests) are paced out";
  Deliver(42, 1);
  EXPECT_EQ(engine_.HealthSnapshot().stream_count, 0u) << "no playback half, no RX tracks";
}

// Calls: StartDuplex is exactly duplex.
TEST_F(MediaSessionSpecTest, StartDuplexIsDuplex) {
  ASSERT_TRUE(engine_.StartDuplex("call:1", CountingSend()));
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex());
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 3; }, std::chrono::seconds(2)));
  Deliver(7, 1);
  EXPECT_EQ(engine_.HealthSnapshot().stream_count, 1u);
}

// A muted call (or one without a mic) keeps sending frames of silence: the peer's "no media" check
// (TX-only escalation) counts frames, not audio level, so it never mistakes a muted user for a
// broken path.
TEST_F(MediaSessionSpecTest, MutedCallStillSendsFrames) {
  ASSERT_TRUE(engine_.StartDuplex("call:muted", CountingSend()));
  engine_.SetMuted(true);
  const int before = sent_.load();
  EXPECT_TRUE(WaitFor([&] { return sent_.load() >= before + 3; }, std::chrono::seconds(2)));
}

TEST_F(MediaSessionSpecTest, InvalidSpecsAreRefused) {
  EXPECT_FALSE(engine_.Start("s", Spec{false, false}, CountingSend())) << "needs a half";
  EXPECT_FALSE(engine_.Start("s", Spec::CaptureOnly(), {})) << "capturing needs a send fn";
  EXPECT_FALSE(engine_.IsActive());
}

// A different spec for the same session id rebuilds instead of send-swapping.
TEST_F(MediaSessionSpecTest, ChangingSpecRebuildsSession) {
  ASSERT_TRUE(engine_.Start("s", Spec::PlaybackOnly(), {}));
  ASSERT_TRUE(engine_.Start("s", Spec::Duplex(), CountingSend()));
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex());
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 1; }, std::chrono::seconds(2)));
  engine_.Stop();
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex()) << "idle reports duplex";
  EXPECT_FALSE(engine_.IsActive());
}

/** Decode-only stub that counts how many decoders get configured; every decode fails. */
class CountingDecoder final : public IVideoCodec {
public:
  explicit CountingDecoder(std::shared_ptr<std::atomic<int>> configured) : configured_(std::move(configured)) {}
  std::string BackendName() const override { return "counting-decoder"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return has_decoder_; }
  Roe<void> ConfigureEncoder(int, int, int) override { return Error("no encoder"); }
  Roe<void> ConfigureDecoder() override {
    has_decoder_ = true;
    configured_->fetch_add(1);
    return {};
  }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return Error("no encoder"); }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return Error("broken session"); }
  void ResetEncoder() override {}
  void ResetDecoder() override { has_decoder_ = false; }

private:
  std::shared_ptr<std::atomic<int>> configured_;
  bool has_decoder_ = false;
};

// A call decodes the peer's video with a fresh decoder: one kept from an earlier call also kept its
// broken session, so the phone never showed the Mac's video again (device test 2026-09-29).
TEST_F(MediaSessionSpecTest, EachCallDecodesPeerVideoWithAFreshDecoder) {
  auto configured = std::make_shared<std::atomic<int>>(0);
  engine_.SetVideoCodecFactoryForTest([configured] { return std::make_unique<CountingDecoder>(configured); });
  MediaEngine::MediaPacket video;
  video.stream_id = 7;
  video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
  video.payload = {0, 0, 0, 1, 0x65, 0x88};

  ASSERT_TRUE(engine_.StartDuplex("call:1", CountingSend()));
  engine_.OnInboundPacket(video);
  engine_.OnInboundPacket(video);
  EXPECT_EQ(configured->load(), 1) << "one decoder per peer stream within a call";
  engine_.Stop();

  ASSERT_TRUE(engine_.StartDuplex("call:2", CountingSend()));
  engine_.OnInboundPacket(video);
  EXPECT_EQ(configured->load(), 2) << "the next call must not reuse the last call's decoder";
}

/** Decode-only stub: every access unit decodes to a 2x2 frame. */
class FrameDecoder final : public IVideoCodec {
public:
  std::string BackendName() const override { return "frame-decoder"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return true; }
  Roe<void> ConfigureEncoder(int, int, int) override { return Error("no encoder"); }
  Roe<void> ConfigureDecoder() override { return {}; }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return Error("no encoder"); }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override {
    VideoFrameRgba frame;
    frame.width = 2;
    frame.height = 2;
    frame.rgba.assign(2 * 2 * 4, 0xff);
    return std::optional<VideoFrameRgba>(std::move(frame));
  }
  void ResetEncoder() override {}
  void ResetDecoder() override {}
};

// B58: the UI trusts arriving frames over a peer's roster "camera off"; frames stop → not live.
TEST_F(MediaSessionSpecTest, RemoteVideoIsLiveOnlyWhileFramesArrive) {
  engine_.SetVideoCodecFactoryForTest([] { return std::make_unique<FrameDecoder>(); });
  ASSERT_TRUE(engine_.StartDuplex("call:live", CountingSend()));
  EXPECT_FALSE(engine_.IsRemoteVideoLive(500)) << "no frame yet";

  MediaEngine::MediaPacket video;
  video.stream_id = 7;
  video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
  video.payload = {0, 0, 0, 1, 0x65, 0x88};
  engine_.OnInboundPacket(video);
  EXPECT_TRUE(engine_.IsRemoteVideoLive(500));

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_FALSE(engine_.IsRemoteVideoLive(50)) << "no frame within the window";
  engine_.ClearRemoteVideo();
  EXPECT_FALSE(engine_.IsRemoteVideoLive(500)) << "cleared (camera off / hard stall)";
}

/** What a ScriptedDecoder's decodes return, and how often the engine reset it. */
struct DecodeScript {
  /** Empty: no frame this call (nullopt). Otherwise every decode fails with this message. */
  std::string error;
  std::atomic<int> decodes{0};
  std::atomic<int> resets{0};
};

/** Decode-only stub whose decodes follow a DecodeScript. */
class ScriptedDecoder final : public IVideoCodec {
public:
  explicit ScriptedDecoder(std::shared_ptr<DecodeScript> script) : script_(std::move(script)) {}
  std::string BackendName() const override { return "scripted-decoder"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return has_decoder_; }
  Roe<void> ConfigureEncoder(int, int, int) override { return Error("no encoder"); }
  Roe<void> ConfigureDecoder() override {
    has_decoder_ = true;
    return {};
  }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return Error("no encoder"); }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override {
    script_->decodes.fetch_add(1);
    if (!script_->error.empty()) {
      return Error(script_->error);
    }
    return std::optional<VideoFrameRgba>();
  }
  void ResetEncoder() override {}
  void ResetDecoder() override {
    has_decoder_ = false;
    script_->resets.fetch_add(1);
  }

private:
  std::shared_ptr<DecodeScript> script_;
  bool has_decoder_ = false;
};

class RemoteVideoDecodeTest : public MediaSessionSpecTest {
protected:
  void StartWith(std::shared_ptr<DecodeScript> script) {
    engine_.SetVideoCodecFactoryForTest([script] { return std::make_unique<ScriptedDecoder>(script); });
    ASSERT_TRUE(engine_.StartDuplex("call:decode", CountingSend()));
  }
  void DeliverVideo(int count) {
    MediaEngine::MediaPacket video;
    video.stream_id = 7;
    video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
    video.payload = {0, 0, 0, 1, 0x41, 0x9a};
    for (int i = 0; i < count; ++i) {
      engine_.OnInboundPacket(video);
    }
  }
  bool KeyframeRequestedFor(uint32_t stream_id) {
    for (const uint32_t id : engine_.PendingVideoRefreshStreamsForTest()) {
      if (id == stream_id) {
        return true;
      }
    }
    return false;
  }
};

// B125: a hardware decoder still starting up has no frame yet. That is not an error: no keyframe
// request (each one made the sender produce a large IDR) and no frame published.
TEST_F(RemoteVideoDecodeTest, EmptyDecodeOutputIsNotAnErrorAndAsksNoKeyframe) {
  auto script = std::make_shared<DecodeScript>();
  StartWith(script);
  DeliverVideo(3);
  EXPECT_EQ(script->decodes.load(), 3);
  EXPECT_TRUE(engine_.PendingVideoRefreshStreamsForTest().empty());
  EXPECT_FALSE(engine_.IsRemoteVideoLive(500));
  EXPECT_EQ(script->resets.load(), 0);
}

TEST_F(RemoteVideoDecodeTest, DecodeErrorStillAsksForAKeyframe) {
  auto script = std::make_shared<DecodeScript>();
  script->error = "broken session";
  StartWith(script);
  DeliverVideo(1);
  EXPECT_TRUE(KeyframeRequestedFor(7));
  EXPECT_FALSE(engine_.IsRemoteVideoLive(500));
  EXPECT_EQ(script->resets.load(), 0) << "an ordinary error never rebuilds the decoder";
}

// Two stalls in a row rebuild the decoder once; two more within ten seconds do not rebuild again
// (a rebuild runs under the media lock).
TEST_F(RemoteVideoDecodeTest, TwoStalledErrorsResetTheDecoderOnce) {
  auto script = std::make_shared<DecodeScript>();
  script->error = std::string(kDecoderStalledError);
  StartWith(script);
  DeliverVideo(2);
  EXPECT_EQ(script->resets.load(), 1);
  EXPECT_TRUE(KeyframeRequestedFor(7));
  DeliverVideo(2);
  EXPECT_EQ(script->resets.load(), 1) << "under ten seconds since the last rebuild";
  EXPECT_EQ(script->decodes.load(), 4) << "the rebuilt decoder is configured again and keeps decoding";
}

/** Decode-only stub whose destructor blocks until released, like a wedged vendor codec (B128). */
class WedgedDecoder final : public IVideoCodec {
public:
  struct Gate {
    std::atomic<bool> release{false};
    std::atomic<bool> destroyed{false};
  };
  explicit WedgedDecoder(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
  ~WedgedDecoder() override {
    // Bounded so a failing test cannot hang the run.
    WaitFor([this] { return gate_->release.load(); }, std::chrono::seconds(10));
    gate_->destroyed.store(true);
  }
  std::string BackendName() const override { return "wedged-decoder"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return true; }
  Roe<void> ConfigureEncoder(int, int, int) override { return Error("no encoder"); }
  Roe<void> ConfigureDecoder() override { return {}; }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return Error("no encoder"); }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override {
    return std::optional<VideoFrameRgba>();
  }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  std::shared_ptr<Gate> gate_;
};

// B128: Stop runs on the calls owner thread; a decoder whose teardown hangs must not hang it.
TEST_F(MediaSessionSpecTest, StopDoesNotWaitForADecoderTeardown) {
  auto gate = std::make_shared<WedgedDecoder::Gate>();
  engine_.SetVideoCodecFactoryForTest([gate] { return std::make_unique<WedgedDecoder>(gate); });
  ASSERT_TRUE(engine_.StartDuplex("call:wedged", CountingSend()));
  MediaEngine::MediaPacket video;
  video.stream_id = 7;
  video.channel_id = mesh::VideoChannel(mesh::kDefaultVideoLevel);
  video.payload = {0, 0, 0, 1, 0x65, 0x88};
  engine_.OnInboundPacket(video);

  const auto started = std::chrono::steady_clock::now();
  engine_.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
  EXPECT_FALSE(gate->destroyed.load()) << "still tearing down, off the stopping thread";

  gate->release.store(true);
  EXPECT_TRUE(WaitFor([&] { return gate->destroyed.load(); }, std::chrono::seconds(2)))
      << "the decoder is still destroyed";
}

} // namespace
} // namespace pbr
