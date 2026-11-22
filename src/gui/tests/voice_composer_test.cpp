#include "gui/chat/VoiceComposer.h"
#include "feature/presenters/VoiceSendQueue.h"
#include "lib/voice/VoiceSupport.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/voice/tests/voice_test_support.h"

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <set>
#include <fstream>

// The microphone button's logic with a scripted microphone and fake ports: no device, no UI, no OS prompt.

namespace pbr {

/** Lets a test deliver a progress tick by hand, as a recorder's late post would. */
struct VoiceComposerTestAccess {
  static void Progress(VoiceComposer& composer, int generation, int64_t elapsed_ms, float level = 0.f) {
    composer.OnProgress(generation, elapsed_ms, level);
  }
};
namespace {

using namespace voice_test;

// Linux has no voice messages (VoiceSupport_Linux.cpp): the microphone button is hidden and these tests do not apply.
#define SKIP_WITHOUT_VOICE()                                             \
  if (!VoiceMessagesSupported()) {                                       \
    GTEST_SKIP() << "voice messages are not supported on this platform"; \
  }

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

/** A moment well past the press: a hold that is not a tap (the scripted microphone is instant, the clock is not). */
std::chrono::steady_clock::time_point Later() {
  return std::chrono::steady_clock::now() + 2s;
}

struct Harness {
  explicit Harness(int max_seconds = 120) {
    VoiceComposerPorts ports;
    ports.limits = [max_seconds]() {
      AttachmentLimits limits;
      limits.voice_max_seconds = max_seconds;
      return limits;
    };
    ports.out_dir = [this]() { return (dir.path / "voice-rec").string(); };
    ports.keep_files = [this]() {
      std::vector<std::string> files = waiting();
      if (!keep.empty()) {
        files.push_back(keep);
      }
      return files;
    };
    ports.thread_id = [this]() { return thread; };
    ports.send = [this](const std::string& tid, const std::string& path) {
      sent.emplace_back(tid, path);
      if (on_send) {
        on_send(tid, path);
      }
    };
    ports.eligible = [this]() { return eligible; };
    ports.dirty = [this]() {
      saw_finishing = saw_finishing || state.voice_finishing;
      ++dirty_calls;
      // The controller's own port: the model changed, so the composer's input state is synced, which refreshes the mic.
      if (refresh_on_dirty && composer && dirty_calls < 1000) {
        composer->Refresh();
      }
    };
    ports.say = [this](const std::string& text) { said.push_back(text); };
    ports.call_live = [this]() { return call_live; };
    ports.queue_full = [this](const std::string& tid) { return tid == full_thread; };
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
    ports.dp_ratio = [this]() { return ratio; };
    ports.viewport_width_dp = [this]() { return viewport_dp; };
    ports.top_limit_dp = [this]() { return top_limit_dp; };
    ports.overlay_top_dp = [this]() { return overlay_top_dp; };
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
  bool Said(const std::string& key) const { return std::find(said.begin(), said.end(), Tr(key)) != said.end(); }

  ChatScreenState state;
  std::shared_ptr<UiQueue> ui_ptr = std::make_shared<UiQueue>();
  UiQueue& ui = *ui_ptr;
  TempDir dir;
  MediaDeviceArbiter arbiter{CreateNullMediaDeviceBackend()};
  std::string thread = "A";
  std::string keep;
  /** The recordings the send line still holds (the chat screen's VoiceSendQueue::FilesInUse). */
  std::function<std::vector<std::string>()> waiting = []() { return std::vector<std::string>(); };
  bool call_live = false;
  /** The chat whose voice messages are at the most that may wait (none when empty). */
  std::string full_thread;
  /** The chat can take voice at all. */
  bool eligible = true;
  /** The window's pixels per dp, its width in dp, and the lowest y the bubble's top may take (dp). */
  float ratio = 1.f;
  float viewport_dp = 393.f;
  float top_limit_dp = 0.f;
  float overlay_top_dp = 0.f;
  /** voice_finishing was true at some point (it is cleared again by the time a test looks). */
  bool saw_finishing = false;
  int dirty_calls = 0;
  bool refresh_on_dirty = false;
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
  std::function<void(const std::string& tid, const std::string& path)> on_send;
  std::vector<std::string> said;
  std::unique_ptr<VoiceComposer> composer;
};

// Where voice messages are not supported, the microphone button never shows and a press does nothing.
TEST(VoiceComposer, NoButtonWherePlatformHasNoVoiceMessages) {
  if (VoiceMessagesSupported()) {
    GTEST_SKIP() << "this platform supports voice messages";
  }
  Harness h;
  h.composer->Refresh();
  EXPECT_FALSE(h.state.show_voice);
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  h.ui.RunAll();
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, TheMessageGoesToTheChatItWasRecordedIn) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.thread = "B"; // another chat is open by the time it is sent
  h.composer->OnSendButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.sent[0].first, "A");
  EXPECT_NE(h.sent[0].second.find("voice-3000.ogg"), std::string::npos);
}

TEST(VoiceComposer, AHoldWhoseFingerIsGoneIsDiscardedAndNeverSent) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, /*hold=*/true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  auto now = std::chrono::steady_clock::now();
  h.composer->Tick(now);
  EXPECT_TRUE(h.composer->Recording()); // the finger is still down
  h.fingers = {};                       // lifted off the window: no mouse-up will ever come
  h.composer->Tick(now + 10ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
  EXPECT_TRUE(h.said.empty());    // a lost press is silent
  h.composer->OnRelease(Later()); // a late release sends nothing either
  h.ui.RunAll();
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, ASecondFingerComingAndGoingDoesNotEndTheHold) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  auto now = std::chrono::steady_clock::now();
  h.fingers = {7, 9};
  h.composer->Tick(now);
  h.fingers = {7};
  h.composer->Tick(now + 10ms);
  EXPECT_TRUE(h.composer->Recording());
  h.composer->OnRelease(Later());
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
}

