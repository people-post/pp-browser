#include "gui/chat/VoicePlayback.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/media/tests/voice_test_support.h"

#include <gtest/gtest.h>

#include <deque>

// The bubbles' playback with a fake-backend arbiter and a test-run UI queue: no speaker is opened.

namespace pbr {
namespace {

using namespace voice_test;

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

} // namespace
} // namespace pbr
