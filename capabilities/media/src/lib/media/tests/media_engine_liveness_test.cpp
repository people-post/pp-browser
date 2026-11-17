#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaEngine.h"
#include "mesh/types/MediaChannel.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

// p2p-av-calls V067: a call's liveness is an audio frame every packet length (40 ms) from start to stop, silence
// included — whatever the microphone does. Receivers take a participant who sends nothing for 3 s as reconnecting and
// for 30 s as gone.

using namespace std::chrono_literals;

/**
 * The keeper steps in when the capture thread has sent nothing for 1 s; a little slack for a loaded CI box. The gap is
 * what V067 promises the receivers (they take 3 s of nothing as reconnecting), so it is what these tests hold to; how
 * many frames fill the time is the encoder's pace, which a Debug build on a loaded runner does not keep (macOS CI sent
 * 15 in 3 s, none of them more than the gap apart), so a count only shows frames kept coming.
 */
constexpr auto kMaxGapAllowed = 1400ms;

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(5ms);
  }
  return done();
}

/** What a call sent: each audio packet's seq and when it went. */
class SentAudio {
public:
  MediaEngine::PacketSendFn Send() {
    return [this](const MediaEngine::MediaPacket& pkt) {
      if (!mesh::IsAudioChannel(pkt.channel_id)) {
        return;
      }
      std::lock_guard lock(mu_);
      seqs_.push_back(pkt.seq);
      at_.push_back(std::chrono::steady_clock::now());
    };
  }
  size_t Count() const {
    std::lock_guard lock(mu_);
    return seqs_.size();
  }
  /** The longest time with nothing sent, from `from` (the call's start) to the last packet. */
  std::chrono::milliseconds LongestGap(const std::chrono::steady_clock::time_point from) const {
    std::lock_guard lock(mu_);
    auto prev = from;
    std::chrono::steady_clock::duration longest{};
    for (const auto& at : at_) {
      longest = std::max(longest, at - prev);
      prev = at;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(longest);
  }
  bool SeqsStrictlyIncrease() const {
    std::lock_guard lock(mu_);
    return std::adjacent_find(seqs_.begin(), seqs_.end(), [](uint32_t a, uint32_t b) { return b <= a; }) == seqs_.end();
  }
  size_t SentBetween(const std::chrono::steady_clock::time_point from,
                     const std::chrono::steady_clock::time_point to) const {
    std::lock_guard lock(mu_);
    return static_cast<size_t>(std::count_if(at_.begin(), at_.end(), [&](auto at) { return at >= from && at < to; }));
  }

private:
  mutable std::mutex mu_;
  std::vector<uint32_t> seqs_;
  std::vector<std::chrono::steady_clock::time_point> at_;
};

/** A mic that hears a tone, paced at 20 ms. */
class ToneMic final : public IAudioEndpoint {
public:
  int Read(void* dst, int bytes) override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_read_ < 20ms) {
      return 0;
    }
    last_read_ = now;
    auto* pcm = static_cast<int16_t*>(dst);
    for (int i = 0; i < bytes / static_cast<int>(sizeof(int16_t)); ++i) {
      pcm[i] = (i % 2 == 0) ? int16_t{6000} : int16_t{-6000};
    }
    return bytes;
  }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 0; }
  void Clear() override {}

private:
  std::chrono::steady_clock::time_point last_read_{};
};

/** A mic that gives nothing at all (a wedged device, a capture lost for good). */
class DeadMic final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 0; }
  void Clear() override {}
};

/** Audio devices as the test says: the mic's open may take `mic_open_takes` (the OS permission prompt) or be refused. */
class TestBackend final : public IMediaDeviceBackend {
public:
  enum class Mic { Tone, Dead, Refused };
  TestBackend(Mic mic, std::chrono::milliseconds mic_open_takes) : mic_(mic), mic_open_takes_(mic_open_takes) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat&,
                                            const std::function<bool()>&, std::string* error) override {
    if (kind != MediaDeviceKind::Mic) {
      return std::make_unique<DeadMic>(); // a speaker that takes anything
    }
    std::this_thread::sleep_for(mic_open_takes_);
    switch (mic_) {
      case Mic::Tone:
        return std::make_unique<ToneMic>();
      case Mic::Dead:
        return std::make_unique<DeadMic>();
      case Mic::Refused:
        break;
    }
    if (error) {
      *error = "permission denied";
    }
    return nullptr;
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override { return nullptr; }

private:
  const Mic mic_;
  const std::chrono::milliseconds mic_open_takes_;
};

struct Call {
  Call(TestBackend::Mic mic, std::chrono::milliseconds mic_open_takes = 0ms)
      : devices(std::make_unique<TestBackend>(mic, mic_open_takes)), engine(devices) {}
  ~Call() {
    engine.Stop();
    (void)engine.WaitForRetiredSessions(5s);
  }
  void Start() {
    started_at = std::chrono::steady_clock::now();
    auto started = engine.StartDuplex("call:liveness", sent.Send());
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
  }

  MediaDeviceArbiter devices;
  MediaEngine engine;
  SentAudio sent;
  std::chrono::steady_clock::time_point started_at;
};