/** A recorder that hits its 1 s limit at once; `h.fingers` already holds the pressing finger. */
void PressAtTheLimit(Harness& h) {
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(1000));
  };
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
}

// At the limit under a finger the recorder stops and nothing is sent until the person lets go.
TEST(VoiceComposer, AtTheLimitUnderAFingerItWaitsForTheReleaseAndTheReleaseSends) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  PressAtTheLimit(h);
  ASSERT_TRUE(h.WaitFile()); // the recorder hit its limit
  ASSERT_TRUE(h.ui.PumpUntil([&] { return h.state.voice_limit; }));
  EXPECT_TRUE(h.sent.empty()) << "no send without the release";
  EXPECT_TRUE(h.state.voice_recording) << "the bar and the bubble still show the hold";
  EXPECT_FALSE(h.state.voice_finishing) << "nothing is being sent yet";
  EXPECT_EQ(h.dir.FileCount(), 1u);
  EXPECT_EQ(std::string(h.state.voice_hint), Tr("chat.voice.limit_reached", {{"minutes", "1"}}));
  EXPECT_TRUE(h.composer->Active());
  h.composer->OnRelease(Later());
  ASSERT_EQ(h.sent.size(), 1u);
  EXPECT_FALSE(h.state.voice_limit);
  EXPECT_FALSE(h.state.voice_recording);
  EXPECT_FALSE(h.composer->Active());
}

TEST(VoiceComposer, AtTheLimitASlideUpAndReleaseCancelsAndDeletesTheFile) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  PressAtTheLimit(h);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return h.state.voice_limit; }));
  h.composer->OnMove(0, 100 - 60); // up past the cancel line
  EXPECT_TRUE(h.state.voice_cancel_zone);
  h.composer->OnRelease(Later());
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
  EXPECT_TRUE(h.said.empty()) << "a cancel is silent";
  EXPECT_FALSE(h.composer->Active());
}

TEST(VoiceComposer, AtTheLimitSlidingBackDownAndReleasingSendsAfterAll) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  PressAtTheLimit(h);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return h.state.voice_limit; }));
  h.composer->OnMove(0, 40);
  h.composer->OnMove(0, 95); // back down: the cancel is off again
  EXPECT_FALSE(h.state.voice_cancel_zone);
  h.composer->OnRelease(Later());
  EXPECT_EQ(h.sent.size(), 1u);
}

