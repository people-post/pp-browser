#include "lib/media/OggOpus.h"
#include "lib/media/VoicePlayer.h"
#include "lib/media/VoiceRecorder.h"
#include "voice_test_support.h"

#include <gtest/gtest.h>

#include <fstream>

// No test here opens a speaker or a microphone: the sink is recorded in memory or comes from a fake-backend arbiter.

namespace pbr {
namespace {

using namespace voice_test;

/** What a player reports. */
struct PlayerProbe {
  std::mutex mutex;
  std::vector<std::pair<std::string, VoicePlayEnd>> ended;
  std::atomic<int64_t> progress_ms{0};
  std::atomic<int64_t> duration_ms{0};

  VoicePlayerCallbacks Callbacks() {
    VoicePlayerCallbacks cb;
    cb.on_progress = [this](const std::string&, int64_t pos, int64_t total) {
      progress_ms = pos;
      duration_ms = total;
    };
    cb.on_ended = [this](const std::string& id, VoicePlayEnd end) {
      std::lock_guard lock(mutex);
      ended.emplace_back(id, end);
    };
    return cb;
  }
  size_t Count() {
    std::lock_guard lock(mutex);
    return ended.size();
  }
  std::pair<std::string, VoicePlayEnd> At(size_t i) {
    std::lock_guard lock(mutex);
    return ended.at(i);
  }
};

/** A real recording of a tone, made through the recorder with a scripted source. */
std::string MakeVoiceFile(const TempDir& dir, const int ms) {
  RecorderProbe probe;
  VoiceRecorder recorder([ms]() { return std::make_unique<ScriptedSource>(Tone(ms)); }, probe.Callbacks());
  VoiceRecorderConfig config;
  config.out_dir = (dir.path / "rec").string();
  recorder.Start(config);
  WaitFor([&] { return probe.last_progress_ms.load() >= ms; });
  recorder.Finish();
  WaitFor([&] { return probe.Done(); });
  return probe.Get().path;
}

VoicePlayer::SinkFactory Sink(const std::shared_ptr<RecordingSink::State>& state) {
  return [state]() { return std::make_unique<RecordingSink>(state); };
}

TEST(VoicePlayer, PlaysARecordingToTheEndAndSaysSo) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 1500);
  ASSERT_FALSE(path.empty());
  auto state = std::make_shared<RecordingSink::State>();
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("m1", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0), std::make_pair(std::string("m1"), VoicePlayEnd::Finished));
  EXPECT_EQ(state->Written(), 72000u); // 1.5 s: pre-skip and padding were dropped
  EXPECT_EQ(probe.duration_ms.load(), 1500);
  EXPECT_EQ(state->opens.load(), 1);
  EXPECT_EQ(state->closes.load(), 1);
  // The tone came through the codec: not silence.
  int64_t energy = 0;
  {
    std::lock_guard lock(state->mutex);
    for (const int16_t v : state->written) {
      energy += std::abs(static_cast<int>(v));
    }
  }
  EXPECT_GT(energy, 1000000);
  EXPECT_EQ(player.CurrentId(), "");
}

TEST(VoicePlayer, PlaysTheFfmpegReferenceFile) {
  auto state = std::make_shared<RecordingSink::State>();
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("ref", std::string(PP_BROWSER_MEDIA_TEST_DATA_DIR) + "/voice_ref_1500ms.ogg", 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Finished);
  EXPECT_EQ(state->Written(), 72000u);
}

