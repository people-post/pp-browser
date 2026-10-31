#include "gui/chat/VoiceComposer.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/media/tests/voice_test_support.h"

#include <gtest/gtest.h>

#include <deque>

// The microphone button's logic with a scripted microphone and fake ports: no device, no UI, no OS prompt.

namespace pbr {
namespace {

using namespace voice_test;

/** The app's UI queue, run by the test. */
struct UiQueue {
  std::mutex mutex;
  std::deque<std::function<void()>> tasks;
  void Post(std::function<void()> task) {
    std::lock_guard lock(mutex);
    tasks.push_back(std::move(task));
  }
  void RunAll() {
    for (;;) {
      std::function<void()> task;
      {
        std::lock_guard lock(mutex);
        if (tasks.empty()) {
          return;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
  }
  bool PumpUntil(const std::function<bool()>& done, std::chrono::milliseconds budget = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
      RunAll();
      if (done()) {
        return true;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
  }
};

/** A microphone whose Open waits until the test lets it go, then fails or succeeds. */
class GatedSource final : public IVoiceCaptureSource {
public:
  GatedSource(std::shared_ptr<std::atomic<bool>> gate, std::shared_ptr<std::atomic<int>> opens, bool fail)
      : gate_(std::move(gate)), opens_(std::move(opens)), fail_(fail) {}
  Roe<void> Open(const std::function<bool()>&) override {
    ++*opens_;
    while (!gate_->load()) {
      std::this_thread::sleep_for(1ms);
    }
    if (fail_) {
      return Error(kOpenNoDevice, "no microphone");
    }
    return {};
  }
  int Read(void*, int) override { return 0; }
  void Close() override {}

private:
  std::shared_ptr<std::atomic<bool>> gate_;
  std::shared_ptr<std::atomic<int>> opens_;
  bool fail_;
};

struct Harness {
  explicit Harness(int max_seconds = 120) {
    VoiceComposerPorts ports;
    ports.limits = [max_seconds]() {
      AttachmentLimits limits;
      limits.voice_max_seconds = max_seconds;
      return limits;
    };
    ports.out_dir = [this]() { return (dir.path / "voice-rec").string(); };
    ports.thread_id = [this]() { return thread; };
    ports.send = [this](const std::string& tid, const std::string& path) { sent.emplace_back(tid, path); };
    ports.eligible = []() { return true; };
    ports.dirty = []() {};
    ports.say = [this](const std::string& text) { said.push_back(text); };
    ports.call_live = [this]() { return call_live; };
    ports.fingers_down = [this]() { return fingers; };
    ports.stop_playback_then = [this](std::function<void()> then) {
      if (defer_playback_stop) {
        pending_then.push_back(std::move(then));
      } else {
        then();
      }
    };
    ports.post_ui = [q = ui_ptr](std::function<void()> task) { q->Post(std::move(task)); };
    ports.mic_state = [this]() { return mic; };
    ports.request_mic = [this](std::function<void(bool)> done) { mic_request = std::move(done); };
    ports.make_source = [cfg = source_cfg]() { return (*cfg)(); };
    composer = std::make_unique<VoiceComposer>(state, std::move(ports), arbiter);
    *source_cfg = [opens = opens_ptr]() {
      ++*opens;
      return std::make_unique<ScriptedSource>(Tone(3000));
    };
  }
  ~Harness() {
    composer->Shutdown();
    // A cancelled recorder ends by itself a few ms later; nothing it still holds points into this object.
    std::this_thread::sleep_for(50ms);
  }

  bool WaitTimer(const std::string& text) {
    return ui.PumpUntil([&] { return std::string(state.voice_timer) == text; });
  }
  bool WaitFile() {
    return WaitFor([&] { return dir.FileCount() == 1; });
  }
  bool Said(const std::string& key) const {
    return std::find(said.begin(), said.end(), Tr(key)) != said.end();
  }

  ChatScreenState state;
  std::shared_ptr<UiQueue> ui_ptr = std::make_shared<UiQueue>();
  UiQueue& ui = *ui_ptr;
  TempDir dir;
  MediaDeviceArbiter arbiter{CreateNullMediaDeviceBackend()};
  std::string thread = "A";
  bool call_live = false;
  std::vector<uint64_t> fingers;
  bool defer_playback_stop = false;
  std::vector<std::function<void()>> pending_then;
  MicPermission mic = MicPermission::Granted;
  std::function<void(bool)> mic_request;
  std::shared_ptr<std::function<std::unique_ptr<IVoiceCaptureSource>()>> source_cfg =
      std::make_shared<std::function<std::unique_ptr<IVoiceCaptureSource>()>>();
  std::function<std::unique_ptr<IVoiceCaptureSource>()>& source = *source_cfg;
  std::shared_ptr<std::atomic<int>> opens_ptr = std::make_shared<std::atomic<int>>(0);
  std::atomic<int>& opens = *opens_ptr;
  std::vector<std::pair<std::string, std::string>> sent;
  std::vector<std::string> said;
  std::unique_ptr<VoiceComposer> composer;
};

TEST(VoiceComposer, TheMessageGoesToTheChatItWasRecordedIn) {
  Harness h;
  h.composer->BeginAt(0, nullptr, /*hold=*/false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.thread = "B"; // another chat is open by the time it is sent
  h.composer->OnSendButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.sent[0].first, "A");
  EXPECT_NE(h.sent[0].second.find("voice-3000.ogg"), std::string::npos);
}

TEST(VoiceComposer, AHoldWhoseFingerIsGoneIsDiscardedAndNeverSent) {
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(100, nullptr, /*hold=*/true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  auto now = std::chrono::steady_clock::now();
  h.composer->Tick(now);
  EXPECT_TRUE(h.composer->Recording());      // the finger is still down
  h.fingers = {};                            // lifted off the window: no mouse-up will ever come
  h.composer->Tick(now + 10ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
  EXPECT_TRUE(h.said.empty());               // a lost press is silent
  h.composer->OnRelease();                   // a late release sends nothing either
  h.ui.RunAll();
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, ASecondFingerComingAndGoingDoesNotEndTheHold) {
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  auto now = std::chrono::steady_clock::now();
  h.fingers = {7, 9};
  h.composer->Tick(now);
  h.fingers = {7};
  h.composer->Tick(now + 10ms);
  EXPECT_TRUE(h.composer->Recording());
  h.composer->OnRelease();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
}

TEST(VoiceComposer, AtTheLimitItSendsOnlyIfTheFingerIsStillDown) {
  for (const bool finger_still_down : {true, false}) {
    Harness h(/*max_seconds=*/1);
    h.source = [opens = h.opens_ptr]() {
      ++*opens;
      return std::make_unique<ScriptedSource>(Tone(1000));
    };
    h.fingers = {7};
    h.composer->BeginAt(100, nullptr, true);
    ASSERT_TRUE(h.WaitFile()); // the recorder hit its limit; the UI has not seen it yet
    if (!finger_still_down) {
      h.fingers = {};
    }
    // The press stays swallowed until its release, so "done" is the send, or the file gone.
    ASSERT_TRUE(h.ui.PumpUntil([&] {
      return finger_still_down ? !h.sent.empty() : (h.dir.FileCount() == 0 && !h.composer->Recording());
    }));
    EXPECT_EQ(h.sent.size(), finger_still_down ? 1u : 0u) << "finger down: " << finger_still_down;
    if (!finger_still_down) {
      EXPECT_EQ(h.dir.FileCount(), 0u);
    }
    h.composer->OnRelease(); // the rest of the press: swallowed, never a second send
    h.ui.RunAll();
    EXPECT_EQ(h.sent.size(), finger_still_down ? 1u : 0u);
  }
}

TEST(VoiceComposer, AMouseHoldWithNoTouchIsEndedByTheRelease) {
  Harness h; // no touch fingers listed at all: a mouse on a tablet
  h.composer->BeginAt(100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->Tick(std::chrono::steady_clock::now());
  h.composer->Tick(std::chrono::steady_clock::now());
  EXPECT_TRUE(h.composer->Recording());
  h.composer->OnRelease();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
}

TEST(VoiceComposer, ACallRefusesANewRecordingAndEndsARunningOne) {
  {
    Harness h;
    h.call_live = true;
    h.composer->BeginAt(0, nullptr, false);
    EXPECT_TRUE(h.Said("chat.voice.busy_call"));
    EXPECT_EQ(h.opens.load(), 0);
    EXPECT_FALSE(h.composer->Active());
    h.composer->Refresh();
    EXPECT_TRUE(h.state.voice_blocked);
  }
  {
    Harness h; // a hold, a call rings meanwhile: what was said is sent, the rest of the press does nothing
    h.fingers = {7};
    h.composer->BeginAt(100, nullptr, true);
    ASSERT_TRUE(h.WaitTimer("0:03"));
    h.call_live = true;
    h.composer->Tick(std::chrono::steady_clock::now() + 1s);
    ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
    h.composer->OnRelease();
    h.ui.RunAll();
    EXPECT_EQ(h.sent.size(), 1u);
  }
}

TEST(VoiceComposer, LockingOrLeavingThrowsAwayARunningRecording) {
  Harness h;
  h.composer->BeginAt(0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->DiscardAll();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
  EXPECT_TRUE(h.said.empty());
}

TEST(VoiceComposer, LockingAMomentAfterTheReleaseStillSendsNothingAndDeletesTheFile) {
  Harness h;
  h.composer->BeginAt(0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnSendButton();
  ASSERT_TRUE(h.WaitFile());       // the file is written; the UI has not yet seen the result
  h.composer->DiscardAll();        // the vault locks now
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
}

TEST(VoiceComposer, ACancelIsSilentEvenWhenTheMicrophoneThenFails) {
  Harness h;
  auto gate = std::make_shared<std::atomic<bool>>(false);
  auto opens = std::make_shared<std::atomic<int>>(0);
  h.source = [=]() { return std::make_unique<GatedSource>(gate, opens, /*fail=*/true); };
  h.composer->BeginAt(0, nullptr, false);
  ASSERT_TRUE(WaitFor([&] { return opens->load() == 1; }));
  h.composer->OnCancelButton();
  gate->store(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.said.empty()) << "a cancel is not a device error";
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, AReleaseBeforeTheMicrophoneIsReadyIsTooShortNotADeviceError) {
  Harness h;
  auto gate = std::make_shared<std::atomic<bool>>(false);
  auto opens = std::make_shared<std::atomic<int>>(0);
  h.source = [=]() { return std::make_unique<GatedSource>(gate, opens, /*fail=*/true); };
  h.composer->BeginAt(0, nullptr, false);
  ASSERT_TRUE(WaitFor([&] { return opens->load() == 1; }));
  h.composer->OnSendButton();
  gate->store(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  ASSERT_EQ(h.said.size(), 1u);
  EXPECT_EQ(h.said[0], Tr("chat.voice.too_short"));
}

TEST(VoiceComposer, ARecordingWaitsForAPlayingClipToLetGoOfTheSpeaker) {
  Harness h;
  h.defer_playback_stop = true;
  h.composer->BeginAt(0, nullptr, false);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(h.opens.load(), 0);      // the clip has not let go yet: the microphone is not asked for
  ASSERT_EQ(h.pending_then.size(), 1u);
  h.pending_then[0]();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_EQ(h.opens.load(), 1);
}

TEST(VoiceComposer, CancelledWhileWaitingForTheClipNeverOpensTheMicrophone) {
  Harness h;
  h.defer_playback_stop = true;
  h.composer->BeginAt(0, nullptr, false);
  h.composer->OnCancelButton();
  h.pending_then[0]();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_TRUE(h.said.empty());
}

TEST(VoiceComposer, TheFirstPressOnlyAsksForThePermission) {
  Harness h;
  h.mic = MicPermission::NotDetermined;
  h.composer->BeginAt(0, nullptr, true);
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_FALSE(h.composer->Active());
  ASSERT_TRUE(h.mic_request != nullptr);
  h.mic_request(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.said.empty(); }));
  EXPECT_EQ(h.said[0], Tr("chat.voice.try_again_hold"));
  EXPECT_EQ(h.opens.load(), 0);

  h.said.clear();
  h.composer->BeginAt(0, nullptr, true);
  h.mic_request(false);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.said.empty(); }));
  EXPECT_EQ(h.said[0], Tr("chat.voice.permission_denied"));
}

} // namespace
} // namespace pbr