TEST(VoiceComposer, AtTheLimitEveryOtherWayOutDeletesTheFileAndSendsNothing) {
  SKIP_WITHOUT_VOICE();
  enum class Exit { FingerGone, LostPress, DiscardAll, ACall };
  for (const Exit exit : {Exit::FingerGone, Exit::LostPress, Exit::DiscardAll, Exit::ACall}) {
    Harness h(/*max_seconds=*/1);
    PressAtTheLimit(h);
    if (exit == Exit::FingerGone) {
      ASSERT_TRUE(h.WaitFile());
      h.fingers = {}; // lifted before the UI saw the limit: no release will ever come
      ASSERT_TRUE(h.ui.PumpUntil([&] { return h.dir.FileCount() == 0 && !h.composer->Active(); }));
    } else {
      ASSERT_TRUE(h.ui.PumpUntil([&] { return h.state.voice_limit; }));
      if (exit == Exit::LostPress) {
        h.composer->Lost(); // another chat opened, the app went to the background
      } else if (exit == Exit::DiscardAll) {
        h.composer->DiscardAll(); // the vault locked
      } else {
        h.call_live = true;
        h.composer->Tick(std::chrono::steady_clock::now() + 1s);
      }
    }
    EXPECT_TRUE(h.sent.empty()) << static_cast<int>(exit);
    EXPECT_EQ(h.dir.FileCount(), 0u) << static_cast<int>(exit);
    EXPECT_FALSE(h.state.voice_limit) << static_cast<int>(exit);
    EXPECT_FALSE(h.state.voice_recording) << static_cast<int>(exit);
    EXPECT_FALSE(h.composer->Active()) << static_cast<int>(exit);
    EXPECT_EQ(std::count(h.said.begin(), h.said.end(), Tr("chat.voice.interrupted_by_call")),
              exit == Exit::ACall ? 1 : 0);
    h.composer->OnRelease(Later()); // the lift that comes later: nothing
    EXPECT_TRUE(h.sent.empty());
  }
}

// The desktop's click mode still ends and sends by itself at the limit (no finger to wait for).
TEST(VoiceComposer, OnTheDesktopTheLimitStillSendsByItself) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(1000));
  };
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  EXPECT_FALSE(h.state.voice_bubble) << "no bubble on the desktop";
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_FALSE(h.state.voice_limit);
}

TEST(VoiceComposer, AMouseHoldWithNoTouchIsEndedByTheRelease) {
  SKIP_WITHOUT_VOICE();
  Harness h; // no touch fingers listed at all: a mouse on a tablet
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->Tick(std::chrono::steady_clock::now());
  h.composer->Tick(std::chrono::steady_clock::now());
  EXPECT_TRUE(h.composer->Recording());
  h.composer->OnRelease(Later());
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
}

TEST(VoiceComposer, ACallRefusesANewRecording) {
  SKIP_WITHOUT_VOICE();
  {
    Harness h;
    h.call_live = true;
    h.composer->BeginAt(0, 0, nullptr, false);
    EXPECT_TRUE(h.Said("chat.voice.busy_call"));
    EXPECT_EQ(h.opens.load(), 0);
    EXPECT_FALSE(h.composer->Active());
    h.composer->Refresh();
    EXPECT_TRUE(h.state.voice_blocked);
  }
}

// Five voice messages of this chat still going out: the bar is greyed (still shown, still in voice mode), a press says
// why and records nothing; another chat is not affected, and the bar works again once the count drops.
TEST(VoiceComposer, AChatWithFiveVoiceMessagesOutRefusesANewOneUntilOneEnds) {
  SKIP_WITHOUT_VOICE();
  {
    Harness h;
    h.state.voice_mode = true;
    h.full_thread = "A";
    h.composer->Refresh();
    EXPECT_TRUE(h.state.show_voice);
    EXPECT_TRUE(h.state.voice_mode) << "greyed, not taken out of voice mode";
    EXPECT_TRUE(h.state.voice_blocked);
    h.composer->BeginAt(0, 0, nullptr, false);
    EXPECT_TRUE(h.Said("chat.voice.queue_full"));
    EXPECT_FALSE(h.Said("chat.voice.busy_call"));
    EXPECT_EQ(h.opens.load(), 0);
    EXPECT_FALSE(h.composer->Active());

    h.thread = "B"; // another chat
    h.composer->Refresh();
    EXPECT_FALSE(h.state.voice_blocked);

    h.thread = "A";
    h.full_thread.clear(); // one of A's ended
    h.composer->Refresh();
    EXPECT_FALSE(h.state.voice_blocked);
  }
}

// A call that arrives while recording throws the recording away: one pill, no file, nothing sent, whichever of the two
// paths (the composer's clock tick, the recorder ending as Interrupted) sees it first.
TEST(VoiceComposer, ACallDuringARecordingDiscardsItOnceWhicheverPathComesFirst) {
  SKIP_WITHOUT_VOICE();
  for (const bool tick_first : {true, false}) {
    Harness h;
    ScriptedSource* source = nullptr;
    h.source = [opens = h.opens_ptr, &source]() {
      ++*opens;
      auto made = std::make_unique<ScriptedSource>(Tone(3000));
      source = made.get();
      return made;
    };
    h.fingers = {7};
    h.composer->BeginAt(0, 100, nullptr, true);
    ASSERT_TRUE(h.WaitTimer("0:03"));
    ASSERT_NE(source, nullptr);
    const auto tick = [&]() {
      h.call_live = true;
      h.composer->Tick(std::chrono::steady_clock::now() + 1s);
    };
    if (tick_first) {
      tick();
      ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
    } else {
      source->interrupted_by = true; // the OS took the microphone: the recorder ends as Interrupted
      ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
      tick(); // the clock tick sees the call afterwards
    }
    h.ui.RunAll();
    std::this_thread::sleep_for(30ms);
    h.ui.RunAll();
    EXPECT_EQ(std::count(h.said.begin(), h.said.end(), Tr("chat.voice.interrupted_by_call")), 1)
        << "tick first: " << tick_first;
    EXPECT_EQ(h.said.size(), 1u);
    EXPECT_TRUE(h.sent.empty());
    EXPECT_EQ(h.dir.FileCount(), 0u);
    EXPECT_FALSE(h.state.voice_recording);
    EXPECT_FALSE(h.state.voice_bubble);
    h.composer->OnRelease(Later()); // the finger comes up later: nothing
    h.ui.RunAll();
    EXPECT_TRUE(h.sent.empty());
  }
}