TEST(VoicePlayer, GarbageAndMissingFilesAreUnplayableAndNeverOpenTheSpeaker) {
  TempDir dir;
  const std::string garbage = (dir.path / "voice-1000.ogg").string();
  {
    std::ofstream out(garbage, std::ios::binary);
    for (int i = 0; i < 4000; ++i) {
      out.put(static_cast<char>(i * 31));
    }
  }
  const std::string good = MakeVoiceFile(dir, 1500);
  std::string cut;
  {
    std::ifstream in(good, std::ios::binary);
    cut.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    cut.resize(cut.size() / 2);
  }
  const std::string truncated = (dir.path / "voice-1500.ogg").string();
  std::ofstream(truncated, std::ios::binary).write(cut.data(), static_cast<std::streamsize>(cut.size()));

  auto state = std::make_shared<RecordingSink::State>();
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  size_t expected = 0;
  for (const std::string& path : {garbage, truncated, (dir.path / "missing.ogg").string()}) {
    player.Play("x", path, 120);
    ++expected;
    ASSERT_TRUE(WaitFor([&] { return probe.Count() == expected; })) << path;
    EXPECT_EQ(probe.At(expected - 1).second, VoicePlayEnd::Unplayable) << path;
  }
  EXPECT_EQ(state->opens.load(), 0);
}

TEST(VoicePlayer, AFileBiggerThanAVoiceMessageCanBeIsNotEvenRead) {
  TempDir dir;
  const std::string path = (dir.path / "voice-1000.ogg").string();
  {
    std::ofstream out(path, std::ios::binary);
    const std::string chunk(1024, 'x');
    for (int i = 0; i < 1100; ++i) { // 1.1 MB
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }
  auto state = std::make_shared<RecordingSink::State>();
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("big", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Unplayable);
  EXPECT_EQ(state->opens.load(), 0);
}

TEST(VoicePlayer, AClipLongerThanTheVoiceLimitIsUnplayable) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 3000);
  auto state = std::make_shared<RecordingSink::State>();
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("long", path, 1); // limit 1 s (+1 s of slack), clip is 3 s
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Unplayable);
  EXPECT_EQ(state->opens.load(), 0);
}

TEST(VoicePlayer, OneAtATime) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 2000);
  auto state = std::make_shared<RecordingSink::State>();
  state->draining = false; // the speaker never empties: both clips would play for ever
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("a", path, 120);
  ASSERT_TRUE(WaitFor([&] { return state->Written() > 0 && player.CurrentId() == "a"; }));
  player.Play("b", path, 120);
  // "a" was ended (Stopped) before "b" began.
  ASSERT_GE(probe.Count(), 1u);
  EXPECT_EQ(probe.At(0), std::make_pair(std::string("a"), VoicePlayEnd::Stopped));
  EXPECT_EQ(player.CurrentId(), "b");
  ASSERT_TRUE(WaitFor([&] { return state->opens.load() == 2; }));
  EXPECT_EQ(state->closes.load(), 1);  // a's speaker was closed before b's opened
  player.Stop();
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 2; }));
  EXPECT_EQ(probe.At(1), std::make_pair(std::string("b"), VoicePlayEnd::Stopped));
  EXPECT_EQ(player.CurrentId(), "");
}

TEST(VoicePlayer, PauseHoldsAndResumeContinuesFromWhereItWasHeard) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 2000);
  auto state = std::make_shared<RecordingSink::State>();
  state->draining = false;
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("m", path, 120);
  // The speaker fills (5 chunks = 100 ms) and the player waits.
  ASSERT_TRUE(WaitFor([&] { return state->queued.load() >= 5 * 1920; }));
  player.Pause();
  EXPECT_TRUE(player.IsPaused());
  ASSERT_TRUE(WaitFor([&] { return state->clears.load() >= 1; })); // the queue was thrown away
  const size_t written_at_pause = state->Written();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(state->Written(), written_at_pause);   // nothing is fed while paused
  EXPECT_EQ(player.CurrentId(), "m");              // and it is still "the clip"
  EXPECT_EQ(probe.Count(), 0u);
  state->draining = true;
  player.Resume();
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }, 10s));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Finished);
  // Heard-but-unplayed audio was fed again: more than the clip's 96000 samples were written in total.
  EXPECT_GT(state->Written(), 96000u);
  EXPECT_LE(state->Written(), 96000u + 6 * 960u);
}

