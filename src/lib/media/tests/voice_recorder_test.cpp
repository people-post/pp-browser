#include "lib/media/OggOpus.h"
#include "lib/media/VoiceFile.h"
#include "lib/media/VoiceRecorder.h"
#include "voice_test_support.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

// No test here opens a microphone: every recording reads a scripted source, or the arbiter over a fake backend.

namespace pbr {
namespace {

using namespace voice_test;

VoiceRecorderConfig ConfigIn(const TempDir& dir, const int max_seconds = 120) {
  VoiceRecorderConfig config;
  config.max_seconds = max_seconds;
  config.out_dir = (dir.path / "voice-rec").string();
  return config;
}

VoiceRecorder::SourceFactory Scripted(std::vector<int16_t> pcm, ScriptedSource** out = nullptr) {
  auto shared = std::make_shared<std::vector<int16_t>>(std::move(pcm));
  return [shared, out]() {
    auto source = std::make_unique<ScriptedSource>(*shared);
    if (out) {
      *out = source.get();
    }
    return source;
  };
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(VoiceRecorder, StopKeepsAnOggFileNamedByItsLength) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted(Tone(2000)), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 2000; }));
  recorder.Finish();
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  const VoiceRecording r = probe.Get();
  ASSERT_EQ(r.end, VoiceEnd::Sent) << r.error;
  EXPECT_EQ(r.duration_ms, 2000);
  EXPECT_EQ(std::filesystem::path(r.path).filename().string(), "voice-2000.ogg");
  EXPECT_EQ(std::filesystem::path(r.path).filename().string(), VoiceFileName(r.duration_ms));
  const auto bytes = ReadAll(r.path);
  auto stream = ParseOggOpus(bytes.data(), bytes.size());
  ASSERT_TRUE(stream) << stream.error().message;
  EXPECT_EQ(stream->DurationMs(), 2000);
  EXPECT_LT(bytes.size(), 20u * 1000u); // ~24 kbps: 2 s is a few KB
  EXPECT_FALSE(recorder.IsRecording());
}

TEST(VoiceRecorder, ABitUnderASecondIsNotSentAndLeavesNoFile) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted(Tone(960)), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 900; }));
  recorder.Finish();
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::TooShort);
  EXPECT_TRUE(probe.Get().path.empty());
  EXPECT_EQ(dir.FileCount(), 0u);
}

TEST(VoiceRecorder, ExactlyTheMinimumIsSent) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted(Tone(1000)), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 1000; }));
  recorder.Finish();
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::Sent);
  EXPECT_EQ(probe.Get().duration_ms, 1000);
}

TEST(VoiceRecorder, ThePassedInLimitDecidesWhenItStops) {
  for (const int seconds : {2, 3}) {
    TempDir dir;
    RecorderProbe probe;
    VoiceRecorder recorder(Scripted(Tone(6000)), probe.Callbacks());
    ASSERT_TRUE(recorder.Start(ConfigIn(dir, seconds)));
    ASSERT_TRUE(WaitFor([&] { return probe.Done(); })) << "limit " << seconds;
    const VoiceRecording r = probe.Get();
    EXPECT_EQ(r.end, VoiceEnd::LimitReached);
    EXPECT_EQ(r.duration_ms, seconds * 1000);
    EXPECT_EQ(std::filesystem::path(r.path).filename().string(), "voice-" + std::to_string(seconds * 1000) + ".ogg");
    EXPECT_EQ(dir.FileCount(), 1u);
  }
}

TEST(VoiceRecorder, CancelLeavesNoFileAndFreesTheSource) {
  TempDir dir;
  RecorderProbe probe;
  std::atomic<bool> source_closed{false};
  VoiceRecorder recorder([&source_closed]() { return std::make_unique<ScriptedSource>(Tone(4000), "", 0, &source_closed); },
                         probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 1500; }));
  recorder.Cancel();
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::Cancelled);
  EXPECT_TRUE(probe.Get().path.empty());
  EXPECT_EQ(dir.FileCount(), 0u);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "voice-rec")); // not even the folder was made
  EXPECT_TRUE(source_closed.load());
}

