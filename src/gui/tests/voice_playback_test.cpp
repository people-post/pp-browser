#include "gui/chat/VoicePlayback.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/voice/tests/voice_test_support.h"

#include <gtest/gtest.h>

#include <deque>

// The bubbles' playback with a fake-backend arbiter and a test-run UI queue: no speaker is opened.

namespace pbr {
namespace {

using namespace voice_test;

/** The system's sound-file player, as the test drives it. */
struct FakeFiles {
  std::vector<std::string> played;
  std::vector<std::string> calls; // "pause", "resume", "stop"
  bool refuse = false;
  AudioFilePlayerCallbacks callbacks;
};

class FakeFilePlayer final : public IAudioFilePlayer {
public:
  explicit FakeFilePlayer(std::shared_ptr<FakeFiles> files) : files_(std::move(files)) {}
  bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) override {
    if (files_->refuse) {
      return false;
    }
    files_->played.push_back(path);
    files_->callbacks = std::move(callbacks);
    return true;
  }
  void Pause() override { files_->calls.push_back("pause"); }
  void Resume() override { files_->calls.push_back("resume"); }
  void Stop() override {
    files_->calls.push_back("stop");
    files_->callbacks = {};
  }

private:
  std::shared_ptr<FakeFiles> files_;
};

struct Env {
  struct Queue {
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
  };
  Env() {
    VoicePlaybackPorts ports;
    ports.max_seconds = []() { return 120; };
    ports.resolve = [this](const std::string&, std::function<void(std::optional<std::string>)> done) {
      pending.push_back(std::move(done));
    };
    ports.element = [](const std::string&) -> ui::Element* { return nullptr; };
    ports.say = [this](const std::string& text) { said.push_back(text); };
    ports.call_live = [this]() { return call_live; };
    ports.post_ui = [q = queue](std::function<void()> task) {
      std::lock_guard lock(q->mutex);
      q->tasks.push_back(std::move(task));
    };
    ports.make_file_player = [f = files]() { return std::make_unique<FakeFilePlayer>(f); };
    ports.file_session = [this](const bool playing) { session.push_back(playing); };
    playback = std::make_unique<VoicePlayback>(std::move(ports), arbiter);
  }
  void Pump() {
    for (;;) {
      std::function<void()> task;
      {
        std::lock_guard lock(queue->mutex);
        if (queue->tasks.empty()) {
          return;
        }
        task = std::move(queue->tasks.front());
        queue->tasks.pop_front();
      }
      task();
    }
  }
  std::shared_ptr<Queue> queue = std::make_shared<Queue>();
  MediaDeviceArbiter arbiter{CreateNullMediaDeviceBackend()};
  bool call_live = false;
  std::vector<std::function<void(std::optional<std::string>)>> pending;
  std::vector<std::string> said;
  std::shared_ptr<FakeFiles> files = std::make_shared<FakeFiles>();
  std::vector<bool> session; // file_session calls: true = playback on, false = handed back
  std::unique_ptr<VoicePlayback> playback;
};

TEST(VoicePlayback, ATapThatWasStoppedBeforeItsFileArrivedPlaysNothing) {
  Env env;
  env.playback->Toggle("m1");
  ASSERT_EQ(env.pending.size(), 1u);
  EXPECT_TRUE(env.playback->Active());
  env.playback->Stop();                    // background, a recording, a call, another chat
  EXPECT_FALSE(env.playback->Active());
  env.pending[0](std::string("/nonexistent/voice-1000.ogg")); // the file arrives late
  env.Pump();
  EXPECT_EQ(env.playback->State("m1").playing, false);
  EXPECT_FALSE(env.playback->Active());
  EXPECT_TRUE(env.said.empty());
}

TEST(VoicePlayback, ANewerTapWinsOverAnOlderLateFile) {
  Env env;
  env.playback->Toggle("m1");
  env.playback->Stop();
  env.playback->Toggle("m2");
  ASSERT_EQ(env.pending.size(), 2u);
  env.pending[0](std::string("/nonexistent/a.ogg")); // m1's late answer is ignored
  EXPECT_TRUE(env.playback->Active());               // m2 is still the one being waited for
  EXPECT_TRUE(env.said.empty());
}

TEST(VoicePlayback, NothingStartsWhileACallIsLive) {
  Env env;
  env.call_live = true;
  env.playback->Toggle("m1");
  EXPECT_TRUE(env.pending.empty());
  ASSERT_EQ(env.said.size(), 1u);
  EXPECT_EQ(env.said[0], Tr("chat.voice.play_in_call"));
}

TEST(VoicePlayback, StopThenRunsAtOnceWhenNothingPlays) {
  Env env;
  bool ran = false;
  env.playback->StopThen([&] { ran = true; });
  EXPECT_TRUE(ran);
}

// #111: an MP3 sent as a file plays in place through the system's player: play, progress, pause / resume, the end.
TEST(VoicePlayback, ASoundFilePlaysPausesResumesAndEnds) {
  Env env;
  ASSERT_TRUE(env.playback->PlaysFiles());
  env.playback->ToggleFile("f1");
  ASSERT_EQ(env.pending.size(), 1u);
  env.pending[0](std::string("/views/song.mp3"));
  ASSERT_EQ(env.files->played, std::vector<std::string>{"/views/song.mp3"});
  EXPECT_EQ(env.session, std::vector<bool>{true}) << "the loudspeaker session is taken before it plays";
  EXPECT_TRUE(env.playback->State("f1").playing);

  env.files->callbacks.on_progress(30000, 120000);
  env.Pump();
  EXPECT_EQ(env.playback->State("f1").position_ms, 30000);
  EXPECT_EQ(env.playback->State("f1").duration_ms, 120000);

  env.playback->ToggleFile("f1"); // pause
  EXPECT_TRUE(env.playback->State("f1").paused);
  env.playback->ToggleFile("f1"); // resume
  EXPECT_TRUE(env.playback->State("f1").playing);
  EXPECT_EQ(env.files->calls, (std::vector<std::string>{"pause", "resume"}));

  env.files->callbacks.on_ended(VoicePlayEnd::Finished);
  env.Pump();
  EXPECT_FALSE(env.playback->State("f1").playing);
  EXPECT_FALSE(env.playback->Active());
  EXPECT_EQ(env.session, (std::vector<bool>{true, false}));
  EXPECT_TRUE(env.said.empty());
}

TEST(VoicePlayback, OneSoundAtATimeAndStopEndsIt) {
  Env env;
  env.playback->ToggleFile("f1");
  env.pending[0](std::string("/views/a.mp3"));
  env.playback->ToggleFile("f2"); // another file: the first stops
  env.pending[1](std::string("/views/b.mp3"));
  EXPECT_EQ(env.files->calls, std::vector<std::string>{"stop"});
  EXPECT_FALSE(env.playback->State("f1").playing);
  EXPECT_TRUE(env.playback->State("f2").playing);

  env.playback->Toggle("v1"); // a voice message: the file stops first
  env.pending[2](std::string("/nonexistent/voice-1000.ogg"));
  EXPECT_EQ(env.files->calls, (std::vector<std::string>{"stop", "stop"}));
  EXPECT_FALSE(env.playback->State("f2").playing);
  env.playback->Stop();
  env.Pump();

  env.playback->ToggleFile("f3");
  env.pending[3](std::string("/views/c.mp3"));
  bool after = false;
  env.playback->StopThen([&after]() { after = true; }); // leaving the chat, a call
  EXPECT_FALSE(after);
  env.Pump();
  EXPECT_TRUE(after);
  EXPECT_FALSE(env.playback->Active());
  EXPECT_EQ(env.session.back(), false);
}

// Review 2026-11-06: a Stop handed the audio session back twice (on the stop and again on its reported end); the
// second release could end a session a call had taken in between.
TEST(VoicePlayback, StoppingASoundFileHandsTheSessionBackOnce) {
  Env env;
  env.playback->ToggleFile("f1");
  env.pending[0](std::string("/views/a.mp3"));
  env.playback->Stop();
  env.Pump();
  EXPECT_EQ(env.files->calls, std::vector<std::string>{"stop"});
  EXPECT_EQ(env.session, (std::vector<bool>{true, false}));
  EXPECT_FALSE(env.playback->Active());
}

TEST(VoicePlayback, ASoundFileTheSystemCannotReadSaysSo) {
  Env env;
  env.files->refuse = true;
  env.playback->ToggleFile("f1");
  env.pending[0](std::string("/views/broken.mp3"));
  EXPECT_FALSE(env.playback->Active());
  ASSERT_EQ(env.said.size(), 1u);
  EXPECT_EQ(env.said[0], Tr("chat.attachment.audio_cannot_play"));
}

TEST(VoicePlayback, ASoundFileTakenOverByAnotherPlayerLeavesTheSession) {
  Env env;
  env.playback->ToggleFile("f1");
  env.pending[0](std::string("/views/a.mp3"));
  env.files->callbacks.on_ended(VoicePlayEnd::Interrupted); // the storage page started one
  env.Pump();
  EXPECT_FALSE(env.playback->Active());
  EXPECT_EQ(env.session, std::vector<bool>{true}) << "the other player has the session now";
}

} // namespace
} // namespace pbr