TEST(VoiceComposer, LockingOrLeavingThrowsAwayARunningRecording) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->DiscardAll();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
  EXPECT_TRUE(h.said.empty());
}

TEST(VoiceComposer, ANewRecordingDoesNotWipeAFailedOneWaitingForRetry) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  const auto dir = h.dir.path / "voice-rec";
  std::filesystem::create_directories(dir);
  const auto kept = dir / "failed.m4a";
  const auto other = dir / "older.m4a";
  std::ofstream(kept) << "kept";
  std::ofstream(other) << "old";
  h.keep = kept.string();
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_TRUE(std::filesystem::exists(kept));
  EXPECT_FALSE(std::filesystem::exists(other));
}

// Device test of 66b4bede: recordings made while an earlier voice message was still uploading failed with "cannot read
// this file" when their turn came: starting the next recording wiped the recording folder, taking the ones still
// waiting in the send line with it. Recorded one after another and handed to a real VoiceSendQueue whose slot is busy,
// each must still be on disk, with its own bytes, when its turn comes.
struct WaitingLine {
  explicit WaitingLine(Harness& h) {
    VoiceSendPorts ports;
    ports.slot_free = []() { return false; }; // an earlier voice message is still uploading
    line = std::make_unique<VoiceSendQueue>(std::move(ports));
    h.on_send = [this](const std::string& tid, const std::string& path) {
      VoiceSendJob job;
      job.row_id = "pending-upload-" + std::to_string(++rows);
      job.thread_id = tid;
      job.path = path;
      std::ifstream in(path, std::ios::binary);
      bytes.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      line->Enqueue(std::move(job));
    };
    h.waiting = [this]() { return line->FilesInUse(); };
  }
  /** Every recording handed over is still there with the bytes it had then. */
  void ExpectAllIntact(const Harness& h) const {
    ASSERT_EQ(h.sent.size(), bytes.size());
    std::set<std::string> paths;
    for (size_t i = 0; i < h.sent.size(); ++i) {
      const std::string& path = h.sent[i].second;
      std::ifstream in(path, std::ios::binary);
      const std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_TRUE(std::filesystem::exists(path)) << "recording " << i + 1 << " was deleted before its turn";
      EXPECT_EQ(now, bytes[i]) << "recording " << i + 1 << " is not the bytes that were recorded";
      paths.insert(path);
    }
    EXPECT_EQ(paths.size(), h.sent.size()) << "each recording is its own file";
  }
  std::unique_ptr<VoiceSendQueue> line;
  std::vector<std::string> bytes;
  int rows = 0;
};

void RecordAndSend(Harness& h, const int seconds) {
  h.source = [opens = h.opens_ptr, seconds]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(seconds * 1000));
  };
  const size_t before = h.sent.size();
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  ASSERT_TRUE(h.WaitTimer("0:0" + std::to_string(seconds)));
  h.composer->OnSendButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return h.sent.size() == before + 1 && !h.composer->Active(); }));
}

TEST(VoiceComposer, RecordingsWaitingToBeSentSurviveTheNextRecording) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  WaitingLine waiting(h);
  RecordAndSend(h, 3);
  RecordAndSend(h, 2);
  RecordAndSend(h, 1);
  ASSERT_EQ(waiting.line->WaitingCount(), 3u);
  waiting.ExpectAllIntact(h);
}

// The file is named after its length (the recipient reads the length from it): two recordings of the same length
// waiting at once must not be one file, the second overwriting the first.
TEST(VoiceComposer, TwoWaitingRecordingsOfTheSameLengthAreTwoFiles) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  WaitingLine waiting(h);
  RecordAndSend(h, 2);
  RecordAndSend(h, 2);
  ASSERT_EQ(waiting.line->WaitingCount(), 2u);
  waiting.ExpectAllIntact(h);
  EXPECT_EQ(std::filesystem::path(h.sent[0].second).filename(), std::filesystem::path(h.sent[1].second).filename())
      << "the name the recipient sees is unchanged";
}