TEST(VoiceRecorder, CancelWinsOverAnEarlierFinish) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted(Tone(3000)), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 1500; }));
  recorder.Finish();
  recorder.Cancel();
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  const auto end = probe.Get().end;
  // Either the cancel got in before the worker looked (no file) or the worker had already finished (a file, which
  // the caller then deletes): the one thing that must not happen is a cancelled result that left a file.
  if (end == VoiceEnd::Cancelled) {
    EXPECT_EQ(dir.FileCount(), 0u);
  } else {
    EXPECT_EQ(end, VoiceEnd::Sent);
  }
}

TEST(VoiceRecorder, ACallNeedingTheDeviceStopsItAndKeepsWhatWasSaid) {
  TempDir dir;
  RecorderProbe probe;
  ScriptedSource* source = nullptr;
  VoiceRecorder recorder(Scripted(Tone(6000), &source), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.last_progress_ms.load() >= 2000; }));
  source->interrupted_by = true;
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  const VoiceRecording r = probe.Get();
  EXPECT_EQ(r.end, VoiceEnd::Interrupted);
  EXPECT_GE(r.duration_ms, 2000);
  EXPECT_FALSE(r.path.empty());
  EXPECT_TRUE(std::filesystem::exists(r.path));
}

TEST(VoiceRecorder, ACallBeforeAnythingWasSaidLeavesNothing) {
  TempDir dir;
  RecorderProbe probe;
  ScriptedSource* source = nullptr;
  auto factory = [&source]() {
    auto s = std::make_unique<ScriptedSource>(Tone(3000));
    s->interrupted_by = true;
    source = s.get();
    return s;
  };
  VoiceRecorder recorder(factory, probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::TooShort);
  EXPECT_EQ(dir.FileCount(), 0u);
}

TEST(VoiceRecorder, OpenFailuresAreTold) {
  TempDir dir;
  {
    RecorderProbe probe;
    VoiceRecorder recorder(
        []() { return std::make_unique<ScriptedSource>(std::vector<int16_t>{}, "mic held by call-9", IVoiceCaptureSource::kOpenBusy); },
        probe.Callbacks());
    ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
    ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
    EXPECT_EQ(probe.Get().end, VoiceEnd::Busy);
    EXPECT_NE(probe.Get().error.find("call-9"), std::string::npos);
  }
  {
    RecorderProbe probe;
    VoiceRecorder recorder(
        []() { return std::make_unique<ScriptedSource>(std::vector<int16_t>{}, "no microphone", IVoiceCaptureSource::kOpenNoDevice); },
        probe.Callbacks());
    ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
    ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
    EXPECT_EQ(probe.Get().end, VoiceEnd::NoDevice);
  }
  EXPECT_EQ(dir.FileCount(), 0u);
}

TEST(VoiceRecorder, NoFolderMeansNoRecording) {
  RecorderProbe probe;
  ScriptedSource* source = nullptr;
  VoiceRecorder recorder(Scripted(Tone(2000), &source), probe.Callbacks());
  VoiceRecorderConfig config;
  config.out_dir = "";
  ASSERT_TRUE(recorder.Start(config));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::Failed);
  EXPECT_TRUE(probe.Get().path.empty());
}

TEST(VoiceRecorder, ADeviceThatGivesNoSoundFails) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted({}), probe.Callbacks());
  VoiceRecorderConfig config = ConfigIn(dir);
  config.no_data_timeout_ms = 100;
  ASSERT_TRUE(recorder.Start(config));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::Failed);
  EXPECT_EQ(dir.FileCount(), 0u);
}