TEST(VoicePlayer, AnIncomingCallStopsPlayback) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 2000);
  auto state = std::make_shared<RecordingSink::State>();
  state->draining = false;
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return state->Written() > 0; }));
  state->interrupted = true; // the ringtone took the speaker
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Interrupted);
  EXPECT_EQ(state->closes.load(), 1);
}

TEST(VoicePlayer, NotPlayedWhileTheDeviceIsInUse) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 1500);
  auto state = std::make_shared<RecordingSink::State>();
  state->open_busy = true;
  PlayerProbe probe;
  VoicePlayer player(Sink(state), probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Busy);
  EXPECT_EQ(state->Written(), 0u);
}

// --- through the arbiter, over a fake backend ---

AudioLeaseRequest Lease(const MediaDeviceKind kind, const std::string& holder) {
  AudioLeaseRequest request;
  request.kind = kind;
  request.holder = holder;
  request.format = AudioDeviceFormat{48000, 1};
  return request;
}

TEST(VoicePlayerArbiter, ACallInProgressRefusesPlayback) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 1500);
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  auto call_speaker = arbiter.AcquireAudio(Lease(MediaDeviceKind::Speaker, "call-1"));
  auto call_mic = arbiter.AcquireAudio(Lease(MediaDeviceKind::Mic, "call-1"));
  ASSERT_TRUE(call_speaker);
  ASSERT_TRUE(call_mic);
  PlayerProbe probe;
  VoicePlayer player([&arbiter]() { return CreateArbiterVoicePlaybackSink(arbiter); }, probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Busy);
  EXPECT_EQ(arbiter.Holders(MediaDeviceKind::Speaker), std::vector<std::string>{"call-1"}); // we took nothing
}

TEST(VoicePlayerArbiter, APlayingClipTakesThenFreesTheSpeaker) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 1500);
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  PlayerProbe probe;
  VoicePlayer player([&arbiter]() { return CreateArbiterVoicePlaybackSink(arbiter); }, probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }, 10s));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Finished);
  EXPECT_TRUE(arbiter.Holders(MediaDeviceKind::Speaker).empty());
}

TEST(VoicePlayerArbiter, ARingingCallStopsAClipThatIsPlaying) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 2000);
  // A speaker that keeps its queue: the clip is still playing when the ringtone arrives.
  class FullSpeaker final : public IAudioEndpoint {
  public:
    int Read(void*, int) override { return 0; }
    bool Write(const void*, int) override { return true; }
    int Queued() const override { return 1 << 20; }
    void Clear() override {}
  };
  class FullBackend final : public IMediaDeviceBackend {
  public:
    std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                              std::string*) override {
      return std::make_unique<FullSpeaker>();
    }
  };
  MediaDeviceArbiter arbiter(std::make_unique<FullBackend>());
  PlayerProbe probe;
  VoicePlayer player([&arbiter]() { return CreateArbiterVoicePlaybackSink(arbiter); }, probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return arbiter.Holders(MediaDeviceKind::Speaker).size() == 1; }));
  auto ring = arbiter.AcquireAudio(Lease(MediaDeviceKind::Speaker, "ringtone"));
  ASSERT_TRUE(ring);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Interrupted);
}

TEST(VoicePlayerArbiter, ARecordingInProgressRefusesPlayback) {
  TempDir dir;
  const std::string path = MakeVoiceFile(dir, 1500);
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  auto mic = arbiter.AcquireAudio(Lease(MediaDeviceKind::Mic, kVoiceRecorderHolder));
  ASSERT_TRUE(mic);
  PlayerProbe probe;
  VoicePlayer player([&arbiter]() { return CreateArbiterVoicePlaybackSink(arbiter); }, probe.Callbacks());
  player.Play("m", path, 120);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  EXPECT_EQ(probe.At(0).second, VoicePlayEnd::Busy);
}

} // namespace
} // namespace pbr