TEST(VoiceComposer, LockingAMomentAfterTheReleaseStillSendsNothingAndDeletesTheFile) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnSendButton();
  ASSERT_TRUE(h.WaitFile()); // the file is written; the UI has not yet seen the result
  h.composer->DiscardAll();  // the vault locks now
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
}

TEST(VoiceComposer, ACancelIsSilentEvenWhenTheMicrophoneThenFails) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  auto gate = std::make_shared<std::atomic<bool>>(false);
  auto opens = std::make_shared<std::atomic<int>>(0);
  h.source = [=]() { return std::make_unique<GatedSource>(gate, opens, /*fail=*/true); };
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(WaitFor([&] { return opens->load() == 1; }));
  h.composer->OnCancelButton();
  gate->store(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.said.empty()) << "a cancel is not a device error";
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, AReleaseBeforeTheMicrophoneIsReadyIsTooShortNotADeviceError) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  auto gate = std::make_shared<std::atomic<bool>>(false);
  auto opens = std::make_shared<std::atomic<int>>(0);
  h.source = [=]() { return std::make_unique<GatedSource>(gate, opens, /*fail=*/true); };
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(WaitFor([&] { return opens->load() == 1; }));
  h.composer->OnSendButton();
  gate->store(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  ASSERT_EQ(h.said.size(), 1u);
  EXPECT_EQ(h.said[0], Tr("chat.voice.too_short"));
}

TEST(VoiceComposer, ARecordingWaitsForAPlayingClipToLetGoOfTheSpeaker) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.defer_playback_stop = true;
  h.composer->BeginAt(0, 0, nullptr, false);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(h.opens.load(), 0); // the clip has not let go yet: the microphone is not asked for
  ASSERT_EQ(h.pending_then.size(), 1u);
  h.pending_then[0]();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_EQ(h.opens.load(), 1);
}

TEST(VoiceComposer, CancelledWhileWaitingForTheClipNeverOpensTheMicrophone) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.defer_playback_stop = true;
  h.composer->BeginAt(0, 0, nullptr, false);
  h.composer->OnCancelButton();
  h.pending_then[0]();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_TRUE(h.said.empty());
}

TEST(VoiceComposer, TheFirstPressOnlyAsksForThePermission) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.mic = MicPermission::NotDetermined;
  h.composer->BeginAt(0, 0, nullptr, true);
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_FALSE(h.composer->Active());
  ASSERT_TRUE(h.mic_request != nullptr);
  h.mic_request(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.said.empty(); }));
  EXPECT_EQ(h.said[0], Tr("chat.voice.try_again_hold"));
  EXPECT_EQ(h.opens.load(), 0);

  h.said.clear();
  h.composer->BeginAt(0, 0, nullptr, true);
  h.mic_request(false);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.said.empty(); }));
  EXPECT_EQ(h.said[0], Tr("chat.voice.permission_denied"));
}

// "Sending…" bridges the gap between the release and the upload chip: set when the recording is handed to the
// encoder, cleared on every way the recording ends.
TEST(VoiceComposer, ReleasingToSendShowsSendingUntilTheFileArrives) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_FALSE(h.state.voice_finishing);
  h.composer->OnSendButton();
  EXPECT_TRUE(h.state.voice_finishing);
  EXPECT_FALSE(h.state.voice_recording);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_FALSE(h.state.voice_finishing);
}

TEST(VoiceComposer, ACancelNeverShowsSending) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnCancelButton();
  EXPECT_FALSE(h.state.voice_finishing);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_FALSE(h.state.voice_finishing);
  EXPECT_FALSE(h.saw_finishing);
}

TEST(VoiceComposer, ALostPressNeverShowsSending) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, /*hold=*/true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->Lost();
  EXPECT_FALSE(h.state.voice_finishing);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_FALSE(h.state.voice_finishing);
  EXPECT_FALSE(h.saw_finishing);
}