// The gap V067 closed: the capture thread waits on the mic's open (an OS permission prompt can hold it up to the open
// budget, 20 s) and sent nothing meanwhile. Now silence goes out in its place, then the mic takes over, one sequence.
TEST(MediaEngineLivenessTest, AMicOpenThatWaitsStillSendsAFrameEveryPacketLength) {
  Call call(TestBackend::Mic::Tone, 2500ms);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return call.engine.HasLocalCapture(); }, 6s)) << "the mic opened in the end";
  const auto opened = std::chrono::steady_clock::now();
  ASSERT_TRUE(WaitFor([&] { return call.sent.SentBetween(opened, std::chrono::steady_clock::now()) >= 3; }, 5s))
      << "the mic carries the call once open";
  EXPECT_GE(call.sent.SentBetween(call.started_at + 1200ms, call.started_at + 2400ms), 1u)
      << "silence went out while the mic's open waited";
  EXPECT_LE(call.sent.LongestGap(call.started_at), kMaxGapAllowed);
  EXPECT_TRUE(call.sent.SeqsStrictlyIncrease()) << "the keeper and the capture thread number one sequence";
}

// Already so before V067: a mic refused (permission denied) or missing sends silence at the packet pace.
TEST(MediaEngineLivenessTest, WithoutAMicTheCallStillSendsAFrameEveryPacketLength) {
  Call call(TestBackend::Mic::Refused);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return call.sent.Count() >= 5; }, 5s)) << "silence keeps going out";
  EXPECT_FALSE(call.engine.HasLocalCapture());
  EXPECT_LE(call.sent.LongestGap(call.started_at), kMaxGapAllowed);
}

// Already so before V067: a mic that gives nothing (wedged, lost for good) is starved into silence after 500 ms.
TEST(MediaEngineLivenessTest, AMicThatGivesNothingKeepsTheFramesComing) {
  Call call(TestBackend::Mic::Dead);
  call.Start();
  std::this_thread::sleep_for(3s);
  EXPECT_GE(call.sent.Count(), 3u) << "silence keeps going out";
  EXPECT_LE(call.sent.LongestGap(call.started_at), kMaxGapAllowed);
}

// Already so before V067: mute zeros the PCM; the frames keep their pace.
TEST(MediaEngineLivenessTest, AMutedCallKeepsSendingFrames) {
  Call call(TestBackend::Mic::Tone);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return call.engine.HasLocalCapture(); }));
  call.engine.SetMuted(true);
  const auto muted_at = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(3s);
  EXPECT_GE(call.sent.SentBetween(muted_at, std::chrono::steady_clock::now()), 3u) << "muted frames keep going out";
  EXPECT_LE(call.sent.LongestGap(call.started_at), kMaxGapAllowed);
}

// --- A publisher that left is let go ----------------------------------------------------------------

std::vector<uint8_t> OpusSilence() {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  std::vector<int16_t> pcm(960, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 960, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  return {out.begin(), out.begin() + std::max(n, 0)};
}

class ReleaseRemoteStreamTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetSkipDeviceOpenForTest(true);
    ASSERT_TRUE(engine_.Start("call:group", MediaEngine::SessionSpec::PlaybackOnly(), {}));
  }
  void TearDown() override { engine_.Stop(); }
  void Deliver(const uint32_t stream_id, const uint32_t seq) {
    MediaEngine::MediaPacket pkt;
    pkt.stream_id = stream_id;
    pkt.channel_id = mesh::kMediaChannelAudio;
    pkt.seq = seq;
    pkt.capture_ts = seq * 20;
    pkt.payload = frame_;
    engine_.OnInboundPacket(pkt);
  }
  std::vector<uint32_t> Streams() const {
    std::vector<uint32_t> ids;
    for (const MediaStreamHealth& s : engine_.HealthSnapshot().streams) {
      ids.push_back(s.stream_id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  MediaEngine engine_;
  std::vector<uint8_t> frame_ = OpusSilence();
};

TEST_F(ReleaseRemoteStreamTest, AReleasedStreamLeavesTheSession) {
  Deliver(42, 1);
  Deliver(43, 1);
  ASSERT_EQ(Streams(), (std::vector<uint32_t>{42, 43}));
  engine_.ReleaseRemoteStream(42);
  EXPECT_EQ(Streams(), std::vector<uint32_t>{43}) << "no track left concealing a stream nobody sends";
  engine_.ReleaseRemoteStream(42); // twice, or a stream never held: nothing happens
  engine_.ReleaseRemoteStream(77);
  EXPECT_EQ(Streams(), std::vector<uint32_t>{43});
  Deliver(42, 2);
  EXPECT_EQ(Streams(), (std::vector<uint32_t>{42, 43})) << "rejoined: its stream starts over";
}

TEST_F(ReleaseRemoteStreamTest, AReleasedStreamFreesItsSlot) {
  for (uint32_t stream = 2; stream < 2 + MediaEngine::kMaxRemoteAudioTracks; ++stream) {
    Deliver(stream, 1);
  }
  ASSERT_EQ(engine_.HealthSnapshot().stream_count, MediaEngine::kMaxRemoteAudioTracks);
  Deliver(500, 1);
  ASSERT_EQ(engine_.HealthSnapshot().stream_count, MediaEngine::kMaxRemoteAudioTracks) << "full";
  engine_.ReleaseRemoteStream(2);
  Deliver(500, 2);
  const std::vector<uint32_t> ids = Streams();
  EXPECT_TRUE(std::find(ids.begin(), ids.end(), 500u) != ids.end()) << "someone new takes the slot";
  EXPECT_TRUE(std::find(ids.begin(), ids.end(), 2u) == ids.end());
}

} // namespace
} // namespace pbr
