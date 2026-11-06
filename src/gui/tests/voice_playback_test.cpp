#include "gui/chat/VoicePlayback.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/voice/tests/voice_test_support.h"

#include "gui/chat/VoiceBubble.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
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
  std::vector<double> seeks; // seconds
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
  Roe<void> Seek(double seconds) override {
    files_->seeks.push_back(seconds);
    return {};
  }
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
  std::function<ui::Element*(const std::string&)> lookup; // the page's elements; none by default
  Env() {
    VoicePlaybackPorts ports;
    ports.max_seconds = []() { return 120; };
    ports.resolve = [this](const std::string&, std::function<void(std::optional<std::string>)> done) {
      pending.push_back(std::move(done));
    };
    ports.element = [this](const std::string& id) -> ui::Element* { return lookup ? lookup(id) : nullptr; };
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

// Seeking: the bar's release asks the player to jump to fraction x length; only for the sound file playing or paused.
TEST(VoicePlayback, SeekingASoundFileJumpsToTheFractionOfItsLength) {
  Env env;
  env.playback->ToggleFile("f1");
  env.pending[0](std::string("/views/a.mp3"));
  EXPECT_FALSE(env.playback->SeekFile("f1", 0.5)) << "the length is not known before the first progress";
  env.files->callbacks.on_progress(10000, 120000);
  env.Pump();
  EXPECT_TRUE(env.playback->SeekFile("f1", 0.5));
  ASSERT_EQ(env.files->seeks.size(), 1u);
  EXPECT_DOUBLE_EQ(env.files->seeks[0], 60.0);
  EXPECT_EQ(env.playback->State("f1").position_ms, 60000);
  EXPECT_TRUE(env.playback->SeekFile("f1", 7.0)) << "past the end stays inside the file";
  EXPECT_DOUBLE_EQ(env.files->seeks[1], 120.0);
  EXPECT_TRUE(env.playback->SeekFile("f1", -1.0));
  EXPECT_DOUBLE_EQ(env.files->seeks[2], 0.0);
  env.playback->ToggleFile("f1"); // paused: it still seeks
  EXPECT_TRUE(env.playback->SeekFile("f1", 0.25));
  EXPECT_DOUBLE_EQ(env.files->seeks[3], 30.0);
  EXPECT_FALSE(env.playback->SeekFile("other", 0.5)) << "not the one playing";
  EXPECT_EQ(env.files->seeks.size(), 4u);
}

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

// The drag knob rides the end of the fill: progress sets its left to the same percentage as the fill's width.
TEST(VoicePlayback, TheKnobMovesWithThePlaybackProgress) {
  NullRender renderer;
  ui::SetRenderInterface(&renderer);
  ASSERT_TRUE(ui::Initialise());
  ui::Context* context = ui::CreateContext("voice-knob-test", {400, 300});
  ASSERT_NE(context, nullptr);
  ui::ElementDocument* doc = context->LoadDocumentFromMemory(
      "<rml><body>" + AudioFileBubbleRml("f1", "f1", AudioCardFace{.title = "x.mp3"}, {}) + "</body></rml>");
  ASSERT_NE(doc, nullptr);
  ui::Element* knob = doc->GetElementById("vn-f1");
  ui::Element* fill = doc->GetElementById("vf-f1");
  ASSERT_NE(knob, nullptr);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(knob->GetProperty("left")->ToString(), "0%");
  {
    Env env;
    env.lookup = [doc](const std::string& id) { return doc->GetElementById(id); };
    env.playback->ToggleFile("f1");
    env.pending[0](std::string("/views/a.mp3"));
    env.files->callbacks.on_progress(30000, 120000);
    env.Pump();
    EXPECT_EQ(fill->GetProperty("width")->ToString(), "25%");
    EXPECT_EQ(knob->GetProperty("left")->ToString(), "25%");
    EXPECT_TRUE(env.playback->SeekFile("f1", 0.75)); // a drag's release moves both to the new spot
    EXPECT_EQ(fill->GetProperty("width")->ToString(), "75%");
    EXPECT_EQ(knob->GetProperty("left")->ToString(), "75%");
    env.lookup = nullptr; // the page goes away before the player does
  }
  ui::RemoveContext("voice-knob-test");
  ui::Shutdown();
  ui::SetRenderInterface(nullptr);
}

} // namespace
} // namespace pbr