TEST(VoiceComposer, TooShortClearsSendingWithoutSending) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  auto gate = std::make_shared<std::atomic<bool>>(false);
  auto opens = std::make_shared<std::atomic<int>>(0);
  h.source = [=]() { return std::make_unique<GatedSource>(gate, opens, /*fail=*/true); };
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(WaitFor([&] { return opens->load() == 1; }));
  h.composer->OnSendButton(); // released before the microphone was ready
  gate->store(true);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_FALSE(h.state.voice_finishing);
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, ALateResultOfAnEarlierRecordingDoesNotLeaveSendingOn) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->BeginAt(0, 0, nullptr, false);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnSendButton();
  ASSERT_TRUE(h.WaitFile()); // its result is queued, not yet seen by the UI
  h.composer->DiscardAll();  // the vault locks: sending is over
  EXPECT_FALSE(h.state.voice_finishing);
  h.composer->BeginAt(0, 0, nullptr, false); // a new recording: the old result is now a stale generation
  h.ui.RunAll();
  EXPECT_FALSE(h.state.voice_finishing);
  EXPECT_TRUE(h.sent.empty());
}

TEST(VoiceComposer, AtTheLimitTheRecorderEndsItselfAndSendingShows) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(1000));
  };
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_TRUE(h.saw_finishing) << "the limit ended it without a release";
  EXPECT_FALSE(h.state.voice_finishing);
}

TEST(VoiceComposer, ALateProgressTickAfterTheResultDoesNotSetSendingAgain) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/1);
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(1000));
  };
  h.composer->BeginAt(0, 0, nullptr, /*hold=*/false);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  ASSERT_FALSE(h.state.voice_finishing);
  VoiceComposerTestAccess::Progress(*h.composer, 1, 1000); // the recording's own generation, past the limit
  EXPECT_FALSE(h.state.voice_finishing);
}

// The wide layout's microphone: one click records, the next click sends (OnToggleButton). A fake source throughout.
TEST(VoiceComposer, AToggleClickStartsAndTheNextClickSendsToTheChatItWasRecordedIn) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->OnToggleButton();
  EXPECT_TRUE(h.state.voice_recording) << "the microphone is lit from the first click";
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.thread = "B";
  h.composer->OnToggleButton();
  EXPECT_FALSE(h.state.voice_recording);
  EXPECT_TRUE(h.state.voice_finishing) << "Sending... until the file is written";
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.sent[0].first, "A");
  EXPECT_NE(h.sent[0].second.find("voice-3000.ogg"), std::string::npos);
  EXPECT_FALSE(h.state.voice_finishing);
}

TEST(VoiceComposer, EscapeCancelsAToggleRecordingAndNothingIsSent) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_TRUE(h.composer->OnEscape());
  EXPECT_FALSE(h.state.voice_recording);
  EXPECT_FALSE(h.state.voice_finishing);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0);
}

TEST(VoiceComposer, TheCancelButtonEndsAToggleRecordingSilently) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnCancelButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_TRUE(h.said.empty());
}

TEST(VoiceComposer, AClickWhileTheFileIsBeingWrittenStartsNothingNew) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnToggleButton(); // sends
  h.composer->OnToggleButton(); // a double click: the recording is over, the file is still being written
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(h.opens.load(), 1);
  EXPECT_EQ(h.sent.size(), 1u);
}

TEST(VoiceComposer, AToggleRecordingUnderTheMinimumSaysTooShortAndSendsNothing) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(300));
  };
  h.composer->OnToggleButton();
  ASSERT_TRUE(WaitFor([&] { return h.opens.load() == 1; }));
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_TRUE(h.Said("chat.voice.too_short"));
}

TEST(VoiceComposer, ACallRefusesAToggleRecording) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.call_live = true;
  h.composer->OnToggleButton();
  h.ui.RunAll();
  EXPECT_EQ(h.opens.load(), 0);
  EXPECT_FALSE(h.state.voice_recording);
  EXPECT_TRUE(h.Said("chat.voice.busy_call"));
}

TEST(VoiceComposer, TheLevelBarMovesWithTheVoiceAndIsEmptyAgainAfterTheEnd) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  EXPECT_EQ(std::string(h.state.voice_level_width), "0%");
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return std::string(h.state.voice_level_width) != "0%"; })) << "the tone has a level";
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_EQ(std::string(h.state.voice_level_width), "0%");
}

// The change callback (the controller's refreshes the composer's input state, which calls Refresh again) must fire only
// when the button's state changed, or Refresh would call itself without end.
TEST(VoiceComposer, RefreshCallsTheChangeCallbackOnlyWhenTheStateChanged) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.refresh_on_dirty = true;
  h.composer->Refresh();
  const int first = h.dirty_calls;
  EXPECT_GE(first, 1) << "the first Refresh shows the button";
  EXPECT_LT(first, 5) << "and does not loop";
  h.composer->Refresh();
  h.composer->Refresh();
  EXPECT_EQ(h.dirty_calls, first) << "nothing changed: the callback stays quiet";
}