TEST(VoiceRecorder, OnlyOneRecordingAtATimeAndTheNextOneWorks) {
  TempDir dir;
  RecorderProbe probe;
  VoiceRecorder recorder(Scripted(Tone(3000)), probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir, 1)));
  EXPECT_FALSE(recorder.Start(ConfigIn(dir, 1)));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  ASSERT_TRUE(WaitFor([&] { return !recorder.IsRecording(); }));
  {
    std::lock_guard lock(probe.mutex);
    probe.finished = false;
  }
  ASSERT_TRUE(recorder.Start(ConfigIn(dir, 2)));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().duration_ms, 2000);
}

// --- through the arbiter, over a fake backend: a call and a recording exclude each other ---

TEST(VoiceRecorderArbiter, ARunningCallRefusesTheMicrophoneAndNothingIsRecorded) {
  TempDir dir;
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  AudioLeaseRequest call;
  call.kind = MediaDeviceKind::Mic;
  call.holder = "call-1";
  call.format = AudioDeviceFormat{48000, 1};
  auto call_mic = arbiter.AcquireAudio(call);
  ASSERT_TRUE(call_mic);

  RecorderProbe probe;
  VoiceRecorder recorder([&arbiter]() { return CreateArbiterVoiceCaptureSource(arbiter); }, probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir)));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::Busy);
  EXPECT_NE(probe.Get().error.find("call-1"), std::string::npos);
  EXPECT_EQ(dir.FileCount(), 0u);
}

TEST(VoiceRecorderArbiter, ARecordingHoldsTheMicrophoneAgainstACallUntilItEnds) {
  TempDir dir;
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  auto source = CreateArbiterVoiceCaptureSource(arbiter);
  ASSERT_TRUE(source->Open([] { return true; }));
  EXPECT_EQ(arbiter.Holders(MediaDeviceKind::Mic), std::vector<std::string>{kVoiceRecorderHolder});

  AudioLeaseRequest call;
  call.kind = MediaDeviceKind::Mic;
  call.holder = "call-2";
  call.format = AudioDeviceFormat{48000, 1};
  EXPECT_FALSE(arbiter.AcquireAudio(call)); // exclusive: the call cannot take it from the recording
  source->Close();
  EXPECT_TRUE(arbiter.AcquireAudio(call));  // freed at the end
}

TEST(VoiceRecorderArbiter, ARingingCallInterruptsButOurOwnPlayerDoesNot) {
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  auto source = CreateArbiterVoiceCaptureSource(arbiter);
  ASSERT_TRUE(source->Open([] { return true; }));
  EXPECT_EQ(source->InterruptedBy(), "");

  AudioLeaseRequest player;
  player.kind = MediaDeviceKind::Speaker;
  player.holder = kVoicePlayerHolder;
  player.format = AudioDeviceFormat{48000, 1};
  auto own = arbiter.AcquireAudio(player);
  ASSERT_TRUE(own);
  EXPECT_EQ(source->InterruptedBy(), "");

  AudioLeaseRequest ring;
  ring.kind = MediaDeviceKind::Speaker;
  ring.holder = "ringtone";
  ring.format = AudioDeviceFormat{48000, 1};
  auto ringing = arbiter.AcquireAudio(ring);
  ASSERT_TRUE(ringing);
  EXPECT_EQ(source->InterruptedBy(), "ringtone");
}

TEST(VoiceRecorderArbiter, RecordsFromTheArbiterMicrophoneUpToTheLimit) {
  TempDir dir;
  MediaDeviceArbiter arbiter(std::make_unique<FakeBackend>());
  RecorderProbe probe;
  VoiceRecorder recorder([&arbiter]() { return CreateArbiterVoiceCaptureSource(arbiter); }, probe.Callbacks());
  ASSERT_TRUE(recorder.Start(ConfigIn(dir, 1)));
  ASSERT_TRUE(WaitFor([&] { return probe.Done(); }));
  EXPECT_EQ(probe.Get().end, VoiceEnd::LimitReached);
  EXPECT_EQ(probe.Get().duration_ms, 1000);
  EXPECT_TRUE(arbiter.Holders(MediaDeviceKind::Mic).empty()); // released
}

} // namespace
} // namespace pbr