TEST(VoiceComposer, TheToggleRecordingNeverReEntersRefreshThroughTheChangeCallback) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.refresh_on_dirty = true;
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.WaitTimer("0:03"));
  const int during = h.dirty_calls;
  EXPECT_LT(during, 200) << "a bounded number of model updates for a 3 s recording (progress, level, hint)";
  h.composer->OnToggleButton();
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  EXPECT_LT(h.dirty_calls, 400);
  EXPECT_EQ(h.sent.size(), 1u);
}

// A tap on the bar is not a message: under 250 ms it only says how the bar works; 250 ms to 1 s is "too short". Neither
// sends and neither leaves a file.
TEST(VoiceComposer, ATapUnderAQuarterSecondSaysHoldToTalkAndSendsNothing) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnRelease(std::chrono::steady_clock::now() + 100ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  h.ui.RunAll();
  ASSERT_EQ(h.said.size(), 1u);
  EXPECT_EQ(h.said[0], Tr("chat.voice.tap_hint"));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
}

TEST(VoiceComposer, BetweenAQuarterAndOneSecondItIsTooShortAndNothingIsKept) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.source = [opens = h.opens_ptr]() {
    ++*opens;
    return std::make_unique<ScriptedSource>(Tone(500)); // half a second of sound, then nothing more
  };
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(WaitFor([&] { return h.opens.load() == 1; }));
  std::this_thread::sleep_for(60ms);
  h.composer->OnRelease(std::chrono::steady_clock::now() + 500ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  ASSERT_EQ(h.said.size(), 1u);
  EXPECT_EQ(h.said[0], Tr("chat.voice.too_short"));
  EXPECT_TRUE(h.sent.empty());
  EXPECT_EQ(h.dir.FileCount(), 0u);
}

TEST(VoiceComposer, ATapThatSlidUpFirstIsSilent) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnMove(0, 20);
  h.composer->OnRelease(std::chrono::steady_clock::now() + 100ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.said.empty());
  EXPECT_TRUE(h.sent.empty());
}

// The pointer arrives in pixels of a 3x screen; the cancel line and the bubble are in dp. The whole path is driven with
// pixel coordinates (BeginAt -> OnMove -> OnRelease).
TEST(VoiceComposer, OnA3xScreenThePixelsAreConvertedToDpForTheCancelAndTheBubble) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.ratio = 3.f;
  h.fingers = {7};
  h.composer->BeginAt(600, 1500, nullptr, true); // x 200dp, y 500dp
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_TRUE(h.state.voice_bubble);
  h.composer->OnMove(600, 1335); // 165 px = 55 dp up: not yet
  EXPECT_FALSE(h.state.voice_cancel_zone);
  h.composer->OnMove(600, 1332); // 168 px = 56 dp up: armed
  EXPECT_TRUE(h.state.voice_cancel_zone);
  h.composer->OnMove(600, 1356); // 144 px = 48 dp up: still armed
  EXPECT_TRUE(h.state.voice_cancel_zone);
  h.composer->OnMove(600, 1359); // 141 px = 47 dp up: let go
  EXPECT_FALSE(h.state.voice_cancel_zone);
  // 3 px is 1 dp: it is not "168 dp".
  h.composer->OnMove(600, 1500 - 56);
  EXPECT_FALSE(h.state.voice_cancel_zone);
  // The bubble: centred on x = 200dp (left 112), its bottom 98dp above y = 500dp (top 500 - 98 - 64).
  EXPECT_EQ(std::string(h.state.voice_bubble_left), "112dp");
  EXPECT_EQ(std::string(h.state.voice_bubble_top), "338dp");
  h.composer->OnMove(600, 1332);
  h.composer->OnRelease(Later());
  EXPECT_TRUE(h.sent.empty()) << "armed and released: cancelled";
  EXPECT_FALSE(h.state.voice_bubble);
}

TEST(VoiceComposer, TheBubbleFollowsTheFingerAndStaysOnTheScreenAtBothEnds) {
  SKIP_WITHOUT_VOICE();
  struct Case {
    float x_px;
    const char* left;
  };
  for (const Case c : {Case{30, "16dp"}, Case{600, "112dp"}, Case{1170, "201dp"}}) {
    Harness h;
    h.ratio = 3.f;
    h.fingers = {7};
    h.composer->BeginAt(c.x_px, 1500, nullptr, true);
    ASSERT_TRUE(h.WaitTimer("0:03"));
    EXPECT_EQ(std::string(h.state.voice_bubble_left), c.left) << c.x_px;
  }
  // Dragging sideways: inside 24dp of the anchor nothing moves; past it the bubble is dragged along.
  Harness h;
  h.ratio = 3.f;
  h.fingers = {7};
  h.composer->BeginAt(600, 1500, nullptr, true); // anchor 200dp
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnMove(600 + 60, 1500); // 20dp aside: inside the dead zone
  EXPECT_EQ(std::string(h.state.voice_bubble_left), "112dp");
  h.composer->OnMove(600 + 150, 1500); // 50dp aside: the anchor follows to 226
  EXPECT_EQ(std::string(h.state.voice_bubble_left), "138dp");
  h.composer->OnMove(0, 1500); // all the way left: clamped to the margin
  EXPECT_EQ(std::string(h.state.voice_bubble_left), "16dp");
}

// The bubble is a child of the chat overlay, which starts under the status bar: its `top` is relative to the overlay.
TEST(VoiceComposer, TheBubbleTopIsRelativeToTheChatOverlayNotTheWindow) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.overlay_top_dp = 60.f;
  h.fingers = {7};
  h.composer->BeginAt(0, 500, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_EQ(std::string(h.state.voice_bubble_top), "278dp") << "500 - 98 - 64 in the window, minus the overlay's 60";
}

TEST(VoiceComposer, TheBubbleNeverRisesIntoTheHeader) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.top_limit_dp = 110.f; // the header's lower edge
  h.fingers = {7};
  h.composer->BeginAt(0, 150, nullptr, true); // the press is 150dp down: 150 - 98 - 64 would be negative
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_EQ(std::string(h.state.voice_bubble_top), "110dp");
}

// The level bars: the newest level enters on the right (UI thread only), 4dp for silence and 20dp for full scale.
TEST(VoiceComposer, TheLevelsFeedTheBarsNewestLast) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  VoiceComposerTestAccess::Progress(*h.composer, 1, 3100, 1.f);
  VoiceComposerTestAccess::Progress(*h.composer, 1, 3200, 0.5f);
  EXPECT_EQ(std::string(h.state.voice_lv[11]), "12dp");
  EXPECT_EQ(std::string(h.state.voice_lv[10]), "20dp");
  VoiceComposerTestAccess::Progress(*h.composer, 7, 3300, 1.f); // another recording's tick: ignored
  EXPECT_EQ(std::string(h.state.voice_lv[11]), "12dp");
}

TEST(VoiceComposer, TheCountdownShowsInTheBubbleAndThePlainHoldHasNoHint) {
  SKIP_WITHOUT_VOICE();
  Harness h(/*max_seconds=*/30);
  h.fingers = {7};
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  EXPECT_FALSE(h.state.voice_countdown);
  EXPECT_EQ(std::string(h.state.voice_hint), "") << "the bubble's own lines say how to cancel";
  VoiceComposerTestAccess::Progress(*h.composer, 1, 21000, 0.f); // 9 s left
  EXPECT_TRUE(h.state.voice_countdown);
  EXPECT_EQ(std::string(h.state.voice_hint), Tr("chat.voice.countdown", {{"seconds", "9"}}));
}

// Voice mode is the person's choice: only the keyboard key, an edit, or a chat that cannot take voice leave it. A send, a
// cancel, a too-short hold or a call do not.
TEST(VoiceComposer, VoiceModeStaysAfterASendACancelATapAndACall) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.state.voice_mode = true;
  h.fingers = {7};
  h.composer->Refresh();
  ASSERT_TRUE(h.state.voice_mode);
  // send
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnRelease(Later());
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.sent.empty(); }));
  h.composer->Refresh();
  EXPECT_TRUE(h.state.voice_mode) << "after a send";
  // cancel (slide up)
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnMove(0, 20);
  h.composer->OnRelease(Later());
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.state.voice_mode) << "after a cancel";
  // tap
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.composer->OnRelease(std::chrono::steady_clock::now() + 50ms);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  EXPECT_TRUE(h.state.voice_mode) << "after a tap that was too short";
  // a call
  h.composer->BeginAt(0, 100, nullptr, true);
  ASSERT_TRUE(h.WaitTimer("0:03"));
  h.call_live = true;
  h.composer->Tick(std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(h.ui.PumpUntil([&] { return !h.composer->Active(); }));
  h.composer->Refresh();
  EXPECT_TRUE(h.state.voice_mode) << "after a recording a call interrupted";
}

TEST(VoiceComposer, AChatThatCannotTakeVoiceLeavesVoiceModeAndNothingElseDoes) {
  SKIP_WITHOUT_VOICE();
  Harness h;
  h.state.voice_mode = true;
  h.eligible = false; // the AI page, a chat with no peer
  h.composer->Refresh();
  EXPECT_FALSE(h.state.voice_mode);
}

} // namespace
} // namespace pbr
